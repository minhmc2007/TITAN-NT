#include <stdint.h>

#include "common/titan_affinity.h"
#include "common/titan_log.h"
#include "common/titan_sys.h"
#include "common/titan_time.h"
#include "ntsync/titan_ntsync.h"

#define T_SHORT_NS 3000000ull
#define T_LONG_NS 10000000000ull

struct ipc {
  volatile uint32_t ready;
  volatile uint32_t flag;
  volatile uint32_t count;
  volatile uint32_t index;
  volatile uint32_t fail;
};

static struct nt_arena *g_arena;
static volatile struct ipc *g_ipc;
static uint64_t g_checks;
static uint64_t g_failures;

static void check(const char *name, int ok) {
  g_checks++;
  if (!ok)
    g_failures++;
  struct titan_buf b;
  char storage[160];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"");
  titan_buf_str(&b, name);
  titan_buf_str(&b, "\",\"ok\":");
  titan_buf_str(&b, ok ? "true" : "false");
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
}

static void check_u32(const char *name, uint32_t got, uint32_t want) {
  int ok = got == want;
  g_checks++;
  if (!ok)
    g_failures++;
  struct titan_buf b;
  char storage[192];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"");
  titan_buf_str(&b, name);
  titan_buf_str(&b, "\",\"got\":");
  titan_buf_u64(&b, got);
  titan_buf_str(&b, ",\"want\":");
  titan_buf_u64(&b, want);
  titan_buf_str(&b, ",\"ok\":");
  titan_buf_str(&b, ok ? "true" : "false");
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
}

static void nap_ns(uint64_t ns) {
  struct titan_timespec ts = {(int64_t)(ns / 1000000000ull),
                              (int64_t)(ns % 1000000000ull)};
  titan_nanosleep(&ts);
}

static int wait_ready(uint64_t timeout_ns) {
  uint64_t end = titan_monotonic_ns() + timeout_ns;
  while (titan_monotonic_ns() < end) {
    if (__atomic_load_n(&g_ipc->ready, __ATOMIC_ACQUIRE))
      return 0;
    nap_ns(200000);
  }
  return -1;
}

static void test_events(void) {
  nt_handle auto_ev = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);
  nt_handle manual_ev = nt_object_create(g_arena, NT_EVENT_MANUAL, 0, 0, 0);
  check("create_auto", auto_ev != 0);
  check("create_manual", manual_ev != 0);

  check_u32("auto_unsignaled", nt_wait_one(g_arena, auto_ev, 0), NT_WAIT_TIMEOUT);
  check_u32("auto_signal", (uint32_t)nt_object_signal(g_arena, auto_ev), 0);
  check_u32("auto_wait", nt_wait_one(g_arena, auto_ev, T_SHORT_NS),
            NT_WAIT_OBJECT_0);
  check_u32("auto_consumed", nt_wait_one(g_arena, auto_ev, T_SHORT_NS),
            NT_WAIT_TIMEOUT);

  check_u32("manual_signal", (uint32_t)nt_object_signal(g_arena, manual_ev), 0);
  for (int i = 0; i < 3; i++)
    check_u32("manual_wait", nt_wait_one(g_arena, manual_ev, 0),
              NT_WAIT_OBJECT_0);

  nt_handle_close(g_arena, auto_ev);
  nt_handle_close(g_arena, manual_ev);
}

static void test_semaphore(void) {
  nt_handle sem = nt_object_create(g_arena, NT_SEMAPHORE, 2, 3, 0);
  check("create_sem", sem != 0);
  check_u32("sem_wait1", nt_wait_one(g_arena, sem, 0), NT_WAIT_OBJECT_0);
  check_u32("sem_wait2", nt_wait_one(g_arena, sem, 0), NT_WAIT_OBJECT_0);
  check_u32("sem_empty", nt_wait_one(g_arena, sem, T_SHORT_NS), NT_WAIT_TIMEOUT);
  check_u32("sem_release3", (uint32_t)nt_semaphore_release(g_arena, sem, 3), 0);
  check_u32("sem_state3", nt_object_state(g_arena, sem), 3);
  check_u32("sem_over_max", (uint32_t)nt_semaphore_release(g_arena, sem, 1), -1);
  for (int i = 0; i < 3; i++)
    check_u32("sem_drain", nt_wait_one(g_arena, sem, 0), NT_WAIT_OBJECT_0);
  check_u32("sem_zero", nt_object_state(g_arena, sem), 0);
  nt_handle_close(g_arena, sem);
}

