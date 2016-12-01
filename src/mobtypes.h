/*
 *	mobtypes.h -- mobile type descriptors for slick
 *	Copyright (C) 2016 Fred Barnes <frmb@kent.ac.uk>
 *	Based on mobile_types.h (2nd gen) from KRoC/CCSP by Carl Ritson, 2007.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef __MOBTYPES_H
#define __MOBTYPES_H

/*
 *	The mobile type is a structure contained in a single
 *	unsigned machine word (64-bit).
 *
 *	N                     5             1             0 (LSB)
 *	| type specific flags | type number | simple flag |
 *
 *	If the LSB is set then the word is a self-contained description
 *	of a mobile type (a "simple" mobile type).  If the simple flag is
 *	0 then the word is a pointer to a mobile type descriptor (described
 *	below).
 *
 *	For simple types, bits [1..4] define the type number [0..15] and
 *	remaining bits, [5..N-1] are flags specific to that type.
 *
 *	Structured mobile types are described by mobile type descriptors,
 *	see type 6 below for a description of these.
 */

#define	MT_SIMPLE	0x1

#define	MT_TYPE_SHIFT	1
#define MT_TYPE(X)	(((X) >> MT_TYPE_SHIFT) & 0xf)
#define MT_MAKE_TYPE(T)	((T) << MT_TYPE_SHIFT)

#define	MT_FLAGS_SHIFT	5
#define MT_FLAGS(X)	((X) >> MT_FLAGS_SHIFT)

#define MTType		(-1)				/* offset of type word */


/*{{{  type 0: numeric/basic data*/
/*
 *	flag bits [0..2] code the type:
 *	
 *	0 = unsigned integer
 *	1 = signed integer
 *	2 = floating-point
 *	3 = string
 *	4-7 = reserved
 *
 *	for integer and floating-point types, flag bits [3..10] specify the bit-width (e.g. 8, 16, 32, 64, 128).
 */

#define MT_NUM		0
#define	MT_NUM_TYPE(X)	mt_num_type(MT_FLAGS(X))
#define MT_MAKE_NUM(I)	(MT_SIMPLE | MT_MAKE_TYPE(MT_NUM) | ((I) << MT_FLAGS_SHIFT))

#define MT_NUM_UINT	0
#define MT_NUM_SINT	1
#define MT_NUM_FP	2
#define MT_NUM_STRING	3

#ifdef MT_DEFINES
static inline uint64_t mt_num_type (uint64_t flags) /*{{{*/
{
	if ((flags & 0x07) < 3) {
		/* integer or FP */
		return (flags & 0x3ff);
	} else {
		return (flags & 0x07);
	}
}
/*}}}*/

#endif	/* MT_DEFINES */


/*}}}*/


#endif	/* !__MOBTYPES_H */

