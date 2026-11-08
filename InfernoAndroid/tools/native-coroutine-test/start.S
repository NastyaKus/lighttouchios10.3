/* SPDX-License-Identifier: GPL-3.0-or-later */
.text
.global _start
_start:
    ldr x0, =main_stack_top
    mov sp, x0
    // Enable floating point; enable pointer authentication when implemented.
    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb
    mrs x0, id_aa64isar1_el1
    ubfx x0, x0, #4, #8
    cbz x0, 1f
    msr APIAKeyLo_EL1, xzr
    msr APIAKeyHi_EL1, xzr
    mrs x0, sctlr_el1
    orr x0, x0, #(1 << 31)
    msr sctlr_el1, x0
    isb
1:  bl test_main
    b .
.global enter_new_stack
.type enter_new_stack, %function
enter_new_stack:
    mov sp, x0
    br x1

// ABI probe: load values in every callee-saved integer/FP register, save,
// clobber them, jump back, compare the actual register contents. x18 must NOT
// be restored, because Android uses it for the current thread's shadow stack.
.global test_registers
.type test_registers, %function
test_registers:
    sub sp, sp, #176
    stp x18, x19, [sp, #0]
    stp x20, x21, [sp, #16]
    stp x22, x23, [sp, #32]
    stp x24, x25, [sp, #48]
    stp x26, x27, [sp, #64]
    stp x28, x29, [sp, #80]
    str x30, [sp, #96]
    stp d8, d9, [sp, #104]
    stp d10, d11, [sp, #120]
    stp d12, d13, [sp, #136]
    stp d14, d15, [sp, #152]
    .irp reg,18,19,20,21,22,23,24,25,26,27,28,29
        mov x\reg, #\reg
    .endr
    .irp reg,8,9,10,11,12,13,14,15
        mov x2, #\reg
        fmov d\reg, x2
    .endr
    ldr x0, =abi_env
    bl qemu_android_fiber_setjmp
    cbnz w0, 2f
    .irp reg,19,20,21,22,23,24,25,26,27,28,29
        mov x\reg, #0
    .endr
    .irp reg,8,9,10,11,12,13,14,15
        fmov d\reg, xzr
    .endr
    mov x18, #99
    ldr x0, =abi_env
    mov w1, #3
    bl qemu_android_fiber_longjmp
2:  cmp w0, #3
    b.ne 8f
    cmp x18, #99
    b.ne 8f
    .irp reg,19,20,21,22,23,24,25,26,27,28,29
        cmp x\reg, #\reg
        b.ne 8f
    .endr
    .irp reg,8,9,10,11,12,13,14,15
        fmov x2, d\reg
        cmp x2, #\reg
        b.ne 8f
    .endr
    mov w0, #0
    b 9f
8:  mov w0, #1
9:  ldp x18, x19, [sp, #0]
    ldp x20, x21, [sp, #16]
    ldp x22, x23, [sp, #32]
    ldp x24, x25, [sp, #48]
    ldp x26, x27, [sp, #64]
    ldp x28, x29, [sp, #80]
    ldr x30, [sp, #96]
    ldp d8, d9, [sp, #104]
    ldp d10, d11, [sp, #120]
    ldp d12, d13, [sp, #136]
    ldp d14, d15, [sp, #152]
    add sp, sp, #176
    ret
.bss
.balign 16
main_stack:
    .skip 65536
main_stack_top:
