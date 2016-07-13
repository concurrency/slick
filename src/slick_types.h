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
#define LTState		-5		/* timeout state field */
#define LTimef		-6		/* timeout time */


typedef struct TAG_slick_t {
	int rt_nthreads;		/* number of run-time threads in use (1 for each CPU by default) */
	char **prog_argv;		/* top-level program arguments (copy at top-level) */
	int prog_argc;			/* number of arguments (left) */
	int verbose;			/* non-zero if verbose */
	int binding;			/* 0=any CPU, 1=one-to-one */

	pthread_t rt_threadid[MAX_RT_THREADS];		/* thread ID for each run-time thread */
	pthread_attr_t rt_threadattr[MAX_RT_THREADS];	/* thread attributes for each run-time thread */
	cpu_set_t rt_threadaff[MAX_RT_THREADS];		/* CPU affinity for each run-time thread */
} slick_t;


typedef struct TAG_pbatch_t {		/* batch of processes */
	struct TAG_pbatch_t *nb;	/* next batch */
	workspace_t fptr;
	workspace_t bptr;
	/* FIXME: more things here */
	uint64_t dummy[5];		/* pack out to 64 bytes */
} __attribute__ ((packed)) pbatch_t;

typedef struct TAG_psched_t {		/* scheduler structure */
	workspace_t fptr;		/* run-queue for this scheduler */
	workspace_t bptr;

	workspace_t tptr;		/* timer-queue for this scheduler */

	pbatch_t *fbch;			/* batch queue */
	pbatch_t *bbch;

	/* offset +40 */
	void *saved_sp;			/* saved C state */
	void *saved_bp;
	void *saved_r10;
	void *saved_r11;

	slick_t *sptr;			/* pointer to global state */
	int32_t sidx;			/* which particular RT thread we are */
	int32_t dummy;
} __attribute__ ((packed)) psched_t;

typedef struct TAG_slickts_t {
	int thridx;			/* thread index */
	slick_t *sptr;

	void *initial_ws;		/* pointers to workspace and code */
	void (*initial_proc)(void);
} __attribute__ ((packed)) slickts_t;				/* used during thread start-up */

#endif	/* !__SLICK_TYPES_H */

