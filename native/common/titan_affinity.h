#ifndef TITAN_AFFINITY_H
#define TITAN_AFFINITY_H

#ifdef __cplusplus
extern "C" {
#endif

#define TITAN_MASK_EFF 0x0full
#define TITAN_MASK_PERF 0xf0ull

/* 1 = A78 cluster (CPU 4-7), 0 = A55 cluster (CPU 0-3). */
int titan_pin_cluster(int perf);

/* Returns the cluster the calling thread may currently run on, -1 if mixed. */
int titan_cluster_of_self(void);

#ifdef __cplusplus
}
#endif

#endif
