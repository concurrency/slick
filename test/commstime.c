/*
 *	commstime.c -- minimal wrapper for commstime test program
 *	Copyright (C) 2016 Fred Barnes, University of Kent <frmb@kent.ac.uk>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>

#include <sched.h>
#include <pthread.h>

#include "slick.h"


extern int64_t ow_commstime;			/* bytes of workspace required */
extern void (*o_commstime_startup)(void);	/* synthetic compiler-generated entry point */


int main (int argc, char **argv)
{
	void *ws, *wstop;

	if (slick_init ((const char **)argv, argc)) {
		fprintf (stderr, "commstime: oops, failed to initialise scheduler\n");
		exit (EXIT_FAILURE);
	}

	ws = malloc (ow_commstime);
	wstop = ws + (int)(ow_commstime - sizeof (uint64_t));
	fprintf (stderr, "commstime: allocated %lu bytes workspace at %p (adjusted %p)\n", ow_commstime, ws, wstop);
	fprintf (stderr, "commstime: entry-point is at %p\n", o_commstime_startup);

	slick_startup (wstop, o_commstime_startup);

	return 0;
}


