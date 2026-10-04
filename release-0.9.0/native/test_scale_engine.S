.text
.globl diag_test_engine
.globl diag_test_site
.p2align 4
diag_test_engine:
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r14
    pushq %r12
    subq $32,%rsp
    movq %rcx,%rbp
    movq %rdx,%r14
    movq %r8,%r12
    xorq %rsi,%rsi
    movdqu diag_test_vector(%rip),%xmm0
    movdqu diag_test_vector(%rip),%xmm1
    movdqu diag_test_vector(%rip),%xmm2
    movdqu diag_test_vector(%rip),%xmm3
    movdqu diag_test_vector(%rip),%xmm4
    movdqu diag_test_vector(%rip),%xmm5
    movq $0x111,%rax
    movq $0x222,%rcx
    movq $0x333,%rdx
    movq $0x444,%r8
    movq $0x555,%r9
    movq $0x666,%r10
    movq $0x777,%r11
    pushfq
    popq 64(%r12)
diag_test_site:
    leaq (%r14,%rsi,4),%rdi
    movss 8(%rdi),%xmm4
    movq %rax,0(%r12)
    movq %rcx,8(%r12)
    movq %rdx,16(%r12)
    movq %r8,24(%r12)
    movq %r9,32(%r12)
    movq %r10,40(%r12)
    movq %r11,48(%r12)
    pushfq
    popq 56(%r12)
    movdqu %xmm0,80(%r12)
    movdqu %xmm1,96(%r12)
    movdqu %xmm2,112(%r12)
    movdqu %xmm3,128(%r12)
    movdqu %xmm4,144(%r12)
    movdqu %xmm5,160(%r12)
    movq %rdi,176(%r12)
    addq $32,%rsp
    popq %r12
    popq %r14
    popq %rdi
    popq %rsi
    popq %rbp
    ret
.section .rdata,"dr"
.p2align 4
diag_test_vector:
    .quad 0x1122334455667788,0x99aabbccddeeff00
