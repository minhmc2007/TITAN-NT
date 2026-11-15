#include "titan_signal.h"

#include <signal.h>
#include <ucontext.h>

#include "../common/titan_log.h"
#include "../common/titan_sys.h"
#include "x18_guard.h"

static void titan_zero(void *p, size_t n) {
  char *c = (char *)p;
  for (size_t i = 0; i < n; i++)
    c[i] = 0;
}

void titan_sig_dispatch(int signo, siginfo_t *si, void *ucv,
                        uint64_t interrupted_x18);

int titan_signal_altstack(void *sp, size_t size) {
  stack_t ss;
  titan_zero(&ss, sizeof(ss));
  ss.ss_sp = sp;
  ss.ss_size = size;
  ss.ss_flags = 0;
  long r = titan_guard_call((void *)sigaltstack, (long)(uintptr_t)&ss, 0, 0, 0,
                            0, 0);
  return r == 0 ? 0 : -1;
}

int titan_signal_install(void) {
  static const int sigs[] = {SIGSEGV, SIGILL, SIGPROF, SIGUSR1};
  struct sigaction sa;

  titan_zero(&sa, sizeof(sa));
  sa.sa_sigaction = titan_sig_trampoline;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;

  for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
    long r = titan_guard_call((void *)sigaction, sigs[i],
                              (long)(uintptr_t)&sa, 0, 0, 0, 0);
    if (r != 0)
      return -1;
  }
  return 0;
}

void titan_sig_dispatch(int signo, siginfo_t *si, void *ucv,
                        uint64_t interrupted_x18) {
  struct titan_teb *teb = titan_teb_current();
  void *scs = titan_scs_current();
  uint64_t sp = (uint64_t)(uintptr_t)__builtin_frame_address(0);

  if (!teb || teb->magic != TITAN_TEB_MAGIC)
    titan_fatal("signal without teb");

  if (interrupted_x18 == (uint64_t)(uintptr_t)teb)
    __atomic_add_fetch(&teb->sig_teb, 1, __ATOMIC_RELAXED);
  else if (interrupted_x18 == (uint64_t)(uintptr_t)scs)
    __atomic_add_fetch(&teb->sig_scs, 1, __ATOMIC_RELAXED);
  else
    __atomic_add_fetch(&teb->sig_bad, 1, __ATOMIC_RELAXED);

  if (sp < teb->altstack_lo || sp >= teb->altstack_hi)
    __atomic_add_fetch(&teb->sig_bad, 1, __ATOMIC_RELAXED);

  if (signo == SIGSEGV) {
    uint64_t addr = (uint64_t)(uintptr_t)si->si_addr;
    if (teb->fault_lo && addr >= teb->fault_lo && addr < teb->fault_hi) {
      long r = titan_mprotect((void *)(uintptr_t)teb->fault_lo,
                              (size_t)(teb->fault_hi - teb->fault_lo),
                              TITAN_PROT_READ | TITAN_PROT_WRITE);
      if (r != 0)
        titan_fatal("handler mprotect failed");
      __atomic_add_fetch(&teb->faults, 1, __ATOMIC_RELAXED);
      return;
    }
    titan_fatal("unexpected SIGSEGV");
  }

  if (signo == SIGILL) {
    uint64_t pc = (uint64_t)(uintptr_t)si->si_addr;
    ucontext_t *uc = (ucontext_t *)ucv;
    if (teb->ill_lo && pc >= teb->ill_lo && pc < teb->ill_hi &&
        uc->uc_mcontext.pc == pc) {
      uc->uc_mcontext.pc = pc + 4;
      __atomic_add_fetch(&teb->ill, 1, __ATOMIC_RELAXED);
      return;
    }
    titan_fatal("unexpected SIGILL");
  }
}
