// SPDX-License-Identifier: GPL-2.0+
/*
 * TMPA9xx LCDC framebuffer driver for Sharp Brain Gen1.
 *
 * Based on the MuCross TMPA9xx/TX09 U-Boot source release:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * Original source:
 * u-boot-tmpa9xx-110310/drivers/video/tmpa9xx_fb.c
 * In particular, the LCDC register programming follows the LCDC setup in
 * drivers/video/tmpa9xx_fb.c from that release.  The fixed framebuffer and
 * legacy cfb_console glue are specific to this board port.
 *
 * The following copyright and license notice is retained from that source:
 *
 * (C) Copyright 2009,2010
 * Kernel Concepts <www.kernelconcepts.de>
 * Thomas Haase (Thomas.Haase@web.de)
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 */

#include <common.h>
#include <video_fb.h>
#include <asm/io.h>

/* TMPA910CRA datasheet, LCDC register block (base 0xf4200000). */
#define TMPA910_LCDC_BASE	0xf4200000
#define TMPA910_LCD_TIMING0	(TMPA910_LCDC_BASE + 0x000)
#define TMPA910_LCD_TIMING1	(TMPA910_LCDC_BASE + 0x004)
#define TMPA910_LCD_TIMING2	(TMPA910_LCDC_BASE + 0x008)
#define TMPA910_LCD_TIMING3	(TMPA910_LCDC_BASE + 0x00c)
#define TMPA910_LCD_UPBASE	(TMPA910_LCDC_BASE + 0x010)
#define TMPA910_LCD_LPBASE	(TMPA910_LCDC_BASE + 0x014)
#define TMPA910_LCD_IMSC	(TMPA910_LCDC_BASE + 0x018)
#define TMPA910_LCD_CONTROL	(TMPA910_LCDC_BASE + 0x01c)

/* LCD output drive strength (the LCD bit is PMCDRV[4]). */
#define TMPA910_PMCDRV		0xf0020260
#define TMPA910_PMCDRV_DRV_LCD	BIT(4)

/* The only documented LCDCOP field in the MuCross TMPA9xx headers. */
#define TMPA910_LCDCOP_STN64CR		0xf00b0000
#define TMPA910_LCDCOP_STN64CR_G64_8BIT	BIT(1)

/* Port J/K alternate functions carry the LCD data/control signals. */
#define TMPA910_GPIOJFR1	0xf0808424
#define TMPA910_GPIOJFR2	0xf0808428
#define TMPA910_GPIOKFR1	0xf0809424
#define TMPA910_GPIOKFR2	0xf0809428

/*
 * These values are a read-only JTAG capture from a Sharp Brain Gen1 while
 * WinCE was displaying normally (2026-08-26).  They supersede the generic
 * MuCross 320x240 defaults, which were not a panel identification.  The
 * TMPA910 LCDC encoding decodes this capture as 512x320, TFT 16bpp RGB565
 * (BGR=0) with active-low HSYNC/VSYNC.  CONTROL also has BEBO=0 and
 * BEPO=1; the latter has no effect in the 16bpp format according to the
 * PrimeCell LCDC definition:
 *
 *   TIMING0=0x0320317c, TIMING1=0x0202253f, TIMING2=0x05ff1800,
 *   TIMING3=0x00010001,
 *   CONTROL=0x00010c29
 *
 * Although bit 26 was set in that capture, the Brain hardware treats it as
 * the PrimeCell divider-bypass bit.  With Linux also accessing SDRAM, bypass
 * caused LCD FIFO underruns and a horizontally repeated image.  Clearing it
 * and selecting PCD=2 (HCLK / (2 + 2)) removed both symptoms on hardware.
 * The TMPA910 manual calls bit 26 reserved/write-zero, so the validated value
 * below also follows the documented requirement.
 *
 * The capture also had LCDCOP[0]=0x500000c1, PMCDRV=0x73, PMCCTL=0x40,
 * and PMCWV1=0x80.  LCDCOP[0] is the STN option register in the MuCross
 * headers; its 16bpp STN bit was clear on this TFT panel, so only that
 * documented bit is explicitly cleared below.  PMCCTL bit 6 (PMCPWE) was
 * also set in the capture and is enforced by the board early init, which
 * also keeps Power Cut Mode disabled.  PMCWV1 is reserved in the TMPA910
 * TRM, and the remaining GPIOs are board power/control signals; they are
 * deliberately not guessed here.
 */
