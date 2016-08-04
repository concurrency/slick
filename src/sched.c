/*
 *	sched.c -- scheduler main-bits
 *	Copyright (C) 2016 Fred Barnes, University of Kent <frmb@kent.ac.uk>
 *
 *	This library is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU Lesser General Public
 *	License as published by the Free Software Foundation; either
 *	version 2.1 of the License, or (at your option) any later version.
 *
 *	This library is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	Lesser General Public License for more details.
 *
 *	You should have received a copy of the GNU Lesser General Public
 *	License along with this library; if not, write to the Free Software
 *	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *	MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#include <sched.h>
#include <pthread.h>

#include "atomics.h"
#include "slick_types.h"
#include "slick_priv.h"
#include "sutil.h"

// #define LOCAL_DEBUG

static __thread pbatch_t dflbatch;		/* default batch */
static __thread psched_t psched;		/* per-thread scheduler structure */

static void deadlock (void) __attribute__ ((noreturn));
static void slick_schedule (psched_t *s) __attribute__ ((noreturn));

static void enqueue (workspace_t w, psched_t *s);

extern void slick_schedlinkage (psched_t *s) __attribute__ ((noreturn));


/*{{{  void *slick_threadentry (void *arg)*/
/*
 *	pthreads entry-point
 */
void *slick_threadentry (void *arg)
{
	slickts_t *tinf = (slickts_t *)arg;
	int fds[2];

	memset (&psched, 0, sizeof (psched_t));

	psched.sptr = tinf->sptr;
	psched.sidx = tinf->thridx;

	att32_init (&(psched.sync), 0);

	init_pbatch_t (&dflbatch);

	psched.cbch = &dflbatch;

#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
fprintf (stderr, "slick_threadentry(): here!  my thread id is %p, index %d\n", (void *)pthread_self (), psched.sidx);
fprintf (stderr, "slick_threadentry(): enqueue initial process at %p, entry-point %p\n", tinf->initial_ws, tinf->initial_proc);
#endif

	/* create the pipe descriptors that we'll need for sleep/wakeup */
	if (pipe (fds) < 0) {
		slick_fatal ("failed to create signalling pipe for thread %d, [%s]", psched.sidx, strerror (errno));
		return NULL;
	}

	psched.signal_in = (int32_t)fds[1];
	psched.signal_out = (int32_t)fds[0];

	if (fcntl (fds[1], F_SETFL, O_NONBLOCK) < 0) {
		slick_fatal ("failed to set NONBLOCK option on pipe for thread %d, [%s]", psched.sidx, strerror (errno));
		return NULL;
	}

	if (tinf->initial_ws && tinf->initial_proc) {
		/* enqueue this process */
		workspace_t iws = (workspace_t)tinf->initial_ws;

		iws[LIPtr] = (uint64_t)tinf->initial_proc;
		enqueue (iws, &psched);
	}

	bis128_set_bit (&slickss.enabled_threads, psched.sidx);
	write_barrier ();

	if (slickss.verbose) {
		slick_message ("run-time thread %d about to enter scheduler.", psched.sidx);
	}

	slick_schedlinkage (&psched);

	/* assert: never get here */
	return NULL;
}
/*}}}*/
/*{{{  static void slick_safe_pause (psched_t *s)*/
/*
 *	puts a run-time thread to sleep
 */
static void slick_safe_pause (psched_t *s)
{
	uint32_t buffer, sync;

#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
fprintf (stderr, "slick_safe_pause(): thread index %d\n", s->sidx);
#endif
	while (!(sync = att32_swap (&(s->sync), 0))) {
		serialise ();
		read (s->signal_out, &buffer, 1);
		serialise ();
	}

	att32_or (&(s->sync), sync);		/* put back the flags */

#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
fprintf (stderr, "slick_safe_pause(): thread index %d about to resume after pause\n", psched.sidx);
#endif
}
/*}}}*/
static void slick_wake_thread (psched_t *s, unsigned int sync_bit) /*{{{*/
{
	uint32_t data = 0;

	bis128_clear_bit (&slickss.sleeping_threads, s->sidx);
	write_barrier ();
	att32_set_bit (&(s->sync), sync_bit);
	serialise ();
	write (s->signal_in, &data, 1);
}
/*}}}*/



/*{{{  static void deadlock (void)*/
/*
 *	called if we get to deadlock
 */
