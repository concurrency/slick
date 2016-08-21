/*
 *	slick.c -- scheduler startup/interface
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
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#include <sched.h>
#include <pthread.h>

#include "slick.h"
#include "atomics.h"
#include "slick_types.h"
#include "slick_priv.h"
#include "sutil.h"

extern void *slick_threadentry (void *arg);

/*{{{  private types*/

/*}}}*/
/*{{{  private data*/
static slick_t slick;
static slickts_t threadargs[MAX_RT_THREADS];

/*}}}*/
/*{{{  public data*/
slick_ss_t slickss;


/*}}}*/


/*{{{  static void slick_sigalrm (int sig)*/
/*
 *	SIGALRM signal handler -- when the timer goes off
 */
static void slick_sigalrm (int sig)
{
	/* signal all enabled threads */
	int i;

	for (i=0; i<slick.rt_nthreads; i++) {
		slick_wake_thread (slickss.schedulers[i], SYNC_TIME_BIT);
	}

	signal (SIGALRM, slick_sigalrm);
}
/*}}}*/
/*{{{  static void slick_sig_fatal (int sig)*/
/*
 *	signal handler for various fatal signals
 */
static void slick_sig_fatal (int sig)
{
	if (sig == SIGSEGV) {
		slick_fatal ("Segmentation fault.");
	} else {
		slick_fatal ("Range error / STOP executed (signal %d)", sig);
	}
}
/*}}}*/
/*{{{  static void slick_sigfpe (int sig)*/
/*
 *	SIGFPE signal handler -- when we get a floating-point exception
 */
static void slick_sigfpe (int sig)
{
	slick_fatal ("Floating-point exception.");
}
/*}}}*/



/*{{{  int slick_init (const char **argv, const int argc)*/
/*
 *	called to initialise the scheduler (command-line arguments given)
 *	returns 0 on success, non-zero on error
 */