#define TMPA910_LCD_SCAN_WIDTH	512
#define TMPA910_LCD_VISIBLE_WIDTH	480
#define TMPA910_LCD_HEIGHT	320
#define TMPA910_LCD_BPP	16
#define TMPA910_LCD_TIMING0_VALUE	0x0320317c
#define TMPA910_LCD_TIMING1_VALUE	0x0202253f
#define TMPA910_LCD_TIMING2_CPL		((TMPA910_LCD_SCAN_WIDTH - 1) << 16)
#define TMPA910_LCD_TIMING2_IHS		BIT(12)
#define TMPA910_LCD_TIMING2_IVS		BIT(11)
#define TMPA910_LCD_TIMING2_PCD		2
#define TMPA910_LCD_TIMING2_VALUE	(TMPA910_LCD_TIMING2_CPL | \
					 TMPA910_LCD_TIMING2_IHS | \
					 TMPA910_LCD_TIMING2_IVS | \
					 TMPA910_LCD_TIMING2_PCD)
#define TMPA910_LCD_TIMING3_VALUE	0x00010001
#define TMPA910_LCD_CONTROL_VALUE	0x00010c29

static GraphicDevice tmpa910_panel;

void *video_hw_init(void)
{
	/* Select the dedicated LCDC function on all eight J/K pins. */
	writel(0xff, (void *)TMPA910_GPIOJFR1);
	writel(0x00, (void *)TMPA910_GPIOJFR2);
	writel(0xff, (void *)TMPA910_GPIOKFR1);
	writel(0x00, (void *)TMPA910_GPIOKFR2);
	/* Enable LCD drive without changing other peripheral drive fields. */
	setbits_le32((void *)TMPA910_PMCDRV, TMPA910_PMCDRV_DRV_LCD);
	/* Keep the TFT path selected; do not alter undocumented LCDCOP bits. */
	clrbits_le32((void *)TMPA910_LCDCOP_STN64CR,
		     TMPA910_LCDCOP_STN64CR_G64_8BIT);
	/* The fixed address is reserved by CONFIG_FB_ADDR. */
	writel(TMPA910_LCD_TIMING0_VALUE, (void *)TMPA910_LCD_TIMING0);
	writel(TMPA910_LCD_TIMING1_VALUE, (void *)TMPA910_LCD_TIMING1);
	writel(TMPA910_LCD_TIMING2_VALUE, (void *)TMPA910_LCD_TIMING2);
	writel(TMPA910_LCD_TIMING3_VALUE, (void *)TMPA910_LCD_TIMING3);
	writel(CONFIG_FB_ADDR, (void *)TMPA910_LCD_UPBASE);
	writel(0, (void *)TMPA910_LCD_LPBASE);
	writel(0, (void *)TMPA910_LCD_IMSC);
	/* Keep the controller disabled until the framebuffer is cleared. */
	writel(TMPA910_LCD_CONTROL_VALUE & ~1, (void *)TMPA910_LCD_CONTROL);

	memset((void *)CONFIG_FB_ADDR, 0,
	       TMPA910_LCD_SCAN_WIDTH * TMPA910_LCD_HEIGHT *
	       TMPA910_LCD_BPP / 8);

	memset(&tmpa910_panel, 0, sizeof(tmpa910_panel));
	tmpa910_panel.frameAdrs = CONFIG_FB_ADDR;
	tmpa910_panel.memSize = TMPA910_LCD_SCAN_WIDTH * TMPA910_LCD_HEIGHT *
				TMPA910_LCD_BPP / 8;
	tmpa910_panel.mode = 0;
	tmpa910_panel.gdfIndex = GDF_16BIT_565RGB;
	tmpa910_panel.gdfBytesPP = 2;
	tmpa910_panel.plnSizeX = TMPA910_LCD_SCAN_WIDTH;
	tmpa910_panel.plnSizeY = TMPA910_LCD_HEIGHT;
	tmpa910_panel.winSizeX = TMPA910_LCD_VISIBLE_WIDTH;
	tmpa910_panel.winSizeY = TMPA910_LCD_HEIGHT;
	strlcpy(tmpa910_panel.modeIdent, "TMPA910 480x320 RGB565 (512 stride)",
		sizeof(tmpa910_panel.modeIdent));

	/* Enable only after all LCDC configuration registers are programmed. */
	writel(TMPA910_LCD_CONTROL_VALUE | 1, (void *)TMPA910_LCD_CONTROL);

	return &tmpa910_panel;
}
