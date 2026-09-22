// SPDX-License-Identifier: GPL-2.0+
/*
 * Ported from MuCross TMPA9xx U-Boot source release:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * Original source:
 * u-boot-tmpa9xx-110310/arch/arm/cpu/arm926ejs/tmpa9xx/timer.c
 * (C) Copyright 2009,2010
 * Kernel Concepts <www.kernelconcepts.de>
 * Florian Boor (florian.boor@kernelconcepts.de)
 */
#include <common.h>
#include <asm/io.h>
#define CLKCR0 0xf0050040
#define CLKCR5 0xf0050054
#define TIMER4 0xf0042000
#define TIMER_LD (TIMER4 + 0x00)
#define TIMER_DATA (TIMER4 + 0x04)
#define TIMER_CONTROL (TIMER4 + 0x08)
#define TIMER_MODE (TIMER4 + 0x1c)
#define TIMER_CAPEN (TIMER4 + 0x60)
#define TIMER_CMPEN (TIMER4 + 0xe0)
#define TIMER_INPUT_DIVIDER 512ULL
static ulong timestamp;
static u16 last_count;

int timer_init(void)
{
	/*
	 * Brain Gen1 leaves the 32.768 kHz source stopped.  Match the WinCE
	 * application clock tree: fFCLK=200 MHz and fPCLK=100 MHz.  Timer4
	 * therefore counts at fPCLK/2/256 = 195312.5 Hz.
	 */
	setbits_le32((void *)CLKCR0, BIT(7));
	setbits_le32((void *)CLKCR5, BIT(2));
	writel(0, (void *)TIMER_MODE);
	writel(0, (void *)TIMER_CMPEN);
	writel(0, (void *)TIMER_CAPEN);
	writel(0xffff, (void *)TIMER_LD);
	writel(0x8a, (void *)TIMER_CONTROL);
	timestamp = 0;
	last_count = readl((void *)TIMER_DATA) & 0xffff;
	return 0;
}

static ulong tmpa9xx_get_ticks(void)
{
	u16 now = readl((void *)TIMER_DATA) & 0xffff;

	if (last_count >= now)
		timestamp += last_count - now;
	else
		timestamp += last_count + 0x10000 - now;
	last_count = now;
	return timestamp;
}

ulong get_timer(ulong base)
{
	return ((u64)tmpa9xx_get_ticks() * CONFIG_SYS_HZ *
		TIMER_INPUT_DIVIDER / TMPA910_PCLK_HZ) - base;
}

void __udelay(unsigned long usec)
{
	ulong start = tmpa9xx_get_ticks();
	u64 denominator = TIMER_INPUT_DIVIDER * 1000000ULL;
	ulong ticks = ((u64)usec * TMPA910_PCLK_HZ + denominator - 1) /
		denominator;
	while (tmpa9xx_get_ticks() - start < ticks)
		;
}

unsigned long long get_ticks(void)
{
	return tmpa9xx_get_ticks();
}
