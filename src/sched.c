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

	init_psched_t (&psched);

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
static INLINE int64_t calculate_dispatches (uint64_t size) /*{{{*/
{
	size <<= BATCH_PPD_SHIFT;
	size |= (one_if_z64 (size, (~BATCH_MD_MASK)) - 1);
	return (int64_t)(size & BATCH_MD_MASK);

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
/*{{{  static void enqueue (workspace_t w, psched_t *s)*/
/*
 *	enqueues a process
 */
static void enqueue (workspace_t w, psched_t *s)
{
	uint64_t priofinity = w[LPriofinity];

	if (s->priofinity == priofinity) {
		batch_enqueue_process (&(s->cbch), w);
	} else {
		// enqueue_far_process (w, s, priofinity);
		slick_fatal ("unimplemented in enqueue()");
	}
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
/*{{{  static workspace_t dequeue (psched_t *s)*/
/*
 *	dequeues a process from the current scheduler's batch
 */
static workspace_t dequeue (psched_t *s)
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

		batch_enqueue_hint (nb, dequeue (s), 1);
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
		
		if (sched_isbatchend (s)) {
			if ((s->cbch.size > BATCH_EMPTIED) && (att64_val (&(s->rqstate)) == 0)) {
				/* scheduled-out batch, but nothing else */
				uint64_t size = s->cbch.size & ~BATCH_EMPTIED;

				s->dispatches = calculate_dispatches (size);
				s->cbch.size = size;

				w = dequeue (s);
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
					rq = bsf64 (tmp);
					nb = sched_pick_batch (s, rq);
				}

				if (nb) {
					/* got a new batch of processes to schedule :) */
					/* FIXME: maybe wake another thread if migration available */

					if (batch_isdirty (nb)) {
						slick_fatal ("slick_schedule(): s=%p, unclean batch at %p", s, nb);
					}
					sched_load_current_batch (s, nb, 0);
					w = dequeue (s);

					/* ELSE: if we can migrate some work from elsewhere, do so */
				} else {
					sched_new_current_batch (s);

					if ((s->loop & 0x0f) == 0) {
						/* FIXME: some tidying up please */
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
						s->loop = s->spin;
					}
				}
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
		w[LPriofinity] = psched.priofinity;
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



