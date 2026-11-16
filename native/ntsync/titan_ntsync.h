#ifndef TITAN_NTSYNC_H
#define TITAN_NTSYNC_H

#include <stdint.h>

#define NT_WAIT_OBJECT_0 0u
#define NT_WAIT_ABANDONED 0x80u
#define NT_WAIT_TIMEOUT 0x102u
#define NT_WAIT_FAILED 0xffffffffu
#define NT_INFINITE UINT64_MAX

/* MAXIMUM_WAIT_OBJECTS. */
#define NT_MAX_WAIT 64

enum nt_object_type {
  NT_EVENT_AUTO = 1,
  NT_EVENT_MANUAL = 2,
  NT_MUTEX = 3,
  NT_SEMAPHORE = 4
};

/* slot + 1 in the low word, slot generation in the high word: a stale handle
   can never resolve to a recycled slot. */
typedef uint64_t nt_handle;

struct nt_arena;

#ifdef __cplusplus
extern "C" {
#endif

struct nt_arena *nt_arena_create(void);
struct nt_arena *nt_arena_attach(int fd);
int nt_arena_fd(struct nt_arena *a);

nt_handle nt_object_create(struct nt_arena *a, uint32_t type, uint32_t initial,
                           uint32_t max_count, const char *name);
nt_handle nt_object_open(struct nt_arena *a, const char *name);
int nt_handle_close(struct nt_arena *a, nt_handle h);

int nt_object_signal(struct nt_arena *a, nt_handle h);
int nt_mutex_release(struct nt_arena *a, nt_handle h);
int nt_semaphore_release(struct nt_arena *a, nt_handle h, uint32_t count);

uint32_t nt_wait_one(struct nt_arena *a, nt_handle h, uint64_t timeout_ns);
uint32_t nt_wait_any(struct nt_arena *a, const nt_handle *h, uint32_t n,
                     uint64_t timeout_ns);
uint32_t nt_wait_all(struct nt_arena *a, const nt_handle *h, uint32_t n,
                     uint64_t timeout_ns);

int nt_process_register(struct nt_arena *a, uint32_t pid);
void nt_process_exit(struct nt_arena *a, uint32_t pid);

/* Test and tuning surface. */
uint32_t nt_spin_hint(struct nt_arena *a, nt_handle h);
uint32_t nt_object_state(struct nt_arena *a, nt_handle h);

#ifdef __cplusplus
}
#endif

#endif
