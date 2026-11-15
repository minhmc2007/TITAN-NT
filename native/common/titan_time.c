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
