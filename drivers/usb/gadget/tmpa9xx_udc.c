// SPDX-License-Identifier: GPL-2.0+
/*
 * Toshiba TMPA9xx USB device controller driver.
 *
 * Based on the MuCross TX09 Linux 2.6.36 TMPA9xx UDC driver:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * Copyright (C) 2008
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * EP0 uses the PVCI FIFO path.  EP1 IN and EP2 OUT use the UDC2AB AHB
 * master-read and master-write paths respectively, following the MuCross
 * driver and avoiding unsupported bulk FIFO assumptions.
 *
 */
#include <common.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <cpu_func.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <malloc.h>

#include "tmpa9xx_udc.h"

#define TMPA9XX_NUM_EPS		3
#define TMPA9XX_RD_TIMEOUT_US	1000
#define TMPA9XX_DMA_BUF_SIZE	512
#define TMPA9XX_DMA_TIMEOUT_US	1000

struct tmpa9xx_udc;
struct tmpa9xx_ep;

struct tmpa9xx_request {
	struct usb_request req;
	struct list_head queue;
	struct tmpa9xx_ep *ep;
};

struct tmpa9xx_ep {
	struct usb_ep ep;
	struct list_head queue;
	struct tmpa9xx_udc *udc;
	const struct usb_endpoint_descriptor *desc;
	u16 maxpacket;
	u8 number;
	bool is_in;
	bool halted;
	bool need_zlp;
	bool tx_active;
	bool rx_zlp_pending;
};

struct tmpa9xx_udc {
	struct usb_gadget gadget;
	struct usb_gadget_driver *driver;
	struct tmpa9xx_ep eps[TMPA9XX_NUM_EPS];
	void __iomem *base;
	u8 address;
	bool address_pending;
	u16 pending_state;
	bool state_pending;
	bool initialized;
	bool fatal_error;
	bool disconnected;
	bool dma_active;
	struct tmpa9xx_request *dma_req;
	u16 dma_len;
	u8 dma_buf[TMPA9XX_DMA_BUF_SIZE] __aligned(ARCH_DMA_MINALIGN);
	bool tx_dma_active;
	struct tmpa9xx_request *tx_dma_req;
	u16 tx_dma_len;
	u8 tx_dma_buf[TMPA9XX_DMA_BUF_SIZE] __aligned(ARCH_DMA_MINALIGN);
};

static struct tmpa9xx_udc controller;

static void tmpa9xx_complete(struct tmpa9xx_ep *ep,
			     struct tmpa9xx_request *req, int status);
static int tmpa9xx_kick(struct tmpa9xx_ep *ep);
static void tmpa9xx_reinit(struct tmpa9xx_udc *udc);
static int tmpa9xx_controller_recover(struct tmpa9xx_udc *udc);
static void tmpa9xx_notify_disconnect(struct tmpa9xx_udc *udc);

static inline struct tmpa9xx_ep *to_tmpa9xx_ep(struct usb_ep *ep)
{
	return container_of(ep, struct tmpa9xx_ep, ep);
}

static inline struct tmpa9xx_request *to_tmpa9xx_req(struct usb_request *req)
{
	return container_of(req, struct tmpa9xx_request, req);
}

static inline u32 ud2ab_read(struct tmpa9xx_udc *udc, u32 reg)
{
	return readl(udc->base + reg);
}

static inline void ud2ab_write(struct tmpa9xx_udc *udc, u32 reg, u32 value)
{
	writel(value, udc->base + reg);
}

static void tmpa9xx_cache_invalidate(void *buf, unsigned int len)
{
	ulong start = (ulong)buf & ~(ARCH_DMA_MINALIGN - 1);
	ulong end = ALIGN((ulong)buf + len, ARCH_DMA_MINALIGN);

	invalidate_dcache_range(start, end);
}

static void tmpa9xx_cache_flush(void *buf, unsigned int len)
{
	ulong start = (ulong)buf & ~(ARCH_DMA_MINALIGN - 1);
	ulong end = ALIGN((ulong)buf + len, ARCH_DMA_MINALIGN);

	flush_dcache_range(start, end);
}

/* UDC2 reads are indirect; writes use the PVCI register window directly. */
static int ud2_read(struct tmpa9xx_udc *udc, u32 reg, u16 *value)
{
	u32 request = (reg & UD2AB_READ_ADDR_MASK) | UD2AB_READ_REQ;
	unsigned int timeout = TMPA9XX_RD_TIMEOUT_US;

	ud2ab_write(udc, UD2AB_UDC2RDREQ, request);
	while (ud2ab_read(udc, UD2AB_UDC2RDREQ) & UD2AB_READ_REQ) {
		if (!timeout) {
			/* Do not leave a PVCI transaction pending after a timeout. */
			ud2ab_write(udc, UD2AB_UDC2RDREQ,
				    UD2AB_READ_CLEAR | (reg & UD2AB_READ_ADDR_MASK));
			ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_UDC2REG_RD);
			return -ETIMEDOUT;
		}
		timeout--;
		udelay(1);
	}

	ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_UDC2REG_RD);
	*value = ud2ab_read(udc, UD2AB_UDC2RDVL);
	return 0;
}

/* MWCADR is read through DMACRDREQ; MWAHBADR is a direct status register. */
static int udmac_read(struct tmpa9xx_udc *udc, u32 reg, u32 *value)
{
	u32 timeout = TMPA9XX_RD_TIMEOUT_US;

	ud2ab_write(udc, UD2AB_DMACRDREQ,
		    (reg & UD2AB_READ_ADDR_MASK) | UD2AB_READ_REQ);
	while (ud2ab_read(udc, UD2AB_DMACRDREQ) & UD2AB_READ_REQ) {
		if (!timeout) {
			ud2ab_write(udc, UD2AB_DMACRDREQ,
				    UD2AB_READ_CLEAR | (reg & UD2AB_READ_ADDR_MASK));
			ud2ab_write(udc, UD2AB_INTSTS, BIT(25));
			return -ETIMEDOUT;
		}
		timeout--;
		udelay(1);
	}
	ud2ab_write(udc, UD2AB_INTSTS, BIT(25));
	*value = ud2ab_read(udc, UD2AB_DMACRDVL);
	return 0;
}

/* Every normal UDC2 PVCI write must complete before the next access. */
static int ud2_write(struct tmpa9xx_udc *udc, u32 reg, u16 value)
{
	unsigned int timeout = TMPA9XX_RD_TIMEOUT_US;

	while (ud2ab_read(udc, UD2AB_UDC2RDREQ) & UD2AB_READ_REQ) {
		if (!timeout--) {
			ud2ab_write(udc, UD2AB_UDC2RDREQ,
				    UD2AB_READ_CLEAR | (reg & UD2AB_READ_ADDR_MASK));
			return -ETIMEDOUT;
		}
		udelay(1);
	}
	timeout = TMPA9XX_RD_TIMEOUT_US;
	ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_UDC2REG_RD);
	ud2ab_write(udc, reg, value);
	while (!(ud2ab_read(udc, UD2AB_INTSTS) & UD2AB_INT_UDC2REG_RD)) {
		if (!timeout) {
			ud2ab_write(udc, UD2AB_UDC2RDREQ,
				    UD2AB_READ_CLEAR | (reg & UD2AB_READ_ADDR_MASK));
			ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_UDC2REG_RD);
			return -ETIMEDOUT;
		}
		timeout--;
		udelay(1);
	}
	ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_UDC2REG_RD);
	return 0;
}

static int tmpa9xx_init_error(struct tmpa9xx_udc *udc, const char *stage,
			      u32 reg, int error)
{
	/* Keep the failure useful even when the USB controller cannot be read. */
	printf("tmpa9xx-udc: init %s reg=0x%03x err=%d\n", stage, reg,
	       error);
	printf("tmpa9xx-udc: INTSTS=0x%08x RDREQ=0x%08x UDMSTSET=0x%08x\n",
	       ud2ab_read(udc, UD2AB_INTSTS),
	       ud2ab_read(udc, UD2AB_UDC2RDREQ),
	       ud2ab_read(udc, UD2AB_UDMSTSET));
	printf("tmpa9xx-udc: PWCTL=0x%08x MWTOUT=0x%08x\n",
	       ud2ab_read(udc, UD2AB_PWCTL), ud2ab_read(udc, UD2AB_MWTOUT));
	return error;
}

static int ud2_command(struct tmpa9xx_udc *udc, u8 ep, u8 command)
{
	/* ud2_write supplies the PVCI completion barrier for command writes. */
	return ud2_write(udc, UD2CMD, UD2_CMD(ep, command));
}

static void tmpa9xx_complete(struct tmpa9xx_ep *ep,
			     struct tmpa9xx_request *req, int status)
{
	if (list_empty(&req->queue))
		return;
	list_del_init(&req->queue);
	req->ep = NULL;
	if (req->req.status == -EINPROGRESS)
		req->req.status = status;
	req->req.complete(&ep->ep, &req->req);
}

static void tmpa9xx_nuke(struct tmpa9xx_ep *ep, int status)
{
	while (!list_empty(&ep->queue)) {
		struct tmpa9xx_request *req;

		req = list_first_entry(&ep->queue, struct tmpa9xx_request,
				       queue);
		tmpa9xx_complete(ep, req, status);
	}
}

static u32 tmpa9xx_ep_msz_reg(unsigned int ep)
{
	return UD2EP0_MSZ + ep * 0x10;
}

static u32 tmpa9xx_ep_dsz_reg(unsigned int ep)
{
	return UD2EP0_DSZ + ep * 0x10;
}

static u32 tmpa9xx_ep_fifo_reg(unsigned int ep)
{
	return UD2EP0_FIFO + ep * 0x10;
}

static void tmpa9xx_dma_drop_state(struct tmpa9xx_udc *udc)
{
	udc->dma_active = false;
	udc->dma_req = NULL;
	udc->dma_len = 0;
}

static int tmpa9xx_dma_master_reset(struct tmpa9xx_udc *udc)
{
	unsigned int timeout = TMPA9XX_DMA_TIMEOUT_US;

	ud2ab_write(udc, UD2AB_UDMSTSET, UD2AB_UDMST_MW_RESET);
	while (ud2ab_read(udc, UD2AB_UDMSTSET) &
	       (UD2AB_UDMST_MW_RESET | UD2AB_UDMST_MW_ACTIVE)) {
		if (!timeout--)
			return -ETIMEDOUT;
		udelay(1);
	}
	ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_DMA_STATUS);
	return 0;
}

static int tmpa9xx_dma_abort(struct tmpa9xx_udc *udc)
{
	unsigned int timeout = TMPA9XX_DMA_TIMEOUT_US;
	int ret;

	if (!udc->dma_active)
		return 0;

	/*
	 * TMPA910CRA datasheet p.530-531 requires this exact order.  In
	 * particular, do not reset the master-write block while mw_enable is
	 * still set: an incomplete AHB write could otherwise escape the FIFO.
	 */
	ret = ud2_command(udc, 2, UD2_CMD_EP_DISABLE);
	if (ret)
		goto fail;
	ud2ab_write(udc, UD2AB_UDMSTSET, UD2AB_UDMST_MW_ABORT);
	while ((ud2ab_read(udc, UD2AB_UDMSTSET) & UD2AB_UDMST_MW_ACTIVE) &&
	       timeout--)
		udelay(1);
	if (ud2ab_read(udc, UD2AB_UDMSTSET) & UD2AB_UDMST_MW_ACTIVE) {
		ret = -ETIMEDOUT;
		goto fail;
	}

	ret = tmpa9xx_dma_master_reset(udc);
	if (ret)
		goto fail;
	ret = ud2_command(udc, 2, UD2_CMD_FIFO_CLEAR);
	if (ret)
		goto fail;
	ret = ud2_command(udc, 2, UD2_CMD_EP_ENABLE);
	if (ret)
		goto fail;
	tmpa9xx_dma_drop_state(udc);
	return 0;

fail:
	/* Do not drop software state while the hardware state is unknown. */
	if (tmpa9xx_controller_recover(udc))
		udc->fatal_error = true;
	return ret;
}

static int tmpa9xx_dma_reset(struct tmpa9xx_udc *udc)
{
	if (tmpa9xx_dma_master_reset(udc))
		return -ETIMEDOUT;
	tmpa9xx_dma_drop_state(udc);
	return 0;
}

static int tmpa9xx_tx_dma_reset(struct tmpa9xx_udc *udc)
{
	unsigned int timeout = TMPA9XX_DMA_TIMEOUT_US;

	ud2ab_write(udc, UD2AB_UDMSTSET, UD2AB_UDMST_MR_RESET);
	while (ud2ab_read(udc, UD2AB_UDMSTSET) &
	       (UD2AB_UDMST_MR_RESET | UD2AB_UDMST_MR_ENABLE)) {
		if (!timeout--)
			return -ETIMEDOUT;
		udelay(1);
	}
	ud2ab_write(udc, UD2AB_INTSTS,
		    UD2AB_INT_MR_DONE | UD2AB_INT_MR_AHBERR);
	udc->tx_dma_active = false;
	udc->tx_dma_req = NULL;
	udc->tx_dma_len = 0;
	return 0;
}

static int tmpa9xx_tx_dma_start(struct tmpa9xx_ep *ep,
				struct tmpa9xx_request *req, unsigned int count)
{
	struct tmpa9xx_udc *udc = ep->udc;
	ulong dma_addr;

	if (!count || count > TMPA9XX_DMA_BUF_SIZE)
		return count ? -EMSGSIZE : 0;
	if (udc->tx_dma_active)
		return 0;
	memcpy(udc->tx_dma_buf,
	       (u8 *)req->req.buf + req->req.actual, count);
	tmpa9xx_cache_flush(udc->tx_dma_buf, count);
	dma_addr = virt_to_phys(udc->tx_dma_buf);
	ud2ab_write(udc, UD2AB_INTSTS,
		    UD2AB_INT_MR_DONE | UD2AB_INT_MR_AHBERR);
	ud2ab_write(udc, UD2AB_MRSADR, dma_addr);
	ud2ab_write(udc, UD2AB_MREADR, dma_addr + count - 1);
	ud2ab_write(udc, UD2AB_UDMSTSET, UD2AB_UDMST_MR_ENABLE);
	udc->tx_dma_active = true;
	udc->tx_dma_req = req;
	udc->tx_dma_len = count;
	ep->tx_active = true;
	return 0;
}

