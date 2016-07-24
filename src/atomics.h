/*
 *	atomics.h -- various inline assembly macros
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

/*
 * Based approximately on CCSP's i386/atomics.h (mostly Carl) with parts
 * of that based on include/asm-i386/spinlock.h in the Linux kernel.
 *
 * and some stuff from i386/asm_ops.h
 */

#ifndef __ATOMICS_H
#define __ATOMICS_H

#ifndef __GNUC__
#error Need GNU C for this scheduler
#endif

/* borrowed from Carl's inlining.h in CCSP */
#define INLINE __attribute__((always_inline)) inline


#define memory_barrier() __asm__ __volatile__ ("mfence\n" : : : "memory");
#define read_barrier() __asm__ __volatile__ ("lfence\n" : : : "memory");
#define write_barrier() __asm__ __volatile__ ("sfence\n" : : : "memory");

/* serialise -- strongest barrier */
static INLINE void serialise (void) {
	__asm__ __volatile__ ("				\n"
			"	movl	$0, %%eax	\n"
			"	cpuid			\n"
			: : : "cc", "memory", "rax", "rbx", "rcx", "rdx");
}


typedef struct TAG_atomic32_t {
	volatile uint32_t value;
} __attribute__ ((packed)) atomic32_t;

typedef struct TAG_atomic64_t {
	volatile uint64_t value;
} __attribute__ ((packed)) atomic64_t;

typedef struct TAG_bitset128_t {
	volatile uint64_t values[2];
} __attribute__ ((packed)) bitset128_t;

/*
 * This particular wierdness is used to 'tell' GCC about what memory, "m" constraints might be using,
 * to avoid having to flush registers to memory before the operation.  Strictly necessary here..?
 * Or maybe I didn't understand it quite right..
 */

typedef struct { uint32_t a[100]; } __dummy_atomic32_t;
typedef struct { uint64_t a[100]; } __dummy_atomic64_t;
typedef struct { uint64_t a[100]; } __dummy_bitset128_t;

#define __dummy_atomic32(val) (*(__dummy_atomic32_t *)(val))
#define __dummy_atomic64(val) (*(__dummy_atomic64_t *)(val))


#define att32_init(X,V) do { (X)->value = (V); } while (0)
#define att64_init(X,V) do { (X)->value = (V); } while (0)

#define bis128_init(X,B) do { (X)->values[0] = ((B) ? 0xffffffffffffffff : 0); (X)->values[1] = ((B) ? 0xffffffffffffffff : 0); } while (0)

static INLINE uint32_t att32_val (atomic32_t *atval) /*{{{*/
{
	uint32_t result;

	__asm__ __volatile__ ("				\n"
			"	movl	%1, %0		\n"
			: "=r" (result)
			: "m" (__dummy_atomic32 (atval))
			);

	return result;
}
/*}}}*/
static INLINE uint64_t att64_val (atomic64_t *atval) /*{{{*/
{
	uint64_t result;

	__asm__ __volatile__ ("				\n"
			"	movq	%1, %0		\n"
			: "=r" (result)
			: "m" (__dummy_atomic64 (atval))
			);

	return result;
}
/*}}}*/

