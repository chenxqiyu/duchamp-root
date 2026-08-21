# shennong (小米 14 Pro) boot.img 内核分析报告

> 分析对象: `output\allimg\boot.img` (96 MiB, HyperOS OS3.0.307.0.WNBCNXM)
> 方法: 自写解析链 `extract_boot.py → kallsyms.py → btf.py`（不依赖任何第三方 PoC 仓库）
> 分析日期: 2026-08-21

---

## 1. boot.img / 内核 Image 基本信息

| 项目 | 值 |
|---|---|
| boot.img 大小 | 100,663,296 字节 (96 MiB) |
| Android boot magic | `ANDROID!` ✓ |
| 内核 payload | gzip 压缩, 解压后 **35,432,960 字节** |
| arm64 Image magic | `ARM\x64` ✓ |
| text_offset | 0x0 |
| image_size | **0x2270000** (35.8 MiB) |
| 内核版本 | **Linux version 6.1.138-android14-11-g0c3d559bcd85** |
| 构建 | OS3.0.307.0.WNBCNXM-user-16.0 (Android 16) |
| `_text` 基址 (KASLR slide=0) | **0xffffffc008000000** |
| `_stext` | 0xffffffc008010000 (RVA 0x10000, 2MiB 对齐) |
| 虚拟地址换算 | `VA = 0xffffffc008000000 + RVA` |

`relative_base`(kallsyms 相对基址)= 0xffffffc008000000,与 `_text` 相等,存于 Image 偏移 `0x131cad8`。

---

## 2. kallsyms 布局(Image 内偏移)

| 区段 | Image 偏移 | 说明 |
|---|---|---|
| kallsyms_offsets (u32×N) | `0x12b9520` | N = **101742** 条,升序 RVA |
| relative_base (u64) | `0x131cad8` | 0xffffffc008000000 |
| num_syms (u32) | `0x131cae0` | 101742 |
| kallsyms_names | `0x131cae8 .. 0x1470c17` | 压缩名字流 |
| markers (u32×398) | `0x1470c17` | 每 256 条一个 |
| seqs_of_names (u8×3×N) | `~0x1470c5d` | 与 token_table 间留验证间隙 |
| token_table (256 串) | `0x14bbaa0 .. 0x14bbe28` | 256 条可打印 token,对齐验证 ✓ |
| token_index (u16×256) | `0x14bbe28` | 严格递增,首项 0 |

### 2.1 名字编码格式(关键发现)

每条目 = `[len][token 流]`,token 流解出的是 **"类型字母 + 符号名" 整体压缩**:

- 首 token 是**单字符**(如 `T`/`d`/`b`)时 = 类型字母,如 `futex_hash`;
- 首 token 是**多字符**时,类型字母并入首 token —— 例:
  - token `"dr"` + `andom_table` → 实际 `random_table`(类型 `d`)
  - token `"tp"` + `roc_do_uuid` → 实际 `proc_do_uuid`(类型 `t`)
  - token `"ts"` + `ock_splice_read` → 实际 `sock_splice_read`

解码器若直接剥掉首 token,这些符号会**丢首字母**(前次分析 `random_table` 显示为 `andom_table` 的根因)。修正后 101742 条全部正确解码。

### 2.2 幽灵符号带(BOLT/PGO 副本)

BOLT 优化在 Image 中留有错位符号副本(同名成对:一个真实地址 + 一个密集窄带内假地址)。直方图定位后剔除窄带副本,保留 **101570 条**有效符号。关键符号(init_task、loggers、random_table 等)均验证为唯一。

---

## 3. 内嵌 BTF

| 项目 | 值 |
|---|---|
| BTF 偏移(Image) | **0x16473cc** |
| type 段长度 | 0x32d7bc |
| 字符串段长度 | 0x2331a2 |
| 完整性 | 唯一大候选,直接可解析 |

以下结构体偏移全部来自该 BTF(编译期权威,与实际运行时布局一致)。

---

## 4. 关键符号地址表(与 target.h 对照,全部 ✓)

### 4.1 提权核心数据符号

