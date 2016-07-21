/*
 *	slick_types.h -- types for the scheduler
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

#ifndef __SLICK_TYPES_H
#define __SLICK_TYPES_H


/* a reasonable limit for now -- mirroring number of cores */
#define MAX_RT_THREADS	(128)


typedef uint64_t *workspace_t;		/* pointer to process workspace */

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


typedef struct TAG_slick_t {
	int rt_nthreads;		/* number of run-time threads in use (1 for each CPU by default) */
	char **prog_argv;		/* top-level program arguments (copy at top-level) */
	int prog_argc;			/* number of arguments (left) */
	int verbose;			/* non-zero if verbose */
	int binding;			/* 0=any CPU, 1=one-to-one */

	pthread_t rt_threadid[MAX_RT_THREADS];		/* thread ID for each run-time thread */
	pthread_attr_t rt_threadattr[MAX_RT_THREADS];	/* thread attributes for each run-time thread */

} slick_t;

typedef struct TAG_slick_ss_t {
	atomic32_t nactive;		/* number of active threads */
	atomic32_t nwaiting;		/* number waiting for something (timeout, etc.) */
	int32_t verbose;
} slick_ss_t;


typedef struct TAG_pbatch_t {		/* batch of processes */
	struct TAG_pbatch_t *nb;	/* next batch */
	workspace_t fptr;
	workspace_t bptr;
	uint64_t priority;
	cpu_set_t cpuset;		/* 128 bytes mostly */
} __attribute__ ((packed)) pbatch_t;

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
	int32_t sidx;			/* which particular RT thread we are */
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

