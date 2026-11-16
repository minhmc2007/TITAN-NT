#include "titan_time.h"

uint64_t titan_monotonic_ns(void) {
  struct titan_timespec ts;
  if (titan_clock_gettime(TITAN_CLOCK_MONOTONIC, &ts) < 0)
    return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void titan_deadline_ns(struct titan_timespec *ts, uint64_t delta_ns) {
  uint64_t now = titan_monotonic_ns() + delta_ns;
  ts->tv_sec = (int64_t)(now / 1000000000ull);
  ts->tv_nsec = (int64_t)(now % 1000000000ull);
}

#ifdef TITAN_HOST_TEST

uint64_t titan_spin_now(void) { return titan_monotonic_ns(); }

uint64_t titan_spin_ticks(uint64_t budget_ns) { return budget_ns; }

#else

uint64_t titan_spin_now(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}

uint64_t titan_spin_ticks(uint64_t budget_ns) {
  uint64_t freq;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  if (freq == 0 || freq > 1000000000ull)
    return 0;
  return budget_ns * freq / 1000000000ull;
}

#endif
