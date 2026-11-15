#include <sys/auxv.h>
#include <time.h>
#include <unistd.h>

#include "../common/titan_log.h"
#include "../common/titan_sys.h"
#include "../common/titan_time.h"
#include "../hal_shim/titan_signal.h"
#include "../hal_shim/x18_guard.h"

#define FORKD_MAGIC 0x544954414e464430ull
#define FORKD_ACK 0x4644524bull

struct forkd_state {
  struct titan_teb teb;
};

static struct forkd_state g_state;

static size_t s_len(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

static int s_eq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a++ != *b++)
      return 0;
  }
  return *a == *b;
}

static int s_prefix(const char *s, const char *prefix) {
  while (*prefix) {
    if (*s++ != *prefix++)
      return 0;
  }
  return 1;
}

static void s_cat(char *dst, size_t cap, const char *src) {
  size_t n = s_len(dst);
  while (*src && n + 1 < cap)
    dst[n++] = *src++;
  dst[n] = '\0';
}

static void u64_dec(uint64_t v, char *out) {
  char tmp[24];
  size_t n = 0;
  if (!v)
    tmp[n++] = '0';
  while (v) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  size_t i = 0;
  while (n)
    out[i++] = tmp[--n];
  out[i] = '\0';
}

static uint64_t dec_u64(const char *s) {
  uint64_t v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (uint64_t)(*s++ - '0');
  return v;
}

static int read_comm(const char *tid, char *out, size_t cap) {
  char path[96] = "/proc/self/task/";
  s_cat(path, sizeof(path), tid);
  s_cat(path, sizeof(path), "/comm");
  long fd = titan_openat(TITAN_AT_FDCWD, path,
                         TITAN_O_RDONLY | TITAN_O_CLOEXEC);
  if (fd < 0)
    return -1;
  long n = titan_read(fd, out, cap - 1);
  titan_close(fd);
  if (n <= 0)
    return -1;
  out[n] = '\0';
  for (long i = 0; i < n; i++) {
    if (out[i] == '\n') {
      out[i] = '\0';
      break;
    }
  }
  return 0;
}

static int is_art_thread(const char *comm) {
  static const char *names[] = {
      "Signal Catcher",  "perfetto_hprof_", "ADB-JDWP Connec",
      "Jit thread pool", "HeapTaskDaemon",   "ReferenceQueueD",
      "FinalizerDaemon", "FinalizerWatchd",  "JDWP Transport Li",
      "JDWP Event Help", "Profile Saver",    "RenderThread",
      "hwuiTask",        "MetricsWorker",
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (s_prefix(comm, names[i]))
      return 1;
  return 0;
}

static void report_threads(void) {
  long fd = titan_openat(TITAN_AT_FDCWD, "/proc/self/task",
                         TITAN_O_RDONLY | TITAN_O_DIRECTORY | TITAN_O_CLOEXEC);
  if (fd < 0) {
    titan_write_str(2, "{\"test\":\"forkd_threads\",\"error\":\"open\"}\n");
    return;
  }

  struct titan_buf b;
  char storage[768];
  uint64_t count = 0, art = 0;
  int first = 1;
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"forkd_threads\",\"names\":[");

  char buf[2048];
  for (;;) {
    long n = titan_getdents64(fd, buf, sizeof(buf));
    if (n <= 0)
      break;
    long off = 0;
    while (off < n) {
      char *ent = buf + off;
      unsigned short reclen = *(unsigned short *)(ent + 16);
      const char *name = ent + 19;
      off += reclen;
      if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        continue;
      char comm[64];
      if (read_comm(name, comm, sizeof(comm)) != 0)
        continue;
      count++;
      if (is_art_thread(comm))
        art++;
      if (!first)
        titan_buf_str(&b, ",");
      titan_buf_str(&b, "\"");
      titan_buf_str(&b, comm);
      titan_buf_str(&b, "\"");
      first = 0;
    }
  }
  titan_close(fd);

  titan_buf_str(&b, "],\"count\":");
  titan_buf_u64(&b, count);
  titan_buf_str(&b, ",\"art\":");
  titan_buf_u64(&b, art);
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
}

static int probe_lowva(uint64_t addr) {
  long p = titan_mmap((void *)(uintptr_t)addr, 4096, TITAN_PROT_NONE,
                      TITAN_MAP_PRIVATE | TITAN_MAP_ANONYMOUS |
                          TITAN_MAP_FIXED_NOREPLACE,
                      -1, 0);
  int ok = p == (long)addr;
  if (ok)
    titan_munmap((void *)(uintptr_t)p, 4096);

  struct titan_buf b;
  char storage[160];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"forkd_lowva\",\"addr\":");
  titan_buf_hex(&b, addr);
  titan_buf_str(&b, ",\"ok\":");
  titan_buf_str(&b, ok ? "true" : "false");
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
  return ok;
}

static void hold_seconds(uint64_t seconds) {
  struct titan_timespec nap = {0, 100000000};
  uint64_t until = titan_monotonic_ns() + seconds * 1000000000ull;
  while (titan_monotonic_ns() < until)
    titan_nanosleep(&nap);
}

