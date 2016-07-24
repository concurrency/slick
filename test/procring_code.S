/*
 *	test stuff for x86-64 scheduler -- process ring
 */

/*
 *	NOTE: when calling os_... as a C function, the only thing we
 *	expect to be preserved is %rbp (Wptr)
 */

.text

.globl	o_procring_shutdown
.type	o_procring_shutdown, @function

o_procring_shutdown:
	movq	%rbp, %rdi
	call	os_shutdown
	ret


.globl	o_procring_startup
.type	o_procring_startup, @function

o_procring_startup:
	leaq	o_procring_shutdown(%rip), %rax
	movq	%rax, 0(%rbp)			/* save return-address */
	jmp	o_procring

#define NPROCS 128

#define WSADJ	(64+((NPROCS+1) * 8))
#define OFFS_A	(56+((NPROCS+1) * 8))
#define OFFS_B	(48+((NPROCS+1) * 8))
#define OFFS_D	(40+((NPROCS+1) * 8))

/*
 *	procring workspace:
 *
 *	[no params]
 *	+64+X	return-addr		<-- call entry Wptr
 *	+56+X	channel a		// local var start
 *	+48+X	channel b
 *	+40+X	channel d
 *	+40	channel c[0..NPROCS]	// size == ((NPROCS+1) * sizeof(channel))
 *	+32	REPL-i
 *	+24	REPL-count
 *	+16	PAR-savedpri
 *	+8	PAR-count
 *	0	PAR-iptrsucc/joinlab	// running Wptr
 *	-8	[iptr]
 *	-16	[link]
 *	-24	[priof]
 *	-32	[ptr]
 *
 *	size = 96+X + 8 = 104+((NPROCS+1) * 8)
 *
 *	<<consume WS>>
 *	<<prefix WS>>
 *	<<delta WS>>
 *	<<succ WS>>
 *	[NPROCS * <<id WS>>]
 */

.section rodata
.align 8
.globl	ow_procring
ow_procring:	.quad	-1
.text
.globl	o_procring
.type	o_procring, @function

o_procring:
	subq	$WSADJ, %rbp


.L12:					/* join lab here */
	addq	$WSADJ, %rbp
	movq	0(%rbp), %r11
	jmp	*%r11
	

/*{{{  o_id*/
/*
 *	id workspace:
 *
 *	+24	param: out
 *	+16	param: in
 *	+8	return-addr			<-- entry Wptr
 *	0	[temp]		// running Wptr
 *	-8	[iptr]
 *	-16	[link]
 *	-24	[priof]
 *	-32	[ptr]
 *
 *	size = 40 + 24 == 64
 */

.section .rodata
.align 8
.globl ow_id
ow_id:	.quad	72				/* bytes of workspace */
.text
.globl	o_id
.type	o_id, @function

o_id:
	subq	$8, %rbp

.L11:
	movq	%rbp, %rdi
	movq	16(%rbp), %rsi			/* param: in */
	leaq	0(%rbp), %rdx
	movl	$8, %ecx
	call	os_chanin

	movq	%rbp, %rdi
	movq	24(%rbp), %rsi			/* param: out */
	leaq	0(%rbp), %rdx
	movl	$8, %ecx
	call	os_chanout

	jmp	.L11

	addq	$8, %rbp
	movq	0(%rbp), %r11
	jmp	*%r11

/*}}}*/
/*{{{  o_prefix*/
/*
 *	prefix workspace:
 *
 *	+32	param: out
 *	+24	param: in
 *	+16	return-addr			<-- entry Wptr
 *	+8	int64 x
 *	0	[temp]		// running Wptr
 *	-8	[iptr]
 *	-16	[link]
 *	-24	[priof]
 *	-32	[ptr]
 *
 *	size = 48 + 24 == 72
 */

.section .rodata
.align 8
.globl	ow_prefix
ow_prefix: .quad	72			/* bytes of workspace */
.text
.globl	o_prefix
.type	o_prefix, @function

o_prefix:
	subq	$16, %rbp

	movq	$0, 8(%rbp)			/* initialise x = 0 */

.L1:
	movq	%rbp, %rdi
	movq	32(%rbp), %rsi
	leaq	8(%rbp), %rdx
	movl	$8, %ecx
	call	os_chanout

	movq	%rbp, %rdi
	movq	24(%rbp), %rsi
	leaq	8(%rbp), %rdx
	movl	$8, %ecx
	call	os_chanin
	
	jmp	.L1				/* loop forever */

	addq	$16, %rbp
	movq	0(%rbp), %r11
	jmp	*%r11
/*}}}*/
/*{{{  o_succ*/
/*
 *	succ workspace:
 *
 *	+32	param: out
 *	+24	param: in
 *	+16	return-addr			<-- entry Wptr
 *	+8	int64 v
 *	0	[temp]		// running Wptr
 *	-8	[iptr]
 *	-16	[link]
 *	-24	[priof]
 *	-32	[ptr]
 *
 *	size = 48 + 24 == 72
 */

