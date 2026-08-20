# duchamp-root 使用命令

项目根目录：`I:\云盘缓存\down\shennong-ota_full-OS3.0.307.0.WNBCNXM-user-16.0-5bcfc9ad5d\output\duchamp-root`

## 1. 编译 preload.so（PROJECT=duchamp）

> 注意：duchamp 有专属 `src/targets/duchamp/slide.c`（legacy words 布局，shennong 6.1 专用），
> 编译时**必须**用该路径替换 `src/slide.c`。

```powershell
cd "I:\云盘缓存\down\shennong-ota_full-OS3.0.307.0.WNBCNXM-user-16.0-5bcfc9ad5d\output\duchamp-root"
$clang = "C:\Users\Administrator\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
& $clang --target=aarch64-linux-android35 -fPIC -O2 -g0 -Wall -Wextra -Isrc -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function '-DTARGET_CONFIG_H="targets/duchamp/target.h"' src/main.c src/util.c src/targets/duchamp/slide.c src/fops.c src/pipe.c src/root.c src/preload.c src/ksud_blob.S -shared -o build/duchamp/bin/preload.so -pthread
```

产物：`build/duchamp/bin/preload.so`

## 2. 编译其他 target（如 blazer）

```powershell
cd "I:\云盘缓存\down\shennong-ota_full-OS3.0.307.0.WNBCNXM-user-16.0-5bcfc9ad5d\output\duchamp-root"
$clang = "C:\Users\Administrator\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
& $clang --target=aarch64-linux-android35 -fPIC -O2 -g0 -Wall -Wextra -Isrc -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function '-DTARGET_CONFIG_H="targets/blazer-CP2A.260605.012/target.h"' src/main.c src/util.c src/slide.c src/fops.c src/pipe.c src/root.c src/preload.c src/ksud_blob.S -shared -o build/blazer-CP2A.260605.012/bin/preload.so -pthread
```

## 3. 推送并运行

```powershell
adb push build/duchamp/bin/preload.so /data/local/tmp/preload.so
adb shell 'chmod 0644 /data/local/tmp/preload.so'
adb shell 'LD_PRELOAD=/data/local/tmp/preload.so /system/bin/true'
```

## 4. 已验证的 shennong 修复点（2026-08-21）

- `src/targets/duchamp/target.h`：FAKE_WAITER_* 为 6.4+ 布局（与 blazer 标准一致）；ASHMEM_*/CONFIGFS_*/COPY_SPLICE_READ/NOOP_LLSEEK/SELINUX_BLOB_SIZES/SECURITY_HOOK_HEADS/KMALLOC_CACHES 已更新为 shennong kallsyms 值
- `src/common.h`：MM_STRUCT_SZ = 0x3c0（shennong sizeof(mm_struct)）
- **新增 `src/targets/duchamp/slide.c`**：pselect words 改为 6.1 legacy 布局（tree@0x00/pi_tree@0x18/task@0x30/lock@0x38/wake+prio@0x40/0x44/deadline@0x48/ww_ctx@0x50），修复 shennong 6.1 legacy 内核下栈上 fake waiter 错位导致的 slide child panic
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
  3. duchamp slide.c 相对参考版（CVE-2026-43499-Poc-Analysis/source/src/slide.c）的改动：固定 nice=19 vs 动态(calls%19)+1、legacy 11-word vs 6.4+ 13-word、consumer 去掉 tgkill 存活检查
- **设备限制**：无 root，/proc/kallsyms 与 /sys/kernel/btf/vmlinux 不可读，无法用 generate_target.py 算法；/proc/config.gz 可读
- 其他 SLIDE_* 偏移经 IDA 核对正确：NFULNL_LOGGER@0x01fe29c8、RANDOM_BOOT_ID_DATA@0x02107448（指向 sysctl_bootid 的 .data 槽，entry mode=0o444）、SYSCTL_BOOTID@0x0224a458
