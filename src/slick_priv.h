/*
 *	slick_priv.h -- definitions for some parts of the scheduler
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

#ifndef __SLICK_PRIV_H
#define __SLICK_PRIV_H

/* global scheduler structure */
extern slick_ss_t slickss;

/* in sched.c */
extern uint64_t sched_time_now (void);
extern void slick_wake_thread (psched_t *s, unsigned int sync_bit);

/* in slick.c */
extern void slick_assert (const int v, const char *file, const int line);


#endif	/* !__SLICK_PRIV_H */

