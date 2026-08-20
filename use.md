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

- **症状**：slide 走 pselect 路由不 panic，但 `bad leaked pointer=7143b9bf1b12b3d4`（顶 16 位 0x7143，非 0xffff 内核指针），KASLR 泄露失败
- **根因**：`SLIDE_LOGGERS_0_1_OFF` 原值 `0x01fe2918` = `loggers[0][0]`（**NF_LOG_TYPE_LOG** 槽）。Android 只把 nfulnl_logger 注册到 `loggers[*][NF_LOG_TYPE_ULOG=1]` 槽，LOG 槽运行时为 NULL。内核读到 0 → `proc_do_uuid` 发现 sysctl_bootid 为空 → 生成随机 UUID → 用户读到垃圾值
- **修正**：`0x01fe2918` → `0x01fe2920`（= loggers 起始 + ULOG*8 = `loggers[0][1]` 槽，运行时存 nfulnl_logger 带KASLR指针）
- **依据**：IDA 验证 nfulnl_logger@0x01fe29c8（name 指针+type=1=ULOG），loggers[11][2]数组=176字节，0x01fe29c8-176=0x01fe2918=loggers 起始；NF_LOG_TYPE_ULOG=1（源码 include/net/netfilter/nf_log.h）；slot 定义 = loggers + ULOG*8
- BUILD_VARIANT_LABEL / BUILD_FINGERPRINT 顺带更正为 shennong_OS3.0.307.0.WNBCNXM（原为 duchamp_303，仅日志标识，不影响逻辑）
- 其他 SLIDE_* 偏移经 IDA 核对正确：NFULNL_LOGGER@0x01fe29c8、RANDOM_BOOT_ID_DATA@0x02107448（指向 sysctl_bootid 的 .data 槽，entry mode=0o444）、SYSCTL_BOOTID@0x0224a458
