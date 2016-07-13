
/*
 *	test stuff for x86-64 scheduler -- commstime
 */

/*
 *	NOTE: when calling os_... as a C function, the only thing we
 *	expect to be preserved is %ebp (Wptr)
 */

.text


.globl	o_commstime_shutdown
.type	o_commstime_shutdown, @function

o_commstime_shutdown:
	movq	%rbp, %rdi
	call	os_shutdown
	ret

.globl	o_commstime_startup
.type	o_commstime_startup, @function

o_commstime_startup:
	leaq	o_commstime_shutdown(%rip), %rax
	movq	%rax, 0(%ebp)			/* save return-address */
	jmp	o_commstime



/*
 *	commstime workspace:
 *
 *	[no params]
 *	+56	return-addr		<-- call entry Wptr
 *	+48	channel a	// local var start
 *	+40	channel b
 *	+32	channel c
 *	+24	channel d
 *	+16	PAR-savedpri
 *	+8	PAR-count
 *	0	PAR-iptrsucc/joinlab		// running Wptr
 *	-8	[iptr]
 *	-16	[link]
 *	-24	[priof]
 *	-32	[ptr]
 *
 *	size = 88 + 8 == 96
 *
 *	<<consume WS>>
 *	<<prefix WS>>
 *	<<delta WS>>
 *	<<succ WS>>
 */

.section .rodata
.align 8

.globl	ow_commstime
ow_commstime:
	.quad	576				/* bytes of workspace */

.text

.globl	o_commstime
.type	o_commstime, @function

	/* XXX: we expect the caller to have pre-set the return-address (part of the call) */
o_commstime:
	subq	$56, %rbp

	movq	$0, 24(%rbp)			/* initialise channel d */
	movq	$0, 32(%rbp)			/* initialise channel c */
	movq	$0, 40(%rbp)			/* initialise channel b */
	movq	$0, 48(%rbp)			/* initialise channel a */

	/* setup for PAR */
	movq	$0, 16(%rbp)			/* saved priority; FIXME */
	movq	$4, 8(%rbp)			/* PAR count */
	movq	$.L10, 0(%rbp)			/* PAR join-lab */

	/* start PAR branch with 'prefix' */
	movq	%rbp, %rdi
	leaq	-128(%rbp), %rsi
	movq	$o_commstime_p0, %rdx
	call	os_startp

	/* start PAR branch with 'delta' */
	movq	%rbp, %rdi
	leaq	-256(%rbp), %rsi
	movq	$o_commstime_p1, %rdx
	call	os_startp

	/* start PAR branch with 'succ' */
	movq	%rbp, %rdi
	leaq	-384(%rbp), %rsi
	movq	$o_commstime_p2, %rdx
	call	os_startp

	/* instance of 'consume' */
	subq	$16, %rbp
	leaq	48(%rbp), %rax			/* address of channel 'c' */
	movq	%rax, 8(%rbp)			/* param: in */
	movq	$.L12, 0(%rbp)			/* return-address */
	jmp	o_consume
.L12:
	addq	$16, %rbp

	/* we're the parallel process in the parent context */
	movq	%rbp, %rdi
	movq	%rbp, %rsi
	call	os_endp


.L10:
	addq	$56, %rbp
	movq	0(%rbp), %r11
	jmp	*%r11

o_commstime_p0:					/* parallel process to run prefix */

	/* adjust for call */
	subq	$24, %rbp			/* parent WS now at 128+24 = 152 */

	leaq	176(%rbp), %rax			/* address of channel 'd' */
	movq	%rax, 8(%rbp)			/* param: in */
	leaq	200(%rbp), %rax			/* address of channel 'a' */
	movq	%rax, 16(%rbp)			/* param: out */
	movq	$.L11, 0(%rbp)			/* return-address */
	jmp	o_prefix
.L11:
	/* adjust after call */
	addq	$24, %rbp

	movq	%rbp, %rdi
	movq	0(%rbp), %rsi			/* startp left workspace address here, should be +128 */
	call	os_endp


o_commstime_p1:					/* parallel process to run delta */

	/* adjust for call */
	subq	$40, %rbp			/* parent WS now at 256+40 = 296 */

	leaq	344(%rbp), %rax			/* address of channel 'a' */
	movq	%rax, 8(%rbp)			/* param: in */
	leaq	336(%rbp), %rax			/* address of channel 'b' */
	movq	%rax, 16(%rbp)			/* param: out0 */
	leaq	328(%rbp), %rax			/* address of channel 'c' */
	movq	%rax, 24(%rbp)			/* param: out1 */
	movq	$.L15, 0(%rbp)			/* return-address */
	jmp	o_delta
.L15:
	/* adjust after call */
	addq	$40, %rbp

	movq	%rbp, %rdi
	movq	0(%rbp), %rsi			/* startp left workspace address here, should be +256 */
	call	os_endp


o_commstime_p2:					/* parallel process to run succ */

	/* adjust for call */
	subq	$32, %rbp			/* parent WS now at 384+32 = 416 */

	leaq	456(%rbp), %rax			/* address of channel 'b' */
	movq	%rax, 8(%rbp)			/* param: in */
	leaq	440(%rbp), %rax			/* address of channel 'd' */
	movq	%rax, 16(%rbp)			/* param: out */
	movq	$.L16, 0(%rbp)			/* return-address */
	jmp	o_succ
.L16:
	/* adjust after call */
	addq	$32, %rbp

	movq	%rbp, %rdi
	movq	0(%rbp), %rsi			/* startp left workspace address here, should be +384 */
	call	os_endp


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
ow_prefix:
	.quad	72				/* bytes of workspace */

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
ow_succ:
	.quad	72

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

	/* t0 := (t1 - t0) / 40000000 */
	movq	16(%rbp), %rax			/* int64 t1 */
	subq	24(%rbp), %rax			/* -t0 */
	movq	$40000000, %rcx
	movq	$0, %rdx
	divq	%rcx				/* rdx:rax div by rcx -> rax=quot, rdx=rem */
	movq	%rax, 24(%rbp)			/* int64 t0 */

	/* print! */
	movl	$.LC0, %edi
	movl	8(%rbp), %esi			/* int64 v */
	movl	24(%rbp), %edx			/* int64 t0 */
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

