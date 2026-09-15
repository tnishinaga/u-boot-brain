/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __CONFIG_BRAIN_GEN1_H
#define __CONFIG_BRAIN_GEN1_H
#define PHYS_SDRAM_1 0x40000000
#define PHYS_SDRAM_1_SIZE 0x04000000
#define CONFIG_SYS_SDRAM_BASE PHYS_SDRAM_1
#define CONFIG_SYS_INIT_SP_ADDR (PHYS_SDRAM_1 + 0x00100000)
#define CONFIG_SYS_LOAD_ADDR 0x41000000
#define CONFIG_SYS_MALLOC_LEN (256 * 1024)
/* The 11.48 MB Gen1 Image needs more than bootm's 8 MB default limit. */
#define CONFIG_SYS_BOOTM_LEN (16 * 1024 * 1024)
/* Keep the fixed framebuffer below the U-Boot relocation area. */
#define CONFIG_FB_ADDR 0x43e00000
/* The LCDC scans 512 pixels per row, but only 480 reach the visible panel. */
#define VIDEO_LINE_LEN (512 * 2)
/*
 * The TMPA910 LCD takes the two 16-bit pixels in each cfb 32-bit glyph
 * store in the opposite order.  Use cfb_console's existing swap path.
 */
#define VIDEO_FB_16BPP_WORD_SWAP
#define CONFIG_KEYBOARD
#define CONFIG_EXTRA_ENV_SETTINGS \
	"stdin=serial,brain-kbd\0" \
	"stdout=serial,vga\0" \
	"stderr=serial,vga\0" \
	/* Fastboot downloads one FIT below the fixed framebuffer. */ \
	"fastboot_bootcmd=bootm 0x42000000\0"
#define TMPA910_FCLK_HZ 200000000
#define TMPA910_PCLK_HZ (TMPA910_FCLK_HZ / 2)
/* Timer4 uses fPCLK/2 followed by its divide-by-256 prescaler. */
#define CONFIG_SYS_TIMER_RATE (TMPA910_PCLK_HZ / 512)
#define CONFIG_SYS_HZ_CLOCK CONFIG_SYS_TIMER_RATE
#define CONFIG_SYS_SERIAL0 0xf2000000
#define CONFIG_PL01x_PORTS { (void *)CONFIG_SYS_SERIAL0 }
#define CONFIG_PL011_CLOCK TMPA910_PCLK_HZ
#define CONFIG_SH_SDHI_FREQ TMPA910_PCLK_HZ
#define CONFIG_SYS_SH_SDHI_NR_CHANNEL 1
#define CONFIG_SYS_MMC_MAX_BLK_COUNT 65535
#endif