static INLINE void att32_set (atomic32_t *atval, uint32_t value) /*{{{*/
{
	__asm__ __volatile__ ("				\n"
			"	movl	%1, %0		\n"
			: "=m" (__dummy_atomic32 (atval))
			: "r" (value)
			);
}
/*}}}*/
static INLINE void att32_inc (atomic32_t *atval) /*{{{*/
{
	__asm__ __volatile__ ("					\n"
			"	lock; addl	$1, %0		\n"
			: "+m" (__dummy_atomic32 (atval))
			: /* no inputs */
			: "cc"
			);
}
/*}}}*/
static INLINE void att32_dec (atomic32_t *atval) /*{{{*/
{
	__asm__ __volatile__ ("					\n"
			"	lock; subl	$1, %0		\n"
			: "+m" (__dummy_atomic32 (atval))
			: /* no inputs */
			: "cc"
			);
}
/*}}}*/
static INLINE unsigned int att32_dec_z (atomic32_t *atval) /*{{{*/
{
	unsigned char result;

	__asm__ __volatile__ ("					\n"
			"	lock; subl	$1, %0		\n"
			"	setz		%1		\n"
			: "+m" (__dummy_atomic32 (atval)), "=q" (result)
			: /* no inputs */
			: "cc"
			);
	return (unsigned int)result;
}
/*}}}*/
static INLINE void att32_add (atomic32_t *atval, uint32_t value) /*{{{*/
{
	__asm__ __volatile__ ("					\n"
			"	lock; addl	%1, %0		\n"
			: "+m" (__dummy_atomic32 (atval))
			: "ir" (value)
			: "cc"
			);
}
/*}}}*/
static INLINE void att32_sub (atomic32_t *atval, uint32_t value) /*{{{*/
{
	__asm__ __volatile__ ("					\n"
			"	lock; subl	%1, %0		\n"
			: "+m" (__dummy_atomic32 (atval))
			: "ir" (value)
			: "cc"
			);
}
/*}}}*/
static INLINE unsigned int att32_sub_z (atomic32_t *atval, uint32_t value) /*{{{*/
{
	unsigned char result;

	__asm__ __volatile__ ("					\n"
			"	lock; subl	%2, %0		\n"
			"	setz		%1		\n"
			: "+m" (__dummy_atomic32 (atval)), "=q" (result)
			: "ir" (value)
			: "cc"
			);

	return (unsigned int)result;
}
/*}}}*/
static INLINE void att32_or (atomic32_t *atval, uint32_t bits) /*{{{*/
{
	__asm__ __volatile__ ("					\n"
			"	lock; orl	%1, %0		\n"
			: "+m" (__dummy_atomic32 (atval))
			: "ir" (bits)
			: "cc"
			);
}
/*}}}*/
static INLINE void att32_and (atomic32_t *atval, uint32_t bits) /*{{{*/
{
	__asm__ __volatile__ ("					\n"
			"	lock; andl	%1, %0		\n"
			: "+m" (__dummy_atomic32 (atval))
			: "ir" (bits)
			: "cc"
			);
}
/*}}}*/
static INLINE uint32_t att32_swap (atomic32_t *atval, uint32_t newval) /*{{{*/
{

	__asm__ __volatile__ ("				\n"
			"	xchgl	%0, %1		\n"
			: "=r" (newval), "+m" (__dummy_atomic32 (atval))
			: "0" (newval)
			: "memory"
			);

	return newval;
}
/*}}}*/
static INLINE unsigned int att32_cas (atomic32_t *atval, uint32_t oldval, uint32_t newval) /*{{{*/
{
	unsigned int result;

	__asm__ __volatile__ ("					\n"
			"	lock; cmpxchgl	%3, %1		\n"
			"	setz		%%al		\n"
			"	andl		$1, %%eax	\n"
			: "=a" (result), "+m" (__dummy_atomic32 (atval))
			: "0" (oldval), "r" (newval)
			: "cc", "memory"
			);

	return result;
}
/*}}}*/
static INLINE void att32_set_bit (atomic32_t *atval, unsigned int bit) /*{{{*/
{
	__asm__ __volatile__ ("					\n"
			"	lock; btsl %1, %0		\n"
			: "+m" (__dummy_atomic32 (atval))
			: "Ir" (bit)
			: "cc"
			);
}
/*}}}*/
static INLINE void att32_clear_bit (atomic32_t *atval, unsigned int bit) /*{{{*/
{
	__asm__ __volatile__ ("					\n"
			"	lock; btrl %1, %0		\n"
			: "+m" (__dummy_atomic32 (atval))
			: "Ir" (bit)
			: "cc"
			);
}
/*}}}*/
static INLINE unsigned int att32_test_set_bit (atomic32_t *atval, unsigned int bit) /*{{{*/
{
	unsigned char result;

	__asm__ __volatile__ ("					\n"
			"	lock; btsl %2, %0		\n"
			"	setc %1				\n"
			: "+m" (__dummy_atomic32 (atval)), "=q" (result)
			: "Ir" (bit)
			: "cc"
			);

	return (unsigned int)result;
}
/*}}}*/
static INLINE unsigned int att32_test_clear_bit (atomic32_t *atval, unsigned int bit) /*{{{*/
{
	unsigned char result;

	__asm__ __volatile__ ("					\n"
			"	lock; btrl %2, %0		\n"
			"	setc %1				\n"
			: "+m" (__dummy_atomic32 (atval)), "=q" (result)
			: "Ir" (bit)
			: "cc"
			);

	return (unsigned int)result;
}
/*}}}*/

