#ifndef TITAN_X18_GUARD_H
#define TITAN_X18_GUARD_H

#include <stddef.h>
#include <stdint.h>

#define TITAN_TEB_MAGIC 0x544954414e544542ull

struct titan_teb {
  uint64_t magic;
  uint64_t self;
  uint64_t tid;
  uint64_t guard_calls;
  uint64_t guard_bad;
  uint64_t sig_teb;
  uint64_t sig_scs;
  uint64_t sig_bad;
  uint64_t faults;
  uint64_t ill;
  uint64_t fault_lo;
  uint64_t fault_hi;
  uint64_t ill_lo;
  uint64_t ill_hi;
  uint64_t scs_ptr;
  uint64_t scs_lo;
  uint64_t altstack_lo;
  uint64_t altstack_hi;
};

#ifdef __cplusplus
extern "C" {
#endif

long titan_guard_call(void *fn, long a0, long a1, long a2, long a3, long a4,
                      long a5);
void *titan_scs_current(void);
struct titan_teb *titan_teb_current(void);
int titan_thread_init(struct titan_teb *teb);
int titan_thread_ok(const struct titan_teb *teb);
void titan_thread_report(struct titan_teb *teb, const char *label);
extern void titan_udf_trap(void);

#ifdef __cplusplus
}
#endif

static inline void *titan_x18_get(void) {
  void *v;
  __asm__ volatile("mov %0, x18" : "=r"(v));
  return v;
}

static inline void titan_x18_set(void *v) {
  __asm__ volatile("mov x18, %0" : : "r"(v) : "memory");
}

#endif
