#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define SYS_FUTEX_WAITV 449

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

struct futex_waitv {
  uint64_t val;
  uint64_t uaddr;
  uint32_t flags;
  uint32_t reserved;
};

struct waitv_result {
  long ret;
  int err;
  int sig;
  double ms;
};

struct waker_arg {
  uint32_t *word;
  int flags;
};

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void *waker_main(void *p) {
  struct waker_arg *a = p;
  usleep(20000);
  __atomic_store_n(a->word, 1u, __ATOMIC_RELEASE);
  syscall(SYS_futex, a->word, FUTEX_WAKE | a->flags, 1, NULL, NULL, 0);
  return NULL;
}

/* Seccomp denies futex_waitv with SIGSYS, which is fatal in-process; isolate each
   attempt so the probe keeps reporting after a kill. */
static void waitv_in_child(int flags, struct waitv_result *out) {
  int fds[2];
  if (pipe(fds) != 0) {
    fprintf(stderr, "pipe: %s\n", strerror(errno));
    exit(2);
  }

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "fork: %s\n", strerror(errno));
    exit(2);
  }

  if (pid == 0) {
    close(fds[0]);
    struct waitv_result r = {.ret = -1, .err = 0, .sig = 0, .ms = 0.0};
    uint32_t *words = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (words == MAP_FAILED) {
      r.err = errno;
    } else {
      struct futex_waitv w[2];
      memset(w, 0, sizeof(w));
      w[0].uaddr = (uintptr_t)&words[0];
      w[1].uaddr = (uintptr_t)&words[16];
      w[0].flags = (uint32_t)flags;
      w[1].flags = (uint32_t)flags;

      struct waker_arg wa = {.word = &words[0], .flags = flags};
      pthread_t t;
      if (pthread_create(&t, NULL, waker_main, &wa) != 0) {
        r.err = errno;
      } else {
        struct timespec ts = {.tv_sec = 2, .tv_nsec = 0};
        double t0 = now_ms();
        r.ret = syscall(SYS_FUTEX_WAITV, w, 2u, 0u, &ts, CLOCK_MONOTONIC);
        r.ms = now_ms() - t0;
        r.err = r.ret < 0 ? errno : 0;
        pthread_join(t, NULL);
      }
    }

    ssize_t ignored = write(fds[1], &r, sizeof(r));
    (void)ignored;
    _exit(0);
  }

  close(fds[1]);
  struct waitv_result r = {.ret = -1, .err = 0, .sig = 0, .ms = 0.0};
  ssize_t n = read(fds[0], &r, sizeof(r));
  close(fds[0]);

  int st = 0;
  waitpid(pid, &st, 0);
  if (WIFSIGNALED(st)) {
    r.sig = WTERMSIG(st);
    r.ret = -1;
    r.err = 0;
    r.ms = 0.0;
  } else if (n != (ssize_t)sizeof(r)) {
    r.ret = -1;
    r.err = EIO;
  }
  *out = r;
}

static void report_waitv(int flags) {
  struct waitv_result r;
  waitv_in_child(flags, &r);
  printf("{\"test\":\"futex_waitv\",\"waiter_flags\":%d,\"ret\":%ld,\"errno\":%d,"
         "\"signal\":%d,\"latency_ms\":%.3f}\n",
         flags, r.ret, r.err, r.sig, r.ms);
}

static void report_va(uint64_t addr) {
  void *p = mmap((void *)(uintptr_t)addr, 65536, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  int ok = p != MAP_FAILED;
  int err = ok ? 0 : errno;
  if (ok)
    munmap(p, 65536);
  printf("{\"test\":\"va_free\",\"addr\":\"0x%llx\",\"ok\":%s,\"errno\":%d}\n",
         (unsigned long long)addr, ok ? "true" : "false", err);
}

static void report_affinity(const char *label, int lo, int hi) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int i = lo; i <= hi; i++)
    CPU_SET(i, &set);

  int rc = sched_setaffinity(0, sizeof(set), &set);
  int err = rc == 0 ? 0 : errno;

  cpu_set_t got;
  CPU_ZERO(&got);
  int count = 0;
  if (sched_getaffinity(0, sizeof(got), &got) == 0)
    count = CPU_COUNT(&got);

  printf("{\"test\":\"affinity\",\"label\":\"%s\",\"ok\":%s,\"errno\":%d,"
         "\"cpus_in_mask\":%d}\n",
         label, rc == 0 ? "true" : "false", err, count);
}

