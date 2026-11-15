#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "common/titan_log.h"
#include "common/titan_sys.h"
#include "common/titan_time.h"
#include "hal_shim/x18_guard.h"
#include "hal_shim/titan_signal.h"

#define MAX_THREADS 8

struct titan_teb g_tebs[MAX_THREADS];
static volatile int g_stop;
static uint64_t g_bad;
static int g_threads = 3;
static uint64_t g_seconds = 600;

struct worker_arg {
  struct titan_teb *teb;
  volatile int *page;
  int ready;
};

static void mark_bad(const char *why) {
  struct titan_buf b;
  char storage[320];
  __atomic_add_fetch(&g_bad, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"x18_stress\",\"error\":\"");
  titan_buf_str(&b, why);
  titan_buf_str(&b, "\",\"tid\":");
  titan_buf_u64(&b, (uint64_t)titan_gettid());
  titan_buf_str(&b, ",\"x18\":");
  titan_buf_hex(&b, (uint64_t)(uintptr_t)titan_x18_get());
  titan_buf_str(&b, ",\"scs\":");
  titan_buf_hex(&b, (uint64_t)(uintptr_t)titan_scs_current());
  struct titan_teb *t = titan_teb_current();
  titan_buf_str(&b, ",\"teb_magic\":");
  titan_buf_hex(&b, t ? t->magic : 0);
  titan_buf_str(&b, ",\"teb_self\":");
  titan_buf_hex(&b, t ? t->self : 0);
  titan_buf_str(&b, ",\"teb_scs\":");
  titan_buf_hex(&b, t ? t->scs_ptr : 0);
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
}

static int jmp_roundtrip(void) {
  void *before = titan_x18_get();
  jmp_buf jb;
  if (setjmp(jb) == 0)
    longjmp(jb, 1);
  return titan_x18_get() == before ? 0 : 1;
}

static void arm_fault(struct titan_teb *teb, volatile int *page) {
  teb->fault_lo = (uint64_t)(uintptr_t)page;
  teb->fault_hi = teb->fault_lo + 4096;
  teb->ill_lo = (uint64_t)(uintptr_t)titan_udf_trap;
  teb->ill_hi = teb->ill_lo + 4;
}

static void fault_once(volatile int *page) {
  if (titan_mprotect((void *)page, 4096, TITAN_PROT_NONE) != 0) {
    mark_bad("mprotect-fault-page");
    return;
  }
  if (*page != 0x5a5a)
    mark_bad("fault-page-value");
}

static void ill_once(void) { titan_udf_trap(); }

static void stress_iteration(struct titan_teb *teb, volatile int *page,
                             uint64_t i) {
  if ((i & 63) == 0)
    fault_once(page);
  if ((i & 127) == 3)
    ill_once();
  if ((i & 255) == 5 && jmp_roundtrip() != 0)
    mark_bad("setjmp-longjmp-x18");

  long pid = titan_guard_call((void *)getpid, 0, 0, 0, 0, 0, 0);
  if (pid <= 0)
    mark_bad("guarded-getpid");

  struct titan_timespec ts;
  if (titan_guard_call((void *)clock_gettime, TITAN_CLOCK_MONOTONIC,
                       (long)(uintptr_t)&ts, 0, 0, 0, 0) != 0)
    mark_bad("guarded-clock_gettime");

  void *mem = (void *)(uintptr_t)titan_guard_call((void *)malloc, 64, 0, 0, 0,
                                                  0, 0);
  if (!mem) {
    mark_bad("guarded-malloc");
  } else {
    *(volatile int *)mem = (int)i;
    titan_guard_call((void *)free, (long)(uintptr_t)mem, 0, 0, 0, 0, 0);
  }

  char buf[32];
  long n = titan_guard_call((void *)snprintf, (long)(uintptr_t)buf,
                            (long)sizeof(buf), (long)(uintptr_t)"%d",
                            (long)(int)(i & 0xffff), 0, 0);
  if (n <= 0 || n >= (long)sizeof(buf))
    mark_bad("guarded-snprintf");

  if (!titan_thread_ok(teb))
    mark_bad("x18-corrupted");
}

