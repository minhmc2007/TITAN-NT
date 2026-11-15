#include <stdint.h>
#include <stddef.h>

#include "common/titan_log.h"
#include "common/titan_sys.h"
#include "common/titan_time.h"

#define EAGAIN_ 11
#define ETIMEDOUT_ 110

#define PING_ROUNDS 20000
#define MULTI_ITERS 200000

struct shared_ping {
  alignas(64) uint32_t req;
  alignas(64) uint32_t req_epoch;
  alignas(64) uint32_t ack;
  alignas(64) uint32_t ack_epoch;
  alignas(64) uint32_t stop;
  alignas(64) uint32_t child_seen;
  alignas(64) uint32_t child_waiting;
};

struct shared_multi {
  alignas(64) uint32_t count0;
  alignas(64) uint32_t count1;
  alignas(64) uint32_t count2;
  alignas(64) uint32_t epoch;
  alignas(64) uint32_t stop;
};

static long shared_page(void) {
  long fd = titan_memfd_create("titan-m1-bench", 0);
  if (fd < 0)
    return -1;
  if (titan_ftruncate((int)fd, 4096) != 0) {
    titan_close(fd);
    return -1;
  }
  long p = titan_mmap(0, 4096, TITAN_PROT_READ | TITAN_PROT_WRITE,
                      TITAN_MAP_SHARED, (int)fd, 0);
  titan_close(fd);
  return p;
}

static void pin_cpu(int cpu) {
  unsigned char mask[128];
  for (size_t i = 0; i < sizeof(mask); i++)
    mask[i] = 0;
  mask[cpu / 8] = (unsigned char)(1u << (cpu % 8));
  titan_sched_setaffinity(0, sizeof(mask), mask);
}

static int wait_eq(uint32_t *word, uint32_t expect, uint64_t timeout_ns) {
  struct titan_timespec ts;
  for (;;) {
    uint32_t seen = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if (seen != expect)
      return 0;
    titan_deadline_ns(&ts, timeout_ns);
    long r = titan_futex(word, TITAN_FUTEX_WAIT_BITSET, seen, &ts, 0,
                         TITAN_FUTEX_BITSET_MATCH_ANY);
    if (r == 0)
      continue;
    if (-r == EAGAIN_)
      continue;
    return (int)(-r);
  }
}

static int wait_change(uint32_t *word, uint32_t seen, uint64_t timeout_ns) {
  uint64_t end = titan_monotonic_ns() + timeout_ns;
  for (;;) {
    if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != seen)
      return 0;
    uint64_t now = titan_monotonic_ns();
    if (now >= end)
      return ETIMEDOUT_;
    int err = wait_eq(word, seen, end - now);
    if (err != 0 && err != ETIMEDOUT_)
      return err;
  }
}

static void wake_all(uint32_t *word) {
  titan_futex(word, TITAN_FUTEX_WAKE, 0x7fffffffu, 0, 0, 0);
}

static void child_futex(struct shared_ping *p) {
  uint32_t seen = 0;
  for (;;) {
    while (__atomic_load_n(&p->req_epoch, __ATOMIC_ACQUIRE) == seen) {
      if (__atomic_load_n(&p->stop, __ATOMIC_ACQUIRE))
        titan_exit_group(0);
      __atomic_store_n(&p->child_seen, seen, __ATOMIC_RELAXED);
      __atomic_store_n(&p->child_waiting, 1, __ATOMIC_RELAXED);
      wait_eq(&p->req_epoch, seen, 2000000000ull);
      __atomic_store_n(&p->child_waiting, 0, __ATOMIC_RELAXED);
    }
    if (__atomic_load_n(&p->stop, __ATOMIC_ACQUIRE))
      titan_exit_group(0);
    seen = __atomic_load_n(&p->req_epoch, __ATOMIC_ACQUIRE);
    __atomic_store_n(&p->ack, __atomic_load_n(&p->req, __ATOMIC_RELAXED),
                     __ATOMIC_RELEASE);
    __atomic_add_fetch(&p->ack_epoch, 1, __ATOMIC_RELEASE);
    wake_all(&p->ack_epoch);
  }
}

static uint64_t loadavg_x100(void) {
  long fd = titan_openat(TITAN_AT_FDCWD, "/proc/loadavg", TITAN_O_RDONLY);
  if (fd < 0)
    return 0;
  char buf[64];
  long n = titan_read(fd, buf, sizeof(buf) - 1);
  titan_close(fd);
  if (n <= 0)
    return 0;
  buf[n] = '\0';
  uint64_t whole = 0, frac = 0;
  const char *c = buf;
  while (*c >= '0' && *c <= '9')
    whole = whole * 10 + (uint64_t)(*c++ - '0');
  if (*c == '.') {
    c++;
    for (int d = 0; d < 2 && *c >= '0' && *c <= '9'; d++)
      frac = frac * 10 + (uint64_t)(*c++ - '0');
    if (frac < 10)
      frac *= 10;
  }
  return whole * 100 + frac;
}

