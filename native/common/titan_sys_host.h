#ifndef TITAN_SYS_HOST_H
#define TITAN_SYS_HOST_H

/* Host build of the same syscall surface. CI runs the sync-layer logic against
   the runner kernel so device time is spent on device-only behaviour. */

#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

static inline long titan_syscall6(long nr, long a0, long a1, long a2, long a3,
                                  long a4, long a5) {
  return syscall(nr, a0, a1, a2, a3, a4, a5);
}

static inline long titan_write(int fd, const void *buf, size_t len) {
  return syscall(SYS_write, fd, buf, len);
}

static inline long titan_read(int fd, void *buf, size_t len) {
  return syscall(SYS_read, fd, buf, len);
}

static inline long titan_openat(int dirfd, const char *path, int flags) {
  return syscall(SYS_openat, dirfd, path, flags, 0);
}

static inline long titan_close(int fd) {
  return syscall(SYS_close, fd);
}

static inline long titan_ftruncate(int fd, long len) {
  return syscall(SYS_ftruncate, fd, len);
}

static inline long titan_mmap(void *addr, size_t len, int prot, int flags,
                              int fd, long offset) {
  return syscall(SYS_mmap, addr, len, prot, flags, fd, offset);
}

static inline long titan_munmap(void *addr, size_t len) {
  return syscall(SYS_munmap, addr, len);
}

static inline long titan_mprotect(void *addr, size_t len, int prot) {
  return syscall(SYS_mprotect, addr, len, prot);
}

static inline long titan_getpid(void) {
  return syscall(SYS_getpid);
}

static inline long titan_gettid(void) {
  return syscall(SYS_gettid);
}

static inline long titan_tgkill(long tgid, long tid, int sig) {
  return syscall(SYS_tgkill, tgid, tid, sig);
}

static inline long titan_futex(uint32_t *uaddr, int op, uint32_t val,
                               const struct titan_timespec *ts,
                               uint32_t *uaddr2, uint32_t val3) {
  return syscall(SYS_futex, uaddr, op, val, ts, uaddr2, val3);
}

static inline long titan_clock_gettime(int clock, struct titan_timespec *ts) {
  return syscall(SYS_clock_gettime, clock, ts);
}

static inline long titan_exit_group(int status) {
  return syscall(SYS_exit_group, status);
}

static inline long titan_nanosleep(const struct titan_timespec *req) {
  return syscall(SYS_nanosleep, req, 0);
}

static inline long titan_setitimer(int which,
                                   const struct titan_itimerval *value,
                                   struct titan_itimerval *old) {
  return syscall(SYS_setitimer, which, value, old);
}

static inline long titan_memfd_create(const char *name, unsigned flags) {
  return syscall(SYS_memfd_create, name, flags);
}

static inline long titan_clone_fork(void) {
  return fork();
}

static inline long titan_execve(const char *path, char *const *argv,
                                char *const *envp) {
  return syscall(SYS_execve, path, argv, envp);
}

static inline long titan_wait4(long pid, int *status, int options) {
  return syscall(SYS_wait4, pid, status, options, 0);
}

static inline long titan_readlinkat(int dirfd, const char *path, char *buf,
                                    size_t len) {
  return syscall(SYS_readlinkat, dirfd, path, buf, len);
}

static inline long titan_getdents64(int fd, void *buf, size_t len) {
  return syscall(SYS_getdents64, fd, buf, len);
}

static inline long titan_sched_setaffinity(long pid, size_t len,
                                           const void *mask) {
  return syscall(SYS_sched_setaffinity, pid, len, mask);
}

static inline long titan_sched_getaffinity(long pid, size_t len, void *mask) {
  return syscall(SYS_sched_getaffinity, pid, len, mask);
}

static inline long titan_socketpair(int domain, int type, int protocol,
                                    int *sv) {
  return syscall(SYS_socketpair, domain, type, protocol, sv);
}

#endif
