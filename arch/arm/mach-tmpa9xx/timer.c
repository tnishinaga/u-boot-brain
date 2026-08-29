// SPDX-License-Identifier: GPL-2.0+
/*
 * Ported from MuCross TMPA9xx U-Boot source release:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
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
static ulong timestamp;
static u16 last_count;
static unsigned int stalled_reads;
int timer_init(void)
{
	/*
	 * Brain Gen1 leaves the 32.768 kHz source stopped.  Enable the
	 * Timer4/5 gate and derive an approximately 32 kHz clock from
	 * fPCLK/2 with the timer's divide-by-256 prescaler instead.
	 */
	setbits_le32((void *)CLKCR0, BIT(7));
	setbits_le32((void *)CLKCR5, BIT(2));
	writel(0, (void *)TIMER_MODE);
	writel(0, (void *)TIMER_CMPEN);
	writel(0, (void *)TIMER_CAPEN);
	writel(0xffff, (void *)TIMER_LD);
	writel(0x8a, (void *)TIMER_CONTROL);
	last_count = readl((void *)TIMER_DATA) & 0xffff;
	return 0;
}
static ulong tmpa9xx_get_ticks(void)
{
	u16 now = readl((void *)TIMER_DATA) & 0xffff;

	/*
	 * Brain Gen1 can enter through WinCE with the Timer4 source stopped.
	 * Keep delay and timeout users alive until its clock source is known.
	 */
	if (now == last_count) {
		if (++stalled_reads == 16) {
			timestamp++;
			stalled_reads = 0;
		}
		return timestamp;
	}
	stalled_reads = 0;

	if (last_count >= now)
		timestamp += last_count - now;
	else
		timestamp += last_count + 0xffff - now;
	last_count = now;
	return timestamp;
}
ulong get_timer(ulong base)
{
	return (tmpa9xx_get_ticks() * CONFIG_SYS_HZ / 32768) - base;
}

void __udelay(unsigned long usec)
{
	ulong start = tmpa9xx_get_ticks();
	ulong ticks = DIV_ROUND_UP(usec * 32768, 1000000);
	while (tmpa9xx_get_ticks() - start < ticks)
		;
}
unsigned long long get_ticks(void) { return tmpa9xx_get_ticks(); }