static void test_mutex(void) {
  nt_handle m = nt_object_create(g_arena, NT_MUTEX, 0, 0, 0);
  check("create_mutex", m != 0);
  check_u32("mutex_wait", nt_wait_one(g_arena, m, 0), NT_WAIT_OBJECT_0);
  check_u32("mutex_recursive", nt_wait_one(g_arena, m, 0), NT_WAIT_OBJECT_0);
  check_u32("mutex_release1", (uint32_t)nt_mutex_release(g_arena, m), 0);
  check_u32("mutex_release2", (uint32_t)nt_mutex_release(g_arena, m), 0);
  check_u32("mutex_release_unowned", (uint32_t)nt_mutex_release(g_arena, m),
            -1);
  check_u32("mutex_free", nt_object_state(g_arena, m), 1);
  nt_handle_close(g_arena, m);
}

static void test_wait_any(void) {
  nt_handle e[3];
  for (int i = 0; i < 3; i++)
    e[i] = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);

  check_u32("any_none", nt_wait_any(g_arena, e, 3, T_SHORT_NS), NT_WAIT_TIMEOUT);
  nt_object_signal(g_arena, e[2]);
  check_u32("any_highest", nt_wait_any(g_arena, e, 3, 0), 2);
  nt_object_signal(g_arena, e[0]);
  nt_object_signal(g_arena, e[1]);
  check_u32("any_lowest", nt_wait_any(g_arena, e, 3, 0), 0);
  check_u32("any_second", nt_wait_any(g_arena, e, 3, 0), 1);
  check_u32("any_drained", nt_wait_any(g_arena, e, 3, 0), NT_WAIT_TIMEOUT);

  check_u32("any_zero_count", nt_wait_any(g_arena, e, 0, 0), NT_WAIT_FAILED);
  nt_handle bad = 0xdeadbeefdeadbeefull;
  check_u32("any_bad_handle", nt_wait_any(g_arena, &bad, 1, 0),
            NT_WAIT_FAILED);

  for (int i = 0; i < 3; i++)
    nt_handle_close(g_arena, e[i]);
}

static void test_wait_all_atomic(void) {
  nt_handle sem = nt_object_create(g_arena, NT_SEMAPHORE, 1, 2, 0);
  nt_handle ev = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);
  nt_handle h[2] = {sem, ev};

  check_u32("all_blocked", nt_wait_all(g_arena, h, 2, T_SHORT_NS),
            NT_WAIT_TIMEOUT);
  check_u32("all_sem_untouched", nt_object_state(g_arena, sem), 1);

  nt_object_signal(g_arena, ev);
  check_u32("all_ready", nt_wait_all(g_arena, h, 2, 0), NT_WAIT_OBJECT_0);
  check_u32("all_sem_taken", nt_object_state(g_arena, sem), 0);
  check_u32("all_ev_taken", nt_object_state(g_arena, ev), 0);

  nt_handle dup[2] = {sem, sem};
  check_u32("all_dup_sem_blocked", nt_wait_all(g_arena, dup, 2, 0),
            NT_WAIT_TIMEOUT);
  nt_semaphore_release(g_arena, sem, 2);
  check_u32("all_dup_sem", nt_wait_all(g_arena, dup, 2, 0), NT_WAIT_OBJECT_0);
  check_u32("all_dup_sem_zero", nt_object_state(g_arena, sem), 0);

  nt_handle evdup[2] = {ev, ev};
  check_u32("all_dup_auto", nt_wait_all(g_arena, evdup, 2, 0), NT_WAIT_FAILED);

  nt_handle_close(g_arena, sem);
  nt_handle_close(g_arena, ev);
}

