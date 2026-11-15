/* Known SCS words; llvm-objdump must decode these as x18 push/pop forms. */
.text
.globl scs_canary
scs_canary:
  .inst 0xf8008652
  .inst 0xf85f8e52
  .inst 0xf85f8652
  ret
