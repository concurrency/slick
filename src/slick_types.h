/*
 *	slick_types.h -- types for the scheduler
 *	Copyright (C) 2016 Fred Barnes, University of Kent <frmb@kent.ac.uk>
 *	Based largely on CCSP code by Carl Ritson, Fred Barnes, Jim Moores, and others
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

#ifndef __SLICK_TYPES_H
#define __SLICK_TYPES_H


/*{{{  assorted limiting constants*/

/* a reasonable limit for now -- mirroring number of cores */
#define MAX_RT_THREADS		(128)
#define MAX_PRIORITY_LEVELS	(32)

/* for batch scheduling */
#define BATCH_EMPTIED		(0x4000000000000000)
#define BATCH_PPD		(8)			/* per-process dispatch */
#define BATCH_PPD_SHIFT		(3)
#define BATCH_MD_MASK		(0x7f)			/* maximum dispatches as mask */

#define BATCH_DIRTY_BIT		(63)
#define BATCH_DIRTY		((uint64_t)1 << BATCH_DIRTY_BIT)

/*}}}*/
/*{{{  typedef .._t type definitions from structures and other*/
typedef uint64_t *workspace_t;		/* pointer to process workspace */

typedef struct TAG_slick_t slick_t;
typedef struct TAG_slick_ss_t slick_ss_t;
typedef struct TAG_pbatch_t pbatch_t;

typedef struct TAG_runqueue_t runqueue_t;
typedef struct TAG_mwindow_t mwindow_t;
typedef struct TAG_tqnode_t tqnode_t;

typedef struct TAG_psched_t psched_t;
typedef struct TAG_slickts_t slickts_t;

/*}}}*/
/*{{{  various virtual transputer constants/offsets*/

/* Note: quadword (8-byte) offsets */
#define LSavedPri	2		/* saved LPriofinity for parallel */
#define LCount		1		/* process count for parallel */
#define LIPtrSucc	0		/* successor IPtr for parallel */
#define LTemp		0		/* temporary slot */
#define LIPtr		-1		/* instruction pointer */
#define LLink		-2		/* link field */
#define LPriofinity	-3		/* priority/affinity */
#define LPointer	-4		/* pointer for I/O */
#define LState		-4		/* ALTer state */
#define LTLink		-5		/* timer-queue link */
#define LTimef		-6		/* timeout time */


#define ALT_ENABLING_BIT	30
#define ALT_ENABLING		(1 << ALT_ENABLING_BIT)
#define ALT_WAITING_BIT		29
#define ALT_WAITING		(1 << ALT_WAITING_BIT)
#define ALT_NOT_READY_BIT	28
#define ALT_NOT_READY		(1 << ALT_NOT_READY_BIT)
#define ALT_GUARDS		0x0000000000ffffff

#define TimeSet_p		0x0001000000000000
#define TimeNotSet_p		0x0002000000000000
#define NoneSelected_o		0x0004000000000000

/*}}}*/
/*{{{  priority and affinity constants/macros*/

#define AFFINITY_MASK		(0xffffffffffffffe0)
#define AFFINITY_SHIFT		(5)
#define PRIORITY_MASK		(0x000000000000001f)

#define PHasAffinity(x)		((x) & AFFINITY_MASK)
#define PAffinity(x)		(((x) & AFFINITY_MASK) >> AFFINITY_SHIFT)
#define PPriority(x)		((x) & PRIORITY_MASK)
#define BuildPriofinity(a,p)	((((a) << AFFINITY_SHIFT) & AFFINITY_MASK) | ((p) & PRIORITY_MASK))

/*}}}*/
/*{{{  slick_t, slickss_t: global scheduler state*/
struct TAG_slick_t {
	int rt_nthreads;		/* number of run-time threads in use (1 for each CPU by default) */
	char **prog_argv;		/* top-level program arguments (copy at top-level) */
	int prog_argc;			/* number of arguments (left) */
	int verbose;			/* non-zero if verbose */
	int binding;			/* 0=any CPU, 1=one-to-one */

	pthread_t rt_threadid[MAX_RT_THREADS];		/* thread ID for each run-time thread */
	pthread_attr_t rt_threadattr[MAX_RT_THREADS];	/* thread attributes for each run-time thread */

};

struct TAG_slick_ss_t {
	/* moderately wide bit-fields */
	bitset128_t enabled_threads;
	bitset128_t idle_threads;
	bitset128_t sleeping_threads;

	int32_t verbose;
};

/*}}}*/
/*{{{  pbatch_t: batch of processes*/

#define PBATCH_ALLOC_SIZE	(sizeof (uint64_t) * 16)

struct TAG_pbatch_t {		/* batch of processes */
	workspace_t fptr;
	workspace_t bptr;
	uint64_t size;

	struct TAG_pbatch_t *nb;	/* next batch */

	atomic64_t state;		/* migration fields */
	uint64_t priofinity;

	struct TAG_pbatch_t *prio[8];	/* barrier fields */
	uint64_t dummy[2];		/* pad to 16*8=128 bytes */
} __attribute__ ((packed));

/*}}}*/
static inline void init_pbatch_t (pbatch_t *b) /*{{{*/
{
	int i;

	b->fptr = NULL;
	b->bptr = NULL;
	b->size = 0;
	b->nb = NULL;
	att64_init (&(b->state), 0);
	b->priofinity = 0;
	for (i=0; i<8; i++) {
		b->prio[i] = NULL;
	}
}
/*}}}*/
static inline void reinit_pbatch_t (pbatch_t *b) /*{{{*/
{
	b->fptr = NULL;
	b->size = 0;
}
/*}}}*/
/*{{{  batch_... macros*/

#define batch_isdirty(b)		(att64_val (&((b)->state)) & BATCH_DIRTY)

#define batch_mark_clean(b)		do { att64_clear_bit (&((b)->state), BATCH_DIRTY_BIT); } while (0)
#define batch_mark_dirty(b)		do { att64_set_bit (&((b)->state), BATCH_DIRTY_BIT); } while (0)

#define batch_set_clean(b)		do { att64_set (&((b)->state), 0); } while (0)
#define batch_set_dirty(b)		do { att64_set (&((b)->state), BATCH_DIRTY); } while (0)

#define batch_set_dirty_value(b,v)	do { att64_set (&((b)->state), (v & 1)); } while (0)
#define batch_window(b)			(att64_val (&((b)->state)) & 0xff)
#define batch_set_window(b,w)		do { att64_set (&((b)->state), BATCH_DIRTY | (w)); } while (0)

#define batch_isempty(b)		((b)->fptr == NULL)

/*}}}*/


/*{{{  runqueue_t: batch queue*/

struct TAG_runqueue_t {
	pbatch_t *fptr;
	pbatch_t *bptr;
	uint64_t priofinity;		/* of pending batch */
	pbatch_t *pending;
} __attribute__ ((packed));


/*}}}*/
static inline void init_runqueue_t (runqueue_t *r) /*{{{*/
{
	r->fptr = NULL;
	r->bptr = NULL;
	r->priofinity = 0;
	r->pending = NULL;
}
/*}}}*/


/*{{{  constants for migration windows*/

/* for migration windows */
#define MWINDOW_BM_OFFSET	(8)
#define MWINDOW_HEAD(s)		((s) & 0xff)
#define MWINDOW_NEW_STATE(s,h)	((((s) | (0x100 << (h))) & ~0xff) | (h))
#define MWINDOW_STATE		(0)

#define MWINDOW_SIZE		(15)
#define MWINDOW_HEAD_WRAP_BIT	(4)
#define MWINDOW_MASK		0xffff


/*}}}*/
/*{{{  mwindow_t: migration window*/

struct TAG_mwindow_t {
	atomic64_t data[MWINDOW_SIZE + 1];
} __attribute__ ((packed));

/*}}}*/
static inline void init_mwindow_t (mwindow_t *w) /*{{{*/
{
	int i;

	for (i=0; i < (MWINDOW_SIZE + 1); i++) {
		if (i == MWINDOW_STATE) {
			att64_init (&(w->data[i]), (uint64_t)0);
		} else {
			att64_init (&(w->data[i]), (uint64_t)NULL);
		}
	}
}

/*}}}*/


/*{{{  tqnode_t: timer-queue node*/

struct TAG_tqnode_t {
	uint64_t time;
	tqnode_t *next;
	tqnode_t *prev;

	pbatch_t *bnext;		/* must match 'nb' in pbatch_t */
	atomic64_t state;		/* must match 'state' in pbatch_t */

	psched_t *scheduler;
	workspace_t wptr;

} __attribute__ ((packed));

/*}}}*/
static inline void init_tqnode_t (tqnode_t *t) /*{{{*/
{
	t->time = 0;
	t->next = NULL;
	t->prev = NULL;

	/* NOTE: don't need to set bnext or state */

	t->scheduler = NULL;
	t->wptr = NULL;
}

/*}}}*/


/*{{{  scheduler sync flags (for psched_t.sync)*/

#define SYNC_INTR_BIT	1
#define SYNC_TIME_BIT	2
#define SYNC_BMAIL_BIT	4
#define SYNC_PMAIL_BIT	5
#define SYNC_WORK_BIT	6
#define SYNC_TQ_BIT	7

#define SYNC_INTR	(1 << SYNC_INTR_BIT)
#define SYNC_TIME	(1 << SYNC_TIME_BIT)
#define SYNC_BMAIL	(1 << SYNC_BMAIL_BIT)
#define SYNC_PMAIL	(1 << SYNC_PMAIL_BIT)
#define SYNC_MAIL	(SYNC_BMAIL | SYNC_PMAIL)
#define SYNC_WORK	(1 << SYNC_WORK_BIT)
#define SYNC_TQ		(1 << SYNC_TQ_BIT)


/*}}}*/
/*{{{  psched_t: per-scheduler-thread state*/
struct TAG_psched_t {
	/* offset +0 */
	void *saved_sp;				/* saved C state: must stay here */
	void *saved_bp;
	void *saved_r10;
	void *saved_r11;

	/* scheduler constants */
	int32_t sidx;				/* which particular thread we are */
	int32_t dummy0;
	bitset128_t id;				/* 1 << sidx */

	int32_t signal_in;			/* sleep/wake-up pipe FDs */
	int32_t signal_out;

	uint64_t spin;
	slick_t *sptr;				/* pointer to global state */

	uint64_t dummy1[CACHELINE_LWORDS] CACHELINE_ALIGN;
	
	/* local scheduler state */
	int64_t dispatches CACHELINE_ALIGN;
	uint64_t priofinity;
	uint64_t loop;
	atomic64_t rqstate;

	pbatch_t *free;
	pbatch_t *laundry;

	tqnode_t *tq_fptr;
	tqnode_t *tq_bptr;

	pbatch_t cbch CACHELINE_ALIGN;		/* current batch */
	runqueue_t rq[MAX_PRIORITY_LEVELS];
	uint64_t dummy2[CACHELINE_LWORDS];

	/* globally accessed scheduler state */
	atomic32_t sync CACHELINE_ALIGN;
	int32_t dummy3;
	uint64_t dummy4[CACHELINE_LWORDS] CACHELINE_ALIGN;

	runqueue_t bmail CACHELINE_ALIGN;	/* batch mail */
	uint64_t dummy5[CACHELINE_LWORDS] CACHELINE_ALIGN;

	runqueue_t pmail CACHELINE_ALIGN;	/* process mail */
	uint64_t dummy7[CACHELINE_LWORDS] CACHELINE_ALIGN;

	atomic64_t mwstate CACHELINE_ALIGN;	/* migration window state */
	uint64_t dummy8[CACHELINE_LWORDS] CACHELINE_ALIGN;

	mwindow_t mw[MAX_PRIORITY_LEVELS];	/* migration windows */

} __attribute__ ((packed));


/*}}}*/
static inline void init_psched_t (psched_t *s) /*{{{*/
{
	int i;

	s->sidx = -1;
	bis128_init (&(s->id), 0);
	s->signal_in = -1;
	s->signal_out = -1;
	s->spin = 0;
	s->sptr = NULL;

	s->dispatches = 0;
	s->priofinity = 0;
	s->loop = 0;
	att64_init (&(s->rqstate), 0);

	s->free = NULL;
	s->laundry = NULL;

	s->tq_fptr = NULL;
	s->tq_bptr = NULL;

	init_pbatch_t (&(s->cbch));

	for (i=0; i<MAX_PRIORITY_LEVELS; i++) {
		init_runqueue_t (&(s->rq[i]));
	}

	att32_init (&(s->sync), 0);
	
	init_runqueue_t (&(s->bmail));
	init_runqueue_t (&(s->pmail));

	att64_init (&(s->mwstate), 0);
	for (i=0; i<MAX_PRIORITY_LEVELS; i++) {
		init_mwindow_t (&(s->mw[i]));
	}
}
/*}}}*/
/*{{{  slickts_t: startup data for threads*/
struct TAG_slickts_t {
	int thridx;				/* thread index */
	slick_t *sptr;

	void *initial_ws;			/* pointers to workspace and code */
	void (*initial_proc)(void);
} __attribute__ ((packed));			/* used during thread start-up */


/*}}}*/

#endif	/* !__SLICK_TYPES_H */