static void test_wait_all_mixed(void) {
  nt_handle man = nt_object_create(g_arena, NT_EVENT_MANUAL, 0, 0, 0);
  nt_handle mut = nt_object_create(g_arena, NT_MUTEX, 0, 0, 0);
  nt_handle sem = nt_object_create(g_arena, NT_SEMAPHORE, 0, 4, 0);
  nt_handle h[3] = {man, mut, sem};

  check_u32("mixed_blocked", nt_wait_all(g_arena, h, 3, T_SHORT_NS),
            NT_WAIT_TIMEOUT);
  check_u32("mixed_mutex_untouched", nt_object_state(g_arena, mut), 1);

  nt_object_signal(g_arena, man);
  nt_semaphore_release(g_arena, sem, 1);
  check_u32("mixed_ready", nt_wait_all(g_arena, h, 3, 0), NT_WAIT_OBJECT_0);
  check_u32("mixed_mutex_owned", nt_object_state(g_arena, mut), 0);
  check_u32("mixed_sem_zero", nt_object_state(g_arena, sem), 0);
  nt_mutex_release(g_arena, mut);

  nt_handle_close(g_arena, man);
  nt_handle_close(g_arena, mut);
  nt_handle_close(g_arena, sem);
}

static void test_aba(void) {
  nt_handle stale = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);
  uint32_t stale_slot = (uint32_t)stale;
  check("aba_create", stale != 0);
  nt_handle_close(g_arena, stale);

  nt_handle fill[1024];
  uint32_t n = 0;
  for (uint32_t i = 0; i < 1024; i++) {
    nt_handle h = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);
    if (!h)
      break;
    fill[n++] = h;
  }
  int reused = 0;
  for (uint32_t i = 0; i < n; i++)
    if ((uint32_t)fill[i] == stale_slot && fill[i] != stale)
      reused = 1;
  check("aba_slot_reused", reused);
  check_u32("aba_stale_wait", nt_wait_one(g_arena, stale, 0), NT_WAIT_FAILED);
  check_u32("aba_stale_close", (uint32_t)nt_handle_close(g_arena, stale), -1);
  for (uint32_t i = 0; i < n; i++)
    nt_handle_close(g_arena, fill[i]);
}

static void test_named(void) {
  nt_handle a = nt_object_create(g_arena, NT_EVENT_MANUAL, 0, 0, "titan.test.ev");
  check("named_create", a != 0);
  nt_handle b = nt_object_open(g_arena, "titan.test.ev");
  check_u32("named_same", b, a);
  nt_handle missing = nt_object_open(g_arena, "titan.test.missing");
  check_u32("named_missing", missing, 0);

  g_ipc->ready = 0;
  g_ipc->flag = 0;
  long pid = titan_clone_fork();
  if (pid == 0) {
    nt_handle h = nt_object_open(g_arena, "titan.test.ev");
    __atomic_store_n(&g_ipc->ready, 1u, __ATOMIC_RELEASE);
    uint32_t r = h ? nt_wait_one(g_arena, h, T_LONG_NS) : NT_WAIT_FAILED;
    titan_exit_group(r == NT_WAIT_OBJECT_0 ? 0 : 1);
  }
  nt_process_register(g_arena, (uint32_t)pid);
  check("named_child_ready", wait_ready(T_LONG_NS) == 0);
  nt_object_signal(g_arena, a);
  int status = 0;
  titan_wait4(pid, &status, 0);
  nt_process_exit(g_arena, (uint32_t)pid);
  check("named_crossproc", TITAN_WIFEXITED(status) &&
                              TITAN_WEXITSTATUS(status) == 0);

  nt_handle_close(g_arena, b);
  nt_handle_close(g_arena, a);
  nt_handle c = nt_object_create(g_arena, NT_EVENT_MANUAL, 0, 0, "titan.test.ev");
  check("named_generation", c != 0 && c != a);
  nt_handle_close(g_arena, c);
}