static INLINE void att64_set (atomic64_t *atval, uint64_t value) /*{{{*/
{
	__asm__ __volatile__ ("				\n"
			"	movq	%1, %0		\n"
			: "=m" (__dummy_atomic64 (atval))
			: "r" (value)
			);
}
/*}}}*/
static INLINE uint64_t att64_swap (atomic64_t *atval, uint64_t newval) /*{{{*/
{

	__asm__ __volatile__ ("				\n"
			"	xchgq	%0, %1		\n"
			: "=r" (newval), "+m" (__dummy_atomic64 (atval))
			: "0" (newval)
			: "memory"
			);

	return newval;
}
/*}}}*/
static INLINE unsigned int att64_cas (atomic64_t *atval, uint64_t oldval, uint64_t newval) /*{{{*/
{
	unsigned int result;

	__asm__ __volatile__ ("					\n"
			"	lock; cmpxchgq	%3, %1		\n"
			"	setz		%%al		\n"
			"	andl		$1, %%eax	\n"
			: "=a" (result), "+m" (__dummy_atomic64 (atval))
			: "0" (oldval), "r" (newval)
			: "cc", "memory"
			);

	return result;
}
/*}}}*/

static INLINE uint64_t bis128_val_hi (bitset128_t *bs) /*{{{*/
{
	return att64_val ((atomic64_t *)&(bs->values[1]));
}
/*}}}*/
static INLINE uint64_t bis128_val_lo (bitset128_t *bs) /*{{{*/
{
	return att64_val ((atomic64_t *)&(bs->values[0]));
}
/*}}}*/
static INLINE void bis128_set_hi (bitset128_t *bs, uint64_t val) /*{{{*/
{
	att64_set ((atomic64_t *)&(bs->values[1]), val);
}
/*}}}*/
static INLINE void bis128_set_lo (bitset128_t *bs, uint64_t val) /*{{{*/
{
	att64_set ((atomic64_t *)&(bs->values[0]), val);
}
/*}}}*/
static INLINE void bis128_set_bit (bitset128_t *bs, unsigned int bit) /*{{{*/
{
	unsigned int idx = bit >> 6;		/* div 64 */

	__asm__ __volatile__ ("				\n"
			"	lock; btsq %1, %0	\n"
			: "+m" (__dummy_atomic64 (&(bs->values[idx])))
			: "Jr" ((uint64_t)(bit & 0x3f))
			: "cc"
			);
}
/*}}}*/
static INLINE void bis128_clear_bit (bitset128_t *bs, unsigned int bit) /*{{{*/
{
	unsigned int idx = bit >> 6;		/* div 64 */

	__asm__ __volatile__ ("				\n"
			"	lock; btrq %1, %0	\n"
			: "+m" (__dummy_atomic64 (&(bs->values[idx])))
			: "Jr" ((uint64_t)(bit & 0x3f))
			: "cc"
			);
}
/*}}}*/
static INLINE unsigned int bis128_bsf (bitset128_t *bs) /*{{{*/
{
	unsigned int res;

	res = 64;
	if (!bs->values[0]) {
		if (!bs->values[1]) {
			return 128;
		}
		__asm__ __volatile__ ("			\n"
			"	bsfq %1, %0		\n"
			: "=r" (res)
			: "r" (bs->values[1])
			: "cc"
			);
		return (res + 64);
	}
	__asm__ __volatile__ ("			\n"
		"	bsfq %1, %0		\n"
		: "=r" (res)
		: "r" (bs->values[0])
		: "cc"
		);
	return res;
}
/*}}}*/
static INLINE void bis128_and (bitset128_t *s0, bitset128_t *s1, bitset128_t *d) /*{{{*/
{
	uint64_t lo = bis128_val_lo (s0) & bis128_val_lo (s1);
	uint64_t hi = bis128_val_hi (s0) & bis128_val_hi (s1);

	bis128_set_hi (d, hi);
	bis128_set_lo (d, lo);
}
/*}}}*/
static INLINE unsigned int bis128_eq (bitset128_t *a, bitset128_t *b) /*{{{*/
{
	return (bis128_val_lo (a) == bis128_val_lo (b)) && (bis128_val_hi (a) == bis128_val_hi (b));
}
/*}}}*/

#endif	/* !__ATOMICS_H */


