/*
 *	procring.c -- minimal wrapper for process-ring test program
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


extern int64_t ow_procring;			/* bytes of workspace required */
extern void o_procring_startup (void);		/* synthetic compiler-generated entry point */


int main (int argc, char **argv)
{
	void *ws, *wstop;

	if (slick_init ((const char **)argv, argc)) {
		fprintf (stderr, "procring: oops, failed to initialise scheduler\n");
		exit (EXIT_FAILURE);
	}

	ws = malloc (ow_procring);
	wstop = ws + (int)(ow_procring - sizeof (uint64_t));
	fprintf (stderr, "procring: allocated %lu bytes workspace at %p (adjusted %p)\n", ow_procring, ws, wstop);
	fprintf (stderr, "procring: entry-point is at %p (0x%16.16lx)\n", o_procring_startup, (uint64_t)o_procring_startup);

	slick_startup (wstop, o_procring_startup);

	return 0;
}


