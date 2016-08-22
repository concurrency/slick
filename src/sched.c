/*
 *	sched.c -- scheduler main-bits
 *	Copyright (C) 2016 Fred Barnes, University of Kent <frmb@kent.ac.uk>
 *	Note: this mostly follows Carl Ritson's multicore CCSP scheduler structure/logic,
 *	in turn based on Jim Moores' CCSP, with contributions from various authors (see AUTHORS).
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
#include <sys/time.h>
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

static __thread psched_t psched;		/* per-thread scheduler structure */

static void deadlock (void) __attribute__ ((noreturn));
static void slick_schedule (psched_t *s) __attribute__ ((noreturn));

static void sched_setup_spin (psched_t *s);
static void sched_enqueue (psched_t *s, workspace_t w);
static void sched_allocate_to_free_list (psched_t *s, unsigned int count);
static INLINE pbatch_t *sched_allocate_batch (psched_t *s);
static INLINE void sched_new_current_batch (psched_t *s);
static INLINE void sched_add_to_runqueue (psched_t *s, uint64_t priofinity, unsigned int rq_n, pbatch_t *bch);
static INLINE void sched_add_affine_batch_to_runqueue (runqueue_t *rq, pbatch_t *bch);

static void batch_enqueue_process (pbatch_t *bch, workspace_t w);

static inline void runqueue_atomic_enqueue (runqueue_t *rq, int isws, void *ptr);

extern void slick_schedlinkage (psched_t *s) __attribute__ ((noreturn));


/*{{{  void *slick_threadentry (void *arg)*/
/*
 *	pthreads entry-point
 */