static void test_crossproc_any(void) {
  nt_handle e[2];
  e[0] = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);
  e[1] = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);
  g_ipc->ready = 0;
  g_ipc->index = 99;

  long pid = titan_clone_fork();
  if (pid == 0) {
    __atomic_store_n(&g_ipc->ready, 1u, __ATOMIC_RELEASE);
    uint32_t r = nt_wait_any(g_arena, e, 2, T_LONG_NS);
    __atomic_store_n(&g_ipc->index, r, __ATOMIC_RELEASE);
    titan_exit_group(r == 1 ? 0 : 1);
  }
  nt_process_register(g_arena, (uint32_t)pid);
  check("any_child_ready", wait_ready(T_LONG_NS) == 0);
  nt_object_signal(g_arena, e[1]);
  int status = 0;
  titan_wait4(pid, &status, 0);
  nt_process_exit(g_arena, (uint32_t)pid);
  check_u32("any_crossproc_index", g_ipc->index, 1);
  check("any_crossproc", TITAN_WIFEXITED(status) &&
                             TITAN_WEXITSTATUS(status) == 0);
  nt_handle_close(g_arena, e[0]);
  nt_handle_close(g_arena, e[1]);
}

static void test_abandoned(void) {
  nt_handle m = nt_object_create(g_arena, NT_MUTEX, 0, 0, 0);
  g_ipc->ready = 0;

  long pid = titan_clone_fork();
  if (pid == 0) {
    uint32_t r = nt_wait_one(g_arena, m, T_LONG_NS);
    if (r != NT_WAIT_OBJECT_0)
      titan_exit_group(1);
    __atomic_store_n(&g_ipc->ready, 1u, __ATOMIC_RELEASE);
    titan_exit_group(0);
  }
  nt_process_register(g_arena, (uint32_t)pid);
  check("abandon_ready", wait_ready(T_LONG_NS) == 0);
  int status = 0;
  titan_wait4(pid, &status, 0);
  nt_process_exit(g_arena, (uint32_t)pid);

  check_u32("abandon_wait", nt_wait_one(g_arena, m, T_SHORT_NS),
            NT_WAIT_ABANDONED);
  check_u32("abandon_release", (uint32_t)nt_mutex_release(g_arena, m), 0);
  check_u32("abandon_free", nt_object_state(g_arena, m), 1);
  nt_handle_close(g_arena, m);
}

static void test_spin_hint(void) {
  nt_handle e = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);
  check_u32("hint_initial", nt_spin_hint(g_arena, e), 2000);
  nt_wait_one(g_arena, e, T_SHORT_NS);
  uint32_t after_one = nt_spin_hint(g_arena, e);
  nt_wait_one(g_arena, e, T_SHORT_NS);
  check_u32("hint_after_block", nt_spin_hint(g_arena, e), 0);
  check("hint_decayed", after_one > 0);
  nt_object_signal(g_arena, e);
  check_u32("hint_wait", nt_wait_one(g_arena, e, T_SHORT_NS),
            NT_WAIT_OBJECT_0);
  nt_handle_close(g_arena, e);
}

static void test_affinity(void) {
#ifndef TITAN_HOST_TEST
  uint64_t orig = 0;
  if (titan_sched_getaffinity(0, sizeof(orig), &orig) < 0) {
    check("affinity_read", 0);
    return;
  }
  check("affinity_perf", titan_pin_cluster(1) == 0 &&
                             titan_cluster_of_self() == 1);
  check("affinity_eff", titan_pin_cluster(0) == 0 &&
                            titan_cluster_of_self() == 0);
  titan_sched_setaffinity(0, sizeof(orig), &orig);
#endif
}

static void test_ping_stress(void) {
  nt_handle to_child = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);
  nt_handle to_parent = nt_object_create(g_arena, NT_EVENT_AUTO, 0, 0, 0);
  g_ipc->count = 0;
  g_ipc->fail = 0;

  long pid = titan_clone_fork();
  if (pid == 0) {
    uint32_t fail = 0;
    for (uint32_t i = 0; i < 20000; i++) {
      if (nt_wait_one(g_arena, to_child, T_LONG_NS) != NT_WAIT_OBJECT_0) {
        fail++;
        break;
      }
      __atomic_add_fetch(&g_ipc->count, 1u, __ATOMIC_RELAXED);
      if (nt_object_signal(g_arena, to_parent) != 0) {
        fail++;
        break;
      }
    }
    titan_exit_group(fail ? 1 : 0);
  }
  nt_process_register(g_arena, (uint32_t)pid);

  uint32_t fail = 0;
  for (uint32_t i = 0; i < 20000; i++) {
    if (nt_object_signal(g_arena, to_child) != 0) {
      fail++;
      break;
    }
    if (nt_wait_one(g_arena, to_parent, T_LONG_NS) != NT_WAIT_OBJECT_0) {
      fail++;
      break;
    }
  }
  int status = 0;
  titan_wait4(pid, &status, 0);
  nt_process_exit(g_arena, (uint32_t)pid);
  check_u32("ping_parent_failures", fail, 0);
  check_u32("ping_child_count", g_ipc->count, 20000);
  check("ping_child_exit", TITAN_WIFEXITED(status) &&
                               TITAN_WEXITSTATUS(status) == 0);
  nt_handle_close(g_arena, to_child);
  nt_handle_close(g_arena, to_parent);
}

