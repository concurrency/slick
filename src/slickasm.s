/*
 *	slickasm.s -- bits and pieces of assembly for the scheduler
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


.text

.globl	slick_schedlinkage
.type	slick_schedlinkage, @function

slick_schedlinkage:
	movq	%rdi, %rax			/* address of psched_t structure for this scheduler (thread-local) */

	movq	%rsp, 40(%rax)			/* save registers */
	movq	%rbp, 48(%rax)
	movq	%r10, 56(%rax)
	movq	%r11, 64(%rax)

	call	os_entry


