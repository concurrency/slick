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

#include "slick_types.h"
#include "sutil.h"
#include "atomics.h"

// #define LOCAL_DEBUG

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

	slick_schedlinkage (&psched);

	/* assert: never get here */
	return NULL;
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
		deadlock ();
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
	workspace_t w = dequeue (s);

#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
	fprintf (stderr, "slick_schedule(): scheduling process at %p\n", w);
#endif
	/* and go! */
	__asm__ __volatile__ ("				\n" \
		"	movq	%%rax, %%rbp		\n" \
		"	movq	-8(%%rbp), %%rax	\n" \
		"	movq	40(%%rcx), %%rsp	\n" \
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
	slick_message ("scheduler entry for thread %d, saved SP is %p", psched.sidx, psched.saved_sp);

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
/*{{{  void os_chanin (workspace_t w, void **chanptr, void *addr, const int count)*/
/*
 *	channel input
 */
void os_chanin (workspace_t w, void **chanptr, void *addr, const int count)
{
#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
	fprintf (stderr, "os_chanin(): w=%p, chanptr=%p, addr=%p, count=%d\n", w, chanptr, addr, count);
#endif
	if (*chanptr == NULL) {
		/* nothing here yet, place ourselves and deschedule */
		w[LIPtr] = (uint64_t)__builtin_return_address (0);
		w[LPriofinity] = 0;
		w[LPointer] = (uint64_t)addr;

		*chanptr = (void *)w;

		slick_schedule (&psched);
	} else {
		/* other here, copy and reschedule */
		workspace_t other = (workspace_t)*chanptr;
		const void *src = (const void *)other[LPointer];

		memcpy (addr, src, count);
		*chanptr = NULL;

		enqueue (other, &psched);
	}
}
/*}}}*/
/*{{{  void os_chanin64 (workspace_t w, void **chanptr, void *addr)*/
/*
 *	channel input (64-bit)
 */
void os_chanin64 (workspace_t w, void **chanptr, void *addr)
{
	if (*chanptr == NULL) {
		/* nothing here yet, place ourselves and deschedule */
		w[LIPtr] = (uint64_t)__builtin_return_address (0);
		w[LPriofinity] = 0;
		w[LPointer] = (uint64_t)addr;

		*chanptr = (void *)w;

		slick_schedule (&psched);
	} else {
		/* other here, copy and reschedule */
		workspace_t other = (workspace_t)*chanptr;
		const void *src = (const void *)other[LPointer];

		*(uint64_t *)addr = *(uint64_t *)src;
		*chanptr = NULL;

		enqueue (other, &psched);
	}
}
/*}}}*/
/*{{{  void os_chanout (workspace_t w, void **chanptr, const void *addr, const int count)*/
/*
 *	channel output
 */
void os_chanout (workspace_t w, void **chanptr, const void *addr, const int count)
{
#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
	fprintf (stderr, "os_chanout(): w=%p, chanptr=%p, addr=%p, count=%d\n", w, chanptr, addr, count);
#endif
	if (*chanptr == NULL) {
		/* nothing here yet, place ourselves and deschedule */
		w[LIPtr] = (uint64_t)__builtin_return_address (0);
		w[LPriofinity] = 0;						/* FIXME */
		w[LPointer] = (uint64_t)addr;

		*chanptr = (void *)w;

		slick_schedule (&psched);
	} else if (((uint64_t)(*chanptr) & 0x0001000000000000) != 0) {
		/* testing overhead of this */
		slick_fatal ("impossible thing in os_chanout()");
	} else {
		/* other here, copy and reschedule */
		workspace_t other = (workspace_t)*chanptr;
		void *dst = (void *)other[LPointer];

		memcpy (dst, addr, count);
		*chanptr = NULL;

		enqueue (other, &psched);
	}
}
/*}}}*/
/*{{{  void os_chanoutv64 (workspace_t w, void **chanptr, const uint64_t val)*/
/*
 *	channel output -- a 64-bit value to output is given, stored in w[LTemp] if channel not-ready
 */
void os_chanoutv64 (workspace_t w, void **chanptr, const uint64_t val)
{
	if (*chanptr == NULL) {
		/* nothing here yet, place ourselves and deschedule */
		w[LIPtr] = (uint64_t)__builtin_return_address (0);
		w[LPriofinity] = 0;						/* FIXME */
		w[LTemp] = val;
		w[LPointer] = (uint64_t)(&w[LTemp]);

		*chanptr = (void *)w;

		slick_schedule (&psched);
	} else if (((uint64_t)(*chanptr) & 0x0001000000000000) != 0) {
		/* testing overhead of this */
		slick_fatal ("impossible thing in os_chanoutv64()");
	} else {
		/* other here, copy and reschedule */
		workspace_t other = (workspace_t)*chanptr;
		void *dst = (void *)other[LPointer];

		*(uint64_t *)dst = val;
		*chanptr = NULL;

		enqueue (other, &psched);
	}
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
	w[LPriofinity] = 0;							/* FIXME */

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
	other[LPriofinity] = 0;							/* FIXME */

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



