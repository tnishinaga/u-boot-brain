// SPDX-License-Identifier: GPL-2.0+
/*
 * Sharp Brain first generation support, ported from MuCross TMPA9xx U-Boot:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * (C) Copyright 2009,2010
 * Kernel Concepts <www.kernelconcepts.de>
 * Florian Boor (florian.boor@kernelconcepts.de)
 */
#include <common.h>
#include <init.h>
#include <mmc.h>
#include <asm/io.h>
#include <asm/arch/sh_sdhi.h>

#define TMPA910_CLKCR0		0xf0050040
#define TMPA910_CLKCR3		0xf005004c
#define TMPA910_GPIOGDIR	0xf0806400
#define TMPA910_GPIOGFR1	0xf0806424
#define TMPA910_GPIONDIR	0xf080c400
#define TMPA910_GPIONFR1	0xf080c424
#define TMPA910_GPIONFR2	0xf080c428
#define TMPA910_SDHC_BASE	0xf2030000

DECLARE_GLOBAL_DATA_PTR;

int board_early_init_f(void)
{
	/* CLKCR0 bit 0 gates the UART0 peripheral clock on TMPA910. */
	setbits_le32((void *)TMPA910_CLKCR0, BIT(0));

	/*
	 * Route PN0/PN1 to UART0 TX/RX.  Clear both function registers
	 * first because the TMPA910 forbids selecting function 1 and 2
	 * simultaneously on a pin.  WinCE leaves PN1 configured as an
	 * output, so explicitly return the UART RX pin to input direction.
	 */
	writel(0, (void *)TMPA910_GPIONFR1);
	writel(0, (void *)TMPA910_GPIONFR2);
	clrbits_le32((void *)TMPA910_GPIONDIR, BIT(1));
	writel(BIT(0), (void *)TMPA910_GPIONFR1);
	writel(BIT(1), (void *)TMPA910_GPIONFR2);

	return 0;
}

int board_init(void)
{
	gd->bd->bi_boot_params = PHYS_SDRAM_1 + 0x100;
	return 0;
}

int board_mmc_init(struct bd_info *bis)
{
	/* Enable SDHC and route Port G to SDHC channel 0. */
	setbits_le32((void *)TMPA910_CLKCR3, BIT(2));
	writel(0, (void *)TMPA910_GPIOGDIR);
	writel(0xff, (void *)TMPA910_GPIOGFR1);

	return sh_sdhi_init(TMPA910_SDHC_BASE, 0,
			    SH_SDHI_QUIRK_16BIT_BUF);
}

int dram_init(void)
{
	gd->ram_size = PHYS_SDRAM_1_SIZE;
	return 0;
}

void lowlevel_init(void)
{
}

void reset_cpu(void)
{
	for (;;)
		;
}