static int tmpa9xx_tx_dma_complete(struct tmpa9xx_udc *udc)
{
	struct tmpa9xx_ep *ep = &udc->eps[1];
	struct tmpa9xx_request *req = udc->tx_dma_req;

	if (!udc->tx_dma_active || !req)
		return 0;
	req->req.actual += udc->tx_dma_len;
	udc->tx_dma_active = false;
	udc->tx_dma_req = NULL;
	udc->tx_dma_len = 0;
	ep->tx_active = false;
	if (req->req.actual == req->req.length)
		tmpa9xx_complete(ep, req, 0);
	else
		return tmpa9xx_kick(ep);
	if (!list_empty(&ep->queue))
		return tmpa9xx_kick(ep);
	return 0;
}

static int tmpa9xx_controller_recover(struct tmpa9xx_udc *udc)
{
	int ret;
	unsigned int i;

	/* Keep queues closed until the hardware reset and gadget disconnect finish. */
	udc->fatal_error = true;
	ud2ab_write(udc, UD2AB_INTENB, 0);
	ud2ab_write(udc, UD2AB_PWCTL, UD2AB_PWCTL_PHY_RESET);
	mdelay(1);
	ud2ab_write(udc, UD2AB_PWCTL,
		    UD2AB_PWCTL_SUSPEND | UD2AB_PWCTL_PHY_RESET);
	mdelay(1);
	ud2ab_write(udc, UD2AB_PWCTL, UD2AB_PWCTL_PHY_RESET);
	mdelay(1);
	ud2ab_write(udc, UD2AB_PWCTL,
		    UD2AB_PWCTL_PHY_RESET | UD2AB_PWCTL_POWER_RESET);
	mdelay(1);
	/* UDMWTOUT resets to timeout enabled; disable it before MW transfers. */
	ud2ab_write(udc, UD2AB_MWTOUT, UD2AB_MWTOUT_DISABLE);

	ret = tmpa9xx_dma_master_reset(udc);
	if (!ret) {
		ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_CLEAR);
		ret = ud2_write(udc, UD2INT, UD2_INT_MASK);
	}
	if (!ret)
		ret = ud2_command(udc, 0, UD2_CMD_ALL_INVALID);
	if (!ret)
		ud2ab_write(udc, UD2AB_INTENB, UD2AB_INT_MASK);

	tmpa9xx_dma_drop_state(udc);
	for (i = 0; i < TMPA9XX_NUM_EPS; i++)
		tmpa9xx_nuke(&udc->eps[i], -EIO);
	tmpa9xx_reinit(udc);
	udc->gadget.speed = USB_SPEED_UNKNOWN;
	tmpa9xx_notify_disconnect(udc);
	if (ret)
		udc->fatal_error = true;
	else
		udc->fatal_error = false;
	return ret;
}

static int tmpa9xx_dma_start(struct tmpa9xx_ep *ep,
			     struct tmpa9xx_request *req, u16 count)
{
	struct tmpa9xx_udc *udc = ep->udc;
	ulong dma_addr;
	u32 status;
	int ret;

	if (!count || count > TMPA9XX_DMA_BUF_SIZE)
		return count ? -EMSGSIZE : 0;
	/* An interrupt/kick while the current packet is active is benign. */
	if (udc->dma_active)
		return 0;

	/* A stale MW_SET status must not retrigger this packet. */
	ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_DMA_SET);
	status = ud2ab_read(udc, UD2AB_INTSTS);
	if (status & (UD2AB_INT_DMA_TIMEOUT | UD2AB_INT_DMA_AHBERR |
		      UD2AB_INT_DMA_RDERR)) {
		ret = tmpa9xx_dma_reset(udc);
		if (ret)
			tmpa9xx_controller_recover(udc);
		return -EIO;
	}
	/* UDMSTSTS bit 0 confirms that the endpoint has a packet for MW. */
	status = ud2ab_read(udc, UD2AB_MSTSTS);
	if (!(status & UD2AB_MSTSTS_MW_EP_DSET))
		return 0;

	/* UDMWSADR/UDMWEADR are byte addresses; the buffer is cache-line aligned. */
	dma_addr = virt_to_phys(udc->dma_buf);
	tmpa9xx_cache_invalidate(udc->dma_buf, count);
	ud2ab_write(udc, UD2AB_MWSADR, dma_addr);
	ud2ab_write(udc, UD2AB_MWEADR, dma_addr + count - 1);
	ud2ab_write(udc, UD2AB_UDMSTSET, UD2AB_UDMST_MW_ENABLE);
	udc->dma_req = req;
	udc->dma_len = count;
	udc->dma_active = true;
	return 0;
}

static int tmpa9xx_dma_complete(struct tmpa9xx_udc *udc)
{
	struct tmpa9xx_ep *ep = &udc->eps[2];
	struct tmpa9xx_request *req = udc->dma_req;
	u32 current, ahb;
	ulong dma_addr, expected_current, expected_ahb;
	unsigned int room, copy;
	bool short_packet;

	if (!udc->dma_active || !req)
		return 0;

	/*
	 * MWCADR documents the completed byte range and must be read through
	 * DMACRDREQ; MWAHBADR is a direct UDC2AB status register.
	 */
	if (udmac_read(udc, UD2AB_MWCADR, &current)) {
		tmpa9xx_dma_abort(udc);
		return -ETIMEDOUT;
	}
	/* MWAHBADR is a direct UDC2AB status register (only MWCADR is indirect). */
	ahb = ud2ab_read(udc, UD2AB_MWAHBADR);
	dma_addr = virt_to_phys(udc->dma_buf);
	/*
	 * TMPA910 reports inclusive addresses: MWCADR is the final byte written,
	 * while MWAHBADR is the final byte of the containing AHB word.  A real
	 * 14-byte Fastboot command at 0x43fefb60 produced 0x43fefb6d and
	 * 0x43fefb6f respectively.
	 */
	expected_current = dma_addr + udc->dma_len - 1;
	expected_ahb = ALIGN(dma_addr + udc->dma_len, sizeof(u32)) - 1;
	if (current != expected_current || ahb != expected_ahb) {
		/* A partial/misaligned completion must never be copied to the request. */
		tmpa9xx_dma_abort(udc);
		return -EIO;
	}
	tmpa9xx_cache_invalidate(udc->dma_buf, udc->dma_len);
	room = req->req.length - req->req.actual;
	copy = min_t(unsigned int, room, udc->dma_len);
	if (copy)
		memcpy((u8 *)req->req.buf + req->req.actual, udc->dma_buf, copy);
	if (udc->dma_len > room)
		req->req.status = -EOVERFLOW;
	req->req.actual += copy;
	short_packet = udc->dma_len < ep->ep.maxpacket;
	if (ep->rx_zlp_pending) {
		/* RX_ZERO, rather than DSZ==0, terminates a full-size OUT request. */
		ep->rx_zlp_pending = false;
		short_packet = true;
	}
	udc->dma_active = false;
	udc->dma_req = NULL;
	udc->dma_len = 0;

	if (short_packet || req->req.actual == req->req.length)
		tmpa9xx_complete(ep, req, req->req.status == -EINPROGRESS ?
				  0 : req->req.status);
	else
		tmpa9xx_kick(ep);
	return 0;
}

static int tmpa9xx_fifo_write(struct tmpa9xx_ep *ep,
			      struct tmpa9xx_request *req)
{
	struct tmpa9xx_udc *udc = ep->udc;
	const u8 *buf = req->req.buf;
	u16 msz;
	unsigned int left, count, i;
	int ret;

	if (ep->tx_active)
		return 0;
	if (ep->need_zlp) {
		ret = ud2_command(udc, ep->number, UD2_CMD_TX_ZLP);
		if (ret)
			return ret;
		ep->need_zlp = false;
		ep->tx_active = true;
		return 0;
	}
	if (ud2_read(udc, tmpa9xx_ep_msz_reg(ep->number), &msz))
		return -ETIMEDOUT;
	if (msz & UD2_EP_DSET)
		return 0;

	left = req->req.length - req->req.actual;
	count = min_t(unsigned int, left, ep->ep.maxpacket);
	if (ep->number == 1)
		return tmpa9xx_tx_dma_start(ep, req, count);
	for (i = 0; i + 1 < count; i += 2)
		if (ud2_write(udc, tmpa9xx_ep_fifo_reg(ep->number),
			      buf[req->req.actual + i] |
			      (buf[req->req.actual + i + 1] << 8)))
			return -ETIMEDOUT;
	if (i < count) {
		/*
		 * A 16-bit PVCI write would append a zero byte to an odd-length
		 * packet.  MuCross temporarily programs a one-byte packet and uses
		 * an 8-bit FIFO write for the tail byte; the configuration descriptor
		 * header is the first standard request that exercises this path.
		 */
		udelay(30);
		ret = ud2_write(udc, tmpa9xx_ep_msz_reg(ep->number), 1);
		if (ret)
			return ret;
		udelay(50);
		writeb(buf[req->req.actual + i],
		       udc->base + tmpa9xx_ep_fifo_reg(ep->number));
		udelay(50);
		ret = ud2_write(udc, tmpa9xx_ep_msz_reg(ep->number), msz);
		if (ret)
			return ret;
		udelay(50);
	}

	req->req.actual += count;
	if (!count)
		ret = ud2_command(udc, ep->number, UD2_CMD_TX_ZLP);
	else if (count < ep->ep.maxpacket)
		ret = ud2_command(udc, ep->number, UD2_CMD_EP_EOP);
	else
		ret = 0;
	if (ret)
		return ret;
	ep->tx_active = true;
	if (req->req.actual == req->req.length &&
	    req->req.zero && count == ep->ep.maxpacket)
		ep->need_zlp = true;

	return 0;
}

static int tmpa9xx_fifo_read(struct tmpa9xx_ep *ep,
			     struct tmpa9xx_request *req)
{
	struct tmpa9xx_udc *udc = ep->udc;
	u16 size;
	unsigned int count, room, copy, i;
	u16 word;

	if (ep->number == 2 && udc->dma_active)
		return 0;

	if (ud2_read(udc, tmpa9xx_ep_dsz_reg(ep->number), &size))
		return -ETIMEDOUT;
	count = size & UD2_EP_DATA_SIZE_MASK;
	if (!count) {
		/* DSZ is not a ZLP indication; RX_ZERO handles OUT ZLPs below. */
		return 0;
	}
	if (ep->number != 2) {
		room = req->req.length - req->req.actual;
		copy = min_t(unsigned int, room, count);
		for (i = 0; i < count; i += 2) {
			if (ud2_read(udc, tmpa9xx_ep_fifo_reg(ep->number), &word))
				return -ETIMEDOUT;
			if (i < copy)
				((u8 *)req->req.buf)[req->req.actual + i] = word & 0xff;
			if (i + 1 < copy)
				((u8 *)req->req.buf)[req->req.actual + i + 1] = word >> 8;
		}
		req->req.actual += copy;
		if (count > room)
			req->req.status = -EOVERFLOW;
		if (count < ep->ep.maxpacket ||
		    req->req.actual == req->req.length)
			tmpa9xx_complete(ep, req, req->req.status == -EINPROGRESS ?
				  0 : req->req.status);
		return 0;
	}

	/* EP2 OUT is the high-throughput path: UDC2AB writes the packet to RAM. */
	return tmpa9xx_dma_start(ep, req, count);
}

static int tmpa9xx_kick(struct tmpa9xx_ep *ep)
{
	struct tmpa9xx_request *req;
	int ret, recover;
	bool dma_recovered = false;

	if (list_empty(&ep->queue) || ep->halted)
		return 0;
	req = list_first_entry(&ep->queue, struct tmpa9xx_request, queue);
	if (ep->number == 2 && ep->udc->dma_active &&
	    ep->udc->dma_req == req)
		return 0;
	if (ep->is_in)
		ret = tmpa9xx_fifo_write(ep, req);
	else
		ret = tmpa9xx_fifo_read(ep, req);
	if (ret) {
		/*
		 * Propagate PVCI/DMA failures to the gadget instead of wedging its
		 * request queue.  Reset the affected FIFO before accepting another.
		 */
		recover = 0;
		if (ep->number == 2 && ep->udc->dma_active &&
		    ep->udc->dma_req == req) {
			recover = tmpa9xx_dma_abort(ep->udc);
			dma_recovered = !recover;
		}
		if (!recover && !dma_recovered) {
			recover = ud2_command(ep->udc, ep->number,
					      UD2_CMD_FIFO_CLEAR);
			if (!recover)
				recover = ud2_command(ep->udc, ep->number, UD2_CMD_EP_RESET);
		}
		tmpa9xx_complete(ep, req, ret);
	}
	return ret;
}

static int tmpa9xx_ep_flush(struct tmpa9xx_ep *ep, int status, bool disable)
{
	int ret = 0;
	bool fifo_cleared = false;

	ep->need_zlp = false;
	ep->tx_active = false;
	ep->rx_zlp_pending = false;
	if (ep->number == 2 && ep->udc->dma_active) {
		ret = tmpa9xx_dma_abort(ep->udc);
		if (!ret)
			fifo_cleared = true;
	}
	if (!ret && !fifo_cleared) {
		ret = ud2_command(ep->udc, ep->number, UD2_CMD_FIFO_CLEAR);
		if (!ret)
			ret = ud2_command(ep->udc, ep->number, UD2_CMD_EP_RESET);
	}
	if (!ret && disable)
		ret = ud2_command(ep->udc, ep->number, UD2_CMD_EP_INVALID);
	tmpa9xx_nuke(ep, status);
	if (ret)
		ep->halted = true;
	return ret;
}