static int bench_futex(struct shared_ping *p, uint64_t *elapsed_ns) {
  uint64_t failures = 0, stalls = 0, max_stall_ns = 0;
  uint64_t stall_entry = 0, stall_wait = 0;
  uint32_t stall_req_epoch = 0, stall_ack_epoch = 0, stall_ack = 0;
  uint32_t stall_seen = 0, stall_waiting = 0;
  uint64_t t0 = titan_monotonic_ns();
  for (uint32_t i = 0; i < PING_ROUNDS; i++) {
    uint64_t round0 = titan_monotonic_ns();
    __atomic_store_n(&p->req, i, __ATOMIC_RELEASE);
    __atomic_add_fetch(&p->req_epoch, 1, __ATOMIC_RELEASE);
    wake_all(&p->req_epoch);
    uint64_t limit = round0 + 10000000000ull;
    while (__atomic_load_n(&p->ack_epoch, __ATOMIC_ACQUIRE) < i + 1) {
      uint32_t seen = __atomic_load_n(&p->ack_epoch, __ATOMIC_ACQUIRE);
      uint64_t now = titan_monotonic_ns();
      if (now >= limit || wait_change(&p->ack_epoch, seen, limit - now) != 0) {
        stalls++;
        if (now >= limit)
          stall_entry++;
        else
          stall_wait++;
        stall_req_epoch = __atomic_load_n(&p->req_epoch, __ATOMIC_RELAXED);
        stall_ack_epoch = __atomic_load_n(&p->ack_epoch, __ATOMIC_RELAXED);
        stall_ack = __atomic_load_n(&p->ack, __ATOMIC_RELAXED);
        stall_seen = __atomic_load_n(&p->child_seen, __ATOMIC_RELAXED);
        stall_waiting = __atomic_load_n(&p->child_waiting, __ATOMIC_RELAXED);
        uint64_t took = titan_monotonic_ns() - round0;
        if (took > max_stall_ns)
          max_stall_ns = took;
        break;
      }
    }
    if (__atomic_load_n(&p->ack, __ATOMIC_ACQUIRE) != i)
      failures++;
  }
  uint64_t elapsed = titan_monotonic_ns() - t0;
  __atomic_store_n(&p->stop, 1, __ATOMIC_RELEASE);
  wake_all(&p->req_epoch);

  struct titan_buf b;
  char storage[320];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"ipc_bench\",\"variant\":\"futex_bitset\""
                     ",\"roundtrips\":");
  titan_buf_u64(&b, PING_ROUNDS);
  titan_buf_str(&b, ",\"total_us\":");
  titan_buf_u64(&b, elapsed / 1000);
  titan_buf_str(&b, ",\"ns_per_roundtrip\":");
  titan_buf_u64(&b, elapsed / PING_ROUNDS);
  titan_buf_str(&b, ",\"failures\":");
  titan_buf_u64(&b, failures);
  titan_buf_str(&b, ",\"stalls\":");
  titan_buf_u64(&b, stalls);
  titan_buf_str(&b, ",\"max_stall_us\":");
  titan_buf_u64(&b, max_stall_ns / 1000);
  titan_buf_str(&b, ",\"loadavg_x100\":");
  titan_buf_u64(&b, loadavg_x100());
  titan_buf_str(&b, ",\"stall_entry\":");
  titan_buf_u64(&b, stall_entry);
  titan_buf_str(&b, ",\"stall_wait\":");
  titan_buf_u64(&b, stall_wait);
  titan_buf_str(&b, ",\"stall_req_epoch\":");
  titan_buf_u64(&b, stall_req_epoch);
  titan_buf_str(&b, ",\"stall_ack_epoch\":");
  titan_buf_u64(&b, stall_ack_epoch);
  titan_buf_str(&b, ",\"stall_ack\":");
  titan_buf_u64(&b, stall_ack);
  titan_buf_str(&b, ",\"stall_seen\":");
  titan_buf_u64(&b, stall_seen);
  titan_buf_str(&b, ",\"stall_waiting\":");
  titan_buf_u64(&b, stall_waiting);
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
  *elapsed_ns = elapsed;
  return failures == 0 ? 0 : -1;
}