static void *worker_main(void *argp) {
  struct worker_arg *arg = (struct worker_arg *)argp;
  if (titan_thread_init(arg->teb) != 0) {
    mark_bad("worker-thread-init");
    return 0;
  }
  arm_fault(arg->teb, arg->page);
  *arg->page = 0x5a5a;
  __atomic_store_n(&arg->ready, 1, __ATOMIC_RELEASE);

  uint64_t i = 0;
  while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED))
    stress_iteration(arg->teb, arg->page, ++i);
  return 0;
}

static int arg_is(const char *arg, const char *name) {
  while (*arg && *name) {
    if (*arg++ != *name++)
      return 0;
  }
  return *arg == *name;
}

static uint64_t parse_u64(const char *s) {
  uint64_t v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (uint64_t)(*s++ - '0');
  return v;
}

static int spawn_worker(struct worker_arg *arg, pthread_t *th) {
  return (int)titan_guard_call((void *)pthread_create, (long)(uintptr_t)th, 0,
                               (long)(uintptr_t)worker_main,
                               (long)(uintptr_t)arg, 0, 0);
}

static int join_worker(pthread_t th) {
  return (int)titan_guard_call((void *)pthread_join, (long)th, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
  struct worker_arg args[MAX_THREADS];
  pthread_t threads[MAX_THREADS];

  for (int i = 1; i + 1 < argc; i += 2) {
    if (arg_is(argv[i], "--seconds"))
      g_seconds = parse_u64(argv[i + 1]);
    else if (arg_is(argv[i], "--threads"))
      g_threads = (int)parse_u64(argv[i + 1]);
  }
  if (g_threads < 1)
    g_threads = 1;
  if (g_threads > MAX_THREADS - 1)
    g_threads = MAX_THREADS - 1;

  if (titan_thread_init(&g_tebs[0]) != 0) {
    titan_write_str(2, "main thread init failed\n");
    return 1;
  }
  if (titan_signal_install() != 0) {
    titan_write_str(2, "signal install failed\n");
    return 1;
  }

  for (int i = 0; i < g_threads; i++) {
    long p = titan_mmap(0, 4096, TITAN_PROT_READ | TITAN_PROT_WRITE,
                        TITAN_MAP_PRIVATE | TITAN_MAP_ANONYMOUS, -1, 0);
    if (p < 0) {
      titan_write_str(2, "mmap fault page failed\n");
      return 1;
    }
    args[i].teb = &g_tebs[i + 1];
    args[i].page = (volatile int *)(uintptr_t)p;
    args[i].ready = 0;
    if (spawn_worker(&args[i], &threads[i]) != 0) {
      titan_write_str(2, "pthread_create failed\n");
      return 1;
    }
  }

  struct titan_timespec nap = {0, 1000000};
  for (int i = 0; i < g_threads; i++)
    while (!__atomic_load_n(&args[i].ready, __ATOMIC_ACQUIRE))
      titan_nanosleep(&nap);

  long main_page = titan_mmap(0, 4096, TITAN_PROT_READ | TITAN_PROT_WRITE,
                              TITAN_MAP_PRIVATE | TITAN_MAP_ANONYMOUS, -1, 0);
  if (main_page < 0) {
    titan_write_str(2, "mmap fault page failed\n");
    return 1;
  }
  volatile int *main_fault = (volatile int *)(uintptr_t)main_page;
  *main_fault = 0x5a5a;
  arm_fault(&g_tebs[0], main_fault);
  struct titan_itimerval itv = {{0, 1000}, {0, 1000}};
  long timer = titan_setitimer(2, &itv, 0);
  if (timer != 0) {
    struct titan_buf tb;
    char tstorage[160];
    titan_buf_init(&tb, tstorage, sizeof(tstorage));
    titan_buf_str(&tb, "{\"test\":\"x18_stress\",\"error\":\"setitimer\""
                       ",\"ret\":");
    titan_buf_i64(&tb, timer);
    titan_buf_str(&tb, "}");
    titan_buf_emit(&tb);
    return 1;
  }

  uint64_t deadline = titan_monotonic_ns() + g_seconds * 1000000000ull;
  uint64_t i = 0;
  while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED) &&
         titan_monotonic_ns() < deadline) {
    stress_iteration(&g_tebs[0], main_fault, ++i);
    if ((i & 0xffff) == 0)
      titan_thread_report(&g_tebs[0], "main");
  }

  struct titan_itimerval off = {{0, 0}, {0, 0}};
  titan_setitimer(2, &off, 0);
  __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);

  for (int t = 0; t < g_threads; t++)
    join_worker(threads[t]);

  titan_thread_report(&g_tebs[0], "main");
  for (int t = 0; t < g_threads; t++) {
    char label[16];
    label[0] = 'w';
    label[1] = (char)('0' + t);
    label[2] = '\0';
    titan_thread_report(&g_tebs[t + 1], label);
  }

  uint64_t total_calls = 0, total_guard_bad = 0, total_sig = 0;
  uint64_t total_sig_bad = 0, total_faults = 0, total_ill = 0;
  for (int t = 0; t <= g_threads; t++) {
    total_calls += __atomic_load_n(&g_tebs[t].guard_calls, __ATOMIC_RELAXED);
    total_guard_bad +=
        __atomic_load_n(&g_tebs[t].guard_bad, __ATOMIC_RELAXED);
    total_sig += __atomic_load_n(&g_tebs[t].sig_teb, __ATOMIC_RELAXED) +
                 __atomic_load_n(&g_tebs[t].sig_scs, __ATOMIC_RELAXED);
    total_sig_bad += __atomic_load_n(&g_tebs[t].sig_bad, __ATOMIC_RELAXED);
    total_faults += __atomic_load_n(&g_tebs[t].faults, __ATOMIC_RELAXED);
    total_ill += __atomic_load_n(&g_tebs[t].ill, __ATOMIC_RELAXED);
  }

  int ok = __atomic_load_n(&g_bad, __ATOMIC_RELAXED) == 0 &&
           total_guard_bad == 0 && total_sig_bad == 0 && total_faults > 0 &&
           total_ill > 0 && total_sig > 0;

  struct titan_buf b;
  char storage[512];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"x18_stress\",\"seconds\":");
  titan_buf_u64(&b, g_seconds);
  titan_buf_str(&b, ",\"threads\":");
  titan_buf_u64(&b, (uint64_t)(g_threads + 1));
  titan_buf_str(&b, ",\"main_iterations\":");
  titan_buf_u64(&b, i);
  titan_buf_str(&b, ",\"guard_calls\":");
  titan_buf_u64(&b, total_calls);
  titan_buf_str(&b, ",\"signals\":");
  titan_buf_u64(&b, total_sig);
  titan_buf_str(&b, ",\"sig_bad\":");
  titan_buf_u64(&b, total_sig_bad);
  titan_buf_str(&b, ",\"guard_bad\":");
  titan_buf_u64(&b, total_guard_bad);
  titan_buf_str(&b, ",\"faults\":");
  titan_buf_u64(&b, total_faults);
  titan_buf_str(&b, ",\"illegal\":");
  titan_buf_u64(&b, total_ill);
  titan_buf_str(&b, ",\"bad\":");
  titan_buf_u64(&b, __atomic_load_n(&g_bad, __ATOMIC_RELAXED));
  titan_buf_str(&b, ",\"status\":\"");
  titan_buf_str(&b, ok ? "ok" : "fail");
  titan_buf_str(&b, "\"}");
  titan_buf_emit(&b);

  return ok ? 0 : 1;
}