int slick_init (const char **argv, const int argc)
{
	char *ch;
	int i;

	memset (&slick, 0, sizeof (slick_t));
	memset (&slickss, 0, sizeof (slick_ss_t));

	if (argc == 0) {
		/*{{{  create some default arguments (incase anyone dereferences argv[0] assumingly) */
		slick.prog_argv = (char **)smalloc (2 * sizeof (char *));
		slick.prog_argv[0] = "SlickScheduler";
		slick.prog_argv[1] = NULL;

		slick.prog_argc = 0;

		/*}}}*/
	} else {
		/*{{{  copy over argument pointers, removing any for us*/
		int i, j;
		const char **av_walk;
		char **sav_walk;

		slick.prog_argv = (char **)smalloc ((argc + 1) * sizeof (char *));
		slick.prog_argv[0] = (char *)argv[0];			/* program name fixed */

		av_walk = argv + 1;
		sav_walk = slick.prog_argv + 1;
		for (i=(argc - 1), j=1; i; i--, av_walk++) {
			/* slight grotty argument processing (expect things as single --rt-..[=val]) */
			if (!strncmp (*av_walk, "--rt-", 5)) {
				/* probably for us */
				if (!strncmp (*av_walk + 5, "verbose", 7)) {
					/*{{{  --rt-verbose[=NN]*/
					if ((*av_walk)[12] == '\0') {
						slick.verbose++;			/* more verbosity */
					} else if ((*av_walk)[12] == '=') {
						int tmp;

						if (sscanf (*av_walk + 13, "%d", &tmp) == 1) {
							slick.verbose = tmp;		/* explicit */
						} else {
							slick_warning ("garbled command-line argument [%s]", *av_walk);
						}
					} else {
						slick_warning ("garbled command-line argument [%s]", *av_walk);
					}
					/*}}}*/
				} else if (!strncmp (*av_walk + 5, "nthreads", 8)) {
					/*{{{  --rt-nthreads=NN*/
					if ((*av_walk)[13] == '=') {
						int tmp;

						if (sscanf (*av_walk + 14, "%d", &tmp) == 1) {
							if ((tmp < 0) || (tmp > MAX_RT_THREADS)) {
								slick_warning ("unsupported number of threads (%d), expect [1..%d]", tmp, MAX_RT_THREADS);
							} else {
								slick.rt_nthreads = tmp;
							}
						} else {
							slick_warning ("garbled command-line argument [%s]", *av_walk);
						}
					} else {
						slick_warning ("garbled command-line argument [%s]", *av_walk);
					}
					/*}}}*/
				} else if (!strcmp (*av_walk + 5, "help")) {
					/*{{{  --rt-help*/
					slick_cmessage (\
						"slick run-time scheduler options (--rt-help):\n" \
						"    --rt-verbose[=N]          set verbosity level\n" \
						"    --rt-nthreads=N           fix number of run-time threads (also )\n" \
						"    --rt-help                 this help\n");

					/* bail out and say we failed */
					return -1;

					/*}}}*/
				} else {
					/*{{{  probably for us, but emit a warning and add it to the program*/
					slick_warning ("passing argument [%s] to application", *av_walk);

					*sav_walk = (char *)*av_walk;
					sav_walk++, j++;
					/*}}}*/
				}
			} else {
				/* not for us, pass through to program */
				*sav_walk = (char *)*av_walk;
				sav_walk++, j++;
			}
		}

		*sav_walk = NULL;
		slick.prog_argc = j;

		/*}}}*/
	}

	if (slick.rt_nthreads == 0) {
		/*{{{  see if number of run-time threads is in the environment (SLICKRTNTHREADS)*/
		ch = getenv ("SLICKRTNTHREADS");
		if (ch) {
			if (sscanf (ch, "%d", &slick.rt_nthreads) != 1) {
				slick_warning ("not using environment variable SLICKRTNTHREADS, not an integer [%s]", ch);
				slick.rt_nthreads = 0;		/* default */
			}
		}

		/*}}}*/
	}

	/* find out how many (real/HT) processors we have, put in slickss.ncpus */
	slickss.ncpus = 0;

	if (slickss.ncpus == 0) {
		/*{{{  see if the number of CPUS is in the environment (SLICKRTNCPUS)*/
		ch = getenv ("SLICKRTNCPUS");
		if (ch) {
			if (sscanf (ch, "%d", &slickss.ncpus) != 1) {
				slick_warning ("not using environment variable SLICKRTNCPUS, not an integer [%s]", ch);
				slickss.ncpus = 0;		/* default */
			}
		}

		/*}}}*/
	}

#ifdef _SC_NPROCESSORS_ONLN
	if (slickss.ncpus == 0) {
		/*{{{  try and figure out how many CPUs we have via sysconf*/
		long nrt = sysconf (_SC_NPROCESSORS_ONLN);

		if (nrt < 0) {
			nrt = 0;
		} else if (nrt > MAX_RT_THREADS) {
			slick_warning ("more CPUs (%d) online than MAX_RT_THREADS (%d)!", (int)nrt, MAX_RT_THREADS);
			nrt = MAX_RT_THREADS;
		}
		slickss.ncpus = (int32_t)nrt;

		/*}}}*/
	}
#endif

	if (slickss.ncpus == 0) {
		/*{{{  try and figure out by reading /proc/cpuinfo*/
		/*
		 *	Note: this just counts up how many lines that look like "processor : NN"
		 */

		int fd;
		struct stat st_buf;
		void *maddr;
		char *mdata;
		int flen, offs;
		int cpucount = 0;

		fd = open ("/proc/cpuinfo", O_RDONLY);
		if (fd < 0) {
			goto skip_cpuinfo;
		}
		if (fstat (fd, &st_buf) < 0) {
			close (fd);
			goto skip_cpuinfo;
		}

		/* attempt to map it */
		maddr = mmap (NULL, (size_t)st_buf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (maddr == (void *)-1) {
			/* failed to map */
			close (fd);
			goto skip_cpuinfo;
		}

		mdata = (char *)maddr;
		flen = (int)st_buf.st_size;		/* assert: it's not massive */
		
		for (offs=0; offs<flen; offs++) {
			int sol, llen;

			for (sol=offs; (offs<flen) && (mdata[offs] != '\n'); offs++);
			if (offs == flen) {
				/* ran out */
				break;			/* for() */
			}
			llen = offs - sol;

			if ((llen > 10) && !strncmp (mdata + sol, "processor", 9)) {
				char *ch;
				int dummy;
				
				for (ch = mdata + (sol + 9); (*ch == '\t') || (*ch == ' '); ch++);
				if (*ch != ':') {
					continue;	/* for() */
				}
				for (ch++; (*ch == '\t') || (*ch == ' '); ch++);
				if (sscanf (ch, "%d", &dummy) == 1) {
					/* match! */
					cpucount++;
				}
			}
		}

		/* unmap and close */
		munmap (maddr, (size_t)st_buf.st_size);
		close (fd);

		slickss.ncpus = (int32_t)cpucount;
		/*}}}*/
	}
skip_cpuinfo:

	if (slick.rt_nthreads == 0) {
		slick.rt_nthreads = slickss.ncpus;
	}

	if (slick.rt_nthreads > MAX_RT_THREADS) {
		slick_warning ("more threads (%d) than MAX_RT_THREADS (%d)!", slick.rt_nthreads, MAX_RT_THREADS);
		slick.rt_nthreads = MAX_RT_THREADS;
	} else if (slick.rt_nthreads == 0) {
		slick_fatal ("could not determine number of run-time threads (nor CPUs); please set SLICKRTNTHREADS and SLICKRTNCPUS.");
	} else if (slickss.ncpus == 0) {
		slick_fatal ("could not determine number of processors, please set SLICKRTNCPUS.");
	}

	/* Note: number of run-time threads may differ from number of CPUs */

	if (slick.verbose) {
		slick_message ("going to use %d run-time threads", slick.rt_nthreads);
	}

	/* initialise some fields in here */
	slickss.verbose = slick.verbose;

	bis128_init (&(slickss.enabled_threads), 0);
	bis128_init (&(slickss.idle_threads), 0);
	bis128_init (&(slickss.sleeping_threads), 0);

	for (i=0; i<MAX_RT_THREADS; i++) {
		slickss.schedulers[i] = NULL;
	}

	return 0;
}
/*}}}*/
/*{{{  void slick_startup (void *ws, void (*proc)(void))*/
/*
 *	create run-time threads and start application
 *	Note: the workspace is intended to point at the topmost (but not beyond) 64-bit word
 */
void slick_startup (void *ws, void (*proc)(void))
{
	int i;

	/* sort out signal handling */
	signal (SIGALRM, slick_sigalrm);
	signal (SIGCHLD, SIG_IGN);		/* ignore exiting child threads */
	signal (SIGILL, slick_sig_fatal);
	signal (SIGBUS, slick_sig_fatal);
	signal (SIGFPE, slick_sigfpe);

	for (i=0; i<slick.rt_nthreads; i++) {
		threadargs[i].thridx = i;
		threadargs[i].sptr = &slick;
		threadargs[i].initial_ws = NULL;
		threadargs[i].initial_proc = NULL;
	}

	threadargs[0].initial_ws = ws;
	threadargs[0].initial_proc = proc;

	// slick.rt_nthreads
	for (i=0; i<slick.rt_nthreads; i++) {
		pthread_attr_init (&slick.rt_threadattr[i]);

		if (pthread_create (&slick.rt_threadid[i], &slick.rt_threadattr[i], slick_threadentry, &threadargs[i])) {
			slick_fatal ("failed to create run-time thread [%s]", strerror (errno));
		}

		if (!i) {
			int count = 10;

			/* first thread is special, wait for it to set the enabled bit */
			while (!(bis128_val_lo (&slickss.enabled_threads) & 1)) {
				struct timespec ts = {tv_sec: 0, tv_nsec: 10000000};		/* 10ms */

				sched_yield ();
				nanosleep (&ts, NULL);
				count--;
				if (!count) {
					slick_fatal ("waited too long for first thread to become enabled.");
				}
			}
		}
	}

#if 1
slick_message ("slick_startup(): here, having created %d threads.. :)", slick.rt_nthreads);
#endif

	// slick.rt_nthreads
	for (i=0; i<slick.rt_nthreads; i++) {
		void *result;

		pthread_join (slick.rt_threadid[i], &result);
	}

	return;
}
/*}}}*/