| 符号 | RVA | VA(slide=0) | target.h 宏 | 状态 |
|---|---|---|---|---|
| `_text` | 0x0000000 | 0xffffffc008000000 | KIMAGE_TEXT_BASE | ✓ |
| `init_task` | 0x01fef600 | 0xffffffc009fef600 | INIT_TASK_OFF | ✓ |
| `init_cred` | 0x02001a68 | 0xffffffc00a001a68 | — | (备用) |
| `init_uts_ns` | 0x02171eb0 | 0xffffffc00a171eb0 | INIT_UTS_NS_OFF | ✓ |
| `empty_zero_page` | 0x021d0000 | 0xffffffc00a1d0000 | EMPTY_ZERO_PAGE_OFF | ✓ |
| `root_task_group` | 0x021d7580 | 0xffffffc00a1d7580 | ROOT_TASK_GROUP_OFF | ✓ |
| `selinux_state` | 0x022293d0 | 0xffffffc00a2293d0 | SELINUX_ENFORCING_OFF | ✓ |
| `security_hook_heads` | 0x015b7078 | 0xffffffc0095b7078 | SECURITY_HOOK_HEADS_OFF | ✓ |
| `kmalloc_caches` | 0x015b6bb8 | 0xffffffc0095b6bb8 | KMALLOC_CACHES_OFF | ✓ |
| `selinux_blob_sizes` | 0x015b7788 | 0xffffffc0095b7788 | SELINUX_BLOB_SIZES_OFF | ✓ |
| `anon_pipe_buf_ops` | 0x010f7150 | 0xffffffc0090f7150 | ANON_PIPE_BUF_OPS_OFF | ✓ |

### 4.2 KASLR 泄露机制符号(slide)

| 符号 | RVA | VA | target.h 宏 | 状态 |
|---|---|---|---|---|
| `loggers` | 0x01fe2910 | 0xffffffc009fe2910 | SLIDE_LOGGERS_0_1_OFF(+0x10=loggers[1]) | ✓ |
| `nfulnl_logger` | 0x01fe29c8 | 0xffffffc009fe29c8 | SLIDE_NFULNL_LOGGER_OFF | ✓ |
| `random_table` | 0x0210740 | 0xffffffc00a107340 | —(符号+结构双验证) | ✓ |
| `sysctl_bootid` | 0x0224a458 | 0xffffffc00a24a458 | SLIDE_SYSCTL_BOOTID_OFF | ✓ |
| random_table[4].data 字段 | 0x02107448 | 0xffffffc00a107448 | SLIDE_RANDOM_BOOT_ID_DATA_OFF | ✓ |
| `proc_do_uuid` | 0x008574a8 | 0xffffffc0088574a8 | —(= random_table[4].handler 交叉验证) | ✓ |

**三重交叉验证**: `random_table[4]` 条目解析 → procname=`boot_id`,data=0xffffffc00a24a458 == 符号 `sysctl_bootid`,handler=0xffffffc0088574a8 == 符号 `proc_do_uuid`。

### 4.3 futex / select(CVE-2026-43499 漏洞路径)

| 符号 | RVA | VA |
|---|---|---|
| `do_futex` | 0x019f5b8 | 0xffffffc00819f5b8 |
| `__arm64_sys_futex` | 0x019f834 | 0xffffffc00819f834 |
| `futex_wait_requeue_pi` | 0x01a214c | 0xffffffc0081a214c |
| `core_sys_select` | 0x03b7a50 | 0xffffffc0083b7a50 |
| `arm64_sys_select` | 0x03b85e0 | 0xffffffc0083b85e0 |
| `__arm64_sys_pselect6` | 0x03b8818 | 0xffffffc0083b8818 |

### 4.4 ashmem(fops 劫持原语)

