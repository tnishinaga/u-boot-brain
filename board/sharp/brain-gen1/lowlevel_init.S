/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * TMPA910 PLL setup for Sharp Brain Gen1.
 *
 * Based on the MuCross TMPA9xx U-Boot source release:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * (C) Copyright 2009,2010
 * Kernel Concepts <www.kernelconcepts.de>
 * Florian Boor (florian.boor@kernelconcepts.de)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <config.h>
#include <linux/linkage.h>

#define TMPA910_SYSCR1		0xf0050004
#define TMPA910_SYSCR2		0xf0050008
#define TMPA910_SYSCR3		0xf005000c
#define TMPA910_SYSCR4		0xf0050010
#define TMPA910_SYSCR5		0xf0050014
#define TMPA910_SYSCR6		0xf0050018
#define TMPA910_SYSCR7		0xf005001c

#define TMPA910_SYSCR2_FCSEL	0x02
#define TMPA910_SYSCR3_PLL_OFF	0x07
#define TMPA910_SYSCR3_PLL_8X	0x87
#define TMPA910_SYSCR4_PLL_8X	0x65

ENTRY(lowlevel_init)
	/* Release the clock-controller write protection if WinCE set it. */
	ldr	r0, =TMPA910_SYSCR5
	ldr	r1, [r0]
	tst	r1, #1
	beq	1f
	ldr	r0, =TMPA910_SYSCR6
	mov	r1, #0xa5
	str	r1, [r0]
	ldr	r0, =TMPA910_SYSCR7
	mov	r1, #0x5a
	str	r1, [r0]

1:	/* Use fc without a clock-gear divider, as in the WinCE app state. */
	ldr	r0, =TMPA910_SYSCR1
	mov	r1, #0
	str	r1, [r0]

	/* Leave an already configured 8x PLL running without disturbing it. */
	ldr	r0, =TMPA910_SYSCR2
	ldr	r1, [r0]
	tst	r1, #TMPA910_SYSCR2_FCSEL
	beq	2f
	ldr	r0, =TMPA910_SYSCR3
	ldr	r1, [r0]
	cmp	r1, #TMPA910_SYSCR3_PLL_8X
	bne	2f
	ldr	r0, =TMPA910_SYSCR4
	ldr	r1, [r0]
	cmp	r1, #TMPA910_SYSCR4_PLL_8X
	beq	5f

2:	/* Return to fOSCH before changing the PLL parameters. */
	ldr	r0, =TMPA910_SYSCR2
	mov	r1, #0
	str	r1, [r0]
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop

	ldr	r0, =TMPA910_SYSCR3
	mov	r1, #TMPA910_SYSCR3_PLL_OFF
	str	r1, [r0]
	ldr	r0, =TMPA910_SYSCR4
	mov	r1, #TMPA910_SYSCR4_PLL_8X
	str	r1, [r0]
	ldr	r0, =TMPA910_SYSCR3
	mov	r1, #TMPA910_SYSCR3_PLL_8X
	str	r1, [r0]

3:	/* The TRM requires waiting for the PLL lock-up counter. */
	ldr	r0, =TMPA910_SYSCR2
	ldr	r1, [r0]
	tst	r1, #1
	beq	3b
	mov	r1, #TMPA910_SYSCR2_FCSEL
	str	r1, [r0]
	nop
	nop
	nop
	nop
	nop

5:	bx	lr
ENDPROC(lowlevel_init)
