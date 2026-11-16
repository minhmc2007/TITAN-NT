/* Prints the syscall number behind a seccomp SIGSYS and exits, so a trap inside
 * a process we cannot rebuild can still be identified. Loaded with LD_PRELOAD
 * into Wine's loader. */
#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

static char *put_uint(char *p, unsigned v)
{
    char tmp[12];
    int n = 0;
    do tmp[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n) *p++ = tmp[--n];
    return p;
}

static void on_sigsys(int sig, siginfo_t *si, void *ctx)
{
    char buf[96], *p = buf;
    (void)sig;
    (void)ctx;
    p = put_str(p, "TITAN_SIGSYS syscall=");
    p = put_uint(p, (unsigned)si->si_syscall);
    p = put_str(p, " pid=");
    p = put_uint(p, (unsigned)getpid());
    *p++ = '\n';
    write(2, buf, (size_t)(p - buf));
    _exit(93);
}

__attribute__((constructor)) static void install_handler(void)
{
    struct sigaction sa;
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, 0);
}
