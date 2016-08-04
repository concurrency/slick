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


/*}}}*/
/*{{{  typedef .._t type definitions from structures and other*/
typedef uint64_t *workspace_t;		/* pointer to process workspace */

typedef struct TAG_slick_t slick_t;
typedef struct TAG_slick_ss_t slick_ss_t;

typedef struct TAG_pbatch_t pbatch_t;

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

#define PBATCH_ALLOC_SIZE	(sizeof (uint64) * 16)

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

/* scheduler sync flags (for psched_t.sync) */
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
#define SYNC_WORK	(1 << SYNC_WORK_BIT)
#define SYNC_TQ		(1 << SYNC_TQ_BIT)

typedef struct TAG_psched_t {		/* scheduler structure */
	workspace_t fptr;		/* run-queue for this scheduler */
	workspace_t bptr;

	workspace_t tptr;		/* timer-queue for this scheduler */

	pbatch_t *cbch;			/* current batch (also priofinity) */
	pbatch_t *fbch;			/* batch queue */
	pbatch_t *bbch;

	/* offset +48 */
	void *saved_sp;			/* saved C state */
	void *saved_bp;
	void *saved_r10;
	void *saved_r11;

	slick_t *sptr;			/* pointer to global state */
	int32_t sidx;			/* which particular RT thread we are [0..] */
	int32_t dummy0;

	int32_t signal_in;		/* sleep/wake-up pipe FDs */
	int32_t signal_out;

	atomic32_t sync;
	int32_t dummy1;
} __attribute__ ((packed)) psched_t;

typedef struct TAG_slickts_t {
	int thridx;			/* thread index */
	slick_t *sptr;

	void *initial_ws;		/* pointers to workspace and code */
	void (*initial_proc)(void);
} __attribute__ ((packed)) slickts_t;				/* used during thread start-up */

#endif	/* !__SLICK_TYPES_H */

