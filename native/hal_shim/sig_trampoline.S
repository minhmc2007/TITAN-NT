.text

.globl titan_sig_trampoline
.type titan_sig_trampoline,%function
titan_sig_trampoline:
  stp x18, x30, [sp, #-16]!
  mov x3, x18
  mrs x10, tpidr_el0
  add x10, x10, #:tprel_hi12:titan_scs_ptr
  add x10, x10, #:tprel_lo12_nc:titan_scs_ptr
  ldr x9, [x10]
  mov x18, x9
  bl titan_sig_dispatch
  ldp x18, x30, [sp], #16
  ret
.size titan_sig_trampoline, .-titan_sig_trampoline