static int tmpa9xx_ep_enable(struct usb_ep *usb_ep,
			     const struct usb_endpoint_descriptor *desc)
{
	struct tmpa9xx_ep *ep = to_tmpa9xx_ep(usb_ep);
	u16 maxpacket;
	int ret;

	if (!desc || ep->number == 0 || ep->desc)
		return -EINVAL;
	maxpacket = usb_endpoint_maxp(desc) & 0x7ff;
	if (!maxpacket || maxpacket > ep->maxpacket)
		return -EINVAL;
	if (usb_endpoint_num(desc) != ep->number ||
	    usb_endpoint_xfer_bulk(desc) == 0 ||
	    usb_endpoint_dir_in(desc) != ep->is_in)
		return -EINVAL;

	ep->desc = desc;
	ep->ep.desc = desc;
	ep->ep.maxpacket = maxpacket;
	ep->halted = false;
	ep->need_zlp = false;
	ep->tx_active = false;
	ep->rx_zlp_pending = false;
	ret = ud2_write(ep->udc, tmpa9xx_ep_msz_reg(ep->number), maxpacket);
	if (!ret)
		ret = ud2_write(ep->udc, UD2EP1_STS +
				(ep->number - 1) * 0x10,
				ep->is_in ? UD2_EP_DUAL_BULK_IN : UD2_EP_DUAL_BULK_OUT);
	if (!ret)
		ret = ud2_command(ep->udc, ep->number, UD2_CMD_FIFO_CLEAR);
	if (!ret)
		ret = ud2_command(ep->udc, ep->number, UD2_CMD_EP_RESET);
	if (ret) {
		ep->desc = NULL;
		ep->ep.desc = NULL;
		return ret;
	}
	return 0;
}

static int tmpa9xx_ep_disable(struct usb_ep *usb_ep)
{
	struct tmpa9xx_ep *ep = to_tmpa9xx_ep(usb_ep);
	int ret;

	if (ep->number == 0)
		return -EINVAL;
	ret = tmpa9xx_ep_flush(ep, -ESHUTDOWN, true);
	ep->desc = NULL;
	ep->ep.desc = NULL;
	ep->ep.maxpacket = ep->maxpacket;
	ep->need_zlp = false;
	return ret;
}

static struct usb_request *tmpa9xx_alloc_request(struct usb_ep *usb_ep,
						 gfp_t gfp_flags)
{
	struct tmpa9xx_request *req = calloc(1, sizeof(*req));

	if (!req)
		return NULL;
	INIT_LIST_HEAD(&req->queue);
	return &req->req;
}

static void tmpa9xx_free_request(struct usb_ep *usb_ep,
				 struct usb_request *usb_req)
{
	struct tmpa9xx_request *req = to_tmpa9xx_req(usb_req);

	free(req);
}

static int tmpa9xx_ep_queue(struct usb_ep *usb_ep,
			    struct usb_request *usb_req, gfp_t gfp_flags)
{
	struct tmpa9xx_ep *ep = to_tmpa9xx_ep(usb_ep);
	struct tmpa9xx_request *req;
	bool idle;

	if (!usb_req || !usb_req->complete ||
	    (usb_req->length && !usb_req->buf))
		return -EINVAL;
	req = to_tmpa9xx_req(usb_req);
	if (!list_empty(&req->queue))
		return -EINVAL;
	if (ep->number && !ep->desc)
		return -ESHUTDOWN;
	if (ep->udc->fatal_error)
		return -EIO;
	if (!ep->udc->driver || ep->udc->gadget.speed == USB_SPEED_UNKNOWN)
		return -ESHUTDOWN;

	idle = list_empty(&ep->queue);
	usb_req->status = -EINPROGRESS;
	usb_req->actual = 0;
	req->ep = ep;
	list_add_tail(&req->queue, &ep->queue);
	/*
	 * UDC2 generates the EP0 status handshake in hardware.  The MuCross
	 * driver completes a zero-length EP0 request without issuing TX_ZLP,
	 * then finishes the transaction through STATUS/STATUS-NAK events.
	 * Sending TX_ZLP here leaves SET_CONFIGURATION waiting forever.
	 */
	if (!ep->number && !usb_req->length) {
		tmpa9xx_complete(ep, req, 0);
		return 0;
	}
	if (ep->number == 2 && !ep->is_in && ep->rx_zlp_pending &&
	    !ep->udc->dma_active) {
		ep->rx_zlp_pending = false;
		tmpa9xx_complete(ep, req, 0);
		return 0;
	}
	if (idle)
		return tmpa9xx_kick(ep);
	return 0;
}

static int tmpa9xx_ep_dequeue(struct usb_ep *usb_ep,
			      struct usb_request *usb_req)
{
	struct tmpa9xx_ep *ep = to_tmpa9xx_ep(usb_ep);
	struct tmpa9xx_request *req;
	bool active;
	bool fifo_cleared = false;
	int ret = 0;

	if (!usb_req)
		return -EINVAL;
	req = to_tmpa9xx_req(usb_req);

	if (list_empty(&req->queue) || req->ep != ep)
		return -EINVAL;
	active = list_first_entry(&ep->queue, struct tmpa9xx_request, queue) == req;
	if (active) {
		ep->need_zlp = false;
		ep->tx_active = false;
		ep->rx_zlp_pending = false;
		if (ep->number == 2 && ep->udc->dma_active &&
		    ep->udc->dma_req == req) {
			ret = tmpa9xx_dma_abort(ep->udc);
			fifo_cleared = !ret;
		}
		if (!ret && !fifo_cleared) {
			ret = ud2_command(ep->udc, ep->number, UD2_CMD_FIFO_CLEAR);
			if (!ret)
				ret = ud2_command(ep->udc, ep->number,
						  UD2_CMD_EP_RESET);
		}
	}
	tmpa9xx_complete(ep, req, -ECONNRESET);
	if (ret)
		ep->halted = true;
	if (active && !ret)
		tmpa9xx_kick(ep);
	return ret;
}

static int tmpa9xx_ep_set_halt(struct usb_ep *usb_ep, int value)
{
	struct tmpa9xx_ep *ep = to_tmpa9xx_ep(usb_ep);
	int ret;

	if (value) {
		ret = ud2_command(ep->udc, ep->number, UD2_CMD_EP_STALL);
		if (ret)
			return ret;
		ep->halted = true;
		ep->need_zlp = false;
		ep->tx_active = false;
		ep->rx_zlp_pending = false;
	} else {
		ep->need_zlp = false;
		if (ep->number == 2 && ep->udc->dma_active) {
			ret = tmpa9xx_dma_abort(ep->udc);
			if (ret) {
				ep->halted = true;
				return ret;
			}
		} else {
			ret = ud2_command(ep->udc, ep->number,
					  UD2_CMD_FIFO_CLEAR);
			if (!ret)
				ret = ud2_command(ep->udc, ep->number,
						  UD2_CMD_EP_RESET);
			if (ret)
				return ret;
		}
		ep->halted = false;
		tmpa9xx_kick(ep);
	}
	return 0;
}

