// x86-64 System V context switch for ember fibers.
// Context layout, must match FiberContext in fiber_posix.cpp:
//   0 rsp   8 rip   16 rbx   24 rbp   32 r12   40 r13   48 r14   56 r15
//   64 mxcsr (32 bit)   68 x87 control word (16 bit)

#if defined(__APPLE__)
	#define SYM(name) _##name
#else
	#define SYM(name) name
#endif

	.text

// void ember_fiber_switch(FiberContext* from /* rdi */, FiberContext* to /* rsi */)
	.globl SYM(ember_fiber_switch)
#if !defined(__APPLE__)
	.type SYM(ember_fiber_switch), @function
#endif
	.p2align 4
SYM(ember_fiber_switch):
	.cfi_startproc
	movq    (%rsp), %rax          // return address of this call
	movq    %rax, 8(%rdi)         // resume here
	leaq    8(%rsp), %rax         // stack pointer as it will be after the return
	movq    %rax, 0(%rdi)
	movq    %rbx, 16(%rdi)
	movq    %rbp, 24(%rdi)
	movq    %r12, 32(%rdi)
	movq    %r13, 40(%rdi)
	movq    %r14, 48(%rdi)
	movq    %r15, 56(%rdi)
	stmxcsr 64(%rdi)
	fnstcw  68(%rdi)

	ldmxcsr 64(%rsi)
	fldcw   68(%rsi)
	movq    16(%rsi), %rbx
	movq    24(%rsi), %rbp
	movq    32(%rsi), %r12
	movq    40(%rsi), %r13
	movq    48(%rsi), %r14
	movq    56(%rsi), %r15
	movq    0(%rsi), %rsp
	jmp     *8(%rsi)
	.cfi_endproc
#if !defined(__APPLE__)
	.size SYM(ember_fiber_switch), .-SYM(ember_fiber_switch)
#endif

// First code a fresh fiber runs. Entered by jmp with rsp 16 byte aligned,
// r12 = entry argument, r13 = entry function. The entry never returns.
	.globl SYM(ember_fiber_trampoline)
#if !defined(__APPLE__)
	.type SYM(ember_fiber_trampoline), @function
#endif
	.p2align 4
SYM(ember_fiber_trampoline):
	.cfi_startproc
	.cfi_undefined rip
	movq    %r12, %rdi
	callq   *%r13
	ud2
	.cfi_endproc
#if !defined(__APPLE__)
	.size SYM(ember_fiber_trampoline), .-SYM(ember_fiber_trampoline)
#endif

#if defined(__linux__)
	.section .note.GNU-stack,"",@progbits
#endif
