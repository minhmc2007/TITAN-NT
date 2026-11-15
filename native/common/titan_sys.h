#ifndef TITAN_SYS_H
#define TITAN_SYS_H

#include <stddef.h>
#include <stdint.h>

#define TITAN_SYS_read 63
#define TITAN_SYS_write 64
#define TITAN_SYS_ftruncate 46
#define TITAN_SYS_openat 56
#define TITAN_SYS_close 57
#define TITAN_SYS_getdents64 61
#define TITAN_SYS_readlinkat 78
#define TITAN_SYS_exit_group 94
#define TITAN_SYS_futex 98
#define TITAN_SYS_nanosleep 101
#define TITAN_SYS_setitimer 103
#define TITAN_SYS_clock_gettime 113
#define TITAN_SYS_sched_setaffinity 122
#define TITAN_SYS_tgkill 131
#define TITAN_SYS_socketpair 199
#define TITAN_SYS_getpid 172
#define TITAN_SYS_gettid 178
#define TITAN_SYS_munmap 215
#define TITAN_SYS_mmap 222
#define TITAN_SYS_mprotect 226
#define TITAN_SYS_clone 220
#define TITAN_SYS_execve 221
#define TITAN_SYS_wait4 260
#define TITAN_SYS_memfd_create 279
#define TITAN_SYS_futex_waitv 449

#define TITAN_FUTEX_WAIT 0
#define TITAN_FUTEX_WAKE 1
#define TITAN_FUTEX_WAIT_BITSET 9
#define TITAN_FUTEX_PRIVATE_FLAG 128
#define TITAN_FUTEX_BITSET_MATCH_ANY 0xffffffffu
#define TITAN_FUTEX_32 2

#define TITAN_AT_FDCWD (-100)
#define TITAN_O_RDONLY 0
#define TITAN_O_CLOEXEC 0x80000

#define TITAN_CLOCK_MONOTONIC 1

#define TITAN_PROT_NONE 0
#define TITAN_PROT_READ 1
#define TITAN_PROT_WRITE 2
#define TITAN_MAP_SHARED 1
#define TITAN_MAP_PRIVATE 2
#define TITAN_MAP_ANONYMOUS 0x20
#define TITAN_MAP_FIXED_NOREPLACE 0x100000

#define TITAN_SIGCHLD 17
#define TITAN_WNOHANG 1
#define TITAN_WIFEXITED(s) (((s) & 0x7f) == 0)
#define TITAN_WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define TITAN_WIFSIGNALED(s) ((((s) & 0x7f) + 1) >> 1 > 0)
#define TITAN_WTERMSIG(s) ((s) & 0x7f)

struct titan_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct titan_itimerval {
  struct titan_timespec it_interval;
  struct titan_timespec it_value;
};

struct titan_futex_waitv {
  uint64_t val;
  uint64_t uaddr;
  uint32_t flags;
  uint32_t reserved;
};

static inline long titan_syscall6(long nr, long a0, long a1, long a2, long a3,
                                  long a4, long a5) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x5 __asm__("x5") = a5;
  __asm__ volatile("svc #0"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
                     "+r"(x5)
                   : "r"(x8)
                   : "memory");
  return x0;
}

static inline long titan_write(int fd, const void *buf, size_t len) {
  return titan_syscall6(TITAN_SYS_write, fd, (long)(uintptr_t)buf, (long)len,
                        0, 0, 0);
}

static inline long titan_read(int fd, void *buf, size_t len) {
  return titan_syscall6(TITAN_SYS_read, fd, (long)(uintptr_t)buf, (long)len,
                        0, 0, 0);
}

static inline long titan_openat(int dirfd, const char *path, int flags) {
  return titan_syscall6(TITAN_SYS_openat, dirfd, (long)(uintptr_t)path, flags,
                        0, 0, 0);
}

static inline long titan_close(int fd) {
  return titan_syscall6(TITAN_SYS_close, fd, 0, 0, 0, 0, 0);
}

static inline long titan_ftruncate(int fd, long len) {
  return titan_syscall6(TITAN_SYS_ftruncate, fd, len, 0, 0, 0, 0);
}

