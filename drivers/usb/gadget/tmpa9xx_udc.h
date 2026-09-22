/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Toshiba TMPA9xx USB device controller definitions.
 *
 * Based on the MuCross TX09 Linux 2.6.36 TMPA9xx UDC driver:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * Original source:
 * linux-tmpa9xx-2.6.36-110310/drivers/usb/gadget/tmpa9xx_udc.h
 * Based on tmpa9xxRM9200 datasheet revision E.
 * The TMPA910CRA register additions follow the 2010-06-02 datasheet
 * (TMPA910CRA-530/531).
 * Copyright (C) 2008
 */
#ifndef __TMPA9XX_UDC_H__
#define __TMPA9XX_UDC_H__

#define TMPA9XX_UDC_BASE	0xf4400000

/* UDC2AB AHB bridge registers. */
#define UD2AB_INTSTS		0x000
#define UD2AB_INTENB		0x004
#define UD2AB_UDMSTSET		0x010
#define UD2AB_DMACRDREQ		0x014
#define UD2AB_DMACRDVL		0x018
#define UD2AB_UDC2RDREQ		0x01c
#define UD2AB_UDC2RDVL		0x020
#define UD2AB_MWTOUT		0x008
#define UD2AB_MWTOUT_DISABLE	0x00000000
#define UD2AB_MWSADR		0x040
#define UD2AB_MWEADR		0x044
#define UD2AB_MWCADR		0x048
#define UD2AB_MWAHBADR		0x04c
#define UD2AB_MRSADR		0x050
#define UD2AB_MREADR		0x054
#define UD2AB_MRCADR		0x058
#define UD2AB_MRAHBADR		0x05c
#define UD2AB_PWCTL		0x080
#define UD2AB_MSTSTS		0x084
#define UD2AB_MSTSTS_MW_EP_DSET	BIT(0)

/* UDC2 registers, accessed through the UDC2AB PVCI interface. */
#define UD2ADR			0x200
#define UD2CMD			0x20c
#define UD2BRQ			0x210
#define UD2VAL			0x214
#define UD2IDX			0x218
#define UD2LEN			0x21c
#define UD2INT			0x220
#define UD2INT_EP		0x224
#define UD2INT_RX_ZERO		0x22c
#define UD2EP0_MSZ		0x230
#define UD2EP0_STS		0x234
#define UD2EP0_DSZ		0x238
#define UD2EP0_FIFO		0x23c
#define UD2EP1_MSZ		0x240
#define UD2EP1_STS		0x244
#define UD2EP1_DSZ		0x248
#define UD2EP1_FIFO		0x24c
#define UD2EP2_MSZ		0x250
#define UD2EP2_STS		0x254
#define UD2EP2_DSZ		0x258
#define UD2EP2_FIFO		0x25c

#define UD2AB_READ_REQ		BIT(31)
#define UD2AB_READ_CLEAR	BIT(30)
#define UD2AB_READ_ADDR_MASK	0x3fc

#define UD2AB_INT_SUSPEND	BIT(8)
#define UD2AB_INT_RESET		BIT(9)
#define UD2AB_INT_RESET_END	BIT(10)
#define UD2AB_INT_UDC2REG_RD	BIT(24)
/* UDINTSTS[29:8] are W1C; [7:0] are UDC2 read-only outputs. */
#define UD2AB_INT_W1C_MASK	(GENMASK(10, 8) | GENMASK(23, 17) | \
				 GENMASK(25, 24) | BIT(29))
#define UD2AB_INT_MASK		(UD2AB_INT_RESET | UD2AB_INT_RESET_END | \
				 UD2AB_INT_SUSPEND | BIT(17) | BIT(18) | BIT(19) | BIT(20) | \
				 BIT(21) | BIT(23) | BIT(24) | BIT(29))
#define UD2AB_INT_PENDING_MASK	(UD2AB_INT_MASK | GENMASK(7, 0))
#define UD2AB_INT_CLEAR		UD2AB_INT_W1C_MASK
#define UD2AB_INT_DMA_DONE	BIT(18)
#define UD2AB_INT_DMA_SET	BIT(17)
#define UD2AB_INT_DMA_TIMEOUT	BIT(19)
#define UD2AB_INT_DMA_AHBERR	BIT(20)
#define UD2AB_INT_DMA_RDERR	BIT(29)
#define UD2AB_INT_DMA_STATUS	(GENMASK(20, 17) | BIT(29))
#define UD2AB_INT_MR_DONE	BIT(21)
#define UD2AB_INT_MR_AHBERR	BIT(23)

#define UD2_INT_SETUP		BIT(0)
#define UD2_INT_STATUS_NAK	BIT(1)
#define UD2_INT_STATUS		BIT(2)
#define UD2_INT_RX_ZERO		BIT(3)
#define UD2_INT_SOF		BIT(4)
#define UD2_INT_EP0		BIT(5)
#define UD2_INT_EP		BIT(6)
#define UD2_INT_NAK		BIT(7)
#define UD2_INT_CLEAR_BASE	0x9000
#define UD2_INT_MASK		0x90ff
#define UD2_INT_EP_EVENT_MASK	GENMASK(3, 1)

#define UD2_CMD(ep, command)	(((ep) << 4) | (command))
#define UD2_CMD_SETUP_FIN	0x1
#define UD2_CMD_EP_RESET		0x3
#define UD2_CMD_EP_STALL		0x4
#define UD2_CMD_EP_INVALID	0x5
#define UD2_CMD_EP_DISABLE	0x7
#define UD2_CMD_EP_ENABLE	0x8
#define UD2_CMD_ALL_INVALID	0x9
#define UD2_CMD_USB_READY	0xa
#define UD2_CMD_SETUP_RECEIVED	0xb
#define UD2_CMD_EP_EOP		0xc
#define UD2_CMD_FIFO_CLEAR	0xd
#define UD2_CMD_TX_ZLP		0xe

#define UD2_EP_DSET		BIT(12)
#define UD2_EP_DATA_SIZE_MASK	GENMASK(10, 0)
#define UD2_EP_DUAL_BULK_IN	0xc088
#define UD2_EP_DUAL_BULK_OUT	0xc008

#define UD2_ADDR_SPEED_MASK	GENMASK(13, 12)
#define UD2_ADDR_FULL_SPEED	BIT(12)
#define UD2_ADDR_HIGH_SPEED	BIT(13)
#define UD2_ADDR_STATE_DEFAULT	BIT(8)
#define UD2_ADDR_STATE_ADDRESSED BIT(9)
#define UD2_ADDR_STATE_CONFIGURED BIT(10)

#define UD2AB_PWCTL_PHY_RESET	BIT(5)
#define UD2AB_PWCTL_SUSPEND	BIT(3)
#define UD2AB_PWCTL_POWER_RESET	BIT(1)

/* UDMSTSET: reserved bits must be written as zero. */
#define UD2AB_UDMST_MR_RESET	BIT(6)
#define UD2AB_UDMST_MR_ABORT	BIT(5)
#define UD2AB_UDMST_MR_ENABLE	BIT(4)
#define UD2AB_UDMST_MW_RESET	BIT(2)
#define UD2AB_UDMST_MW_ABORT	BIT(1)
#define UD2AB_UDMST_MW_ENABLE	BIT(0)

#define UD2AB_UDMST_DMA_MASK	(UD2AB_UDMST_MW_RESET | \
				 UD2AB_UDMST_MW_ABORT | UD2AB_UDMST_MW_ENABLE)
#define UD2AB_UDMST_MW_ACTIVE	UD2AB_UDMST_MW_ENABLE

#endif /* __TMPA9XX_UDC_H__ */
