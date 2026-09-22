// SPDX-License-Identifier: GPL-2.0+
/*
 * Sharp Brain first generation support, ported from MuCross TMPA9xx U-Boot:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * Original source:
 * u-boot-tmpa9xx-110310/board/tmpa9xx/tmpa9xx.c
 * (C) Copyright 2009,2010
 * Kernel Concepts <www.kernelconcepts.de>
 * Florian Boor (florian.boor@kernelconcepts.de)
 */
#include <common.h>
#include <init.h>
#include <mmc.h>
#include <usb.h>
#include <asm/io.h>
#include <asm/arch/sh_sdhi.h>

#define TMPA910_SYSCR0		0xf0050000
/* SYSCR0: USBCLKSEL source, reserved write-one bits [5] and [1]. */
#if IS_ENABLED(CONFIG_TMPA9XX_USBCLK_X1USB)
#define TMPA910_SYSCR0_USB_DEVICE_VALUE	(BIT(7) | BIT(5) | BIT(1))
#define TMPA910_USBCLK_SOURCE		"X1USB"
#else
#define TMPA910_SYSCR0_USB_DEVICE_VALUE	(BIT(6) | BIT(5) | BIT(1))
#define TMPA910_USBCLK_SOURCE		"X1"
#endif
#define TMPA910_CLKCR0		0xf0050040
#define TMPA910_CLKCR3		0xf005004c
#define TMPA910_CLKCR4		0xf0050050
#define TMPA910_PMCCTL		0xf0020300
#define TMPA910_PMCCTL_PCM_ON	BIT(7)
#define TMPA910_PMCCTL_PMCPWE	BIT(6)
#define TMPA910_PMCCTL_WUTM_MASK	GENMASK(1, 0)
#define TMPA910_GPIOGDIR	0xf0806400
#define TMPA910_GPIOGFR1	0xf0806424
#define TMPA910_GPIONDIR	0xf080c400
#define TMPA910_GPIONFR1	0xf080c424
#define TMPA910_GPIONFR2	0xf080c428
#define TMPA910_SDHC_BASE	0xf2030000

#if IS_ENABLED(CONFIG_USB_GADGET_TMPA9XX)
int tmpa9xx_udc_init(void);
int tmpa9xx_udc_shutdown(void);
#endif

DECLARE_GLOBAL_DATA_PTR;

int board_early_init_f(void)
{
	u32 pmcctl;

	/*
	 * PCM_ON=1 explicitly enters Power Cut Mode.  Keep it disabled while
	 * preserving only the documented warm-up field; reserved bits must be
	 * written as zero.  PMCPWE keeps the normal-run PWE supply output active.
	 */
	pmcctl = readl((void *)TMPA910_PMCCTL);
	pmcctl &= TMPA910_PMCCTL_WUTM_MASK;
	pmcctl |= TMPA910_PMCCTL_PMCPWE;
	writel(pmcctl & ~TMPA910_PMCCTL_PCM_ON,
	       (void *)TMPA910_PMCCTL);

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

#if IS_ENABLED(CONFIG_USB_GADGET_TMPA9XX)
int board_usb_init(int index, enum usb_init_type init)
{
	int ret;

	if (index || init != USB_INIT_DEVICE)
		return -ENODEV;

	/*
	 * The TMPA910 manual section 3.16.1 requires a 24 MHz UDC clock.
	 * The default X1 source follows the MuCross TX09 low-level code.  A
	 * board wired to the dedicated input can select X1USB at build time
	 * with CONFIG_TMPA9XX_USBCLK_X1USB; SYSCR0 cannot auto-detect the input.
	 * The TRM requires reserved SYSCR0 bits 5 and 1 to be written as one;
	 * use the documented constant rather than a read/modify/write.
	 */
	writel(TMPA910_SYSCR0_USB_DEVICE_VALUE, (void *)TMPA910_SYSCR0);

	/*
	 * WinCE changes CLKCR4 from 0x00 to 0x01 only when its USB device
	 * function is enabled.  Without this gate UDC2 PVCI accesses never
	 * complete and the driver reports -ETIMEDOUT.
	 */
	setbits_le32((void *)TMPA910_CLKCR4, BIT(0));

	ret = tmpa9xx_udc_init();
	if (ret)
		printf("tmpa910-usb: init failed source=%s clk=0x%08x err=%d\n",
		       TMPA910_USBCLK_SOURCE, readl((void *)TMPA910_SYSCR0), ret);
	return ret;
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	if (index || init != USB_INIT_DEVICE)
		return -ENODEV;
	return tmpa9xx_udc_shutdown();
}

int g_dnl_board_usb_cable_connected(void)
{
	/* Brain Gen1 has no known software-readable VBUS detect signal. */
	return 1;
}
#endif

int dram_init(void)
{
	gd->ram_size = PHYS_SDRAM_1_SIZE;
	return 0;
}

void reset_cpu(void)
{
	for (;;)
		;
}
