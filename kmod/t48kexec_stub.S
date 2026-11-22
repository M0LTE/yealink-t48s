/*
 * Position-independent stub for t48kexec. Copied to the start of a block that is
 * mapped 1:1, and entered at its physical address with interrupts off and
 * caches clean. Parameters live STUB_CODE_MAX (1024) bytes after the start:
 *   +0 entry, +4 machine number, +8 watchdog load (0 = off), +12 npages,
 *   +16 pairs of (dest, src) physical page addresses.
 */
	.text
	.align	5
	.globl	t48_stub_start
	.globl	t48_stub_end
t48_stub_start:
	/* where are we: r12 = stub start (we run at its physical address) */
	sub	r12, pc, #8

	/* MMU, D cache and I cache off; we keep running at the same address (1:1) */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #0x0005		/* M, C */
	bic	r0, r0, #0x1000		/* I */
	mcr	p15, 0, r0, c1, c0, 0
	nop
	nop
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0	/* invalidate I cache */
	mcr	p15, 0, r0, c8, c7, 0	/* invalidate TLBs */
	mcr	p15, 0, r0, c7, c10, 4	/* drain write buffer */

	add	r11, r12, #1024		/* parameters */

	/* stop every channel on both DMA controllers: Yealink's audio (DMA0) and
	 * Ethernet (DMA1) rings keep writing into RAM the new kernel will own.
	 * ChEnReg takes write-enable bits 8-15 alongside the enable bits 0-7. */
	ldr	r0, =0x302003a0		/* DMA0 ChEnReg */
	ldr	r1, =0x303003a0		/* DMA1 ChEnReg */
	mov	r2, #0xff00
	str	r2, [r0]
	str	r2, [r1]
	ldr	r3, =1000000
5:	ldr	r2, [r0]
	ldr	r4, [r1]
	orr	r2, r2, r4
	ands	r2, r2, #0xff
	beq	6f
	subs	r3, r3, #1
	bne	5b
6:

	/* copy the pages */
	ldr	r10, [r11, #12]		/* npages */
	add	r9, r11, #16		/* pairs */
1:	cmp	r10, #0
	beq	3f
	ldr	r0, [r9], #4		/* dest */
	ldr	r1, [r9], #4		/* src */
	mov	r2, #4096
2:	ldmia	r1!, {r3-r6}		/* 16 bytes at a time, 256 per page */
	stmia	r0!, {r3-r6}
	subs	r2, r2, #16
	bne	2b
	sub	r10, r10, #1
	b	1b

	/* arm the SP805 watchdog: unlock, load, enable interrupt+reset */
3:	ldr	r0, [r11, #8]
	cmp	r0, #0
	beq	4f
	ldr	r1, =0x80017000
	ldr	r2, =0x1ACCE551
	str	r2, [r1, #0xc00]	/* WdogLock: unlock */
	str	r0, [r1, #0x800]	/* WdogLoad */
	mov	r2, #3
	str	r2, [r1, #0x808]	/* WdogControl: INTEN | RESEN */
	mov	r2, #0
	str	r2, [r1, #0xc00]	/* lock again */

	/* enter the new kernel: r0 = 0, r1 = machine, r2 = 0 (no tags) */
4:	ldr	r3, [r11, #0]
	ldr	r1, [r11, #4]
	mov	r0, #0
	mov	r2, #0
	mov	pc, r3
	.ltorg
t48_stub_end:
