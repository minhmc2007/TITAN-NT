.text

.globl titan_x18_call_raw
.type titan_x18_call_raw,%function
titan_x18_call_raw:
  stp x29, x30, [sp, #-32]!
  mov x29, sp
  stp x19, x20, [sp, #16]
  mov x19, x18
  mrs x10, tpidr_el0
  add x10, x10, #:tprel_hi12:titan_scs_ptr
  add x10, x10, #:tprel_lo12_nc:titan_scs_ptr
  ldr x20, [x10]
  mov x18, x20
  mov x9, x0
  mov x0, x1
  mov x1, x2
  mov x2, x3
  mov x3, x4
  mov x4, x5
  mov x5, x6
  blr x9
  mov x9, x0
  mov x18, x19
  ldp x19, x20, [sp, #16]
  ldp x29, x30, [sp], #32
  mov x0, x9
  ret
.size titan_x18_call_raw, .-titan_x18_call_raw

.globl titan_udf_trap
.type titan_udf_trap,%function
titan_udf_trap:
  .inst 0x00000000
  ret
.size titan_udf_trap, .-titan_udf_trap
