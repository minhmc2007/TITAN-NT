/* Known SCS prologue/epilogue words; llvm-objdump must decode them as x18 forms. */
.text
.globl scs_canary
scs_canary:
  .inst 0xf8008e52
  .inst 0xf81f8e52
  ret
