.text
.globl scale_diag_hook
.p2align 4
scale_diag_hook:
    # Entered by a jump at the located hook point (0x2d9c683 in 4.1.1.7398727),
    # where engine RSP is 16-byte aligned.
    # Preserve all volatile integer/SIMD state and flags; C preserves nonvolatiles.
    pushfq
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    subq $144, %rsp
    movdqu %xmm0, 32(%rsp)
    movdqu %xmm1, 48(%rsp)
    movdqu %xmm2, 64(%rsp)
    movdqu %xmm3, 80(%rsp)
    movdqu %xmm4, 96(%rsp)
    movdqu %xmm5, 112(%rsp)
    stmxcsr 128(%rsp)
    leaq (%r14,%rsi,4), %rcx
    movq %rbp, %rdx
    call scale_diag_capture
    ldmxcsr 128(%rsp)
    movdqu 32(%rsp), %xmm0
    movdqu 48(%rsp), %xmm1
    movdqu 64(%rsp), %xmm2
    movdqu 80(%rsp), %xmm3
    movdqu 96(%rsp), %xmm4
    movdqu 112(%rsp), %xmm5
    addq $144, %rsp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdx
    popq %rcx
    popq %rax
    popfq
    # MinHook replays LEA RDI,[R14+RSI*4]; MOVSS XMM4,[RDI+8] then resumes.
    jmp *scale_diag_trampoline(%rip)
