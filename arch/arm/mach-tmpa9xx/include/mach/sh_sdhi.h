/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TMPA9xx SD host register definitions.
 *
 * Based on the Renesas SH-SDHI definitions in U-Boot and the MuCross
 * TMPA9xx/TX09 source release:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 *
 * Copyright (C) 2013-2017 Renesas Electronics Corporation
 * Copyright (C) 2014 Nobuhiro Iwamatsu <nobuhiro.iwamatsu.yj@renesas.com>
 * Copyright (C) 2008-2009 Renesas Solutions Corp.
 */
#ifndef __TMPA9XX_SH_SDHI_H
#define __TMPA9XX_SH_SDHI_H

#include <linux/bitops.h>

#define SDHI_CMD			(0x0000 >> 1)
#define SDHI_PORTSEL			(0x0004 >> 1)
#define SDHI_ARG0			(0x0008 >> 1)
#define SDHI_ARG1			(0x000c >> 1)
#define SDHI_STOP			(0x0010 >> 1)
#define SDHI_SECCNT			(0x0014 >> 1)
#define SDHI_RSP00			(0x0018 >> 1)
#define SDHI_RSP01			(0x001c >> 1)
#define SDHI_RSP02			(0x0020 >> 1)
#define SDHI_RSP03			(0x0024 >> 1)
#define SDHI_RSP04			(0x0028 >> 1)
#define SDHI_RSP05			(0x002c >> 1)
#define SDHI_RSP06			(0x0030 >> 1)
#define SDHI_RSP07			(0x0034 >> 1)
#define SDHI_INFO1			(0x0038 >> 1)
#define SDHI_INFO2			(0x003c >> 1)
#define SDHI_INFO1_MASK			(0x0040 >> 1)
#define SDHI_INFO2_MASK			(0x0044 >> 1)
#define SDHI_CLK_CTRL			(0x0048 >> 1)
#define SDHI_SIZE			(0x004c >> 1)
#define SDHI_OPTION			(0x0050 >> 1)
#define SDHI_ERR_STS1			(0x0058 >> 1)
#define SDHI_ERR_STS2			(0x005c >> 1)
#define SDHI_BUF0			(0x0060 >> 1)
#define SDHI_SDIO_MODE			(0x0068 >> 1)
#define SDHI_SDIO_INFO1			(0x006c >> 1)
#define SDHI_SDIO_INFO1_MASK		(0x0070 >> 1)
#define SDHI_CC_EXT_MODE		(0x01b0 >> 1)
#define SDHI_SOFT_RST			(0x01c0 >> 1)
#define SDHI_VERSION			(0x01c4 >> 1)
#define SDHI_HOST_MODE			(0x01c8 >> 1)
#define SDHI_SDIF_MODE			(0x01cc >> 1)
#define SDHI_EXT_SWAP			(0x01e0 >> 1)
#define SDHI_SD_DMACR			(0x0324 >> 1)

#define CMD_MASK			0xffff
#define USE_1PORT			BIT(8)
#define ARG0_MASK			0xffff
#define ARG1_MASK			0xffff
#define STOP_SEC_ENABLE			BIT(8)

#define INFO1_RESP_END			BIT(0)
#define INFO1_ACCESS_END		BIT(2)
#define INFO1_CARD_RE			BIT(3)
#define INFO1_CARD_IN			BIT(4)
#define INFO1_ISD0CD			BIT(5)
#define INFO1_WRITE_PRO			BIT(7)
#define INFO1_DATA3_CARD_RE		BIT(8)
#define INFO1_DATA3_CARD_IN		BIT(9)
#define INFO1_DATA3			BIT(10)

#define INFO2_CMD_ERROR			BIT(0)
#define INFO2_CRC_ERROR			BIT(1)
#define INFO2_END_ERROR			BIT(2)
#define INFO2_TIMEOUT			BIT(3)
#define INFO2_BUF_ILL_WRITE		BIT(4)
#define INFO2_BUF_ILL_READ		BIT(5)
#define INFO2_RESP_TIMEOUT		BIT(6)
#define INFO2_SDDAT0			BIT(7)
#define INFO2_BRE_ENABLE		BIT(8)
#define INFO2_BWE_ENABLE		BIT(9)
#define INFO2_CBUSY			BIT(14)
#define INFO2_ILA			BIT(15)
#define INFO2_ALL_ERR			0x807f

#define INFO1M_RESP_END			BIT(0)
#define INFO1M_ACCESS_END		BIT(2)
#define INFO1M_CARD_RE			BIT(3)
#define INFO1M_CARD_IN			BIT(4)
#define INFO1M_DATA3_CARD_RE		BIT(8)
#define INFO1M_DATA3_CARD_IN		BIT(9)
#define INFO1M_ALL			0xffff
#define INFO1M_SET			(INFO1M_RESP_END | INFO1M_ACCESS_END | \
					 INFO1M_DATA3_CARD_RE | \
					 INFO1M_DATA3_CARD_IN)

#define INFO2M_CMD_ERROR		BIT(0)
#define INFO2M_CRC_ERROR		BIT(1)
#define INFO2M_END_ERROR		BIT(2)
#define INFO2M_TIMEOUT			BIT(3)
#define INFO2M_BUF_ILL_WRITE		BIT(4)
#define INFO2M_BUF_ILL_READ		BIT(5)
#define INFO2M_RESP_TIMEOUT		BIT(6)
#define INFO2M_BRE_ENABLE		BIT(8)
#define INFO2M_BWE_ENABLE		BIT(9)
#define INFO2M_ILA			BIT(15)
#define INFO2M_ALL			0xffff
#define INFO2M_ALL_ERR			0x807f

#define CLK_ENABLE			BIT(8)
#define OPT_BUS_WIDTH_M			(5 << 13)
#define OPT_BUS_WIDTH_1			(4 << 13)
#define OPT_BUS_WIDTH_4			(0 << 13)
#define OPT_BUS_WIDTH_8			(1 << 13)

#define ERR_STS1_CRC_ERROR		(BIT(11) | BIT(10) | BIT(9) | \
					 BIT(8) | BIT(5))
#define ERR_STS1_CMD_ERROR		(BIT(4) | BIT(3) | BIT(2) | \
					 BIT(1) | BIT(0))
#define ERR_STS2_RES_TIMEOUT		BIT(0)
#define ERR_STS2_RES_STOP_TIMEOUT	(BIT(0) | BIT(1))
#define ERR_STS2_SYS_ERROR		0x7f

#define SDIO_MODE_ON			BIT(0)
#define SDIO_MODE_OFF			0
#define SDIO_INFO1_IOIRQ		BIT(0)
#define SDIO_INFO1_EXPUB52		BIT(14)
#define SDIO_INFO1_EXWT			BIT(15)
#define SDIO_INFO1M_CLEAR		(BIT(1) | BIT(2))
#define SDIO_INFO1M_ON			(BIT(15) | BIT(14) | BIT(2) | \
					 BIT(1) | BIT(0))

#define SET_SWAP			(BIT(6) | BIT(7))
#define SOFT_RST_ON			0
#define SOFT_RST_OFF			BIT(0)

#define CLKDEV_SD_DATA			25000000
#define CLKDEV_HS_DATA			50000000
#define CLKDEV_MMC_DATA			20000000
#define CLKDEV_INIT			400000

#define SH_SDHI_QUIRK_16BIT_BUF		BIT(0)
#define SH_SDHI_QUIRK_64BIT_BUF		BIT(1)

int sh_sdhi_init(unsigned long addr, int ch, unsigned long quirks);

#endif
