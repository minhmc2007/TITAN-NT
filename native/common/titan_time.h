#ifndef TITAN_TIME_H
#define TITAN_TIME_H

#include <stdint.h>

#include "titan_sys.h"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t titan_monotonic_ns(void);
void titan_deadline_ns(struct titan_timespec *ts, uint64_t delta_ns);

#ifdef __cplusplus
}
#endif

#endif