static const struct usb_ep_ops tmpa9xx_ep_ops = {
	.enable = tmpa9xx_ep_enable,
	.disable = tmpa9xx_ep_disable,
	.alloc_request = tmpa9xx_alloc_request,
	.free_request = tmpa9xx_free_request,
	.queue = tmpa9xx_ep_queue,
	.dequeue = tmpa9xx_ep_dequeue,
	.set_halt = tmpa9xx_ep_set_halt,
};

static int tmpa9xx_set_selfpowered(struct usb_gadget *gadget, int value)
{
	return 0;
}

static int tmpa9xx_pullup(struct usb_gadget *gadget, int value)
{
	struct tmpa9xx_udc *udc = &controller;

	return ud2_command(udc, 0, value ? UD2_CMD_USB_READY :
			   UD2_CMD_ALL_INVALID);
}

static const struct usb_gadget_ops tmpa9xx_gadget_ops = {
	.set_selfpowered = tmpa9xx_set_selfpowered,
	.pullup = tmpa9xx_pullup,
};

static void tmpa9xx_reinit(struct tmpa9xx_udc *udc)
{
	unsigned int i;

	INIT_LIST_HEAD(&udc->gadget.ep_list);
	for (i = 0; i < TMPA9XX_NUM_EPS; i++) {
		struct tmpa9xx_ep *ep = &udc->eps[i];

		INIT_LIST_HEAD(&ep->queue);
		INIT_LIST_HEAD(&ep->ep.ep_list);
		ep->desc = NULL;
		ep->ep.desc = NULL;
		ep->halted = false;
		ep->need_zlp = false;
		ep->tx_active = false;
		ep->rx_zlp_pending = false;
		usb_ep_set_maxpacket_limit(&ep->ep, ep->maxpacket);
		if (i)
			list_add_tail(&ep->ep.ep_list, &udc->gadget.ep_list);
	}
	udc->address = 0;
	udc->address_pending = false;
	udc->pending_state = UD2_ADDR_STATE_DEFAULT;
	udc->state_pending = false;
}

static void tmpa9xx_notify_disconnect(struct tmpa9xx_udc *udc)
{
	if (udc->driver && udc->driver->disconnect && !udc->disconnected) {
		udc->driver->disconnect(&udc->gadget);
		udc->disconnected = true;
	}
}

static void tmpa9xx_stop_activity(struct tmpa9xx_udc *udc)
{
	unsigned int i;

	for (i = 0; i < TMPA9XX_NUM_EPS; i++)
		tmpa9xx_ep_flush(&udc->eps[i], -ECONNRESET, false);
	udc->gadget.speed = USB_SPEED_UNKNOWN;
	tmpa9xx_notify_disconnect(udc);
	tmpa9xx_reinit(udc);
}

static int tmpa9xx_handle_setup(struct tmpa9xx_udc *udc)
{
	struct usb_ctrlrequest ctrl;
	struct tmpa9xx_ep *ep0 = &udc->eps[0];
	u16 brq;
	int ret;

	tmpa9xx_nuke(ep0, -EPROTO);
	if (ud2_read(udc, UD2BRQ, &brq) ||
	    ud2_read(udc, UD2VAL, &ctrl.wValue) ||
	    ud2_read(udc, UD2IDX, &ctrl.wIndex) ||
	    ud2_read(udc, UD2LEN, &ctrl.wLength))
		return -ETIMEDOUT;
	ctrl.bRequestType = brq;
	ctrl.bRequest = brq >> 8;
	ep0->is_in = !!(ctrl.bRequestType & USB_DIR_IN);
	ret = ud2_command(udc, 0, UD2_CMD_SETUP_RECEIVED);
	if (ret)
		return ret;

	if (ctrl.bRequestType == (USB_DIR_OUT | USB_RECIP_DEVICE) &&
	    ctrl.bRequest == USB_REQ_SET_ADDRESS) {
		udc->address = le16_to_cpu(ctrl.wValue) & 0x7f;
		udc->address_pending = true;
		udc->pending_state = udc->address ? UD2_ADDR_STATE_ADDRESSED :
			UD2_ADDR_STATE_DEFAULT;
		return ud2_command(udc, 0, UD2_CMD_TX_ZLP);
	}
	if (ctrl.bRequestType ==
	    (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT) &&
	    ctrl.bRequest == USB_REQ_CLEAR_FEATURE &&
	    le16_to_cpu(ctrl.wValue) == USB_ENDPOINT_HALT &&
	    !le16_to_cpu(ctrl.wLength)) {
		u16 index = le16_to_cpu(ctrl.wIndex);
		u8 number = index & USB_ENDPOINT_NUMBER_MASK;
		struct tmpa9xx_ep *ep;

		if (!number || number >= TMPA9XX_NUM_EPS)
			return -EINVAL;
		ep = &udc->eps[number];
		if (!!(index & USB_DIR_IN) != ep->is_in)
			return -EINVAL;
		ret = tmpa9xx_ep_set_halt(&ep->ep, 0);
		if (ret)
			return ret;
		/* MuCross finishes CLEAR_FEATURE through the UDC2 setup FSM. */
		return ud2_command(udc, 0, UD2_CMD_SETUP_FIN);
	}

	ret = udc->driver->setup(&udc->gadget, &ctrl);
	if (ret < 0) {
		int stall_ret = ud2_command(udc, 0, UD2_CMD_EP_STALL);

		return stall_ret ? stall_ret : ret;
	}

	if (ctrl.bRequestType == (USB_DIR_OUT | USB_RECIP_DEVICE) &&
	    ctrl.bRequest == USB_REQ_SET_CONFIGURATION) {
		udc->pending_state = le16_to_cpu(ctrl.wValue) ?
			UD2_ADDR_STATE_CONFIGURED : UD2_ADDR_STATE_ADDRESSED;
		udc->state_pending = true;
	}
	return 0;
}

static int tmpa9xx_handle_tx_ack(struct tmpa9xx_ep *ep)
{
	struct tmpa9xx_request *req;

	if (!ep->tx_active)
		return 0;
	ep->tx_active = false;
	if (list_empty(&ep->queue))
		return 0;
	req = list_first_entry(&ep->queue, struct tmpa9xx_request, queue);
	if (ep->need_zlp || req->req.actual < req->req.length)
		return tmpa9xx_kick(ep);
	tmpa9xx_complete(ep, req, 0);
	if (!ep->udc->fatal_error && !list_empty(&ep->queue))
		return tmpa9xx_kick(ep);
	return 0;
}

static int tmpa9xx_handle_ep0(struct tmpa9xx_udc *udc)
{
	struct tmpa9xx_ep *ep = &udc->eps[0];
	int ret;

	ret = ud2_write(udc, UD2INT, UD2_INT_CLEAR_BASE | UD2_INT_EP0);
	if (ret)
		return ret;
	if (ep->is_in)
		return tmpa9xx_handle_tx_ack(ep);
	if (!list_empty(&ep->queue))
		return tmpa9xx_kick(ep);
	return 0;
}

