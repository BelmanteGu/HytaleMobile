// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

    .text

// Entry for imports without a shim. x16 holds the import index; x0-x3 are the
// caller's first arguments.
    .globl macho_trap_entry
    .type macho_trap_entry, %function
macho_trap_entry:
    mov x4, x3
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x16
    b macho_unresolved_trap

// Thread-local variable thunk. ld64 code calls it through the descriptor with
// x0 = descriptor and expects every register except x0, x16 and x17 to survive.
    .globl macho_tlv_get_addr
    .type macho_tlv_get_addr, %function
macho_tlv_get_addr:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #(18 * 8 + 32 * 16)
    stp x1, x2, [sp, #0]
    stp x3, x4, [sp, #16]
    stp x5, x6, [sp, #32]
    stp x7, x8, [sp, #48]
    stp x9, x10, [sp, #64]
    stp x11, x12, [sp, #80]
    stp x13, x14, [sp, #96]
    stp x15, x18, [sp, #112]
    add x16, sp, #144
    stp q0, q1, [x16, #0]
    stp q2, q3, [x16, #32]
    stp q4, q5, [x16, #64]
    stp q6, q7, [x16, #96]
    stp q8, q9, [x16, #128]
    stp q10, q11, [x16, #160]
    stp q12, q13, [x16, #192]
    stp q14, q15, [x16, #224]
    stp q16, q17, [x16, #256]
    stp q18, q19, [x16, #288]
    stp q20, q21, [x16, #320]
    stp q22, q23, [x16, #352]
    stp q24, q25, [x16, #384]
    stp q26, q27, [x16, #416]
    stp q28, q29, [x16, #448]
    stp q30, q31, [x16, #480]
    bl macho_tlv_lookup
    add x16, sp, #144
    ldp q0, q1, [x16, #0]
    ldp q2, q3, [x16, #32]
    ldp q4, q5, [x16, #64]
    ldp q6, q7, [x16, #96]
    ldp q8, q9, [x16, #128]
    ldp q10, q11, [x16, #160]
    ldp q12, q13, [x16, #192]
    ldp q14, q15, [x16, #224]
    ldp q16, q17, [x16, #256]
    ldp q18, q19, [x16, #288]
    ldp q20, q21, [x16, #320]
    ldp q22, q23, [x16, #352]
    ldp q24, q25, [x16, #384]
    ldp q26, q27, [x16, #416]
    ldp q28, q29, [x16, #448]
    ldp q30, q31, [x16, #480]
    ldp x1, x2, [sp, #0]
    ldp x3, x4, [sp, #16]
    ldp x5, x6, [sp, #32]
    ldp x7, x8, [sp, #48]
    ldp x9, x10, [sp, #64]
    ldp x11, x12, [sp, #80]
    ldp x13, x14, [sp, #96]
    ldp x15, x18, [sp, #112]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret

// Darwin arm64 stack probe: x9 = bytes about to be allocated below sp (ld64 emits
// "mov x9, #size; bl ___chkstk_darwin; sub sp, sp, ..."). Touches each page so the
// guard page is hit in order. x9 is preserved; only x16 and x17 are clobbered.
    .globl macho_chkstk_darwin
    .type macho_chkstk_darwin, %function
macho_chkstk_darwin:
    mov x16, sp
    mov x17, x9
1:
    cmp x17, #4096
    b.lo 2f
    sub x16, x16, #4096
    ldr xzr, [x16]
    sub x17, x17, #4096
    b 1b
2:
    sub x16, x16, x17
    ldr xzr, [x16]
    ret

// Variadic Darwin functions pass every variadic argument on the stack, while
// AAPCS64 passes them in registers first. These entry points hand the caller's
// stack pointer (the first variadic slot) to a C implementation.
.macro VARIADIC name, fixed, target
    .globl \name
    .type \name, %function
\name:
    mov x\fixed, sp
    b \target
.endm

    VARIADIC apple_printf, 1, shim_printf_stack
    VARIADIC apple_fprintf, 2, shim_fprintf_stack
    VARIADIC apple_snprintf, 3, shim_snprintf_stack
    VARIADIC apple_asprintf, 2, shim_asprintf_stack
    VARIADIC apple_sscanf, 2, shim_sscanf_stack
    VARIADIC apple_syslog, 2, shim_syslog_stack
    VARIADIC apple_open, 2, shim_open_stack
    VARIADIC apple_fcntl, 2, shim_fcntl_stack
    VARIADIC apple_ioctl, 2, shim_ioctl_stack

// Darwin packs stack arguments at their natural size, while AAPCS64 gives each
// one an 8-byte slot. These entry points take eight register arguments and pass
// the caller's stack argument area to C as a ninth argument, which reads the
// remaining values at their Darwin offsets (see gl.c).
.macro STACK_ARGS name, target
    .globl \name
    .type \name, %function
\name:
    sub sp, sp, #32
    stp x29, x30, [sp, #16]
    add x29, sp, #16
    add x16, sp, #32
    str x16, [sp]
    bl \target
    ldp x29, x30, [sp, #16]
    add sp, sp, #32
    ret
.endm

    STACK_ARGS apple_glTexSubImage3D, gl_tex_sub_image_3d_stack
    STACK_ARGS apple_glCompressedTexSubImage3D, gl_compressed_tex_sub_image_3d_stack
    STACK_ARGS apple_glClearTexSubImage, gl_clear_tex_sub_image_stack
    STACK_ARGS apple_glBlitFramebuffer, gl_blit_framebuffer_stack
    STACK_ARGS apple_glCopyImageSubData, gl_copy_image_sub_data_stack
    STACK_ARGS apple_CCCryptorCreateWithMode, cc_cryptor_create_with_mode_stack

// Tracing thunk target: x16 = import index. Logs the call, then tail-jumps to
// the real target returned by macho_trace_call with the caller's registers and
// stack pointer intact (variadic shims read the caller's stack).
    .globl macho_trace_entry
    .type macho_trace_entry, %function
macho_trace_entry:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #(10 * 8 + 8 * 16)
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x15, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    mov x1, x0
    mov x2, x1
    ldr x1, [sp, #0]
    ldr x2, [sp, #8]
    ldr x3, [sp, #16]
    mov x0, x16
    bl macho_trace_call
    mov x16, x0
    ldp q6, q7, [sp, #176]
    ldp q4, q5, [sp, #144]
    ldp q2, q3, [sp, #112]
    ldp q0, q1, [sp, #80]
    ldp x8, x15, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    mov sp, x29
    ldp x29, x30, [sp], #16
    br x16

// objc_msgSend(self, _cmd, ...): resolve the method and tail-call it with every
// argument register and the caller's stack untouched.
    .globl macho_objc_msgSend
    .type macho_objc_msgSend, %function
macho_objc_msgSend:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #(10 * 8 + 8 * 16)
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    str x8, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    bl macho_objc_lookup
    mov x16, x0
    ldp q6, q7, [sp, #176]
    ldp q4, q5, [sp, #144]
    ldp q2, q3, [sp, #112]
    ldp q0, q1, [sp, #80]
    ldr x8, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    mov sp, x29
    ldp x29, x30, [sp], #16
    br x16

// Result of a message to nil or to an unknown method: integer and floating
// point return registers are zero.
    .globl macho_objc_nil
    .type macho_objc_nil, %function
macho_objc_nil:
    mov x0, #0
    mov x1, #0
    movi d0, #0
    movi d1, #0
    movi d2, #0
    movi d3, #0
    ret

// Darwin's jmp_buf is 192 bytes (int[48]) and Bionic's is larger, so setjmp and
// longjmp keep their own layout inside it: x19-x30, sp and d8-d15 (168 bytes). The
// signal mask is not saved; libpng, the only user, does not need it.
    .globl apple_setjmp
    .type apple_setjmp, %function
apple_setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x16, sp
    str x16, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret

    .globl apple_longjmp
    .type apple_longjmp, %function
apple_longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x16, [x0, #96]
    mov sp, x16
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    cmp w1, #0
    csinc w0, w1, wzr, ne // setjmp returns val, or 1 when val is 0
    ret
