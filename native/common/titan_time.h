#ifndef TITAN_TIME_H
#define TITAN_TIME_H

#include <stdint.h>

#include "titan_sys.h"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t titan_monotonic_ns(void);
void titan_deadline_ns(struct titan_timespec *ts, uint64_t delta_ns);

/* Fixed-rate counter for spin budgets. CNTVCT keeps the budget in wall time on
   every cluster; an iteration count would stretch on A55. */
uint64_t titan_spin_now(void);
uint64_t titan_spin_ticks(uint64_t budget_ns);

#ifdef __cplusplus
}
#endif

#endif
