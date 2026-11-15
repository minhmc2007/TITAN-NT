#include "x18_guard.h"

#include <sys/auxv.h>

#include "../common/titan_log.h"
#include "../common/titan_sys.h"
#include "titan_signal.h"

extern long titan_x18_call_raw(void *fn, long a0, long a1, long a2, long a3,
                               long a4, long a5);

__attribute__((tls_model("local-exec"))) __thread void *titan_scs_ptr;
__attribute__((tls_model("local-exec"))) __thread struct titan_teb *titan_teb_ptr;

static char titan_bootstrap_scs[4096] __attribute__((aligned(16)));

void *titan_scs_current(void) { return titan_scs_ptr; }

struct titan_teb *titan_teb_current(void) { return titan_teb_ptr; }

long titan_guard_call(void *fn, long a0, long a1, long a2, long a3, long a4,
                      long a5) {
  struct titan_teb *teb = (struct titan_teb *)titan_x18_get();
  if (!teb || teb->magic != TITAN_TEB_MAGIC ||
      teb->self != (uint64_t)(uintptr_t)teb)
    titan_fatal("guard called without teb in x18");
  if (!titan_scs_ptr)
    titan_fatal("guard called without scs pointer");

  long r = titan_x18_call_raw(fn, a0, a1, a2, a3, a4, a5);
  __atomic_add_fetch(&teb->guard_calls, 1, __ATOMIC_RELAXED);
  if (titan_x18_get() != teb)
    __atomic_add_fetch(&teb->guard_bad, 1, __ATOMIC_RELAXED);
  return r;
}

static long titan_map_stack(size_t guard_bytes, size_t payload_bytes,
                            uint64_t *map_lo, uint64_t *map_hi,
                            uint64_t *payload_lo) {
  size_t total = guard_bytes + payload_bytes;
  long p = titan_mmap(0, total, TITAN_PROT_NONE,
                      TITAN_MAP_PRIVATE | TITAN_MAP_ANONYMOUS, -1, 0);
  if (p < 0)
    return p;
  if (titan_mprotect((void *)(uintptr_t)(p + (long)guard_bytes), payload_bytes,
                     TITAN_PROT_READ | TITAN_PROT_WRITE) < 0) {
    titan_munmap((void *)(uintptr_t)p, total);
    return -1;
  }
  *map_lo = (uint64_t)p;
  *map_hi = (uint64_t)p + total;
  *payload_lo = (uint64_t)p + guard_bytes;
  return 0;
}

int titan_thread_init(struct titan_teb *teb) {
  long page = -1;
  uint64_t scs_lo = 0, scs_hi = 0, scs_payload = 0;
  uint64_t alt_lo = 0, alt_hi = 0, alt_payload = 0;

  teb->magic = TITAN_TEB_MAGIC;
  teb->self = (uint64_t)(uintptr_t)teb;
  teb->tid = (uint64_t)titan_gettid();

  titan_scs_ptr = titan_bootstrap_scs + sizeof(titan_bootstrap_scs);
  titan_teb_ptr = teb;
  titan_x18_set(teb);

  page = titan_guard_call((void *)getauxval, AT_PAGESZ, 0, 0, 0, 0, 0);
  if (page <= 0)
    return -1;

  if (titan_map_stack((size_t)page, (size_t)page, &scs_lo, &scs_hi,
                      &scs_payload) < 0)
    return -1;
  teb->scs_lo = scs_payload;
  teb->scs_ptr = scs_hi;
  titan_scs_ptr = (void *)(uintptr_t)scs_hi;

  if (titan_map_stack((size_t)page, (size_t)page * 3, &alt_lo, &alt_hi,
                      &alt_payload) < 0)
    return -1;
  teb->altstack_lo = alt_lo;
  teb->altstack_hi = alt_hi;

  if (titan_signal_altstack((void *)(uintptr_t)alt_payload, (size_t)page * 3) <
      0)
    return -1;

  return titan_thread_ok(teb) ? 0 : -1;
}

int titan_thread_ok(const struct titan_teb *teb) {
  return titan_x18_get() == (const void *)teb &&
         teb->magic == TITAN_TEB_MAGIC &&
         teb->self == (uint64_t)(uintptr_t)teb && titan_scs_ptr != 0 &&
         titan_scs_ptr == (const void *)(uintptr_t)teb->scs_ptr;
}

void titan_thread_report(struct titan_teb *teb, const char *label) {
  struct titan_buf b;
  char storage[384];
  titan_buf_init(&b, storage, sizeof(storage));
  titan_buf_str(&b, "{\"test\":\"x18_thread\",\"label\":\"");
  titan_buf_str(&b, label);
  titan_buf_str(&b, "\",\"tid\":");
  titan_buf_u64(&b, __atomic_load_n(&teb->tid, __ATOMIC_RELAXED));
  titan_buf_str(&b, ",\"guard_calls\":");
  titan_buf_u64(&b, __atomic_load_n(&teb->guard_calls, __ATOMIC_RELAXED));
  titan_buf_str(&b, ",\"guard_bad\":");
  titan_buf_u64(&b, __atomic_load_n(&teb->guard_bad, __ATOMIC_RELAXED));
  titan_buf_str(&b, ",\"sig_teb\":");
  titan_buf_u64(&b, __atomic_load_n(&teb->sig_teb, __ATOMIC_RELAXED));
  titan_buf_str(&b, ",\"sig_scs\":");
  titan_buf_u64(&b, __atomic_load_n(&teb->sig_scs, __ATOMIC_RELAXED));
  titan_buf_str(&b, ",\"sig_bad\":");
  titan_buf_u64(&b, __atomic_load_n(&teb->sig_bad, __ATOMIC_RELAXED));
  titan_buf_str(&b, ",\"faults\":");
  titan_buf_u64(&b, __atomic_load_n(&teb->faults, __ATOMIC_RELAXED));
  titan_buf_str(&b, ",\"ill\":");
  titan_buf_u64(&b, __atomic_load_n(&teb->ill, __ATOMIC_RELAXED));
  titan_buf_str(&b, ",\"ok\":");
  titan_buf_str(&b, titan_thread_ok(teb) ? "true" : "false");
  titan_buf_str(&b, "}");
  titan_buf_emit(&b);
}