static void report_memfd(void) {
  int fd = memfd_create("titan_m0", 0);
  if (fd < 0) {
    printf("{\"test\":\"memfd\",\"ok\":false,\"errno\":%d}\n", errno);
    return;
  }

  int ok = ftruncate(fd, 4096) == 0;
  void *p = MAP_FAILED;
  if (ok)
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ok = ok && p != MAP_FAILED;

  int visible = 0;
  if (ok) {
    *(volatile uint32_t *)p = 0x7ec7a11u;
    pid_t pid = fork();
    if (pid == 0)
      _exit(*(volatile uint32_t *)p == 0x7ec7a11u ? 0 : 1);
    if (pid > 0) {
      int st = 0;
      waitpid(pid, &st, 0);
      visible = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
  }

  printf("{\"test\":\"memfd\",\"ok\":%s,\"cross_process\":%s,\"errno\":%d}\n",
         ok ? "true" : "false", visible ? "true" : "false", ok ? 0 : errno);
  if (p != MAP_FAILED)
    munmap(p, 4096);
  close(fd);
}

static void report_proc_line(const char *path, const char *key) {
  FILE *f = fopen(path, "r");
  if (!f) {
    printf("{\"test\":\"proc\",\"key\":\"%s\",\"ok\":false}\n", key);
    return;
  }
  char line[512];
  int found = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, key, strlen(key)) == 0) {
      char *v = line + strlen(key);
      v[strcspn(v, "\r\n")] = '\0';
      printf("{\"test\":\"proc\",\"key\":\"%s\",\"value\":\"%s\"}\n", key, v);
      found = 1;
    }
  }
  fclose(f);
  if (!found)
    printf("{\"test\":\"proc\",\"key\":\"%s\",\"ok\":false}\n", key);
}

static void report_min_map(void) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f)
    return;
  char line[512];
  if (fgets(line, sizeof(line), f)) {
    unsigned long long lo = 0;
    if (sscanf(line, "%llx-", &lo) == 1)
      printf("{\"test\":\"min_map\",\"addr\":\"0x%llx\"}\n", lo);
  }
  fclose(f);
}

int main(void) {
  struct utsname u;
  memset(&u, 0, sizeof(u));
  uname(&u);

  printf("{\"test\":\"info\",\"pagesize\":%ld,\"ncpu_conf\":%ld,"
         "\"kernel\":\"%s\",\"machine\":\"%s\"}\n",
         sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_CONF), u.release,
         u.machine);

  report_proc_line("/proc/self/status", "Seccomp:");
  report_proc_line("/proc/self/status", "Seccomp_filters:");
  report_proc_line("/proc/self/attr/current", "");

  report_waitv(0);
  report_waitv(FUTEX_PRIVATE_FLAG);

  report_memfd();

  report_min_map();
  report_va(0x10000ull);
  report_va(0x100000ull);
  report_va(0x1000000ull);
  report_va(0x40000000ull);
  report_va(0x80000000ull);
  report_va(0x100000000ull);

  report_affinity("a55", 0, 3);
  report_affinity("a78", 4, 7);

  cpu_set_t all;
  CPU_ZERO(&all);
  for (int i = 0; i < CPU_SETSIZE; i++)
    CPU_SET(i, &all);
  sched_setaffinity(0, sizeof(all), &all);

  printf("{\"test\":\"done\"}\n");
  return 0;
}