static void deadlock (void)
{
	slick_fatal ("deadlocked, no processes left");
}
/*}}}*/


/*{{{  static void enqueue (workspace_t w, psched_t *s)*/
/*
 *	enqueues a process
 */
static void enqueue (workspace_t w, psched_t *s)
{
	if (s->fptr == NULL) {
		s->bptr = w;
		s->fptr = w;
	} else {
		s->bptr[LLink] = (uint64_t)w;
		s->bptr = w;
	}
}
/*}}}*/
/*{{{  static workspace_t dequeue (psched_t *s)*/
/*
 *	dequeues a process
 */
static workspace_t dequeue (psched_t *s)
{
	workspace_t tmp = s->fptr;

	if (tmp == NULL) {
		return NULL;
	} else if (tmp == s->bptr) {
		/* last one */
		s->fptr = NULL;
	} else {
		s->fptr = (workspace_t)tmp[LLink];
	}
	return tmp;
}
/*}}}*/


/*{{{  static void slick_schedule (psched_t *s)*/
/*
 *	picks a new process to run and dispatches
 */
static void slick_schedule (psched_t *s)
{
	workspace_t w = NULL;

	do {
		if (att32_val (&(s->sync))) {
			uint32_t sync = att32_swap (&(s->sync), 0);

			if (sync & SYNC_TIME) {
				/*  FIXME: check timer queue */
			}

		}
		
		if (s->fptr == NULL) {
			/* no more processes -- consider going to sleep */
			bis128_set_bit (&slickss.sleeping_threads, s->sidx);
			read_barrier ();

			if (s->tptr != NULL) {
				slick_safe_pause (s);
				/* FIXME: check timer queue */
			} else if (!att32_val (&(s->sync))) {
				bitset128_t idle;

				bis128_set_bit (&slickss.idle_threads, s->sidx);

				/* FIXME: check for blocking calls, etc. */
				read_barrier ();

				bis128_and (&slickss.idle_threads, &slickss.sleeping_threads, &idle);

				if (bis128_eq (&idle, &slickss.enabled_threads)) {
					/* (idle & sleeping) == enabled, so all stuck */
					deadlock ();
				} else {
					slick_safe_pause (s);
				}

				bis128_clear_bit (&slickss.idle_threads, s->sidx);
			} else {
				bis128_clear_bit (&slickss.sleeping_threads, s->sidx);
			}
		} else {
			w = dequeue (s);
		}

	} while (w == NULL);

#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
	fprintf (stderr, "slick_schedule(): scheduling process at %p\n", w);
#endif
	/* and go! */
	__asm__ __volatile__ ("				\n" \
		"	movq	%%rax, %%rbp		\n" \
		"	movq	-8(%%rbp), %%rax	\n" \
		"	movq	48(%%rcx), %%rsp	\n" \
		"	jmp	*%%rax			\n" \
		:: "a" (w), "c" (s) : "rbx", "rdx", "rdi", "rsi", "memory", "cc");
	_exit (42);		/* assert: never get here (prevent gcc warning about returning non-return function) */
}
/*}}}*/


/*{{{  void os_entry (void)*/
/*
 *	entry-point from slick_schedlinkage
 */
void os_entry (void)
{
#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
	slick_message ("scheduler entry for thread %d, saved SP is %p", psched.sidx, psched.saved_sp);
#endif

	slick_schedule (&psched);
}
/*}}}*/
/*{{{  void os_shutdown (workspace_t w)*/
/*
 *	called when return from the top-level thing
 */
void os_shutdown (workspace_t w)
{
	slick_message ("scheduler exit for process at %p", w);

	pthread_exit (NULL);
}
/*}}}*/
/*{{{  channel flags (integer)*/
#define CIO_NONE	(0x00000000)
#define CIO_INPUT	(0x00000001)
#define CIO_OUTPUT	(0x00000002)

/*}}}*/
/*{{{  static INLINE void trigger_alt_guard (uint64_t val)*/
/*
 *	called when we're doing channel I/O and we find something ALTy in there
 */
