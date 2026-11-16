#include "titan_ntsync.h"

#include "../common/titan_sys.h"
#include "../common/titan_time.h"

#define NT_MAGIC 0x4e54534eu
#define NT_VERSION 1u
#define NT_ARENA_BYTES (512u * 1024u)

#define NT_MAX_OBJECTS 1024u
#define NT_MAX_WAITERS 512u
#define NT_MAX_PROCS 64u
#define NT_NAME_SLOTS 128u
#define NT_OBJ_QUEUE 16u
#define NT_OBJ_REFS 4u

#define NT_SPIN_MAX_NS 2000u
#define NT_SPIN_BLOCK_LIMIT 2u
#define NT_LOCK_SPIN_NS 2000u
#define NT_LOCK_SLEEP_NS 2000000ull
#define NT_EPOCH_FALLBACK_NS 1000000ull

struct nt_obj_ref {
  uint32_t pid;
  uint32_t count;
};

struct nt_object {
  /* futex word: bumped on every state transition a waiter can observe. */
  _Alignas(64) uint32_t epoch;
  uint32_t state;
  uint32_t type;
  uint32_t generation;
  uint32_t owner_pid;
  uint32_t owner_tid;
  uint32_t recursion;
  uint32_t abandoned;
  uint32_t max_count;
  uint32_t refs;
  uint32_t queue_len;
  uint32_t spin_ns;
  uint32_t blocks;
  uint32_t queue[NT_OBJ_QUEUE];
  struct nt_obj_ref ref_by[NT_OBJ_REFS];
  uint32_t pad[10];
};

struct nt_waiter {
  _Alignas(64) uint32_t word;
  uint32_t state;
  uint32_t generation;
  uint32_t count;
  uint32_t pid;
  uint32_t objects[NT_MAX_WAIT];
  uint32_t pad[11];
};

struct nt_process {
  uint32_t pid;
  uint32_t generation;
  uint32_t alive;
  uint32_t pad;
};

struct nt_name {
  uint64_t hash;
  uint32_t slot;
  uint32_t generation;
};

struct nt_arena {
  _Alignas(64) uint32_t lock;
  uint32_t lock_waiters;
  uint32_t magic;
  uint32_t version;
  uint32_t nobjects;
  uint32_t nwaiters;
  uint32_t nprocs;
  uint32_t nnames;
  uint32_t next_obj;
  uint32_t next_waiter;
  int32_t fd;
  uint32_t pad[3];
  uint64_t bytes;
  struct nt_process procs[NT_MAX_PROCS];
  struct nt_name names[NT_NAME_SLOTS];
  struct nt_object objects[NT_MAX_OBJECTS];
  struct nt_waiter waiters[NT_MAX_WAITERS];
};

_Static_assert(sizeof(struct nt_object) == 192, "object layout");
_Static_assert(sizeof(struct nt_waiter) == 320, "waiter layout");
_Static_assert(sizeof(struct nt_arena) <= NT_ARENA_BYTES, "arena size");

static uint32_t nt_strlen(const char *s) {
  uint32_t n = 0;
  while (s[n])
    n++;
  return n;
}

static uint64_t nt_hash(const char *s) {
  uint64_t h = 1469598103934665603ull;
  uint32_t n = nt_strlen(s);
  for (uint32_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ull;
  }
  return h;
}

static void nt_zero(void *p, size_t n) {
  uint64_t *w = (uint64_t *)p;
  for (size_t i = 0; i < n / 8; i++)
    w[i] = 0;
}

static void nt_relax(void) {
#ifdef TITAN_HOST_TEST
  __asm__ volatile("pause" ::: "memory");
#else
  __asm__ volatile("yield" ::: "memory");
#endif
}

