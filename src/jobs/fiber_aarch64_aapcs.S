// AArch64 AAPCS64 context switch for ember fibers.
// Context layout, must match FiberContext in fiber_posix.cpp:
//   0 sp   8 lr   16 x19 ... 88 x28   96 x29   104 d8 ... 160 d15   168 fpcr

#if defined(__aarch64__)

#if defined(__APPLE__)
	#define SYM(name) _##name
#else
	#define SYM(name) name
#endif

	.text

// void ember_fiber_switch(FiberContext* from /* x0 */, FiberContext* to /* x1 */)
	.globl SYM(ember_fiber_switch)
#if !defined(__APPLE__)
	.type SYM(ember_fiber_switch), %function
#endif
	.p2align 2
SYM(ember_fiber_switch):
	.cfi_startproc
	mov     x9, sp
	str     x9, [x0, #0]
	str     x30, [x0, #8]             // resume here
	stp     x19, x20, [x0, #16]
	stp     x21, x22, [x0, #32]
	stp     x23, x24, [x0, #48]
	stp     x25, x26, [x0, #64]
	stp     x27, x28, [x0, #80]
	str     x29, [x0, #96]
	stp     d8,  d9,  [x0, #104]
	stp     d10, d11, [x0, #120]
	stp     d12, d13, [x0, #136]
	stp     d14, d15, [x0, #152]
	mrs     x9, fpcr
	str     x9, [x0, #168]

	ldr     x9, [x1, #168]
	msr     fpcr, x9
	ldp     d8,  d9,  [x1, #104]
	ldp     d10, d11, [x1, #120]
	ldp     d12, d13, [x1, #136]
	ldp     d14, d15, [x1, #152]
	ldr     x29, [x1, #96]
	ldp     x27, x28, [x1, #80]
	ldp     x25, x26, [x1, #64]
	ldp     x23, x24, [x1, #48]
	ldp     x21, x22, [x1, #32]
	ldp     x19, x20, [x1, #16]
	ldr     x30, [x1, #8]
	ldr     x9, [x1, #0]
	mov     sp, x9
	ret
	.cfi_endproc
#if !defined(__APPLE__)
	.size SYM(ember_fiber_switch), .-SYM(ember_fiber_switch)
#endif

// First code a fresh fiber runs. Entered by ret with sp 16 byte aligned,
// x19 = entry argument, x20 = entry function. The entry never returns.
	.globl SYM(ember_fiber_trampoline)
#if !defined(__APPLE__)
	.type SYM(ember_fiber_trampoline), %function
#endif
	.p2align 2
SYM(ember_fiber_trampoline):
	.cfi_startproc
	.cfi_undefined x30
	mov     x0, x19
	blr     x20
	brk     #0
	.cfi_endproc
#if !defined(__APPLE__)
	.size SYM(ember_fiber_trampoline), .-SYM(ember_fiber_trampoline)
#endif

#endif // __aarch64__

#if defined(__linux__)
	.section .note.GNU-stack,"",%progbits
#endif