static INLINE void trigger_alt_guard (uint64_t val)
{
	workspace_t other = (workspace_t)(val & ~1);
	uint64_t state, nstate;

	do {
		state = att64_val ((atomic64_t *)&(other[LState]));
		nstate = (state - 1) & (~(ALT_NOT_READY | ALT_WAITING));		/* decrement guard count, clear NOT_READY and WAITING flags */
	} while (!att64_cas ((atomic64_t *)&(other[LState]), state, nstate));

	if ((state & ALT_WAITING) || (nstate == 0)) {
		enqueue (other, &psched);
	}
}
/*}}}*/
/*{{{  static INLINE void channel_io (const int flags, workspace_t w, void **chanptr, void *addr, const int count, uint64_t raddr)*/
/*
 *	channel communication -- both ways.  This gets inlined and, hopefully, gcc optimises away everything
 *	that isn't used or can't be determined statically.
 */
static INLINE void channel_io (const int flags, workspace_t w, void **chanptr, void *addr, const int count, uint64_t raddr)
{
	uint64_t *chanval;
	workspace_t other;
	void *optr;

#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
	fprintf (stderr, "channel_io(): flags=0x%8.8x, w=%p, chanptr=%p, addr=%p, raddr=%p, count=%d\n", flags, w, chanptr, addr, raddr, count);
#endif

	chanval = (uint64_t *)att64_val ((atomic64_t *)chanptr);

	if (!chanval || ((uint64_t)chanval & 1)) {
		/* not here, or ALTing -- prepare to sleep */
		w[LIPtr] = raddr;
		w[LPriofinity] = (uint64_t)psched.cbch;
		w[LPointer] = (uint64_t)addr;

		write_barrier ();

		chanval = (uint64_t *)att64_swap ((atomic64_t *)chanptr, (uint64_t)w);
		if (!chanval) {
			/* we're in the channel now */
			slick_schedule (&psched);
		} else if ((uint64_t)chanval & 1) {
			/* something ALTy in the channel, but we're there now */
			trigger_alt_guard ((uint64_t)chanval);
			slick_schedule (&psched);
		}
		/* else, something arrived in the channel along the way, so go with it */
	}

	other = (workspace_t)chanval;
	optr = (void *)other[LPointer];

	if (flags & CIO_INPUT) {
		switch (count) {
		case 0:							break;			/* a signalling mechanism */
		case 1:	*(uint8_t *)(addr) = *(uint8_t *)(optr);	break;
		case 2:	*(uint16_t *)(addr) = *(uint16_t *)(optr);	break;
		case 4:	*(uint32_t *)(addr) = *(uint32_t *)(optr);	break;
		case 8:	*(uint64_t *)(addr) = *(uint64_t *)(optr);	break;
		default:
			memcpy (addr, optr, count);
			break;
		}
	} else {
		switch (count) {
		case 0:							break;			/* a signalling mechanism */
		case 1:	*(uint8_t *)(optr) = *(uint8_t *)(addr);	break;
		case 2:	*(uint16_t *)(optr) = *(uint16_t *)(addr);	break;
		case 4:	*(uint32_t *)(optr) = *(uint32_t *)(addr);	break;
		case 8:	*(uint64_t *)(optr) = *(uint64_t *)(addr);	break;
		default:
			memcpy (optr, addr, count);
			break;
		}
	}

	*chanptr = NULL;			/* write barrier will make sure this goes first */
	// att64_set ((atomic64_t *)chanptr, (uint64_t)NULL);
	write_barrier ();
	enqueue (other, &psched);
	return;
}
/*}}}*/
/*{{{  void os_chanin (workspace_t w, void **chanptr, void *addr, const int count)*/
/*
 *	channel input
 */
void os_chanin (workspace_t w, void **chanptr, void *addr, const int count)
{
	channel_io (CIO_INPUT, w, chanptr, addr, count, (uint64_t)__builtin_return_address (0));
}
/*}}}*/
/*{{{  void os_chanin64 (workspace_t w, void **chanptr, void *addr)*/
/*
 *	channel input (64-bit)
 */
void os_chanin64 (workspace_t w, void **chanptr, void *addr)
{
	channel_io (CIO_INPUT, w, chanptr, addr, 8, (uint64_t)__builtin_return_address (0));
}
/*}}}*/
/*{{{  void os_chanout (workspace_t w, void **chanptr, const void *addr, const int count)*/
/*
 *	channel output
 */