static void child_socket(int fd) {
  unsigned char byte;
  for (;;) {
    long n = titan_read(fd, &byte, 1);
    if (n != 1)
      titan_exit_group(0);
    if (titan_write(fd, &byte, 1) != 1)
      titan_exit_group(0);
  }
}

static int bench_socket(int fd, uint64_t *elapsed_ns) {
  unsigned char byte = 1;
  uint64_t failures = 0;
  uint64_t t0 = titan_monotonic_ns();
  for (int i = 0; i < PING_ROUNDS; i++) {
    if (titan_write(fd, &byte, 1) != 1)
      failures++;
    if (titan_read(fd, &byte, 1) != 1)
      failures++;
  }
  uint64_t elapsed = titan_monotonic_ns() - t0;

  struct titan_buf b;
  char storage[320];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"ipc_bench\",\"variant\":\"unix_socket\""
                     ",\"roundtrips\":");
  titan_buf_u64(&b, PING_ROUNDS);
  titan_buf_str(&b, ",\"total_us\":");
  titan_buf_u64(&b, elapsed / 1000);
  titan_buf_str(&b, ",\"ns_per_roundtrip\":");
  titan_buf_u64(&b, elapsed / PING_ROUNDS);
  titan_buf_str(&b, ",\"failures\":");
  titan_buf_u64(&b, failures);
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
  *elapsed_ns = elapsed;
  return failures == 0 ? 0 : -1;
}

static void child_multi(struct shared_multi *m) {
  for (uint32_t i = 0; i < MULTI_ITERS; i++) {
    uint32_t j = i % 3;
    if (j == 0)
      __atomic_add_fetch(&m->count0, 1, __ATOMIC_RELEASE);
    else if (j == 1)
      __atomic_add_fetch(&m->count1, 1, __ATOMIC_RELEASE);
    else
      __atomic_add_fetch(&m->count2, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&m->epoch, 1, __ATOMIC_RELEASE);
    wake_all(&m->epoch);
  }
  __atomic_store_n(&m->stop, 1, __ATOMIC_RELEASE);
  wake_all(&m->epoch);
  titan_exit_group(0);
}

static int bench_multi(struct shared_multi *m) {
  uint64_t failures = 0;
  for (uint32_t i = 0; i < MULTI_ITERS; i++) {
    uint32_t target = i + 1;
    for (;;) {
      uint32_t sum = __atomic_load_n(&m->count0, __ATOMIC_ACQUIRE) +
                     __atomic_load_n(&m->count1, __ATOMIC_ACQUIRE) +
                     __atomic_load_n(&m->count2, __ATOMIC_ACQUIRE);
      if (sum >= target)
        break;
      uint32_t seen = __atomic_load_n(&m->epoch, __ATOMIC_ACQUIRE);
      if (wait_change(&m->epoch, seen, 10000000000ull) != 0) {
        failures++;
        break;
      }
    }
  }
  uint32_t sum = __atomic_load_n(&m->count0, __ATOMIC_ACQUIRE) +
                 __atomic_load_n(&m->count1, __ATOMIC_ACQUIRE) +
                 __atomic_load_n(&m->count2, __ATOMIC_ACQUIRE);
  if (sum != MULTI_ITERS)
    failures++;

  struct titan_buf b;
  char storage[320];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"multi_wait\",\"iterations\":");
  titan_buf_u64(&b, MULTI_ITERS);
  titan_buf_str(&b, ",\"signals_seen\":");
  titan_buf_u64(&b, sum);
  titan_buf_str(&b, ",\"failures\":");
  titan_buf_u64(&b, failures);
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
  return failures == 0 ? 0 : -1;
}

/* 449 is SIGSYS under untrusted_app, so it must die in a child. */
static void probe_waitv(void) {
  long page = titan_mmap(0, 4096, TITAN_PROT_READ | TITAN_PROT_WRITE,
                         TITAN_MAP_PRIVATE | TITAN_MAP_ANONYMOUS, -1, 0);
  if (page < 0)
    return;
  struct shared_ping *p = (struct shared_ping *)(uintptr_t)page;
  struct titan_futex_waitv w[2];
  for (int i = 0; i < 2; i++) {
    w[i].val = 0;
    w[i].uaddr = (uint64_t)(uintptr_t)&p->req_epoch + (uint64_t)i * 64u;
    w[i].flags = TITAN_FUTEX_32;
    w[i].reserved = 0;
  }

  long pid = titan_clone_fork();
  if (pid == 0) {
    struct titan_timespec ts;
    titan_deadline_ns(&ts, 1000000ull);
    long r = titan_syscall6(TITAN_SYS_futex_waitv, (long)(uintptr_t)w, 2, 0,
                            (long)(uintptr_t)&ts, 0, 0);
    titan_exit_group(r < 0 ? -r : 0);
  }
  if (pid < 0) {
    titan_munmap((void *)(uintptr_t)page, 4096);
    return;
  }

  int status = 0;
  titan_wait4(pid, &status, 0);
  titan_munmap((void *)(uintptr_t)page, 4096);

  struct titan_buf b;
  char storage[192];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"futex_waitv\",\"how\":");
  if (TITAN_WIFSIGNALED(status)) {
    titan_buf_str(&b, "\"signal\",\"value\":");
    titan_buf_i64(&b, TITAN_WTERMSIG(status));
  } else {
    titan_buf_str(&b, "\"exit\",\"value\":");
    titan_buf_i64(&b, TITAN_WEXITSTATUS(status));
  }
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
}