static int tmpa9xx_handle_bulk(struct tmpa9xx_udc *udc)
{
	struct tmpa9xx_ep *in = &udc->eps[1];
	struct tmpa9xx_ep *out = &udc->eps[2];
	u16 events;
	int ret;

	if (ud2_read(udc, UD2INT_EP, &events)) {
		tmpa9xx_ep_flush(in, -ETIMEDOUT, false);
		tmpa9xx_ep_flush(out, -ETIMEDOUT, false);
		return -ETIMEDOUT;
	}
	events &= UD2_INT_EP_EVENT_MASK;
	if (events & BIT(1)) {
		ret = ud2_write(udc, UD2INT_EP, BIT(1));
		if (ret) {
			tmpa9xx_ep_flush(in, -ETIMEDOUT, false);
			return ret;
		}
		ret = tmpa9xx_handle_tx_ack(in);
		if (ret)
			return ret;
	}
	if (events & BIT(2)) {
		ret = ud2_write(udc, UD2INT_EP, BIT(2));
		if (ret) {
			tmpa9xx_ep_flush(out, -ETIMEDOUT, false);
			return ret;
		}
		ret = tmpa9xx_kick(out);
		if (ret)
			return ret;
	}
	/* Clear the aggregate only after the per-endpoint flags are handled. */
	ret = ud2_write(udc, UD2INT,
			UD2_INT_CLEAR_BASE | UD2_INT_EP);
	return ret;
}

static void tmpa9xx_handle_out_zlp(struct tmpa9xx_udc *udc)
{
	struct tmpa9xx_ep *ep = &udc->eps[2];
	struct tmpa9xx_request *req;

	ep->rx_zlp_pending = true;
	if (udc->dma_active || list_empty(&ep->queue))
		return;
	ep->rx_zlp_pending = false;
	req = list_first_entry(&ep->queue, struct tmpa9xx_request, queue);
	tmpa9xx_complete(ep, req, 0);
}

static int tmpa9xx_poll(struct tmpa9xx_udc *udc)
{
	unsigned int loops = 32;

	if (udc->fatal_error)
		return -EIO;
	while (loops--) {
		u32 status = ud2ab_read(udc, UD2AB_INTSTS);
		int ret;

		if (!(status & UD2AB_INT_PENDING_MASK))
			break;
		if (status & UD2AB_INT_RESET) {
			ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_RESET);
			tmpa9xx_stop_activity(udc);
			continue;
		}
		if (status & UD2AB_INT_RESET_END) {
			u16 address;

			ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_RESET_END);
			if (!ud2_read(udc, UD2ADR, &address) &&
			    (address & UD2_ADDR_SPEED_MASK) == UD2_ADDR_HIGH_SPEED)
				udc->gadget.speed = USB_SPEED_HIGH;
			else
				udc->gadget.speed = USB_SPEED_FULL;
			udc->eps[1].maxpacket =
				udc->gadget.speed == USB_SPEED_HIGH ? 512 : 64;
			udc->eps[2].maxpacket = udc->eps[1].maxpacket;
			if (tmpa9xx_dma_reset(udc))
				tmpa9xx_controller_recover(udc);
			continue;
		}
		if (status & (UD2AB_INT_DMA_TIMEOUT | UD2AB_INT_DMA_AHBERR |
			      UD2AB_INT_DMA_RDERR)) {
			struct tmpa9xx_request *req = udc->dma_req;
			int ret;

			ret = tmpa9xx_dma_abort(udc);
			if (req && !list_empty(&req->queue))
				tmpa9xx_complete(&udc->eps[2], req, -EIO);
			if (ret)
				udc->eps[2].halted = true;
			continue;
		}
		if (status & UD2AB_INT_MR_AHBERR) {
			struct tmpa9xx_request *req = udc->tx_dma_req;

			ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_MR_AHBERR);
			ret = tmpa9xx_tx_dma_reset(udc);
			if (req && !list_empty(&req->queue))
				tmpa9xx_complete(&udc->eps[1], req,
						 ret ? ret : -EIO);
			continue;
		}
		if (status & UD2AB_INT_MR_DONE) {
			ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_MR_DONE);
			ret = tmpa9xx_tx_dma_complete(udc);
			if (ret)
				return ret;
			continue;
		}
		if (status & UD2AB_INT_DMA_SET) {
			ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_DMA_SET);
			ret = tmpa9xx_kick(&udc->eps[2]);
			if (ret)
				return ret;
			continue;
		}
		if (status & UD2AB_INT_DMA_DONE) {
			struct tmpa9xx_request *req = udc->dma_req;
			int ret;

			ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_DMA_DONE);
			ret = tmpa9xx_dma_complete(udc);
			if (ret) {
				if (req && !list_empty(&req->queue))
					tmpa9xx_complete(&udc->eps[2], req, ret);
				udc->eps[2].halted = true;
			}
			continue;
		}
		if (status & UD2_INT_SETUP) {
			int ret;

			ret = ud2_write(udc, UD2INT,
					UD2_INT_CLEAR_BASE | UD2_INT_SETUP);
			if (ret) {
				tmpa9xx_ep_flush(&udc->eps[0], ret, false);
				return ret;
			}
			ret = tmpa9xx_handle_setup(udc);
			if (ret)
				tmpa9xx_ep_flush(&udc->eps[0], ret, false);
			continue;
		}
		if (status & UD2_INT_EP0) {
			ret = tmpa9xx_handle_ep0(udc);
			if (ret) {
				tmpa9xx_ep_flush(&udc->eps[0], ret, false);
				return ret;
			}
			continue;
		}
		if (status & UD2_INT_RX_ZERO) {
			u16 zero;

			if (ud2_read(udc, UD2INT_RX_ZERO, &zero)) {
				tmpa9xx_ep_flush(&udc->eps[2], -ETIMEDOUT, false);
				continue;
			}
			ret = ud2_write(udc, UD2INT_RX_ZERO,
					zero & UD2_INT_EP_EVENT_MASK);
			if (!ret)
				ret = ud2_write(udc, UD2INT,
						UD2_INT_CLEAR_BASE | UD2_INT_RX_ZERO);
			if (ret) {
				tmpa9xx_ep_flush(&udc->eps[2], ret, false);
				return ret;
			}
			if (zero & BIT(2))
				tmpa9xx_handle_out_zlp(udc);
			continue;
		}
		if (status & UD2_INT_NAK) {
			ret = ud2_write(udc, UD2INT,
					UD2_INT_CLEAR_BASE | UD2_INT_NAK);
			if (ret)
				return ret;
			continue;
		}
		if (status & UD2_INT_EP) {
			ret = tmpa9xx_handle_bulk(udc);
			if (ret)
				return ret;
			continue;
		}
		if (status & UD2_INT_STATUS) {
			ret = ud2_write(udc, UD2INT,
					UD2_INT_CLEAR_BASE | UD2_INT_STATUS);
			if (ret)
				return ret;
			if (udc->address_pending) {
				ret = ud2_write(udc, UD2ADR, udc->pending_state |
						udc->address);
				if (ret)
					return ret;
				udc->address_pending = false;
			}
			if (udc->state_pending) {
				ret = ud2_write(udc, UD2ADR, udc->pending_state |
						udc->address);
				if (ret)
					return ret;
				udc->state_pending = false;
			}
			continue;
		}
		if (status & UD2_INT_STATUS_NAK) {
			ret = ud2_write(udc, UD2INT,
					UD2_INT_CLEAR_BASE | UD2_INT_STATUS_NAK);
			if (!ret)
				ret = ud2_command(udc, 0, UD2_CMD_SETUP_FIN);
			if (ret)
				return ret;
			continue;
		}

		/* Clear only documented pending sources which need no work. */
		if (status & UD2_INT_SOF) {
			ret = ud2_write(udc, UD2INT,
					UD2_INT_CLEAR_BASE | UD2_INT_SOF);
			if (ret)
				return ret;
		}
		if (status & UD2AB_INT_SUSPEND)
			ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_SUSPEND);
		if (status & UD2_INT_RX_ZERO) {
			ret = ud2_write(udc, UD2INT,
					UD2_INT_CLEAR_BASE | UD2_INT_RX_ZERO);
			if (ret)
				return ret;
		}
	}
	return 0;
}