static inline long titan_mmap(void *addr, size_t len, int prot, int flags,
                              int fd, long offset) {
  return titan_syscall6(TITAN_SYS_mmap, (long)(uintptr_t)addr, (long)len, prot,
                        flags, fd, offset);
}

static inline long titan_munmap(void *addr, size_t len) {
  return titan_syscall6(TITAN_SYS_munmap, (long)(uintptr_t)addr, (long)len, 0,
                        0, 0, 0);
}

static inline long titan_mprotect(void *addr, size_t len, int prot) {
  return titan_syscall6(TITAN_SYS_mprotect, (long)(uintptr_t)addr, (long)len,
                        prot, 0, 0, 0);
}

static inline long titan_getpid(void) {
  return titan_syscall6(TITAN_SYS_getpid, 0, 0, 0, 0, 0, 0);
}

static inline long titan_gettid(void) {
  return titan_syscall6(TITAN_SYS_gettid, 0, 0, 0, 0, 0, 0);
}

static inline long titan_tgkill(long tgid, long tid, int sig) {
  return titan_syscall6(TITAN_SYS_tgkill, tgid, tid, sig, 0, 0, 0);
}

static inline long titan_futex(uint32_t *uaddr, int op, uint32_t val,
                               const struct titan_timespec *ts,
                               uint32_t *uaddr2, uint32_t val3) {
  return titan_syscall6(TITAN_SYS_futex, (long)(uintptr_t)uaddr, op, val,
                        (long)(uintptr_t)ts, (long)(uintptr_t)uaddr2, val3);
}

static inline long titan_clock_gettime(int clock, struct titan_timespec *ts) {
  return titan_syscall6(TITAN_SYS_clock_gettime, clock, (long)(uintptr_t)ts,
                        0, 0, 0, 0);
}

static inline long titan_exit_group(int status) {
  return titan_syscall6(TITAN_SYS_exit_group, status, 0, 0, 0, 0, 0);
}

static inline long titan_nanosleep(const struct titan_timespec *req) {
  return titan_syscall6(TITAN_SYS_nanosleep, (long)(uintptr_t)req, 0, 0, 0, 0,
                        0);
}

static inline long titan_setitimer(int which,
                                   const struct titan_itimerval *value,
                                   struct titan_itimerval *old) {
  return titan_syscall6(TITAN_SYS_setitimer, which,
                        (long)(uintptr_t)value, (long)(uintptr_t)old, 0, 0, 0);
}

static inline long titan_memfd_create(const char *name, unsigned flags) {
  return titan_syscall6(TITAN_SYS_memfd_create, (long)(uintptr_t)name,
                        (long)flags, 0, 0, 0, 0);
}

static inline long titan_clone_fork(void) {
  return titan_syscall6(TITAN_SYS_clone, TITAN_SIGCHLD, 0, 0, 0, 0, 0);
}

static inline long titan_execve(const char *path, char *const *argv,
                                char *const *envp) {
  return titan_syscall6(TITAN_SYS_execve, (long)(uintptr_t)path,
                        (long)(uintptr_t)argv, (long)(uintptr_t)envp, 0, 0, 0);
}

static inline long titan_wait4(long pid, int *status, int options) {
  return titan_syscall6(TITAN_SYS_wait4, pid, (long)(uintptr_t)status, options,
                        0, 0, 0);
}

static inline long titan_readlinkat(int dirfd, const char *path, char *buf,
                                    size_t len) {
  return titan_syscall6(TITAN_SYS_readlinkat, dirfd, (long)(uintptr_t)path,
                        (long)(uintptr_t)buf, (long)len, 0, 0);
}

static inline long titan_getdents64(int fd, void *buf, size_t len) {
  return titan_syscall6(TITAN_SYS_getdents64, fd, (long)(uintptr_t)buf,
                        (long)len, 0, 0, 0);
}

static inline long titan_sched_setaffinity(long pid, size_t len,
                                           const void *mask) {
  return titan_syscall6(TITAN_SYS_sched_setaffinity, pid, (long)len,
                        (long)(uintptr_t)mask, 0, 0, 0);
}

static inline long titan_socketpair(int domain, int type, int protocol,
                                    int *sv) {
  return titan_syscall6(TITAN_SYS_socketpair, domain, type, protocol,
                        (long)(uintptr_t)sv, 0, 0);
}

#endif
