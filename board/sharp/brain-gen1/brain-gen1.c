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
DECLARE_GLOBAL_DATA_PTR;
int board_init(void)
{
	gd->bd->bi_boot_params = PHYS_SDRAM_1 + 0x100;
	return 0;
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