static int probe_timeout(void) {
  long page = titan_mmap(0, 4096, TITAN_PROT_READ | TITAN_PROT_WRITE,
                         TITAN_MAP_PRIVATE | TITAN_MAP_ANONYMOUS, -1, 0);
  if (page < 0)
    return -1;
  uint32_t *word = (uint32_t *)(uintptr_t)page;
  int err = wait_eq(word, 0, 1000000ull);
  titan_munmap((void *)(uintptr_t)page, 4096);

  struct titan_buf b;
  char storage[192];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"wait_timeout\",\"expected\":");
  titan_buf_u64(&b, ETIMEDOUT_);
  titan_buf_str(&b, ",\"errno\":");
  titan_buf_u64(&b, err > 0 ? (uint64_t)err : 0);
  titan_buf_str(&b, ",\"ok\":");
  titan_buf_str(&b, err == ETIMEDOUT_ ? "true" : "false");
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
  return err == ETIMEDOUT_ ? 0 : -1;
}

int main(void) {
  long ping_page = shared_page();
  long multi_page = shared_page();
  if (ping_page < 0 || multi_page < 0) {
    titan_write_str(2, "mmap failed\n");
    return 1;
  }
  struct shared_ping *p = (struct shared_ping *)(uintptr_t)ping_page;
  struct shared_multi *m = (struct shared_multi *)(uintptr_t)multi_page;

  pin_cpu(0);

  int rc = 0;
  long pid = titan_clone_fork();
  if (pid == 0) {
    pin_cpu(1);
    child_futex(p);
    titan_exit_group(0);
  }
  if (pid < 0) {
    titan_write_str(2, "fork failed\n");
    return 1;
  }
  uint64_t futex_ns = 0;
  if (bench_futex(p, &futex_ns) != 0)
    rc = 1;
  int status = 0;
  titan_wait4(pid, &status, 0);

  int sv[2];
  if (titan_socketpair(1, 1, 0, sv) != 0) {
    titan_write_str(2, "socketpair failed\n");
    return 1;
  }
  pid = titan_clone_fork();
  if (pid == 0) {
    pin_cpu(1);
    titan_close(sv[0]);
    child_socket(sv[1]);
    titan_exit_group(0);
  }
  if (pid < 0) {
    titan_write_str(2, "fork failed\n");
    return 1;
  }
  uint64_t socket_ns = 0;
  if (bench_socket(sv[0], &socket_ns) != 0)
    rc = 1;
  titan_close(sv[0]);
  titan_close(sv[1]);
  titan_wait4(pid, &status, 0);

  struct titan_buf ratio;
  char ratio_storage[256];
  titan_buf_init(&ratio, ratio_storage, sizeof(ratio_storage));
  titan_buf_str(&ratio, "{\"test\":\"ipc_compare\",\"futex_ns\":");
  titan_buf_u64(&ratio, futex_ns / PING_ROUNDS);
  titan_buf_str(&ratio, ",\"socket_ns\":");
  titan_buf_u64(&ratio, socket_ns / PING_ROUNDS);
  titan_buf_str(&ratio, ",\"speedup_x100\":");
  titan_buf_u64(&ratio, futex_ns ? socket_ns * 100 / futex_ns : 0);
  titan_buf_str(&ratio, "}");
  titan_buf_emit(&ratio);

  pid = titan_clone_fork();
  if (pid == 0) {
    pin_cpu(1);
    child_multi(m);
    titan_exit_group(0);
  }
  if (pid < 0) {
    titan_write_str(2, "fork failed\n");
    return 1;
  }
  if (bench_multi(m) != 0)
    rc = 1;
  titan_wait4(pid, &status, 0);

  probe_waitv();
  if (probe_timeout() != 0)
    rc = 1;

  return rc;
}