.section .rodata
.align 8
.globl	ow_succ
ow_succ: .quad	72
.text
.globl	o_succ
.type	o_succ, @function

o_succ:
	subq	$16, %rbp

.L2:
	movq	%rbp, %rdi
	movq	24(%rbp), %rsi			/* param: in */
	leaq	8(%rbp), %rdx			/* int64 v */
	movl	$8, %ecx
	call	os_chanin

	movq	8(%rbp), %rax
	incq	%rax
	movq	%rax, 8(%rbp)

	movq	%rbp, %rdi
	movq	32(%rbp), %rsi			/* param: out */
	leaq	8(%rbp), %rdx			/* int64 v */
	movl	$8, %ecx
	call	os_chanout

	jmp	.L2				/* loop forever */

	addq	$16, %rbp
	movq	0(%rbp), %r11
	jmp	*%r11
/*}}}*/
/*{{{  o_delta*/
/*
 *	delta workspace:
 *
 *	+40	param: out1
 *	+32	param: out0
 *	+24	param: in
 *	+16	return-addr			<-- entry Wptr
 *	+8	int64 y
 *	0	[temp]		// running Wptr
 *	-8	[iptr]
 *	-16	[link]
 *	-24	[priof]
 *	-32	[ptr]
 *
 *	size = 48 + 32 == 80
 */

.section .rodata
.align 8

.globl	ow_delta
ow_delta:
	.quad	80

.text

.globl	o_delta
.type	o_delta, @function

o_delta:
	subq	$16, %rbp

.L3:
	movq	%rbp, %rdi
	movq	24(%rbp), %rsi			/* param: in */
	leaq	8(%rbp), %rdx			/* int64 y */
	movl	$8, %ecx
	call	os_chanin

	movq	%rbp, %rdi
	movq	32(%rbp), %rsi			/* param: out0 */
	leaq	8(%rbp), %rdx			/* int64 y */
	movl	$8, %ecx
	call	os_chanout

	movq	%rbp, %rdi
	movq	40(%rbp), %rsi			/* param: out1 */
	leaq	8(%rbp), %rdx			/* int64 y */
	movl	$8, %ecx
	call	os_chanout

	jmp	.L3				/* loop forever */

	addq	$16, %rbp
	movq	0(%rbp), %r11
	jmp	*%r11
/*}}}*/
/*{{{  o_consume*/
/*
 *	consume workspace:
 *
 *	+40	param: in
 *	+32	return-addr			<-- entry Wptr
 *	+24	int64 t0
 *	+16	int64 t1
 *	+8	int64 v
 *	0	[temp] / lctr		// running Wptr
 *	-8	[iptr]
 *	-16	[link]
 *	-24	[priof]
 *	-32	[ptr]
 *
 *	size = 56 + 16 == 72
 */

.section .rodata
.align 8

.globl	ow_consume
ow_consume:
	.quad	72

.LC0:
	.string	"The value was %ld, nanosecs %lu\n"
.LC1:
	.string "Loop time %lu\n"

.text

.globl	o_consume
.type	o_consume, @function

o_consume:
	subq	$32, %rbp

	/* read current time into t0 */
	movq	%rbp, %rdi
	call	os_ldtimer
	movq	%rax, 24(%rbp)			/* int64 t0 */

.L4:
	/* do 1000000 inputs, use Wptr[0] as counter */
	movq	$10000000, 0(%rbp)
.L5:

	movq	%rbp, %rdi
	movq	40(%rbp), %rsi			/* param: in */
	leaq	8(%rbp), %rdx			/* int64 v */
	movl	$8, %ecx
	call	os_chanin

	decq	0(%rbp)
	jnz	.L5

	/* read current time into t1 */
	movq	%rbp, %rdi
	call	os_ldtimer
	movq	%rax, 16(%rbp)			/* int64 t1 */

	/* t0 := (t1 - t0) / 10000000 */
	movq	16(%rbp), %rax			/* int64 t1 */
	subq	24(%rbp), %rax			/* -t0 */
	movq	$10000000, %rcx
	movq	$0, %rdx
	divq	%rcx				/* rdx:rax div by rcx -> rax=quot, rdx=rem */
	movq	%rax, 24(%rbp)			/* int64 t0 */

	/* print! */
	movq	$.LC0, %rdi
	movq	8(%rbp), %rsi			/* int64 v */
	movq	24(%rbp), %rdx			/* int64 t0 */
	addq	$3, %rdx			/* += 3 (round up) */
	shrq	$2, %rdx			/* /4 */
	movl	$0, %eax			/* vector count */
	call	printf

	movq	$.LC1, %rdi
	movq	24(%rbp), %rsi			/* int64 t0 */
	movl	$0, %eax			/* vector count */
	call	printf

	/* t0 := t1 */
	movq	16(%rbp), %rax			/* int64 t1 */
	movq	%rax, 24(%rbp)			/* int64 t0 */

	jmp	.L4

	addq	$32, %rbp
	movq	0(%rbp), %r11
	jmp	*%r11
/*}}}*/

