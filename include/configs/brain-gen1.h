/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __CONFIG_BRAIN_GEN1_H
#define __CONFIG_BRAIN_GEN1_H
#define PHYS_SDRAM_1 0x40000000
#define PHYS_SDRAM_1_SIZE 0x04000000
#define CONFIG_SYS_SDRAM_BASE PHYS_SDRAM_1
#define CONFIG_SYS_INIT_SP_ADDR (PHYS_SDRAM_1 + 0x00100000)
#define CONFIG_SYS_LOAD_ADDR 0x41000000
#define CONFIG_SYS_MALLOC_LEN (256 * 1024)
/* Keep the fixed framebuffer below the U-Boot relocation area. */
#define CONFIG_FB_ADDR 0x43e00000
/* The LCDC scans 512 pixels per row, but only 480 reach the visible panel. */
#define VIDEO_LINE_LEN (512 * 2)
/*
 * The TMPA910 LCD takes the two 16-bit pixels in each cfb 32-bit glyph
 * store in the opposite order.  Use cfb_console's existing swap path.
 */
#define VIDEO_FB_16BPP_WORD_SWAP
#define CONFIG_EXTRA_ENV_SETTINGS \
	"stdin=serial\0" \
	"stdout=serial,vga\0" \
	"stderr=serial,vga\0"
#define CONFIG_SYS_TIMER_RATE 32768
#define CONFIG_SYS_HZ_CLOCK CONFIG_SYS_TIMER_RATE
#define CONFIG_SYS_SERIAL0 0xf2000000
#define CONFIG_PL01x_PORTS { (void *)CONFIG_SYS_SERIAL0 }
#define CONFIG_PL011_CLOCK 96000000
#define CONFIG_SH_SDHI_FREQ 96000000
#define CONFIG_SYS_SH_SDHI_NR_CHANNEL 1
#define CONFIG_SYS_MMC_MAX_BLK_COUNT 65535
#define CONFIG_BOOTCOMMAND ""
#endif
