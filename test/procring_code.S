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


#define NPROCS		1024
#define NPROCSP1	(NPROCS+1)

#define WSADJ		(64+(NPROCSP1 * 8))
#define OFFS_A		(56+(NPROCSP1 * 8))
#define OFFS_B		(48+(NPROCSP1 * 8))
#define OFFS_D		(40+(NPROCSP1 * 8))
#define OFFS_CN		(40+(NPROCS * 8))

#define PCOUNT		(4+NPROCS)
#define PCOUNTP1	(PCOUNT+1)

#define WSSIZE		((768 + (NPROCSP1 * 8)) + (NPROCS * 128))

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
 *	<<prefix WS>>
 *	<<delta WS>>
 *	<<succ WS>>
 *	<<consume WS>>
 *	[NPROCS * <<id WS>>]
 */

.section .rodata
.align 8
.globl	ow_procring
ow_procring:	.quad	WSSIZE
.text
.globl	o_procring
.type	o_procring, @function

o_procring:
	subq	$WSADJ, %rbp

	movq	$0, OFFS_A(%rbp)	/* initialise channel a */
	movq	$0, OFFS_B(%rbp)	/* initialise channel b */
	movq	$0, OFFS_D(%rbp)	/* initialise channel d */

	movq	$NPROCSP1, 24(%rbp)	/* replicator count */
	movq	$0, 32(%rbp)		/* replicator var */
	leaq	40(%rbp), %rax		/* base address of channel-array */
.L110:
	movq	$0, (%rax)		/* initialise channel c[..] */
	addq	$8, %rax		/* next channel */
	decq	24(%rbp)
	jnz	.L110

	/* setup for PAR */
	movq	$0, 16(%rbp)		/* FIXME: priofinity */
	movq	$PCOUNTP1, 8(%rbp)	/* PAR count (plus one for ourselves, since the last thing is the replicated bit) */
	movq	$.L12, 0(%rbp)		/* PAR join-lab */

	/* start PAR branch with 'prefix' */
	movq	%rbp, %rdi
	leaq	-128(%rbp), %rsi
	movq	$o_procring_p0, %rdx
	call	os_startp

	/* start PAR branch with 'delta' */
	movq	%rbp, %rdi
	leaq	-256(%rbp), %rsi
	movq	$o_procring_p1, %rdx
	call	os_startp

	/* start PAR branch with 'succ' */
	movq	%rbp, %rdi
	leaq	-384(%rbp), %rsi
	movq	$o_procring_p2, %rdx
	call	os_startp

	/* start PAR branch with 'consume' */
	movq	%rbp, %rdi
	leaq	-512(%rbp), %rsi
	movq	$o_procring_p3, %rdx
	call	os_startp

	/* start 'id' processes */
	movq	$NPROCS, 24(%rbp)	/* replicator count */
	movq	$0, 32(%rbp)		/* replicator var */
.L111:
	movq	32(%rbp), %rax		/* i */
	shlq	$7, %rax		/* times 128 */
	leaq	-640(%rbp), %rcx
	subq	%rax, %rcx		/* drop down to relevant process */
	/* XXX: assume we have a word _above_ the startp'd thing allocated for locals (i.e. 'i') */
	movq	32(%rbp), %rax		/* i */
	movq	%rax, 8(%rcx)		/* store in new workspace */

	movq	%rbp, %rdi
	movq	%rcx, %rsi
	movq	$o_procring_p4, %rdx
	call	os_startp

	/* started, next please */
	movq	32(%rbp), %rax		/* i */
	incq	%rax
	movq	%rax, 32(%rbp)		/* i++ */
	movq	24(%rbp), %rax		/* replicator count */
	decq	%rax
	jz	.L112
	movq	%rax, 24(%rbp)		/* count-- */
	jmp	.L111
.L112:

	/* when we get here, all parallel processes started, so we just stop */
	movq	%rbp, %rdi
	movq	%rbp, %rsi
	call	os_endp


o_procring_p0:				/*{{{  parallel process for prefix */
	subq	$24, %rbp		/* adjust for call */
	movq	24(%rbp), %rax		/* staticlink */
	leaq	OFFS_B(%rax), %rcx
	movq	%rcx, 8(%rbp)		/* param: in */
	leaq	40(%rax), %rcx
	movq	%rcx, 16(%rbp)		/* param: out == c[0] */
	movq	$.L150, 0(%rbp)		/* return-address */
	jmp	o_prefix