| 符号/字段 | RVA | 验证方式 |
|---|---|---|
| `ashmem_fops` | 0x0126d7b8 | 符号 + miscdevice.fops 指针双验证 |
| `ashmem_ioctl`(fops+0x50) | 0x00c2db80 | fops 表内指针 == 符号 |
| `compat_ashmem_ioctl`(fops+0x58) | 0x00c2e4b8 | 同上 |
| `ashmem_mmap`(fops+0x60) | 0x00c2e510 | 同上 |
| `ashmem_open`(fops+0x70) | 0x00c2e730 | 同上 |
| `ashmem_release`(fops+0x80) | 0x00c2e7b8 | 同上 |
| `ashmem_show_fdinfo`(fops+0xe0) | 0x00c2e8d8 | 同上 |
| `ashmem_miscs`(miscdevice) | 0x0214beb0 | minor=0xff(DYNAMIC), name→.rodata, fops@+0x10=ashmem_fops ✓ |
| ASHMEM_MISC_FOPS_OFF | 0x0214bec0 | = &ashmem_miscs.fops ✓ |

ashmem_fops 实测指针表(owner=0, read/write=0, read_iter=ashmem_read_iter@0xc2dac0, splice_read=0):与 target.h 全部一致。

### 4.5 configfs / splice(信息泄露原语)

| 符号 | RVA | target.h 宏 | 状态 |
|---|---|---|---|
| `configfs_read_iter` | 0x00461b9c | CONFIGFS_READ_ITER_OFF | ✓ |
| `configfs_bin_write_iter` | 0x004620cc | CONFIGFS_BIN_WRITE_ITER_OFF | ✓ |
| `generic_file_splice_read` | 0x003e3fa8 | COPY_SPLICE_READ_OFF(6.1 无 copy_splice_read) | ✓ |
| `noop_llseek` | 0x00396780 | NOOP_LLSEEK_OFF | ✓ |
| `sock_splice_read` | 0x00d0ea5c | —(未使用) | 备查 |

---

## 5. BTF 结构体偏移与 target.h 修正

### 5.1 验证一致(无需修改)

**task_struct**(size=0x12c0): usage=0x40, prio=0x84, normal_prio=0x8c, sched_task_group=0x348, uclamp_req=0x350, uclamp=0x358, tasks=0x550, real_cred=0x830, cred=0x838, pi_lock=0x924, pi_waiters=0x938, pi_top_task=0x948, pi_blocked_on=0x950, thread_info.flags=0x0, mm=0x5a0, comm=0x848(见修正)

**rt_mutex_waiter**(size=0x58): tree_entry=0x0, pi_tree_entry=0x18, task=0x30, lock=0x38, wake_state=0x40, prio=0x44, deadline=0x48, ww_ctx=0x50 —— 与 WAITER_*/FAKE_WAITER_* 宏全部一致 ✓

**cred**(size=0xb0): uid=0x4, securebits=0x24, cap_inheritable=0x28

**pipe_inode_info**(size=0xb8): head=0x60, tail=0x64, max_usage=0x68, ring_size=0x6c, nr_accounted=0x70, readers=0x74, writers=0x78, files=0x7c, tmp_page=0x90, bufs=0xa8, user=0xb0 —— 全部 ✓

**configfs_buffer**(size=0x80): page=0x10, needs_read_fill=0x50, bin_buffer=0x58, bin_buffer_size=0x60, cb_max_size=0x64 —— 全部 ✓

**seccomp**(size=0x10): mode=0, filter_count=4, filter=8 ✓;**file_operations**: unlocked_ioctl=0x50, compat_ioctl=0x58, mmap=0x60, open=0x70, release=0x80, show_fdinfo=0xe0 ✓

### 5.2 本次修正的 8 个偏移(旧值 → BTF 权威值)

| 宏 | 旧值(旧内核布局) | **BTF 新值** | 影响 |
|---|---|---|---|
| TASK_PID_OFF | 0x618 | **0x630** | root.c 读 pid 校验 task |
| TASK_TGID_OFF | 0x61c | **0x634** | 同上 |
| TASK_REAL_PARENT_OFF | 0x628 | **0x640** | parent 校验 |
| TASK_ATOMIC_FLAGS_OFF | 0x5d8 | **0x5f0** | 清 PF_ 标志,写错会损坏其它字段 |
| TASK_COMM_OFF | 0x830 | **0x848** | **0x830 是 real_cred 指针!旧值读 comm 校验必然失败 → 提权中断的直接原因** |
| TASK_SECCOMP_OFF | 0x8e8 | **0x900** | 清 seccomp;写 0x8e8 会破坏相邻字段 |
| CRED_SECURITY_OFF | 128 (0x80) | **0x78** | 0x80 实为 cred.user 指针,SELinux label 读取错位 |
| FOPS_SPLICE_READ_OFF | 0xc0 | **0xc8** | 0xc0 是 splice_write;劫持 copy_splice_read 写错槽位 |