static int forkd_child(int fd) {
  long peer = titan_mmap(0, 4096, TITAN_PROT_READ | TITAN_PROT_WRITE,
                         TITAN_MAP_SHARED, fd, 0);
  if (peer < 0)
    return 2;

  volatile uint64_t *words = (volatile uint64_t *)(uintptr_t)peer;
  int ok = words[0] == FORKD_MAGIC;
  words[1] = FORKD_ACK;

  if (titan_thread_init(&g_state.teb) != 0) {
    titan_write_str(2, "child x18 init failed\n");
    return 3;
  }
  long pid = titan_guard_call((void *)getpid, 0, 0, 0, 0, 0, 0);
  ok = ok && pid > 0 && titan_thread_ok(&g_state.teb);

  struct titan_buf b;
  char storage[192];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"forkd_child\",\"pid\":");
  titan_buf_u64(&b, (uint64_t)pid);
  titan_buf_str(&b, ",\"magic\":");
  titan_buf_str(&b, words[0] == FORKD_MAGIC ? "true" : "false");
  titan_buf_str(&b, ",\"x18\":");
  titan_buf_str(&b, titan_thread_ok(&g_state.teb) ? "ok" : "bad");
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
  return ok ? 0 : 4;
}

static int spawn_child(int fd) {
  char path[256];
  long n = titan_readlinkat(TITAN_AT_FDCWD, "/proc/self/exe", path,
                            sizeof(path) - 1);
  if (n <= 0)
    return -1;
  path[n] = '\0';

  char fdstr[24];
  u64_dec((uint64_t)fd, fdstr);

  char *argv[4];
  argv[0] = path;
  argv[1] = (char *)"--child";
  argv[2] = fdstr;
  argv[3] = 0;
  extern char **environ;

  long pid = titan_clone_fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    titan_execve(path, argv, environ);
    titan_exit_group(127);
  }

  int status = 0;
  if (titan_wait4(pid, &status, 0) < 0)
    return -1;

  struct titan_buf b;
  char storage[224];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"forkd_spawn\",\"pid\":");
  titan_buf_u64(&b, (uint64_t)pid);
  titan_buf_str(&b, ",\"exited\":");
  titan_buf_str(&b, TITAN_WIFEXITED(status) ? "true" : "false");
  titan_buf_str(&b, ",\"code\":");
  titan_buf_u64(&b, (uint64_t)TITAN_WEXITSTATUS(status));
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);

  return TITAN_WIFEXITED(status) && TITAN_WEXITSTATUS(status) == 0 ? 0 : -1;
}

int main(int argc, char **argv) {
  int child_fd = -1;
  uint64_t hold = 5;

  for (int i = 1; i < argc; i++) {
    if (s_eq(argv[i], "--child") && i + 1 < argc)
      child_fd = (int)dec_u64(argv[++i]);
    else if (s_eq(argv[i], "--hold") && i + 1 < argc)
      hold = dec_u64(argv[++i]);
  }

  if (child_fd >= 0)
    return forkd_child(child_fd);

  if (titan_thread_init(&g_state.teb) != 0) {
    titan_write_str(2, "thread init failed\n");
    return 1;
  }
  if (titan_signal_install() != 0) {
    titan_write_str(2, "signal install failed\n");
    return 1;
  }

  long page = titan_guard_call((void *)getauxval, AT_PAGESZ, 0, 0, 0, 0, 0);
  struct titan_buf b;
  char storage[256];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"forkd\",\"pid\":");
  titan_buf_u64(&b, (uint64_t)titan_getpid());
  titan_buf_str(&b, ",\"pagesize\":");
  titan_buf_u64(&b, (uint64_t)page);
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);

  int ok = 1;
  ok &= probe_lowva(0x10000);
  ok &= probe_lowva(0x10000000);
  ok &= probe_lowva(0x100000000ull);

  long fd = titan_memfd_create("titan-forkd", 0);
  if (fd < 0 || titan_ftruncate((int)fd, 4096) != 0) {
    titan_write_str(2, "memfd_create failed\n");
    return 1;
  }
  long arena = titan_mmap(0, 4096, TITAN_PROT_READ | TITAN_PROT_WRITE,
                          TITAN_MAP_SHARED, (int)fd, 0);
  if (arena < 0) {
    titan_write_str(2, "arena mmap failed\n");
    return 1;
  }
  volatile uint64_t *words = (volatile uint64_t *)(uintptr_t)arena;
  words[0] = FORKD_MAGIC;
  words[1] = 0;

  if (spawn_child((int)fd) != 0)
    ok = 0;
  int ack_ok = words[0] == FORKD_MAGIC && words[1] == FORKD_ACK;
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"forkd_arena\",\"shared_after_exec\":");
  titan_buf_str(&b, ack_ok ? "true" : "false");
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);

  uint64_t sig_before = __atomic_load_n(&g_state.teb.sig_teb, __ATOMIC_RELAXED) +
                        __atomic_load_n(&g_state.teb.sig_scs, __ATOMIC_RELAXED);
  titan_guard_call((void *)getpid, 0, 0, 0, 0, 0, 0);
  struct titan_timespec ts;
  titan_guard_call((void *)clock_gettime, TITAN_CLOCK_MONOTONIC,
                   (long)(uintptr_t)&ts, 0, 0, 0, 0);
  titan_tgkill(titan_getpid(), titan_gettid(), 10);
  uint64_t sig_after = __atomic_load_n(&g_state.teb.sig_teb, __ATOMIC_RELAXED) +
                       __atomic_load_n(&g_state.teb.sig_scs, __ATOMIC_RELAXED);
  int x18_ok = titan_thread_ok(&g_state.teb) && sig_after > sig_before;

  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"forkd_x18\",\"signal_seen\":");
  titan_buf_str(&b, sig_after > sig_before ? "true" : "false");
  titan_buf_str(&b, ",\"ok\":");
  titan_buf_str(&b, x18_ok ? "true" : "false");
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);

  report_threads();
  hold_seconds(hold);

  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"forkd\",\"status\":\"");
  titan_buf_str(&b, ok && ack_ok && x18_ok ? "ok" : "fail");
  titan_buf_str(&b, "\"}");
  titan_buf_emit(&b);
  return ok && ack_ok && x18_ok ? 0 : 1;
}