int tmpa9xx_udc_init(void)
{
	struct tmpa9xx_udc *udc = &controller;
	u32 power;
	int ret;

	if (udc->initialized)
		return 0;
	memset(udc, 0, sizeof(*udc));
	udc->base = (void __iomem *)TMPA9XX_UDC_BASE;
	udc->gadget.ops = &tmpa9xx_gadget_ops;
	udc->gadget.ep0 = &udc->eps[0].ep;
	udc->gadget.speed = USB_SPEED_UNKNOWN;
	udc->gadget.max_speed = USB_SPEED_HIGH;
	udc->gadget.is_dualspeed = 1;
	udc->gadget.name = "tmpa9xx-udc";

	udc->eps[0].ep.name = "ep0";
	udc->eps[0].ep.caps.type_control = true;
	udc->eps[0].ep.caps.dir_in = true;
	udc->eps[0].ep.caps.dir_out = true;
	udc->eps[0].maxpacket = 64;
	udc->eps[1].ep.name = "ep1in-bulk";
	udc->eps[1].ep.caps.type_bulk = true;
	udc->eps[1].ep.caps.dir_in = true;
	udc->eps[1].maxpacket = 512;
	udc->eps[1].is_in = true;
	udc->eps[2].ep.name = "ep2out-bulk";
	udc->eps[2].ep.caps.type_bulk = true;
	udc->eps[2].ep.caps.dir_out = true;
	udc->eps[2].maxpacket = 512;

	for (power = 0; power < TMPA9XX_NUM_EPS; power++) {
		udc->eps[power].ep.ops = &tmpa9xx_ep_ops;
		udc->eps[power].udc = udc;
		udc->eps[power].number = power;
	}
	tmpa9xx_reinit(udc);

	/*
	 * Datasheet 3.16.2.11: reset PHY, leave suspend, then release UDC.
	 * UDPWCTL reserved bits must be written as zero.
	 */
	power = UD2AB_PWCTL_SUSPEND;
	ud2ab_write(udc, UD2AB_PWCTL, power);
	mdelay(1);
	power = UD2AB_PWCTL_SUSPEND | UD2AB_PWCTL_PHY_RESET;
	ud2ab_write(udc, UD2AB_PWCTL, power);
	mdelay(1);
	power = UD2AB_PWCTL_PHY_RESET;
	ud2ab_write(udc, UD2AB_PWCTL, power);
	mdelay(1);
	power = UD2AB_PWCTL_PHY_RESET | UD2AB_PWCTL_POWER_RESET;
	ud2ab_write(udc, UD2AB_PWCTL, power);
	mdelay(1);
	/* UDMWTOUT defaults to enabled; fastboot uses explicit timeout disable. */
	ud2ab_write(udc, UD2AB_MWTOUT, UD2AB_MWTOUT_DISABLE);

	ret = ud2_write(udc, UD2INT, UD2_INT_MASK);
	if (ret)
		return tmpa9xx_init_error(udc, "ud2-write-int-mask", UD2INT, ret);
	ud2ab_write(udc, UD2AB_INTSTS, UD2AB_INT_CLEAR);
	ud2ab_write(udc, UD2AB_INTENB, UD2AB_INT_MASK);
	ret = tmpa9xx_tx_dma_reset(udc);
	if (ret)
		return tmpa9xx_init_error(udc, "mr-reset", UD2AB_UDMSTSET, ret);
	ret = tmpa9xx_dma_reset(udc);
	if (ret)
		return tmpa9xx_init_error(udc, "mw-reset", UD2AB_UDMSTSET, ret);
	ret = ud2_command(udc, 0, UD2_CMD_ALL_INVALID);
	if (ret)
		return tmpa9xx_init_error(udc, "all-invalid", UD2CMD, ret);
	udc->initialized = true;
	return 0;
}

int tmpa9xx_udc_shutdown(void)
{
	struct tmpa9xx_udc *udc = &controller;
	int ret = 0;

	if (!udc->initialized)
		return 0;
	if (udc->dma_active)
		ret = tmpa9xx_dma_abort(udc);
	if (!ret) {
		ret = ud2_command(udc, 0, UD2_CMD_ALL_INVALID);
		if (!ret)
			ret = tmpa9xx_dma_reset(udc);
	}
	/* Assert only the documented power-reset bit; reserved bits are zero. */
	ud2ab_write(udc, UD2AB_PWCTL, UD2AB_PWCTL_PHY_RESET);
	udc->initialized = false;
	return ret;
}

int usb_gadget_handle_interrupts(int index)
{
	return tmpa9xx_poll(&controller);
}

int usb_gadget_register_driver(struct usb_gadget_driver *driver)
{
	struct tmpa9xx_udc *udc = &controller;
	int ret;

	if (!driver || !driver->bind || !driver->setup)
		return -EINVAL;
	if (!udc->initialized)
		return -ENODEV;
	if (udc->driver)
		return -EBUSY;

	udc->driver = driver;
	udc->disconnected = false;
	ret = driver->bind(&udc->gadget);
	if (ret) {
		udc->driver = NULL;
		return ret;
	}
	ret = ud2_command(udc, 0, UD2_CMD_USB_READY);
	if (ret) {
		driver->unbind(&udc->gadget);
		udc->driver = NULL;
		return ret;
	}
	return 0;
}

int usb_gadget_unregister_driver(struct usb_gadget_driver *driver)
{
	struct tmpa9xx_udc *udc = &controller;
	unsigned int i;

	if (!driver || driver != udc->driver)
		return -EINVAL;
	udc->gadget.speed = USB_SPEED_UNKNOWN;
	tmpa9xx_notify_disconnect(udc);
	for (i = 0; i < TMPA9XX_NUM_EPS; i++)
		tmpa9xx_ep_flush(&udc->eps[i], -ESHUTDOWN, false);
	ud2_command(udc, 0, UD2_CMD_ALL_INVALID);
	driver->unbind(&udc->gadget);
	udc->driver = NULL;
	tmpa9xx_reinit(udc);
	return 0;
}
