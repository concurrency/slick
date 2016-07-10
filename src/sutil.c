/*
 *	sutil.c -- various utility routines
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
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#include <sched.h>
#include <pthread.h>

#include "sutil.h"


/*{{{  void slick_fatal (const char *fmt, ...)*/
/*
 *	called in the event of a fatal error
 */
void slick_fatal (const char *fmt, ...)
{
	va_list ap;

	va_start (ap, fmt);
	fprintf (stderr, "slick: fatal error: ");
	vfprintf (stderr, fmt, ap);
	fprintf (stderr, "\n");
	va_end (ap);

	_exit (EXIT_FAILURE);
}
/*}}}*/


/*{{{  void *smalloc (const size_t bytes)*/
/*
 *	checked memory allocator
 */
void *smalloc (const size_t bytes)
{
	void *ptr = malloc (bytes);

	if (!ptr) {
		slick_fatal ("out of memory (allocating %lu bytes)", bytes);
	}

	return ptr;
}
/*}}}*/
/*{{{  void sfree (void *ptr)*/
/*
 *	checked memory free
 */
void sfree (void *ptr)
{
	if (!ptr) {
		slick_fatal ("attempt to free NULL pointer");
	}
	free (ptr);

	return;
}
/*}}}*/