void *slick_threadentry (void *arg)
{
	slickts_t *tinf = (slickts_t *)arg;
	int fds[2];
	int i;

	memset (&psched, 0, sizeof (psched_t));

	init_psched_t (&psched);

	psched.sptr = tinf->sptr;
	psched.sidx = tinf->thridx;
	psched.priofinity = BuildPriofinity (0, (MAX_PRIORITY_LEVELS / 2));

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

	sched_allocate_to_free_list (&psched, MAX_PRIORITY_LEVELS * 2);
	for (i=0; i<MAX_PRIORITY_LEVELS; i++) {
		psched.rq[i].pending = sched_allocate_batch (&psched);
	}

	sched_new_current_batch (&psched);

	if (tinf->initial_ws && tinf->initial_proc) {
		/* enqueue this process */
		workspace_t iws = (workspace_t)tinf->initial_ws;

		iws[LIPtr] = (uint64_t)tinf->initial_proc;
		iws[LPriofinity] = psched.priofinity;
		sched_enqueue (&psched, iws);
	}

	slickss.schedulers[psched.sidx] = &psched;

	sched_setup_spin (&psched);

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
void slick_wake_thread (psched_t *s, unsigned int sync_bit) /*{{{*/
{
	uint32_t data = 0;

	bis128_clear_bit (&slickss.sleeping_threads, s->sidx);
	write_barrier ();
	att32_set_bit (&(s->sync), sync_bit);
	serialise ();
	write (s->signal_in, &data, 1);
}
/*}}}*/
static INLINE int64_t calculate_dispatches (uint64_t size) /*{{{*/
{
	size <<= BATCH_PPD_SHIFT;
	size |= (one_if_z64 (size, (~BATCH_MD_MASK)) - 1);
	return (int64_t)(size & BATCH_MD_MASK);

}
/*}}}*/
/*{{{  unsigned int sched_spin_us (void)*/
/*
 *	determines the spin-time for a scheduler, based on the number of CPUs
 *	FIXME: preset and put in slickss
 */
unsigned int sched_spin_us (void)
{
	char *ch;
	unsigned int ncpus;

	if (slickss.ncpus < 2) {
		return 0;
	}

	ch = getenv ("SLICKSCHEDULERSPIN");
	if (ch) {
		if (sscanf (ch, "%u", &ncpus) != 1) {
			slick_warning ("sched_spin_us(): not using environment variable SLICKSCHEDULERSPIN, not integer [%s]", ch);
		} else {
			return ncpus;
		}
	}

	return 16;
}
/*}}}*/
/*{{{  static void sched_setup_spin (psched_t *s)*/
/*
 *	sets up the spin counter for a scheduler
 */
static void sched_setup_spin (psched_t *s)
{
	uint64_t start, end, ns;
	int i = 10000;

	start = sched_time_now ();
	while (i--) {
		idle_cpu ();
	}

	end = sched_time_now ();
	ns = end - start;

	psched.spin = (sched_spin_us () * 1000) / (ns ? ns : 1);
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


/*{{{  static INLINE void sched_release_clean_batch (psched_t *s, pbatch_t *bch)*/
/*
 *	puts a batch on the scheduler's free-list (assumes clean)
 */
static INLINE void sched_release_clean_batch (psched_t *s, pbatch_t *bch)
{
	if (batch_isdirty (bch)) {
		slick_fatal ("sched_release_clean_batch(): dirty batch at %p", bch);
	}
	bch->nb = s->free;
	s->free = bch;
}
		
/*}}}*/
/*{{{  static INLINE void sched_release_dirty_batch (psched_t *s, pbatch_t *bch)*/
/*
 *	puts a batch on the scheduler's dirty-list
 */
static INLINE void sched_release_dirty_batch (psched_t *s, pbatch_t *bch)
{
	bch->nb = s->laundry;
	s->laundry = bch;
}
/*}}}*/
/*{{{  static INLINE void sched_release_batch (psched_t *s, pbatch_t *bch)*/
/*
 *	releases a batch, dirty flag decides whether 'free' or 'laundry'
 */
static INLINE void sched_release_batch (psched_t *s, pbatch_t *bch)
{
	if (batch_isdirty (bch)) {
		sched_release_dirty_batch (s, bch);
	} else {
		reinit_pbatch_t (bch);
		sched_release_clean_batch (s, bch);
	}
}
/*}}}*/
/*{{{  static void sched_do_laundry (psched_t *s)*/
/*
 *	goes through the laundry batch list, releasing to the free-list those marked clean
 */
static void sched_do_laundry (psched_t *s)
{
	pbatch_t *bch = s->laundry;
	pbatch_t *prev = NULL;

	while (bch) {
		if (batch_isdirty (bch)) {
			prev = bch;
			bch = bch->nb;
		} else {
			pbatch_t *next = bch->nb;

			reinit_pbatch_t (bch);
			sched_release_clean_batch (s, bch);
			if (!prev) {
				s->laundry = next;
			} else {
				prev->nb = next;
			}
			bch = next;
		}
	}
}
/*}}}*/
/*{{{  static void sched_release_excess_memory (psched_t *s)*/
/*
 *	keeps no more than 32 batches on the scheduler's free-list
 */
static void sched_release_excess_memory (psched_t *s)
{
	pbatch_t *bch = s->free;
	int count;

	for (count=0; bch && (count < 32); count++) {
		bch = bch->nb;
	}

	if (bch) {
		pbatch_t *next = bch->nb;

		bch->nb = NULL;
		bch = next;

		while (bch) {
			next = bch->nb;
			sfree (bch);
			bch = next;
		}
	}
}
/*}}}*/
/*{{{  static void sched_allocate_to_free_list (psched_t *s, unsigned int count)*/
/*
 *	allocates and assigns batches to a scheduler's free-list
 */
static void sched_allocate_to_free_list (psched_t *s, unsigned int count)
{
	while (count--) {
		pbatch_t *bch = (pbatch_t *)smalloc (PBATCH_ALLOC_SIZE);

		init_pbatch_t (bch);
		sched_release_clean_batch (s, bch);
	}
}
/*}}}*/
/*{{{  static INLINE pbatch_t *sched_allocate_batch (psched_t *s)*/
/*
 *	allocates a new batch
 */
static INLINE pbatch_t *sched_allocate_batch (psched_t *s)
{
	pbatch_t *bch = s->free;

	if (!bch) {
		sched_allocate_to_free_list (s, 16);
		sched_do_laundry (s);
		bch = s->free;
	}
	s->free = bch->nb;
	bch->nb = (pbatch_t *)(-1);

	return bch;
}
/*}}}*/
/*{{{  static INLINE pbatch_t *sched_save_current_batch (psched_t *s)*/
/*
 *	saves the contents of the current scheduler batch
 */
static INLINE pbatch_t *sched_save_current_batch (psched_t *s)
{
	pbatch_t *nb = sched_allocate_batch (s);

	nb->fptr = s->cbch.fptr;
	nb->bptr = s->cbch.bptr;
	nb->size = (s->cbch.size & (~BATCH_EMPTIED));

	return nb;
}
/*}}}*/
/*{{{  static INLINE void sched_load_current_batch (psched_t *s, pbatch_t *bch, int remote)*/
/*
 *	loads a particular batch into the scheduler
 */
static INLINE void sched_load_current_batch (psched_t *s, pbatch_t *bch, int remote)
{
	s->cbch.fptr = bch->fptr;
	s->cbch.bptr = bch->bptr;
	s->cbch.size = (bch->size & (~BATCH_EMPTIED));

	s->dispatches = calculate_dispatches (s->cbch.size);
	s->priofinity = s->cbch.fptr[LPriofinity];

	if (!remote) {
		reinit_pbatch_t (bch);
		sched_release_clean_batch (s, bch);
	} else {
		batch_mark_clean (bch);			/* owning scheduler needs to clean */
	}
}
/*}}}*/
/*{{{  static void mail_process (uint64_t affinity, workspace_t w)*/
/*
 *	sends a process to another scheduler
 */
static void mail_process (uint64_t affinity, workspace_t w)
{
	bitset128_t targets;
	unsigned int n;
	psched_t *s;

	if (!affinity) {
		bis128_copy (&targets, &slickss.enabled_threads);
	} else {
		bis128_set_hi (&targets, 0);
		bis128_set_lo (&targets, bis128_val_lo (&slickss.enabled_threads) & affinity);

		if (!bis128_val_lo (&targets)) {
			/* impossible: no such scheduler */
			slick_fatal ("mail_process(): impossible affinity detected: 0x%16.16lx.", affinity);
		}
	}

	n = bis128_pick_random_bit (&targets);
	s = slickss.schedulers[n];

	runqueue_atomic_enqueue (&(s->pmail), 1, w);
	write_barrier ();
	att32_set_bit (&(s->sync), SYNC_PMAIL_BIT);
	read_barrier ();

	if (bis128_isbitset (&slickss.sleeping_threads, s->sidx)) {
		slick_wake_thread (s, SYNC_PMAIL_BIT);
	}
}
/*}}}*/
/*{{{  static void sched_enqueue_far_process (psched_t *s, uint64_t priofinity, workspace_t w)*/
/*
 *	enqueues a process elsewhere
 */
static void sched_enqueue_far_process (psched_t *s, uint64_t priofinity, workspace_t w)
{
	if (!PHasAffinity (priofinity)) {
		int pri = PPriority (priofinity);
		runqueue_t *rq = &(s->rq[pri]);

		if (PHasAffinity (rq->priofinity)) {
			sched_add_affine_batch_to_runqueue (rq, rq->pending);
			rq->pending = sched_allocate_batch (s);
		}

		rq->priofinity = BuildPriofinity (0, 1);
		batch_enqueue_process (rq->pending, w);

		att64_unsafe_set_bit (&(s->rqstate), pri);
		if (pri < PPriority (s->priofinity)) {
			/* force new-batch pick next time */
			s->dispatches = 0;
		}
	} else if ((PAffinity (priofinity) & bis128_val_lo (&s->id)) != 0) {		/* XXX: only handles affinity for low-order 59 threads */
		/* affinity for this scheduler (and maybe others) */
		int pri = PPriority (priofinity);
		runqueue_t *rq = &(s->rq[pri]);

		if (rq->priofinity && (PAffinity (rq->priofinity) != PAffinity (priofinity))) {
			sched_add_to_runqueue (s, rq->priofinity, pri, rq->pending);
			rq->pending = sched_allocate_batch (s);
		}

		rq->priofinity = priofinity;
		batch_enqueue_process (rq->pending, w);

		att64_unsafe_set_bit (&(s->rqstate), pri);
		if (pri < PPriority (s->priofinity)) {
			/* force new-batch pick next time */
			s->dispatches = 0;
		}
	} else {
		mail_process (PAffinity (priofinity), w);
	}
}
/*}}}*/

/*{{{  static INLINE void batch_enqueue_hint (pbatch_t *bch, workspace_t w, int isempty)*/
/*
 *	enqueues a process to a particular batch, with hint for empty/non-empty
 */
static INLINE void batch_enqueue_hint (pbatch_t *bch, workspace_t w, int isempty)
{
	w[LLink] = (uint64_t)NULL;

	if (isempty) {
		bch->fptr = w;
		bch->bptr = w;
		bch->size = 1;
	} else {
		bch->bptr[LLink] = (uint64_t)w;
		bch->bptr = w;
		bch->size++;
	}
}
/*}}}*/
/*{{{  static void batch_enqueue_process (pbatch_t *bch, workspace_t w)*/
/*
 *	enqueues a process to a particular batch (current typically)
 */
static void batch_enqueue_process (pbatch_t *bch, workspace_t w)
{
	w[LLink] = (uint64_t)NULL;

	if (bch->fptr == NULL) {
		bch->fptr = w;
	} else {
		bch->bptr[LLink] = (uint64_t)w;
	}
	bch->bptr = w;
	bch->size++;
}
/*}}}*/
/*{{{  static inline void batch_enqueue_process_front (pbatch_t *bch, workspace_t w)*/
/*
 *	enqueues a process to the front of a batch
 */
static inline void batch_enqueue_process_front (pbatch_t *bch, workspace_t w)
{
	w[LLink] = (uint64_t)bch->fptr;

	if (w[LLink] == (uint64_t)NULL) {
		bch->fptr = w;
		bch->bptr = w;
	} else {
		bch->fptr = w;
	}
	bch->size++;
}
/*}}}*/

/*{{{  static void sched_enqueue (psched_t *s, workspace_t w)*/
/*
 *	enqueues a process
 */
static void sched_enqueue (psched_t *s, workspace_t w)
{
	uint64_t priofinity = w[LPriofinity];

	if (s->priofinity == priofinity) {
		batch_enqueue_process (&(s->cbch), w);
	} else {
		sched_enqueue_far_process (s, priofinity, w);
	}
}
/*}}}*/
/*{{{  static INLINE void sched_enqueue_nopri (psched_t *s, workspace_t w)*/
/*
 *	enqueues a process on the current scheduler's batch, ignoring priority
 */
static INLINE void sched_enqueue_nopri (psched_t *s, workspace_t w)
{
	batch_enqueue_process (&(s->cbch), w);
}
/*}}}*/
/*{{{  static workspace_t batch_dequeue_process (pbatch_t *bch)*/
/*
 *	dequeues a process from a specific batch (assumes non-empty)
 */
static workspace_t batch_dequeue_process (pbatch_t *bch)
{
	workspace_t tmp = bch->fptr;
	uint64_t bsize = bch->size;

	bch->fptr = (workspace_t)(tmp[LLink]);
	bch->size = ((bsize - 2) & BATCH_EMPTIED) | (bsize - 1);
	/*
	 *	Note from Carl Ritson's CCSP on the above:
	 *	The previous line is "clever":
	 *
	 *	If bsize is 1, i.e. the last process in the batch,
	 *	then (size - 2) will be -1 which is 0xffffffff.
	 *
	 *	Complementing this with the flag we want to set will
	 *	give us either 0 or BATCH_EMPTIED, which we then OR
	 *	with the real new size.
	 */

	return tmp;
}
/*}}}*/
/*{{{  static workspace_t sched_dequeue (psched_t *s)*/
/*
 *	dequeues a process from the current scheduler's batch
 */
static workspace_t sched_dequeue (psched_t *s)
{
	return batch_dequeue_process (&(s->cbch));
}
/*}}}*/
/*{{{  static INLINE int sched_isbatchend (psched_t *s)*/
/*
 *	determines whether we are at the end of the current batch (either by running out of dispatches or emptied batch).
 */
static INLINE int sched_isbatchend (psched_t *s)
{
	return ((s->dispatches < 0) || (s->cbch.fptr == NULL));
}
/*}}}*/

/*{{{  static INLINE int batch_empty (pbatch_t *b)*/
/*
 *	determines whether a particular batch is empty
 */
static INLINE int batch_empty (pbatch_t *b)
{
	return (b->fptr == NULL);
}
/*}}}*/
/*{{{  static void batch_verify_integrity (pbatch_t *bch)*/
/*
 *	verifies batch integrity (sanity)
 */
static void batch_verify_integrity (pbatch_t *bch)
{
	workspace_t ptr = bch->fptr;
	uint64_t size = 1;

	while (ptr[LLink] != (uint64_t)NULL) {
		if (size > (bch->size & ~BATCH_EMPTIED)) {
			slick_fatal ("batch_verify_integrity(): batch at %p, size = 0x%16.16lx, counted = 0x%16.16lx", bch, bch->size, size);
		}
		size++;
		ptr = (workspace_t)(ptr[LLink]);
	}

	if (ptr != bch->bptr) {
		slick_fatal ("batch_verify_integrity(): batch at %p, size = 0x%16.16lx, ptr=%p, bptr=%p", bch, bch->size, ptr, bch->bptr);
	}
	if ((bch->size & ~BATCH_EMPTIED) != size) {
		slick_fatal ("batch_verify_integrity(): batch at %p, size = 0x%16.16lx, counted=0x%16.16lx", bch, bch->size, size);
	}
}
/*}}}*/

/*{{{  static inline void runqueue_atomic_enqueue (runqueue_t *rq, int isws, void *ptr)*/
/*
 *	atomically adds a process or batch to a run-queue
 */
static inline void runqueue_atomic_enqueue (runqueue_t *rq, int isws, void *ptr)
{
	void *back;

	if (isws) {
		att64_set ((atomic64_t *)&(((workspace_t)ptr)[LLink]), (uint64_t)NULL);
	} else {
		att64_set ((atomic64_t *)&(((pbatch_t *)ptr)->nb), (uint64_t)NULL);
	}

	write_barrier ();

	back = (void *)att64_swap ((atomic64_t *)&(rq->bptr), (uint64_t)ptr);

	if (!back) {
		att64_set ((atomic64_t *)&(rq->fptr), (uint64_t)ptr);
	} else if (isws) {
		att64_set ((atomic64_t *)&(((workspace_t)back)[LLink]), (uint64_t)ptr);
	} else {
		att64_set ((atomic64_t *)&(((pbatch_t *)back)->nb), (uint64_t)ptr);
	}
}
/*}}}*/
/*{{{  static inline void *runqueue_atomic_dequeue (runqueue_t *rq, int isws)*/
/*
 *	atomically removes a process or batch from a run-queue's batch
 */
static inline void *runqueue_atomic_dequeue (runqueue_t *rq, int isws)
{
	void *ptr = (void *)att64_val ((atomic64_t *)&(rq->fptr));

	if (ptr) {
		void *next;

		if (ptr == (void *)att64_val ((atomic64_t *)&(rq->bptr))) {
			/* last thing in the queue, CAS it out */
			if (att64_cas ((atomic64_t *)&(rq->fptr), (uint64_t)ptr, (uint64_t)NULL)) {
				/* succeeded in swapping in NULL for ptr */

				att64_cas ((atomic64_t *)&(rq->bptr), (uint64_t)ptr, (uint64_t)NULL);
				/* Note: this must be CAS'd in, should we race with something that sees NULL and sets fptr/bptr */

				if (isws) {
					att64_set ((atomic64_t *)&(((workspace_t)ptr)[LLink]), ~((uint64_t)NULL));
				} else {
					att64_set ((atomic64_t *)&(((pbatch_t *)ptr)->nb), (uint64_t)-1);
				}

				return ptr;
			}
			read_barrier ();
		}

		if (isws) {
			next = (void *)att64_val ((atomic64_t *)&(((workspace_t)ptr)[LLink]));
		} else {
			next = (void *)att64_val ((atomic64_t *)&(((pbatch_t *)ptr)->nb));
		}

		/* XXX: frmb note to check: better not have two threads trying to dequeue here? */
		if (next) {
			att64_set ((atomic64_t *)&(rq->fptr), (uint64_t)next);
			write_barrier ();

			if (isws) {
				att64_set ((atomic64_t *)&(((workspace_t)ptr)[LLink]), ~((uint64_t)NULL));
			} else {
				att64_set ((atomic64_t *)&(((pbatch_t *)ptr)->nb), (uint64_t)-1);
			}

			return ptr;
		}
	}

	return NULL;
}
/*}}}*/

/*{{{  static INLINE void sched_add_to_local_runqueue (runqueue_t *rq, pbatch_t *bch)*/
/*
 *	attaches a batch to the relevant run-queue
 */
static INLINE void sched_add_to_local_runqueue (runqueue_t *rq, pbatch_t *bch)
{
	bch->nb = NULL;

	if (rq->fptr == NULL) {
		rq->fptr = bch;
		rq->bptr = bch;
	} else {
		rq->bptr->nb = bch;
		rq->bptr = bch;
	}
}
/*}}}*/
/*{{{  static INLINE uint64_t increment_mwindow_head (uint64_t head)*/
/*
 *	increments the migration window head value
 */
static INLINE uint64_t increment_mwindow_head (uint64_t head)
{
	head++;
	return (head | (head >> MWINDOW_HEAD_WRAP_BIT)) & MWINDOW_SIZE;
}
/*}}}*/
/*{{{  static INLINE void sched_add_to_visible_runqueue (runqueue_t *rq, mwindow_t *mw, pbatch_t *bch)*/
/*
 *	adds a batch of processes to the run-queue, made visible in the migration window
 */
static INLINE void sched_add_to_visible_runqueue (runqueue_t *rq, mwindow_t *mw, pbatch_t *bch)
{
	uint64_t state = att64_val (&(mw->data[MWINDOW_STATE]));
	uint64_t w = increment_mwindow_head (MWINDOW_HEAD (state));

	batch_set_window (bch, w);
	write_barrier ();

	if (att64_val (&(mw->data[w])) != (uint64_t)NULL) {
		pbatch_t *old = (pbatch_t *)att64_swap (&(mw->data[w]), (uint64_t)bch);

		if (old) {
			batch_set_clean (old);
		}
	} else {
		att64_set (&(mw->data[w]), (uint64_t)bch);
		write_barrier ();
	}
	att64_set (&(mw->data[MWINDOW_STATE]), MWINDOW_NEW_STATE (state, w));

	sched_add_to_local_runqueue (rq, bch);
}
/*}}}*/
/*{{{  static INLINE void sched_add_affine_batch_to_runqueue (runqueue_t *rq, pbatch_t *bch)*/
/*
 *	adds a batch to the local run-queue, clears any window on the way
 */
static INLINE void sched_add_affine_batch_to_runqueue (runqueue_t *rq, pbatch_t *bch)
{
	batch_set_clean (bch);				/* make sure the batch doesn't have a window */
	sched_add_to_local_runqueue (rq, bch);
}
/*}}}*/
/*{{{  static INLINE void sched_add_to_runqueue (psched_t *s, uint64_t priofinity, unsigned int rq_n, pbatch_t *bch)*/
/*
 *	adds a batch to a specific run-queue, allows for migration out
 */
static INLINE void sched_add_to_runqueue (psched_t *s, uint64_t priofinity, unsigned int rq_n, pbatch_t *bch)
{
	batch_verify_integrity (bch);

	if (PHasAffinity (priofinity)) {
		sched_add_affine_batch_to_runqueue (&(s->rq[rq_n]), bch);
	} else {
		sched_add_to_visible_runqueue (&(s->rq[rq_n]), &(s->mw[rq_n]), bch);
		att64_unsafe_set_bit (&(s->mwstate), rq_n);
	}
}
/*}}}*/
/*{{{  static INLINE pbatch_t *sched_try_pull_from_runqueue (psched_t *s, unsigned int rq_n)*/
/*
 *	attempts to grab a batch of processes from a particular run-queue, removing from the migration window if there.
 */
static INLINE pbatch_t *sched_try_pull_from_runqueue (psched_t *s, unsigned int rq_n)
{
	runqueue_t *rq = &(s->rq[rq_n]);
	pbatch_t *bch = rq->fptr;

	if (bch) {
		unsigned int window;

		rq->fptr = bch->nb;
		window = batch_window (bch);

		if (window) {
			mwindow_t *mw = &(s->mw[rq_n]);

			if ((window < 0) || (window > MWINDOW_SIZE)) {
				slick_fatal ("sched_try_pull_from_runqueue(): s=%p, rq_n=%u, window=%u", s, rq_n, window);
			}

			if (att64_cas (&(mw->data[window]), (uint64_t)bch, (uint64_t)NULL)) {
				att64_unsafe_clear_bit (&(mw->data[MWINDOW_STATE]), window + MWINDOW_BM_OFFSET);
				batch_set_clean (bch);
			} else {
				sched_release_dirty_batch (s, bch);
				bch = NULL;
			}
		}
	} else if (rq->priofinity) {
		bch = rq->pending;

		rq->priofinity = 0;
		rq->pending = sched_allocate_batch (s);
	}

	if (bch) {
		if (batch_isempty (bch)) {
			slick_fatal ("sched_try_pull_from_runqueue(): s=%p, rq_n=%u, empty batch collected..", s, rq_n);
		}

		bch->nb = (pbatch_t *)(-1);
	}

	return bch;
}
/*}}}*/
/*{{{  static INLINE void sched_push_batch (psched_t *s, uint64_t priofinity, pbatch_t *bch)*/
/*
 *	pushes a batch of processes for execution (assumes non-empty batch)
 */
static INLINE void sched_push_batch (psched_t *s, uint64_t priofinity, pbatch_t *bch)
{
	unsigned int rq_n = PPriority (priofinity);
	runqueue_t *rq = &(s->rq[rq_n]);

	if (!bch->fptr) {
		slick_fatal ("sched_push_batch(): empty batch (fptr == NULL) in scheduler at %p, batch at %p", s, bch);
	}
	if ((bch->size & ~BATCH_EMPTIED) == 0) {
		slick_fatal ("sched_push_batch(): empty batch (size == 0) in scheduler at %p, batch at %p", s, bch);
	}
	batch_verify_integrity (bch);

	if (rq->priofinity) {
		pbatch_t *p_bch;
		uint64_t p_priofinity;

		p_bch = rq->pending;
		p_priofinity = rq->priofinity;

		rq->priofinity = priofinity | BuildPriofinity (0, 1);
		rq->pending = bch;

		sched_add_to_runqueue (s, p_priofinity, rq_n, p_bch);
	} else {
		sched_release_clean_batch (s, rq->pending);
		rq->priofinity = priofinity | BuildPriofinity (0, 1);
		rq->pending = bch;
		att64_unsafe_set_bit (&(s->rqstate), rq_n);
	}
}
/*}}}*/
/*{{{  static INLINE void sched_new_current_batch (psched_t *s)*/
/*
 *	clears the scheduler's current batch
 */
static INLINE void sched_new_current_batch (psched_t *s)
{
	s->dispatches = BATCH_PPD;
	s->cbch.fptr = NULL;
	s->cbch.size = BATCH_EMPTIED;
}
/*}}}*/
/*{{{  static INLINE void sched_push_current_batch (psched_t *s)*/
/*
 *	saves the current batch, possibly splits it
 */
static INLINE void sched_push_current_batch (psched_t *s)
{
	if ((s->dispatches <= 0) && ((s->cbch.size ^ BATCH_EMPTIED) > (BATCH_EMPTIED + 1))) {
		/* split batch */
		pbatch_t *nb = sched_allocate_batch (s);

		batch_enqueue_hint (nb, sched_dequeue (s), 1);
		sched_push_batch (s, s->priofinity, nb);
	}
	sched_push_batch (s, s->priofinity, sched_save_current_batch (s));
}
/*}}}*/
/*{{{  static INLINE void sched_pick_batch (psched_t *s, unsigned int rq_n)*/
/*
 *	picks a batch from a particular run-queue
 */
static INLINE pbatch_t *sched_pick_batch (psched_t *s, unsigned int rq_n)
{
	pbatch_t *bch;

	for (;;) {
		bch = sched_try_pull_from_runqueue (s, rq_n);

		if (bch != NULL) {
			return bch;
		} else if ((s->rq[rq_n].fptr == NULL) && !(s->rq[rq_n].priofinity)) {
			att64_unsafe_clear_bit (&(s->rqstate), rq_n);
			att64_unsafe_clear_bit (&(s->mwstate), rq_n);
			return NULL;
		}
	}
}
/*}}}*/
/*{{{  static pbatch_t *sched_try_migrate_from_scheduler (psched_t *s, unsigned int rq_n)*/
/*
 *	attempts to migrate some work from a specific scheduler
 */
static pbatch_t *sched_try_migrate_from_scheduler (psched_t *s, unsigned int rq_n)
{
	mwindow_t *mw = &(s->mw[rq_n]);
	uint64_t state = att64_val (&(mw->data[MWINDOW_STATE]));
	uint64_t head, bm;
	pbatch_t *bch = NULL;

	head = MWINDOW_HEAD (state);
	bm = state >> MWINDOW_BM_OFFSET;

	while (bm && !bch) {
		uint64_t w;

		w = bm & (MWINDOW_MASK << head);
		if (w) {
			w = (uint64_t)bsr64 (w);
		} else {
			w = (uint64_t)bsr64 (bm & (MWINDOW_MASK >> ((MWINDOW_SIZE + 1) - head)));
		}

		att64_clear_bit (&(mw->data[MWINDOW_STATE]), w + MWINDOW_BM_OFFSET);
		bch = (pbatch_t *)att64_swap (&(mw->data[w]), (uint64_t)NULL);

		bm &= ~(1ULL << w);
	}

	/* Carl: don't worry about race in following line */
	if (!bm && (head == att64_val (&(mw->data[MWINDOW_STATE])))) {
		att64_clear_bit (&(s->mwstate), rq_n);
	}

	return bch;
}
/*}}}*/
/*{{{  static pbatch_t *sched_migrate_some_work (psched_t *s)*/
/*
 *	migrates some work
 */
static pbatch_t *sched_migrate_some_work (psched_t *s)
{
	bitset128_t active;
	unsigned int shift = (s->sidx & ~0x03);
	pbatch_t *bch = NULL;

	bis128_andinv (&slickss.enabled_threads, &slickss.sleeping_threads, &active);

	while (!bis128_iszero (&active) && !bch) {
		unsigned int best_n = MAX_RT_THREADS;
		unsigned int best_pri = MAX_PRIORITY_LEVELS;
		unsigned int i;

		for (i=0; i<MAX_RT_THREADS; i++) {
			unsigned int n = (i + shift) & (MAX_RT_THREADS - 1);

			if (bis128_isbitset (&active, n)) {
				uint64_t work = att64_val (&(slickss.schedulers[n]->mwstate));

				if (work) {
					unsigned int pri = bsf64 (work);

					if (pri < best_pri) {
						best_n = n;
						best_pri = pri;
					}
				} else {
					bis128_clear_bit (&active, n);
				}
			}
		}

		if (best_n < MAX_RT_THREADS) {
			bch = sched_try_migrate_from_scheduler (slickss.schedulers[best_n], best_pri);
		}
	}

	return bch;
}
/*}}}*/


/*{{{  uint64_t sched_time_now (void)*/
/*
 *	reads the current time (uses POSIX clock)
 */
uint64_t sched_time_now (void)
{
	struct timespec ts;

	if (clock_gettime (CLOCK_MONOTONIC_COARSE, &ts) != 0) {
		slick_fatal ("sched_time_now(): clock_gettime() failed with: %s", strerror (errno));
		return 0;
	}
	return (((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec);
}
/*}}}*/
/*{{{  static INLINE void sched_time_settimeoutn (psched_t *s, uint64_t now, uint64_t timeout)*/
/*
 *	sets up a particular timeout (timeout) based on current time (now)
 */
static INLINE void sched_time_settimeoutn (psched_t *s, uint64_t now, uint64_t timeout)
{
	uint64_t nsecs = timeout - now;
	struct itimerval itv;
	int ret;
	uint64_t next_alarm;

	getitimer (ITIMER_REAL, &itv);
	next_alarm = (((uint64_t)itv.it_value.tv_sec * 1000000ULL) + (uint64_t)itv.it_value.tv_usec) * 1000ULL;

	while (nsecs && (!next_alarm || (next_alarm > nsecs))) {
		itv.it_interval.tv_sec = 0;
		itv.it_interval.tv_usec = 0;
		itv.it_value.tv_sec = (uint32_t)(nsecs / 1000000000ULL);
		itv.it_value.tv_usec = (uint32_t)((nsecs % 1000000000ULL) / 1000ULL);

		ret = setitimer (ITIMER_REAL, &itv, &itv);
		if (ret < 0) {
			slick_fatal ("sched_time_settimeoutn(): setitimer() failed with [%s]", strerror (errno));
		}

		next_alarm = nsecs;
		nsecs = (((uint64_t)itv.it_value.tv_sec * 1000000ULL) + (uint64_t)itv.it_value.tv_usec) * 1000ULL;
	}
}
/*}}}*/
/*{{{  static INLINE void sched_time_settimeout (psched_t *s, uint64_t time)*/
/*
 *	sets the next timeout
 */
static INLINE void sched_time_settimeout (psched_t *s, uint64_t time)
{
	uint64_t now = sched_time_now ();

	if (time > now) {
		sched_time_settimeoutn (s, now, time);
	} else {
		sched_time_settimeoutn (s, now, now + 1);
	}
}
/*}}}*/

/*{{{  static INLINE void sched_setup_tqnode (tqnode_t *tn, workspace_t wptr, uint64_t time, int alt)*/
/*
 *	initialises a timer-queue node with workspace pointer and timeout, low-order bit set if alt (and batch dirty bit)
 */
static INLINE void sched_setup_tqnode (tqnode_t *tn, workspace_t wptr, uint64_t time, int alt)
{
	tn->wptr = (workspace_t)((uint64_t)wptr | (uint64_t)alt);
	tn->time = time;

	batch_set_dirty_value ((pbatch_t *)tn, (uint64_t)alt);
}
/*}}}*/
/*{{{  static INLINE tqnode_t *sched_init_tqnode (psched_t *s, workspace_t wptr, uint64_t time, int alt)*/
/*
 *	allocates and initialises a new timer-queue node
 */
static INLINE tqnode_t *sched_init_tqnode (psched_t *s, workspace_t wptr, uint64_t time, int alt)
{
	tqnode_t *tn = (tqnode_t *)sched_allocate_batch (s);

	sched_setup_tqnode (tn, wptr, time, alt);
	tn->scheduler = s;

	return tn;
}
/*}}}*/
/*{{{  static INLINE void sched_release_tqnode (psched_t *s, tqnode_t *tn)*/
/*
 *	frees a timer-queue node
 */
static INLINE void sched_release_tqnode (psched_t *s, tqnode_t *tn)
{
	sched_release_batch (s, (pbatch_t *)tn);
}
/*}}}*/
/*{{{  static inline tqnode_t *sched_insert_tqnode (psched_t *s, tqnode_t *node, int before, workspace_t wptr, uint64_t time, int alt)*/
/*
 *	inserts a new timer-queue node into the list of existing nodes (s->tq_[fb]ptr).
 *	returns new node.
 */
static inline tqnode_t *sched_insert_tqnode (psched_t *s, tqnode_t *node, int before, workspace_t wptr, uint64_t time, int alt)
{
	tqnode_t *tn;

	if (node->wptr || batch_isdirty ((pbatch_t *)node)) {
		/* insert new node */
		tn = sched_init_tqnode (s, wptr, time, alt);
		if (before) {
			/* before current */
			tn->next = node;
			tn->prev = node->prev;
			if (!node->prev) {
				/* front of queue */
				s->tq_fptr = tn;
				sched_time_settimeout (s, tn->time);
			} else {
				tn->prev->next = tn;
			}
			node->prev = tn;
		} else {
			/* after current */
			tn->next = node->next;
			if (!node->next) {
				/* end of queue */
				s->tq_bptr = tn;
			} else {
				tn->next->prev = tn;
			}
			node->next = tn;
			tn->prev = node;
		}
	} else {
		/* node->wptr == NULL and node is clean -- reuse it */
		tn = node;
		sched_setup_tqnode (tn, wptr, time, alt);
		if (!tn->prev) {
			sched_time_settimeout (s, tn->time);
		}
	}

	return tn;
}
/*}}}*/
/*{{{  static inline void sched_delete_tqnode (psched_t *s, tqnode_t *tn)*/
/*
 *	removes a timer-queue node from the in-scheduler list
 */
static inline void sched_delete_tqnode (psched_t *s, tqnode_t *tn)
{
	if (!tn->prev) {
		/* front of queue */
		s->tq_fptr = tn->next;
		if (!tn->next) {
			/* back of queue */
			s->tq_bptr = NULL;
		} else {
			/* not back of queue */
			s->tq_fptr->prev = NULL;
			sched_time_settimeout (s, s->tq_fptr->time);
		}
	} else {
		/* not front of queue */
		tn->prev->next = tn->next;

		if (!tn->next) {
			/* back of queue */
			s->tq_bptr = tn->prev;
		} else {
			/* not back of queue */
			tn->next->prev = tn->prev;
		}
	}
}
/*}}}*/
/*{{{  static INLINE void sched_trigger_alt_guard (psched_t *s, uint64_t val)*/
/*
 *	called when we're doing channel I/O and we find something ALTy in there
 */
static INLINE void sched_trigger_alt_guard (psched_t *s, uint64_t val)
{
	workspace_t other = (workspace_t)(val & ~1);
	uint64_t state, nstate;

	do {
		state = att64_val ((atomic64_t *)&(other[LState]));
		nstate = (state - 1) & (~(ALT_NOT_READY | ALT_WAITING));		/* decrement guard count, clear NOT_READY and WAITING flags */
	} while (!att64_cas ((atomic64_t *)&(other[LState]), state, nstate));

	if ((state & ALT_WAITING) || (nstate == 0)) {
		sched_enqueue (s, other);
	}
}
/*}}}*/
/*{{{  static inline void sched_clean_timer_queue (psched_t *s)*/
/*
 *	cleans the timer queue, removing timer-queue nodes that have been dealt with
 */
static inline void sched_clean_timer_queue (psched_t *s)
{
	tqnode_t *tn = s->tq_fptr;

	while (tn) {
		if (tn->wptr == NULL) {
			tqnode_t *next = tn->next;

			sched_delete_tqnode (s, tn);
			batch_set_clean ((pbatch_t *)tn);
			tn = next;
		} else {
			tn = tn->next;
		}
	}
}
/*}}}*/
/*{{{  static inline void sched_walk_timer_queue (psched_t *s)*/
/*
 *	walks along a non-empty timer-queue checking for expired timeouts
 */
static inline void sched_walk_timer_queue (psched_t *s)
{
	tqnode_t *tn = s->tq_fptr;
	uint64_t now = sched_time_now ();

	do {
		if (!tn->wptr || (tn->time <= now)) {
			/* expired */
			uint64_t ptr = att64_val ((atomic64_t *)&(tn->wptr));
			tqnode_t *next = tn->next;

			if ((ptr != (uint64_t)NULL) && !(ptr & 1)) {
				/* not an ALT, simply reschedule */
				tn->wptr[LTimef] = now;

				sched_enqueue (s, tn->wptr);
				sched_release_tqnode (s, tn);
			} else {
				if (ptr != (uint64_t)NULL) {
					/* challenge ALT */
					tn->time = now;
					write_barrier ();

					ptr = att64_swap ((atomic64_t *)&(tn->wptr), (uint64_t)NULL);
					if (ptr != (uint64_t)NULL) {
						sched_trigger_alt_guard (s, ptr);
					}
					compiler_barrier ();
				}
				batch_set_clean ((pbatch_t *)tn);
			}
			tn = next;
		} else {
			/* valid node, becomes new head */
			tn->prev = NULL;
			s->tq_fptr = tn;
			sched_time_settimeoutn (s, now, tn->time);
			return;
		}
	} while (tn);

	/* if we get here, timer queue is empty */
	s->tq_fptr = NULL;
	s->tq_bptr = NULL;
}

/*}}}*/
/*{{{  static INLINE void sched_check_timer_queue (psched_t *s)*/
/*
 *	checks the timer-queue for expired timeouts
 */
static INLINE void sched_check_timer_queue (psched_t *s)
{
	if (s->tq_fptr) {
		sched_walk_timer_queue (s);
	}
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
				sched_check_timer_queue (s);
			}

			while (sync & SYNC_BMAIL) {
				pbatch_t *bch = (pbatch_t *)runqueue_atomic_dequeue (&(s->bmail), 0);

				if (bch) {
					sched_push_batch (s, bch->priofinity, bch);
				} else {
					sync &= ~SYNC_BMAIL;
				}
			}

			while (sync & SYNC_PMAIL) {
				workspace_t ptr = (workspace_t)runqueue_atomic_dequeue (&(s->pmail), 1);

				if (ptr) {
					sched_enqueue (s, ptr);
				} else {
					sync &= SYNC_PMAIL;
				}
			}

			if (sync & SYNC_TQ) {
				sched_clean_timer_queue (s);
				sched_check_timer_queue (s);
			}

		}
		
		if (sched_isbatchend (s)) {
			if ((s->cbch.size > BATCH_EMPTIED) && (att64_val (&(s->rqstate)) == 0)) {
				/* scheduled-out batch, but nothing else */
				uint64_t size = s->cbch.size & ~BATCH_EMPTIED;

				s->dispatches = calculate_dispatches (size);
				s->cbch.size = size;

				w = sched_dequeue (s);
			} else {
				uint64_t tmp;
				pbatch_t *nb = NULL;

				if (!batch_empty (&(s->cbch))) {
					/* current batch still has stuff in it -- save */
					sched_push_current_batch (s);
				}

				/* pick batch from the run-queue with highest priority */
				while (nb == NULL) {
					unsigned int rq;

					tmp = att64_val (&(s->rqstate));
					if (!tmp) {
						break;
					}
					rq = bsf64 (tmp);
					nb = sched_pick_batch (s, rq);
				}

				if (nb) {
					/* got a new batch of processes to schedule :) */
					unsigned int sidx = bis128_bsf (&slickss.sleeping_threads);

					if (att64_val (&(s->mwstate)) && (sidx < 128)) {
						slick_wake_thread (slickss.schedulers[sidx], SYNC_WORK_BIT);
					}

					if (batch_isdirty (nb)) {
						slick_fatal ("slick_schedule(): s=%p, unclean batch at %p", s, nb);
					}
					sched_load_current_batch (s, nb, 0);
					w = sched_dequeue (s);

				} else if ((nb = sched_migrate_some_work (s)) != NULL) {
					/* got some work! */
					if (!batch_isdirty (nb)) {
						slick_fatal ("slick_schedule(): s=%p, migrated clean batch at %p", s, nb);
					}

					batch_verify_integrity (nb);
					s->loop = s->spin;
					sched_load_current_batch (s, nb, 1);
					w = sched_dequeue (s);
				} else {
					sched_new_current_batch (s);

					if ((s->loop & 0x0f) == 0) {
						sched_clean_timer_queue (s);
						sched_do_laundry (s);
						sched_release_excess_memory (s);
					}

					if (s->loop > 0) {
						s->loop--;
						idle_cpu ();
					} else {
						
						/* no more processes -- consider going to sleep */
						bis128_set_bit (&slickss.sleeping_threads, s->sidx);
						read_barrier ();

						if (s->tq_fptr != NULL) {
							slick_safe_pause (s);
							sched_check_timer_queue (s);
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
						s->loop = s->spin;
					}
				}
			}

		} else {
			w = sched_dequeue (s);
		}

	} while (w == NULL);

#if defined(SLICK_DEBUG) || defined(LOCAL_DEBUG)
	fprintf (stderr, "slick_schedule(): scheduling process at %p\n", w);
#endif
	/* and go! */
	__asm__ __volatile__ ("				\n" \
		"	movq	%%rax, %%rbp		\n" \
		"	movq	-8(%%rbp), %%rax	\n" \
		"	movq	0(%%rcx), %%rsp		\n" \
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
		w[LPriofinity] = psched.priofinity;
		w[LPointer] = (uint64_t)addr;

		write_barrier ();

		chanval = (uint64_t *)att64_swap ((atomic64_t *)chanptr, (uint64_t)w);
		if (!chanval) {
			/* we're in the channel now */
			slick_schedule (&psched);
		} else if ((uint64_t)chanval & 1) {
			/* something ALTy in the channel, but we're there now */
			sched_trigger_alt_guard (&psched, (uint64_t)chanval);
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
	sched_enqueue (&psched, other);
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
	sched_enqueue (&psched, other);
}
/*}}}*/
/*{{{  void os_runp (workspace_t w, workspace_t other)*/
/*
 *	run process: just pop it on the run-queue (simple enqueue for generated code)
 */
void os_runp (workspace_t w, workspace_t other)
{
	sched_enqueue (&psched, (workspace_t)((uint64_t)other & ~0x07));
}
/*}}}*/
/*{{{  void os_stopp (workspace_t w)*/
/*
 *	stop process: save return address (and priof) and schedule another
 */
void os_stopp (workspace_t w)
{
	w[LIPtr] = (uint64_t)__builtin_return_address (0);
	w[LPriofinity] = psched.priofinity;

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
	other[LPriofinity] = psched.priofinity;

	if (psched.cbch.fptr) {
		batch_verify_integrity (&(psched.cbch));
	}

	sched_enqueue_nopri (&psched, other);

	batch_verify_integrity (&(psched.cbch));
	psched.dispatches--;
	if (psched.dispatches <= 0) {
		/* force a reschedule */
		w[LPriofinity] = psched.priofinity;
		w[LIPtr] = (uint64_t)__builtin_return_address (0);

		batch_enqueue_process_front (&(psched.cbch), w);
		slick_schedule (&psched);
	}
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

		sched_enqueue (&psched, other);
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
/*{{{  void os_pause (workspace_t w)*/
/*
 *	reschedule (yield)
 */
void os_pause (workspace_t w)
{
	w[LPriofinity] = psched.priofinity;
	w[LIPtr] = (uint64_t)__builtin_return_address (0);

	sched_enqueue_nopri (&psched, w);
	slick_schedule (&psched);
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