static void test_sem_stress(void) {
  nt_handle sem = nt_object_create(g_arena, NT_SEMAPHORE, 0, 100000, 0);
  g_ipc->count = 0;
  g_ipc->fail = 0;

  long kids[2];
  for (int k = 0; k < 2; k++) {
    long pid = titan_clone_fork();
    if (pid == 0) {
      uint32_t fail = 0;
      for (uint32_t i = 0; i < 25000; i++) {
        if (nt_wait_one(g_arena, sem, T_LONG_NS) != NT_WAIT_OBJECT_0) {
          fail++;
          break;
        }
        __atomic_add_fetch(&g_ipc->count, 1u, __ATOMIC_RELAXED);
      }
      titan_exit_group(fail ? 1 : 0);
    }
    kids[k] = pid;
    nt_process_register(g_arena, (uint32_t)pid);
  }

  uint32_t released = 0;
  while (released < 50000) {
    uint32_t batch = 50000 - released;
    if (batch > 1000)
      batch = 1000;
    if (nt_semaphore_release(g_arena, sem, batch) != 0)
      break;
    released += batch;
    if (g_ipc->count + 2000 < released)
      nap_ns(100000);
  }

  uint32_t exits = 0;
  for (int k = 0; k < 2; k++) {
    int status = 0;
    titan_wait4(kids[k], &status, 0);
    nt_process_exit(g_arena, (uint32_t)kids[k]);
    if (TITAN_WIFEXITED(status) && TITAN_WEXITSTATUS(status) == 0)
      exits++;
  }
  check_u32("sem_stress_released", released, 50000);
  check_u32("sem_stress_count", g_ipc->count, 50000);
  check_u32("sem_stress_exits", exits, 2);
  nt_handle_close(g_arena, sem);
}

int main(void) {
  long fd = titan_memfd_create("titan-m2-test", 0);
  if (fd < 0 || titan_ftruncate((int)fd, 4096) != 0) {
    titan_write_str(2, "ipc page failed\n");
    return 1;
  }
  long p = titan_mmap(0, 4096, TITAN_PROT_READ | TITAN_PROT_WRITE,
                      TITAN_MAP_SHARED, (int)fd, 0);
  if (p < 0) {
    titan_write_str(2, "ipc mmap failed\n");
    return 1;
  }
  g_ipc = (volatile struct ipc *)(uintptr_t)p;

  g_arena = nt_arena_create();
  if (!g_arena) {
    titan_write_str(2, "arena create failed\n");
    return 1;
  }
  nt_process_register(g_arena, (uint32_t)titan_getpid());

  test_events();
  test_semaphore();
  test_mutex();
  test_wait_any();
  test_wait_all_atomic();
  test_wait_all_mixed();
  test_aba();
  test_named();
  test_crossproc_any();
  test_abandoned();
  test_spin_hint();
  test_ping_stress();
  test_sem_stress();
  test_affinity();

  struct titan_buf b;
  char storage[192];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"ntsync_summary\",\"checks\":");
  titan_buf_u64(&b, g_checks);
  titan_buf_str(&b, ",\"failures\":");
  titan_buf_u64(&b, g_failures);
  titan_buf_str(&b, ",\"status\":\"");
  titan_buf_str(&b, g_failures == 0 ? "ok" : "fail");
  titan_buf_str(&b, "\"}");
  titan_buf_emit(&b);
  return g_failures == 0 ? 0 : 1;
}
