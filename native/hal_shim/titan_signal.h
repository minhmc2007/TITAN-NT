#ifndef TITAN_SIGNAL_H
#define TITAN_SIGNAL_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int titan_signal_install(void);
int titan_signal_altstack(void *sp, size_t size);
extern void titan_sig_trampoline(int signo, siginfo_t *si, void *uc);

#ifdef __cplusplus
}
#endif

#endif