.L150:
	addq	$24, %rbp		/* adjust after call */
	movq	%rbp, %rdi
	movq	0(%rbp), %rsi		/* staticlink == PAR WS */
	call	os_endp


/*}}}*/
o_procring_p1:				/*{{{  parallel process for delta */
	subq	$40, %rbp		/* adjust for call */
	movq	40(%rbp), %rax		/* staticlink */

	leaq	OFFS_CN(%rax), %rcx
	movq	%rcx, 8(%rbp)		/* param: in */
	leaq	OFFS_D(%rax), %rcx
	movq	%rcx, 16(%rbp)		/* param: out0 */
	leaq	OFFS_A(%rax), %rcx
	movq	%rcx, 24(%rbp)		/* param: out1 */

	movq	$.L151, 0(%rbp)		/* return-address */
	jmp	o_delta
.L151:
	addq	$40, %rbp		/* adjust after call */
	movq	%rbp, %rdi
	movq	0(%rbp), %rsi		/* staticlink == PAR WS */
	call	os_endp


/*}}}*/
o_procring_p2:				/*{{{  parallel process for succ*/
	subq	$24, %rbp		/* adjust for call */
	movq	24(%rbp), %rax		/* staticlink */

	leaq	OFFS_A(%rax), %rcx
	movq	%rcx, 8(%rbp)		/* param: in */
	leaq	OFFS_B(%rax), %rcx
	movq	%rcx, 16(%rbp)		/* param: out */

	movq	$.L152, 0(%rbp)		/* return-address */
	jmp	o_succ
.L152:
	addq	$24, %rbp		/* adjust after call */
	movq	%rbp, %rdi
	movq	0(%rbp), %rsi		/* staticlink == PAR WS */
	call	os_endp


/*}}}*/
o_procring_p3:				/*{{{  parallel process for consume*/
	subq	$16, %rbp		/* adjust for call */
	movq	16(%rbp), %rax		/* staticlink */

	leaq	OFFS_D(%rax), %rcx
	movq	%rcx, 8(%rbp)		/* param: in */
	movq	$.L153, 0(%rbp)		/* return-address */
	jmp	o_consume
.L153:
	addq	$16, %rbp		/* adjust after call */
	movq	%rbp, %rdi
	movq	0(%rbp), %rsi		/* staticlink == PAR WS */
	call	os_endp


/*}}}*/
o_procring_p4:				/*{{{  replicated parallel process for id*/
	subq	$24, %rbp		/* adjust for call */
	movq	24(%rbp), %rax		/* staticlink */

	movq	32(%rbp), %rcx		/* replicator value [0..] */
	shlq	$3, %rcx		/* times 8 */
	leaq	40(%rax), %rdx		/* address of c[0] */
	addq	%rcx, %rdx		/* address of c[i] */
	movq	%rdx, 8(%rbp)		/* param: in */
	addq	$8, %rdx		/* address of c[i+1] */
	movq	%rdx, 16(%rbp)		/* param: out */

	movq	$.L154, 0(%rbp)		/* return-address */
	jmp	o_id
.L154:
	addq	$24, %rbp		/* adjust after call */
	movq	%rbp, %rdi
	movq	0(%rbp), %rsi		/* staticlink == PAR WS */
	call	os_endp

/*}}}*/


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
	.string	"The value was %ld, nanosecs per loop %lu\n"

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
	movq	$1000000, 0(%rbp)
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

	/* t0 := (t1 - t0) / 1000000 */
	movq	16(%rbp), %rax			/* int64 t1 */
	subq	24(%rbp), %rax			/* -t0 */
	movq	$1000000, %rcx
	movq	$0, %rdx
	divq	%rcx				/* rdx:rax div by rcx -> rax=quot, rdx=rem */
	movq	%rax, 24(%rbp)			/* int64 t0 */

	/* print! */
	movq	$.LC0, %rdi
	movq	8(%rbp), %rsi			/* int64 v */
	movq	24(%rbp), %rdx			/* int64 t0 */
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