static void nt_lock(struct nt_arena *a) {
  uint32_t expected = 0;
  if (__atomic_compare_exchange_n(&a->lock, &expected, 1u, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return;

  uint64_t end = titan_spin_now() + titan_spin_ticks(NT_LOCK_SPIN_NS);
  while (titan_spin_now() < end) {
    expected = 0;
    if (__atomic_compare_exchange_n(&a->lock, &expected, 1u, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return;
    nt_relax();
  }

  __atomic_add_fetch(&a->lock_waiters, 1u, __ATOMIC_RELAXED);
  for (;;) {
    expected = 0;
    if (__atomic_compare_exchange_n(&a->lock, &expected, 1u, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      break;
    struct titan_timespec ts;
    titan_deadline_ns(&ts, NT_LOCK_SLEEP_NS);
    titan_futex(&a->lock, TITAN_FUTEX_WAIT_BITSET, 1u, &ts, 0,
                TITAN_FUTEX_BITSET_MATCH_ANY);
  }
  __atomic_sub_fetch(&a->lock_waiters, 1u, __ATOMIC_RELAXED);
}

static void nt_unlock(struct nt_arena *a) {
  __atomic_store_n(&a->lock, 0u, __ATOMIC_RELEASE);
  if (__atomic_load_n(&a->lock_waiters, __ATOMIC_RELAXED))
    titan_futex(&a->lock, TITAN_FUTEX_WAKE, 1u, 0, 0, 0);
}

static int nt_spin_changed(uint32_t *word, uint32_t seen, uint32_t spin_ns) {
  if (spin_ns == 0)
    return 0;
  uint64_t end = titan_spin_now() + titan_spin_ticks(spin_ns);
  for (;;) {
    uint32_t v;
#ifdef TITAN_HOST_TEST
    v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
#else
    __asm__ volatile("ldaxr %w0, [%1]" : "=&r"(v) : "r"(word) : "memory");
#endif
    if (v != seen) {
#ifndef TITAN_HOST_TEST
      __asm__ volatile("clrex" ::: "memory");
#endif
      return 1;
    }
    if (titan_spin_now() >= end) {
#ifndef TITAN_HOST_TEST
      __asm__ volatile("clrex" ::: "memory");
#endif
      return 0;
    }
#ifdef TITAN_HOST_TEST
    __asm__ volatile("pause" ::: "memory");
#else
    __asm__ volatile("wfe" ::: "memory");
#endif
  }
}

static void nt_wait_word(uint32_t *word, uint32_t seen, uint64_t deadline) {
  struct titan_timespec ts;
  const struct titan_timespec *tsp = 0;
  if (deadline) {
    ts.tv_sec = (int64_t)(deadline / 1000000000ull);
    ts.tv_nsec = (int64_t)(deadline % 1000000000ull);
    tsp = &ts;
  }
  titan_futex(word, TITAN_FUTEX_WAIT_BITSET, seen, tsp, 0,
              TITAN_FUTEX_BITSET_MATCH_ANY);
}

static int nt_resolve(struct nt_arena *a, nt_handle h, uint32_t *slot) {
  uint32_t s = (uint32_t)h;
  uint32_t g = (uint32_t)(h >> 32);
  if (s == 0 || s > NT_MAX_OBJECTS)
    return 0;
  struct nt_object *o = &a->objects[s - 1];
  if (o->refs == 0 || o->type == 0 || o->generation != g)
    return 0;
  *slot = s - 1;
  return 1;
}

static int nt_resolve_all(struct nt_arena *a, const nt_handle *hs, uint32_t n,
                          uint32_t *slots) {
  for (uint32_t i = 0; i < n; i++)
    if (!nt_resolve(a, hs[i], &slots[i]))
      return 0;
  return 1;
}

static int nt_ref_add(struct nt_object *o, uint32_t pid) {
  for (uint32_t i = 0; i < NT_OBJ_REFS; i++) {
    if (o->ref_by[i].pid == pid) {
      o->ref_by[i].count++;
      return 0;
    }
  }
  for (uint32_t i = 0; i < NT_OBJ_REFS; i++) {
    if (o->ref_by[i].pid == 0) {
      o->ref_by[i].pid = pid;
      o->ref_by[i].count = 1;
      return 0;
    }
  }
  /* More than NT_OBJ_REFS processes on one object: the slot is reclaimed when
     every tracked reference closes. */
  return -1;
}

static void nt_ref_drop(struct nt_object *o, uint32_t pid) {
  for (uint32_t i = 0; i < NT_OBJ_REFS; i++) {
    if (o->ref_by[i].pid == pid && o->ref_by[i].count) {
      o->ref_by[i].count--;
      if (o->ref_by[i].count == 0)
        o->ref_by[i].pid = 0;
      return;
    }
  }
}

static void nt_queue_push(struct nt_object *o, uint32_t wi) {
  if (o->queue_len < NT_OBJ_QUEUE)
    o->queue[o->queue_len++] = wi;
}

static int nt_queue_has_room(struct nt_object *o) {
  return o->queue_len < NT_OBJ_QUEUE;
}

static void nt_queue_remove(struct nt_object *o, uint32_t wi) {
  for (uint32_t i = 0; i < o->queue_len; i++) {
    if (o->queue[i] == wi) {
      o->queue[i] = o->queue[--o->queue_len];
      return;
    }
  }
}

static void nt_wake_locked(struct nt_arena *a, struct nt_object *o);

static void nt_free_locked(struct nt_arena *a, struct nt_object *o) {
  nt_wake_locked(a, o);
  o->type = 0;
  o->refs = 0;
  o->generation++;
  o->queue_len = 0;
}

static void nt_wake_locked(struct nt_arena *a, struct nt_object *o) {
  o->epoch++;
  uint32_t n = o->queue_len;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t wi = o->queue[i];
    struct nt_waiter *w = &a->waiters[wi];
    if (w->state == 1) {
      w->state = 2;
      w->word++;
      titan_futex(&w->word, TITAN_FUTEX_WAKE, 1u, 0, 0, 0);
    }
  }
  o->queue_len = 0;
}

static int nt_owned_by(const struct nt_object *o, uint32_t pid, uint32_t tid) {
  return o->owner_pid == pid && o->owner_tid == tid;
}

static int nt_acquirable(const struct nt_object *o, uint32_t demand,
                         uint32_t pid, uint32_t tid) {
  switch (o->type) {
  case NT_EVENT_AUTO:
    return demand == 1 && o->state != 0;
  case NT_EVENT_MANUAL:
    return o->state != 0;
  case NT_SEMAPHORE:
    return o->state >= demand;
  case NT_MUTEX:
    return demand == 1 &&
           (o->state != 0 || o->abandoned || nt_owned_by(o, pid, tid));
  default:
    return 0;
  }
}

/* Returns 1 when the acquired mutex was abandoned. */
static int nt_acquire(struct nt_object *o, uint32_t demand, uint32_t pid,
                      uint32_t tid) {
  switch (o->type) {
  case NT_EVENT_AUTO:
    o->state = 0;
    return 0;
  case NT_EVENT_MANUAL:
    return 0;
  case NT_SEMAPHORE:
    o->state -= demand;
    return 0;
  case NT_MUTEX:
    if (nt_owned_by(o, pid, tid)) {
      o->recursion += demand;
      return 0;
    }
    if (o->abandoned) {
      o->abandoned = 0;
      o->owner_pid = pid;
      o->owner_tid = tid;
      o->recursion = 1;
      o->state = 0;
      return 1;
    }
    o->owner_pid = pid;
    o->owner_tid = tid;
    o->recursion = 1;
    o->state = 0;
    return 0;
  default:
    return 0;
  }
}

static uint32_t nt_waiter_alloc(struct nt_arena *a, uint32_t pid) {
  for (uint32_t k = 0; k < NT_MAX_WAITERS; k++) {
    uint32_t i = (a->next_waiter + k) % NT_MAX_WAITERS;
    struct nt_waiter *w = &a->waiters[i];
    if (w->state == 0) {
      w->state = 1;
      w->word = 1;
      w->generation++;
      w->count = 0;
      w->pid = pid;
      a->next_waiter = (i + 1) % NT_MAX_WAITERS;
      return i;
    }
  }
  return NT_MAX_WAITERS;
}

static int nt_waiter_arm(struct nt_arena *a, uint32_t wi, const uint32_t *slots,
                         uint32_t n) {
  struct nt_waiter *w = &a->waiters[wi];
  w->count = n;
  for (uint32_t i = 0; i < n; i++) {
    w->objects[i] = slots[i];
    if (!nt_queue_has_room(&a->objects[slots[i]])) {
      for (uint32_t j = 0; j < i; j++)
        nt_queue_remove(&a->objects[slots[j]], wi);
      w->state = 0;
      return -1;
    }
    nt_queue_push(&a->objects[slots[i]], wi);
  }
  return 0;
}

static void nt_waiter_disarm(struct nt_arena *a, uint32_t wi) {
  struct nt_waiter *w = &a->waiters[wi];
  if (w->state == 0)
    return;
  for (uint32_t i = 0; i < w->count; i++)
    nt_queue_remove(&a->objects[w->objects[i]], wi);
  w->state = 0;
}

static uint32_t nt_unique(const uint32_t *slots, uint32_t n, uint32_t *unique,
                          uint32_t *demand) {
  uint32_t nu = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t k = 0;
    while (k < nu && unique[k] != slots[i])
      k++;
    if (k == nu) {
      unique[nu] = slots[i];
      demand[nu] = 1;
      nu++;
    } else {
      demand[k]++;
    }
  }
  return nu;
}

static uint32_t nt_wait_multi(struct nt_arena *a, const nt_handle *hs,
                              uint32_t n, uint64_t timeout_ns, int all,
                              uint32_t *abandoned) {
  if (!a || !hs || n == 0 || n > NT_MAX_WAIT)
    return NT_WAIT_FAILED;

  uint32_t pid = (uint32_t)titan_getpid();
  uint32_t tid = (uint32_t)titan_gettid();
  uint64_t deadline = 0;
  if (timeout_ns != NT_INFINITE)
    deadline = titan_monotonic_ns() + timeout_ns;

  uint32_t slots[NT_MAX_WAIT];
  uint32_t unique[NT_MAX_WAIT];
  uint32_t demand[NT_MAX_WAIT];

  for (;;) {
    nt_lock(a);
    if (!nt_resolve_all(a, hs, n, slots)) {
      nt_unlock(a);
      return NT_WAIT_FAILED;
    }

    uint32_t nu = nt_unique(slots, n, unique, demand);
    if (all) {
      for (uint32_t k = 0; k < nu; k++) {
        struct nt_object *o = &a->objects[unique[k]];
        if (demand[k] > 1 && o->type != NT_SEMAPHORE &&
            o->type != NT_EVENT_MANUAL) {
          nt_unlock(a);
          return NT_WAIT_FAILED;
        }
      }
      int ready = 1;
      for (uint32_t k = 0; k < nu; k++)
        if (!nt_acquirable(&a->objects[unique[k]], demand[k], pid, tid)) {
          ready = 0;
          break;
        }
      if (ready) {
        uint32_t ab = 0;
        for (uint32_t k = 0; k < nu; k++)
          ab |= (uint32_t)nt_acquire(&a->objects[unique[k]], demand[k], pid,
                                     tid);
        nt_unlock(a);
        *abandoned = ab;
        return NT_WAIT_OBJECT_0;
      }
    } else {
      for (uint32_t i = 0; i < n; i++) {
        struct nt_object *o = &a->objects[slots[i]];
        if (nt_acquirable(o, 1, pid, tid)) {
          uint32_t ab = (uint32_t)nt_acquire(o, 1, pid, tid);
          nt_unlock(a);
          *abandoned = ab;
          return i;
        }
      }
    }

    if (deadline && titan_monotonic_ns() >= deadline) {
      nt_unlock(a);
      return NT_WAIT_TIMEOUT;
    }

    struct nt_object *first = &a->objects[unique[0]];
    uint32_t spin = first->spin_ns;
    uint32_t epoch = first->epoch;
    uint32_t wi = nt_waiter_alloc(a, pid);
    int armed = 0;
    if (wi != NT_MAX_WAITERS)
      armed = nt_waiter_arm(a, wi, unique, nu) == 0;
    nt_unlock(a);

    if (armed) {
      if (spin && nt_spin_changed(&first->epoch, epoch, spin)) {
        nt_lock(a);
        nt_waiter_disarm(a, wi);
        nt_unlock(a);
        continue;
      }
      if (spin) {
        nt_lock(a);
        if (++first->blocks >= NT_SPIN_BLOCK_LIMIT)
          first->spin_ns = 0;
        nt_unlock(a);
      }
      nt_wait_word(&a->waiters[wi].word, 1, deadline);
      nt_lock(a);
      nt_waiter_disarm(a, wi);
      nt_unlock(a);
    } else {
      uint64_t slice = titan_monotonic_ns() + NT_EPOCH_FALLBACK_NS;
      uint64_t until = deadline && deadline < slice ? deadline : slice;
      nt_wait_word(&first->epoch, epoch, until);
    }
  }
}

struct nt_arena *nt_arena_create(void) {
  long fd = titan_memfd_create("titan-ntsync", 0);
  if (fd < 0)
    return 0;
  if (titan_ftruncate((int)fd, NT_ARENA_BYTES) != 0) {
    titan_close(fd);
    return 0;
  }
  long p = titan_mmap(0, NT_ARENA_BYTES, TITAN_PROT_READ | TITAN_PROT_WRITE,
                      TITAN_MAP_SHARED, (int)fd, 0);
  if (p < 0) {
    titan_close(fd);
    return 0;
  }
  struct nt_arena *a = (struct nt_arena *)(uintptr_t)p;
  nt_zero(a, NT_ARENA_BYTES);
  a->magic = NT_MAGIC;
  a->version = NT_VERSION;
  a->nobjects = NT_MAX_OBJECTS;
  a->nwaiters = NT_MAX_WAITERS;
  a->nprocs = NT_MAX_PROCS;
  a->nnames = NT_NAME_SLOTS;
  a->bytes = NT_ARENA_BYTES;
  a->fd = (int)fd;
  return a;
}

struct nt_arena *nt_arena_attach(int fd) {
  long p = titan_mmap(0, NT_ARENA_BYTES, TITAN_PROT_READ | TITAN_PROT_WRITE,
                      TITAN_MAP_SHARED, fd, 0);
  if (p < 0)
    return 0;
  struct nt_arena *a = (struct nt_arena *)(uintptr_t)p;
  if (a->magic != NT_MAGIC || a->version != NT_VERSION) {
    titan_munmap(a, NT_ARENA_BYTES);
    return 0;
  }
  return a;
}

int nt_arena_fd(struct nt_arena *a) { return a ? a->fd : -1; }

nt_handle nt_object_create(struct nt_arena *a, uint32_t type, uint32_t initial,
                           uint32_t max_count, const char *name) {
  if (!a || type < NT_EVENT_AUTO || type > NT_SEMAPHORE)
    return 0;
  uint32_t pid = (uint32_t)titan_getpid();
  uint64_t hash = 0;
  int named = name && name[0] != '\0';
  if (named)
    hash = nt_hash(name);

  nt_lock(a);
  if (named) {
    for (uint32_t i = 0; i < NT_NAME_SLOTS; i++) {
      if (a->names[i].hash != hash || a->names[i].slot == 0)
        continue;
      uint32_t s = a->names[i].slot;
      struct nt_object *o = &a->objects[s - 1];
      if (o->refs == 0 || o->generation != a->names[i].generation ||
          o->type != type)
        continue;
      o->refs++;
      nt_ref_add(o, pid);
      nt_handle h = ((uint64_t)o->generation << 32) | s;
      nt_unlock(a);
      return h;
    }
  }

  uint32_t idx = NT_MAX_OBJECTS;
  for (uint32_t k = 0; k < NT_MAX_OBJECTS; k++) {
    uint32_t s = (a->next_obj + k) % NT_MAX_OBJECTS;
    if (a->objects[s].refs == 0) {
      idx = s;
      break;
    }
  }
  if (idx == NT_MAX_OBJECTS) {
    nt_unlock(a);
    return 0;
  }

  struct nt_object *o = &a->objects[idx];
  uint32_t gen = o->generation + 1;
  nt_zero(o, sizeof(*o));
  o->generation = gen;
  o->type = type;
  o->spin_ns = NT_SPIN_MAX_NS;
  o->refs = 1;
  o->ref_by[0].pid = pid;
  o->ref_by[0].count = 1;
  if (type == NT_MUTEX)
    o->state = 1;
  else if (type == NT_SEMAPHORE)
    o->state = initial;
  else
    o->state = initial ? 1 : 0;
  o->max_count = max_count;
  a->next_obj = (idx + 1) % NT_MAX_OBJECTS;

  if (named) {
    uint32_t slot = NT_NAME_SLOTS;
    for (uint32_t i = 0; i < NT_NAME_SLOTS; i++) {
      if (a->names[i].slot == 0 ||
          a->objects[a->names[i].slot - 1].refs == 0) {
        slot = i;
        break;
      }
    }
    if (slot != NT_NAME_SLOTS) {
      a->names[slot].hash = hash;
      a->names[slot].slot = idx + 1;
      a->names[slot].generation = gen;
    }
  }

  nt_handle h = ((uint64_t)gen << 32) | (idx + 1);
  nt_unlock(a);
  return h;
}

nt_handle nt_object_open(struct nt_arena *a, const char *name) {
  if (!a || !name || !name[0])
    return 0;
  uint64_t hash = nt_hash(name);
  uint32_t pid = (uint32_t)titan_getpid();
  nt_lock(a);
  for (uint32_t i = 0; i < NT_NAME_SLOTS; i++) {
    if (a->names[i].hash != hash || a->names[i].slot == 0)
      continue;
    uint32_t s = a->names[i].slot;
    struct nt_object *o = &a->objects[s - 1];
    if (o->refs == 0 || o->generation != a->names[i].generation)
      continue;
    o->refs++;
    nt_ref_add(o, pid);
    nt_handle h = ((uint64_t)o->generation << 32) | s;
    nt_unlock(a);
    return h;
  }
  nt_unlock(a);
  return 0;
}

int nt_handle_close(struct nt_arena *a, nt_handle h) {
  if (!a)
    return -1;
  uint32_t pid = (uint32_t)titan_getpid();
  uint32_t slot;
  nt_lock(a);
  if (!nt_resolve(a, h, &slot)) {
    nt_unlock(a);
    return -1;
  }
  struct nt_object *o = &a->objects[slot];
  nt_ref_drop(o, pid);
  if (o->refs)
    o->refs--;
  if (o->refs == 0)
    nt_free_locked(a, o);
  nt_unlock(a);
  return 0;
}

int nt_object_signal(struct nt_arena *a, nt_handle h) {
  if (!a)
    return -1;
  uint32_t slot;
  nt_lock(a);
  if (!nt_resolve(a, h, &slot)) {
    nt_unlock(a);
    return -1;
  }
  struct nt_object *o = &a->objects[slot];
  if (o->type != NT_EVENT_AUTO && o->type != NT_EVENT_MANUAL) {
    nt_unlock(a);
    return -1;
  }
  if (!o->state) {
    o->state = 1;
    nt_wake_locked(a, o);
  }
  nt_unlock(a);
  return 0;
}

int nt_mutex_release(struct nt_arena *a, nt_handle h) {
  if (!a)
    return -1;
  uint32_t pid = (uint32_t)titan_getpid();
  uint32_t tid = (uint32_t)titan_gettid();
  uint32_t slot;
  nt_lock(a);
  if (!nt_resolve(a, h, &slot)) {
    nt_unlock(a);
    return -1;
  }
  struct nt_object *o = &a->objects[slot];
  if (o->type != NT_MUTEX || !nt_owned_by(o, pid, tid) || o->recursion == 0) {
    nt_unlock(a);
    return -1;
  }
  if (--o->recursion == 0) {
    o->owner_pid = 0;
    o->owner_tid = 0;
    o->state = 1;
    nt_wake_locked(a, o);
  }
  nt_unlock(a);
  return 0;
}

int nt_semaphore_release(struct nt_arena *a, nt_handle h, uint32_t count) {
  if (!a || count == 0)
    return -1;
  uint32_t slot;
  nt_lock(a);
  if (!nt_resolve(a, h, &slot)) {
    nt_unlock(a);
    return -1;
  }
  struct nt_object *o = &a->objects[slot];
  if (o->type != NT_SEMAPHORE ||
      (o->max_count && o->state + count > o->max_count)) {
    nt_unlock(a);
    return -1;
  }
  o->state += count;
  nt_wake_locked(a, o);
  nt_unlock(a);
  return 0;
}

uint32_t nt_wait_one(struct nt_arena *a, nt_handle h, uint64_t timeout_ns) {
  uint32_t ab = 0;
  uint32_t r = nt_wait_multi(a, &h, 1, timeout_ns, 0, &ab);
  if (r == NT_WAIT_OBJECT_0 && ab)
    return NT_WAIT_ABANDONED;
  return r;
}

uint32_t nt_wait_any(struct nt_arena *a, const nt_handle *h, uint32_t n,
                     uint64_t timeout_ns) {
  uint32_t ab = 0;
  return nt_wait_multi(a, h, n, timeout_ns, 0, &ab);
}

uint32_t nt_wait_all(struct nt_arena *a, const nt_handle *h, uint32_t n,
                     uint64_t timeout_ns) {
  uint32_t ab = 0;
  uint32_t r = nt_wait_multi(a, h, n, timeout_ns, 1, &ab);
  if (r == NT_WAIT_OBJECT_0 && ab)
    return NT_WAIT_ABANDONED;
  return r;
}

int nt_process_register(struct nt_arena *a, uint32_t pid) {
  if (!a || pid == 0)
    return -1;
  nt_lock(a);
  for (uint32_t i = 0; i < NT_MAX_PROCS; i++) {
    if (a->procs[i].alive && a->procs[i].pid == pid) {
      nt_unlock(a);
      return 0;
    }
  }
  for (uint32_t i = 0; i < NT_MAX_PROCS; i++) {
    if (!a->procs[i].alive) {
      a->procs[i].pid = pid;
      a->procs[i].generation++;
      a->procs[i].alive = 1;
      nt_unlock(a);
      return 0;
    }
  }
  nt_unlock(a);
  return -1;
}

void nt_process_exit(struct nt_arena *a, uint32_t pid) {
  if (!a || pid == 0)
    return;
  nt_lock(a);
  for (uint32_t i = 0; i < NT_MAX_PROCS; i++)
    if (a->procs[i].pid == pid)
      a->procs[i].alive = 0;

  for (uint32_t i = 0; i < NT_MAX_WAITERS; i++)
    if (a->waiters[i].state != 0 && a->waiters[i].pid == pid)
      nt_waiter_disarm(a, i);

  for (uint32_t i = 0; i < NT_MAX_OBJECTS; i++) {
    struct nt_object *o = &a->objects[i];
    if (o->refs == 0)
      continue;
    if (o->type == NT_MUTEX && o->owner_pid == pid) {
      o->owner_pid = 0;
      o->owner_tid = 0;
      o->recursion = 0;
      o->state = 1;
      o->abandoned = 1;
      nt_wake_locked(a, o);
    }
    for (uint32_t r = 0; r < NT_OBJ_REFS; r++) {
      if (o->ref_by[r].pid == pid && o->ref_by[r].count) {
        uint32_t n = o->ref_by[r].count;
        o->ref_by[r].pid = 0;
        o->ref_by[r].count = 0;
        o->refs = o->refs > n ? o->refs - n : 0;
      }
    }
    if (o->refs == 0)
      nt_free_locked(a, o);
  }
  nt_unlock(a);
}

uint32_t nt_spin_hint(struct nt_arena *a, nt_handle h) {
  if (!a)
    return 0;
  uint32_t slot;
  nt_lock(a);
  uint32_t v = nt_resolve(a, h, &slot) ? a->objects[slot].spin_ns : 0;
  nt_unlock(a);
  return v;
}

uint32_t nt_object_state(struct nt_arena *a, nt_handle h) {
  if (!a)
    return 0;
  uint32_t slot;
  nt_lock(a);
  uint32_t v = nt_resolve(a, h, &slot) ? a->objects[slot].state : 0;
  nt_unlock(a);
  return v;
}
