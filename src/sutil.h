/*
 *	sutil.h -- prototypes for utility routines
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

#ifndef __SUTIL_H
#define __SUTIL_H

extern void slick_fatal (const char *fmt, ...) __attribute__ ((format (printf, 1, 2), noreturn));
extern int slick_warning (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
extern int slick_message (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
extern int slick_cmessage (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

extern void *smalloc (const size_t bytes);
extern void sfree (void *ptr);


#endif	/* !__SUTIL_H */

