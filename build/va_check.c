#include <stdint.h>
#include "targets/shennong/target.h"

#define IMAGE(x) (KIMAGE_TEXT_BASE + (x))
#define RUNTIME_DATA_ALIAS(off) (P0_PAGE_OFFSET + P0_KERNEL_PHYS_LOAD + (off))

_Static_assert(P0_PAGE_OFFSET == 0xffffff8000000000ULL, "39-bit PAGE_OFFSET");
_Static_assert(DIRECT_MAP_BASE == P0_PAGE_OFFSET + P0_PHYS_OFFSET,
               "direct-map scan window starts at DRAM linear alias");
_Static_assert(DIRECT_MAP_END == P0_PAGE_OFFSET + (64ULL << 30),
               "direct-map scan window upper bound = PAGE_OFFSET+64GB");
_Static_assert(KERNELSNITCH_IDENTITY_START == P0_PAGE_OFFSET,
               "ksnitch window starts at PAGE_OFFSET");
_Static_assert(KERNELSNITCH_IDENTITY_END - KERNELSNITCH_IDENTITY_START == 64ULL << 30,
               "ksnitch window covers 64GB");

_Static_assert(P0_DATA_ALIAS_CONST(IMAGE(INIT_TASK_OFF)) == 0xffffff80a9fef600ULL,
               "init_task linear alias must equal run2 measured value");
_Static_assert(P0_DATA_ALIAS_CONST(IMAGE(INIT_TASK_OFF)) ==
                   RUNTIME_DATA_ALIAS(INIT_TASK_OFF),
               "compile-time macro must match runtime p0_data_alias formula");
_Static_assert(P0_DATA_ALIAS_CONST(IMAGE(SLIDE_LOGGERS_0_1_OFF)) ==
                   0xffffff80a9fe2918ULL,
               "loggers[1] alias");
_Static_assert(P0_DATA_ALIAS_CONST(IMAGE(SLIDE_SYSCTL_BOOTID_OFF)) ==
                   0xffffff80aa24a458ULL,
               "sysctl bootid alias");
_Static_assert(RUNTIME_DATA_ALIAS(ASHMEM_MISC_FOPS_OFF) == 0xffffff80aa14bec0ULL,
               "misc_fops linear alias = PAGE_OFFSET + 0xaa14bec0");

_Static_assert(P0_DATA_ALIAS_CONST(IMAGE(INIT_TASK_OFF)) >= KERNELSNITCH_IDENTITY_START &&
                   P0_DATA_ALIAS_CONST(IMAGE(INIT_TASK_OFF)) < KERNELSNITCH_IDENTITY_END,
               "kernel image aliases fall inside ksnitch DRAM window");
_Static_assert(RUNTIME_DATA_ALIAS(ASHMEM_MISC_FOPS_OFF) >= DIRECT_MAP_BASE &&
                   RUNTIME_DATA_ALIAS(ASHMEM_MISC_FOPS_OFF) < DIRECT_MAP_END,
               "data alias inside linear region");

int va_check_main(void);
int va_check_main(void) { return 0; }