旧值来源判断: 旧 target.h 的 real_cred/cred(0x830/0x838)与本内核一致,但 pid/tgid/comm 等是旧 build(6.1.75 时代)布局,从未随 6.1.138 更新。

### 5.3 待运行时确认项

- `MM_OWNER_OFF=1032`: 本内核 BTF 中 **mm_struct 无 owner 字段**(未启用相关配置);该宏在 .c 中无使用点,保持原值不影响。
- `SLIDE_RT_MUTEX_WAITER_LEGACY=0`: BTF 证实 6.1.138 的 rt_mutex_waiter 为标准布局(tree_entry@0),legacy 布局未启用 —— 正确。
- `PSELECT_WAITER_WORD_SHIFT=3`: 由反汇编推导,支持 `SLIDE_SHIFT` 环境变量运行时覆盖。

---

## 6. random_table 完整条目(RVA 0x2107340, ctl_table=0x40)

| # | procname | data(RVA) | maxlen | mode | handler |
|---|---|---|---|---|---|
| 0 | poolsize | 0x2107500(sysctl_poolsize) | 4 | 0444 | proc_dointvec@0x8bb1f0 |
| 1 | entropy_avail | 0x21072c4 | 4 | 0444 | proc_dointvec@0x8bb1f0 |
| 2 | write_wakeup_threshold | 0x2107504 | 4 | 0644 | proc_douintvec@0x885747c |
| 3 | urandom_min_reseed_secs | 0x2107508 | 4 | 0644 | proc_douintvec@0x885747c |
| 4 | **boot_id** | **0x224a458(sysctl_bootid)** | 0 | 0444 | **proc_do_uuid@0x88574a8** |
| 5 | uuid | 0(NULL) | 0 | 0444 | proc_do_uuid@0x88574a8 |
| 6 | (表尾哨兵) | — | — | — | — |

slide 机制: `SLIDE_RANDOM_BOOT_ID_DATA_OFF(0x2107448)` 是 boot_id 条目的 **data 指针字段**位置 —— UAF 改写它后,读 `/proc/sys/kernel/random/boot_id` 即可回读任意内核指针;`SLIDE_SYSCTL_BOOTID_OFF(0x224a458)` 则是 boot_id 缓冲区本体(16 字节 UUID)。

## 7. loggers 数组(RVA 0x1fe2910)

`nf_logger *loggers[NFPROTO_NUMPROTO]`(约 0xb0 字节,_nfnetlink 子系统)。`loggers[1]`(0x1fe2920)= SLIDE_LOGGERS_0_1_OFF;`nfulnl_logger`(0x1fe29c8)在数组后 0xb8 处,内容:

```
+0x00 name    → 0xffffffc009511d48 (.rodata "ULOG")
+0x08 type    = 1 (NFLOGGER_TYPE_UNSPEC)
+0x10 logfn   → 0xffffffc008dfbca4 (nfulnl_log_packet)
```

邻域符号(nf_conntrack_locks_all=0x1fe29f0 等)与 kallsyms 排序完全吻合,进一步确认 loggers 基址无误。

---

## 8. 结论与下一步

1. **符号层**: target.h 全部 25 个符号宏经符号表+指针内容+结构解析三重验证,**零修正**;KIMAGE_TEXT_BASE、SLIDE_* 泄露锚点全部正确。
2. **结构层**: 发现并修正 8 个过期偏移(§5.2)。其中 `TASK_COMM_OFF=0x830`(撞 real_cred 指针)是此前 task 校验失败/提权中断的直接原因;`FOPS_SPLICE_READ_OFF=0xc0` 使 splice 劫持写到 splice_write 槽位,读原语失效。
3. 修正后需重编译 `preload.so` 并在设备上重新执行 `LD_PRELOAD=/data/local/tmp/preload.so /system/bin/true` 验证 slide 泄露与 root 流程。
