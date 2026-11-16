#include "titan_affinity.h"

#include "titan_sys.h"

int titan_pin_cluster(int perf) {
  uint64_t mask = perf ? TITAN_MASK_PERF : TITAN_MASK_EFF;
  return (int)titan_sched_setaffinity(0, sizeof(mask), &mask);
}

int titan_cluster_of_self(void) {
  uint64_t mask = 0;
  if (titan_sched_getaffinity(0, sizeof(mask), &mask) < 0)
    return -1;
  uint64_t eff = mask & TITAN_MASK_EFF;
  uint64_t perf = mask & TITAN_MASK_PERF;
  if (perf && !eff)
    return 1;
  if (eff && !perf)
    return 0;
  return -1;
}