void os_chanout (workspace_t w, void **chanptr, void *addr, const int count)
{
	channel_io (CIO_OUTPUT, w, chanptr, addr, count, (uint64_t)__builtin_return_address (0));
}
/*}}}*/
/*{{{  void os_chanoutv64 (workspace_t w, void **chanptr, const uint64_t val)*/
/*
 *	channel output -- a 64-bit value to output is given, stored in w[LTemp] if channel not-ready
 */
void os_chanoutv64 (workspace_t w, void **chanptr, const uint64_t val)
{
	uint64_t *chanval;
	workspace_t other;
	void *dptr;

#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
	fprintf (stderr, "os_chanoutv64(): w=%p, chanptr=%p, val=%16.16lx\n", w, chanptr, val);
#endif

	chanval = (uint64_t *)att64_val ((atomic64_t *)chanptr);

	/* if nothing here, or ALTy, save value and go to channel code */
	if (!chanval || ((uint64_t)chanval & 1)) {
		w[LTemp] = val;
		channel_io (CIO_OUTPUT, w, chanptr, (void *)w, 8, (uint64_t)__builtin_return_address (0));
		/* if we get this far, we raced with the input, but done now */
		return;
	}

	/* channel has a process in it, and it's not ALTing, so not going anywhere */
	other = (workspace_t)chanval;
	dptr = (void *)other[LPointer];

	*(uint64_t *)dptr = val;
	*chanptr = NULL;

	write_barrier ();
	enqueue (other, &psched);
}
/*}}}*/
/*{{{  void os_runp (workspace_t w, workspace_t other)*/
/*
 *	run process: just pop it on the run-queue (simple enqueue for generated code)
 */
void os_runp (workspace_t w, workspace_t other)
{
	enqueue (other, &psched);
}
/*}}}*/
/*{{{  void os_stopp (workspace_t w)*/
/*
 *	stop process: save return address (and priof) and schedule another
 */
void os_stopp (workspace_t w)
{
	w[LIPtr] = (uint64_t)__builtin_return_address (0);
	w[LPriofinity] = (uint64_t)psched.cbch;

	slick_schedule (&psched);
}
/*}}}*/
/*{{{  void os_startp (workspace_t w, workspace_t other, void *entrypoint)*/
/*
 *	start process: setup a process and enqueue it
 */
void os_startp (workspace_t w, workspace_t other, void *entrypoint)
{
#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
	fprintf (stderr, "os_startp(): w=%p, other=%p, entrypoint=%p\n", w, other, entrypoint);
#endif
	other[LTemp] = (uint64_t)w;						/* parent workspace */
	other[LIPtr] = (uint64_t)entrypoint;
	other[LPriofinity] = (uint64_t)psched.cbch;

	enqueue (other, &psched);
}
/*}}}*/
/*{{{  void os_endp (workspace_t w, workspace_t other)*/
/*
 *	end process: decrement par-count and reschedule if done
 */
void os_endp (workspace_t w, workspace_t other)
{
	other[LCount]--;
	if (!other[LCount]) {
		/* we were the last */
		other[LPriofinity] = other[LSavedPri];
		other[LIPtr] = other[LIPtrSucc];

		enqueue (other, &psched);
	}
	/* else we were not the last -- reschedule */
	slick_schedule (&psched);
}
/*}}}*/
/*{{{  uint64_t os_ldtimer (workspace_t w)*/
/*
 *	read the current time in nanoseconds
 */
uint64_t os_ldtimer (workspace_t w)
{
	struct timespec ts;

	if (clock_gettime (CLOCK_MONOTONIC_COARSE, &ts) != 0) {
		slick_fatal ("clock_gettime() failed with: %s", strerror (errno));
		return 0;
	}
	return (((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec);
}
/*}}}*/
/*{{{  void os_alt (workspace_t w)*/
/*
 *	alternative start
 */
void os_alt (workspace_t w)
{
	att64_set ((atomic64_t *)&(w[LState]), ALT_ENABLING | ALT_NOT_READY | 1);
	write_barrier ();
	return;
}
/*}}}*/
/*{{{  void os_talt (workspace_t w)*/
/*
 *	time(out) alternative start
 */
void os_talt (workspace_t w)
{
	att64_set ((atomic64_t *)&(w[LState]), ALT_ENABLING | ALT_NOT_READY | 1);
	att64_set ((atomic64_t *)&(w[LTLink]), TimeNotSet_p);
	write_barrier ();
	return;
}
/*}}}*/



