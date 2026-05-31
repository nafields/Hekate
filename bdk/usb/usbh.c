/*
 * USB Host (XHCI) driver for Tegra X1
 *
 * Copyright (c) 2024 Hekate contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <string.h>

#include <usb/usbh.h>
#include <usb/usbd.h>
#include <usb/usb_t210.h>

#include <gfx_utils.h>
#include <mem/mc.h>
#include <soc/bpmp.h>
#include <soc/clock.h>
#include <soc/fuse.h>
#include <soc/pmc.h>
#include <soc/timer.h>
#include <soc/t210.h>

#include <memory_map.h>

/* XHCI host IPFS/PCI spaces mirror the device side, offset by 0x8000/0x9000. */
#define XUSB_HOST_PCI(off)  MMIO_REG32(XUSB_HOST_BASE + 0x8000, off)
#define XUSB_HOST_CFG(off)  MMIO_REG32(XUSB_HOST_BASE + 0x9000, off)

/* XHCI operational register offsets from op_base. */
#define XHCI_OP_USBCMD   0x00
#define  OP_CMD_RUN      BIT(0)
#define  OP_CMD_HCRST    BIT(1)
#define XHCI_OP_USBSTS   0x04
#define  OP_STS_HCH      BIT(0)  /* HC Halted */
#define  OP_STS_HSE      BIT(2)
#define  OP_STS_EINT     BIT(3)
#define  OP_STS_PCD      BIT(4)
#define XHCI_OP_CRCR_LO  0x18   /* Command Ring Control Register */
#define  OP_CRCR_RCS     BIT(0)  /* Ring Cycle State */
#define XHCI_OP_CRCR_HI  0x1C
#define XHCI_OP_DCBAAP_LO 0x30  /* Device Context Base Address Array Pointer */
#define XHCI_OP_DCBAAP_HI 0x34
#define XHCI_OP_CONFIG   0x38

/* XHCI port registers at op_base + 0x400 (port 0, USB2). */
#define XHCI_PORT_SC     0x400
#define  PORT_CCS        BIT(0)   /* Current Connect Status */
#define  PORT_PED        BIT(1)   /* Port Enabled */
#define  PORT_PR         BIT(4)   /* Port Reset */
#define  PORT_PS_SHIFT   10
#define  PORT_PS_MASK    (0xF << PORT_PS_SHIFT)
#define  PORT_CSC        BIT(17)  /* Connect Status Change */
#define  PORT_PRC        BIT(21)  /* Port Reset Change */
/* Write-1-to-clear bits that must be preserved when writing PORTSC. */
#define  PORT_W1C_BITS   (PORT_CSC | PORT_PRC | BIT(19) | BIT(23))

/* XHCI runtime interrupter 0 offsets from rt_base. */
#define XHCI_RT_IMAN     0x20
#define  RT_IMAN_IP      BIT(0)
#define  RT_IMAN_IE      BIT(1)
#define XHCI_RT_IMOD     0x24
#define XHCI_RT_ERSTSZ   0x28
#define XHCI_RT_ERSTBA_LO 0x30
#define XHCI_RT_ERSTBA_HI 0x34
#define XHCI_RT_ERDP_LO  0x38
#define  RT_ERDP_EHB     BIT(3)
#define XHCI_RT_ERDP_HI  0x3C

/* XHCI clock source for XUSB host core (adjacent to device core at 0x60C). */
#define CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_HOST 0x614

/* Timeout for polling loops (microseconds). */
#define USBH_TIMEOUT_US  500000

static usbh_ctxt_t usbh_ctxt;

usbh_ctxt_t *usbh_get_ctxt(void) { return &usbh_ctxt; }
bool          usbh_is_ready(void) { return usbh_ctxt.ready; }

/* ---------- helpers ---------------------------------------------------- */

#define OP(r)     MMIO_REG32(usbh_ctxt.op_base, r)
#define RT(r)     MMIO_REG32(usbh_ctxt.rt_base, r)
#define DB(slot)  MMIO_REG32(usbh_ctxt.db_base, (slot) * 4)

static int _wait_op_bits(u32 reg, u32 mask, u32 val)
{
	u32 retries = USBH_TIMEOUT_US;
	while ((OP(reg) & mask) != val) {
		if (!--retries)
			return USB_ERROR_TIMEOUT;
		usleep(1);
	}
	return USB_RES_OK;
}

/* ---------- PHY init (mirrors _xusb_init_phy in xusbd.c) --------------- */

static void _usbh_init_phy(void)
{
	clock_enable_pllu();

	CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) =
		(CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) & 0xFFFFFFFC) | 1;
	clock_enable_utmipll();
	CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) =
		(CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) & 0xFEFFFFE8) | 0x2000008 | 0x20 | 2;
	usleep(2);

	u32 fuse_usb_calib = FUSE(FUSE_USB_CALIB);
	XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_0) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_0) & 0xFFFFFFC0) | (fuse_usb_calib & 0x3F);
	XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1) & 0x83FFFF87) |
		((fuse_usb_calib & 0x780) >> 4) |
		((u32)(FUSE(FUSE_USB_CALIB_EXT) << 27) >> 1);

	XUSB_PADCTL(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD0_CTL1) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD0_CTL1) & 0xFFFFFE3F) | 0x80;

	XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_0) &= 0xDBFFFFFF;
	XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1) &= 0xFFFFFFFB;
	XUSB_PADCTL(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD0_CTL0) &= 0xFFFFFFFE;
	XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_0) &= 0xFFFFF7FF;
	(void)XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1);

	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_Y_SET)            = BIT(CLK_Y_USB2_TRK);
	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_USB2_HSIC_TRK) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_USB2_HSIC_TRK) & 0xFFFFFF00) | 6;

	XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) = 0x451E000;
	XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) =  0x51E000;
	usleep(100);
	XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) = 0x451E000;
	usleep(3);
	XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) =  0x51E000;
	usleep(100);
	XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) |= 0x4000000;

	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_Y_CLR) = BIT(CLK_Y_USB2_TRK);
	usleep(30);
}

/* ---------- host clock init -------------------------------------------- */

static void _usbh_init_host_clocks(void)
{
	CLOCK(CLK_RST_CONTROLLER_PLLU_OUTA) |= 1;
	usleep(2);

	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_SET) = BIT(CLK_U_XUSB_HOST);
	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_HOST) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_HOST) & 0x1FFFFF00) | (1 << 29) | 6;
	usleep(2);

	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FS) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FS) & 0x1FFFFFFF) | (2 << 29);

	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_W_SET)  = BIT(CLK_W_XUSB_SS);
	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_SS) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_SS) & 0x1FFFFF00) | (3 << 29) | 6;

	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB_SS);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_CLR) = BIT(CLK_U_XUSB_HOST);
	usleep(2);
}

/* ---------- ring helpers ----------------------------------------------- */

static void _ring_link_trb(u32 (*ring)[4], u32 count, u8 pcs)
{
	/* Initialise the Link TRB at the end of a transfer/command ring. */
	u32 *ltrb = ring[count - 1];
	ltrb[0] = (u32)ring[0];   /* Ring back to slot 0. */
	ltrb[1] = 0;
	ltrb[2] = 0;
	ltrb[3] = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC | (pcs & 1);
}

static u32 *_cmd_enqueue(u32 dw0, u32 dw1, u32 dw2, u32 dw3)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u32 *trb = cx->rings->cmd_ring[cx->cmd_idx];
	trb[0] = dw0; trb[1] = dw1; trb[2] = dw2;
	trb[3] = dw3 | (cx->cmd_pcs & 1);
	/* Advance, handling ring wrap.
	 * Per XHCI §4.9.3: update link TRB with CURRENT PCS first, then toggle PCS. */
	if (++cx->cmd_idx == USBH_TRB_RING_SZ - 1) {
		_ring_link_trb(cx->rings->cmd_ring, USBH_TRB_RING_SZ, cx->cmd_pcs);
		cx->cmd_idx = 0;
		cx->cmd_pcs ^= 1;
	}
	/* Ring host controller doorbell (slot 0 = command ring). */
	DB(0) = 0;
	return trb;
}

/* Poll event ring until an event TRB arrives; return pointer to it. */
static int _evt_wait(u32 *out_trb, u32 timeout_us)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u32 *trb = cx->rings->evt_ring[cx->evt_idx];

	while ((trb[3] & 1) != (cx->evt_ccs & 1)) {
		if (!timeout_us--)
			return USB_ERROR_TIMEOUT;
		usleep(1);
	}

	if (out_trb) {
		out_trb[0] = trb[0]; out_trb[1] = trb[1];
		out_trb[2] = trb[2]; out_trb[3] = trb[3];
	}

	/* Advance dequeue pointer; toggle CCS on wrap. */
	if (++cx->evt_idx == USBH_EVT_RING_SZ) {
		cx->evt_idx = 0;
		cx->evt_ccs ^= 1;
	}

	/* Update ERDP to tell controller we consumed this event. */
	u32 deq = (u32)cx->rings->evt_ring[cx->evt_idx];
	MMIO_REG32(cx->rt_base, XHCI_RT_ERDP_LO) = deq | RT_ERDP_EHB;
	MMIO_REG32(cx->rt_base, XHCI_RT_ERDP_HI) = 0;

	return USB_RES_OK;
}

/* ---------- command ring operations ------------------------------------ */

static int _cmd_enable_slot(u8 *slot_out)
{
	u32 *cmd = _cmd_enqueue(0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_EN_SLOT));
	(void)cmd;

	u32 evt[4];
	int res = _evt_wait(evt, USBH_TIMEOUT_US);
	if (res)
		return res;
	if (XHCI_EVT_TYPE(evt) != XHCI_TRB_EVT_CMD || XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS)
		return USB_ERROR_XFER_ERROR;
	*slot_out = XHCI_EVT_SLOT(evt);
	return USB_RES_OK;
}

static int _cmd_address_device(u8 slot_id, bool bsr)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u32 in_ctx_addr = (u32)cx->rings->in_ctrl;
	u32 dw3 = XHCI_TRB_SLOT(slot_id) | XHCI_TRB_TYPE(XHCI_TRB_ADDR_DEV) |
	          (bsr ? BIT(9) : 0);
	_cmd_enqueue(in_ctx_addr, 0, 0, dw3);

	u32 evt[4];
	int res = _evt_wait(evt, USBH_TIMEOUT_US);
	if (res)
		return res;
	if (XHCI_EVT_TYPE(evt) != XHCI_TRB_EVT_CMD || XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

static int _cmd_configure_ep(u8 slot_id)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u32 in_ctx_addr = (u32)cx->rings->in_ctrl;
	u32 dw3 = XHCI_TRB_SLOT(slot_id) | XHCI_TRB_TYPE(XHCI_TRB_CFG_EP);
	_cmd_enqueue(in_ctx_addr, 0, 0, dw3);

	u32 evt[4];
	int res = _evt_wait(evt, USBH_TIMEOUT_US);
	if (res)
		return res;
	if (XHCI_EVT_TYPE(evt) != XHCI_TRB_EVT_CMD || XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

/* ---------- EP0 control transfer --------------------------------------- */

static int _ctrl_xfer(const usb_ctrl_setup_t *setup, void *data, u32 len)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	bool data_in = (setup->bmRequestType & 0x80) != 0;
	u8   pcs     = cx->ep0_pcs;

	/* Setup Stage TRB — Immediate Data (IDT=1), TRT=3 for IN data, TRT=0 for no data. */
	u32 trt = (len > 0) ? (data_in ? 3 : 2) : 0;
	u32 *s = cx->rings->ep0_ring[cx->ep0_idx];
	memcpy(s, setup, 8);     /* DW0+DW1 = 8-byte setup packet */
	s[2] = 8;                /* TRB_TX_LEN = 8 */
	s[3] = XHCI_TRB_TYPE(XHCI_TRB_SETUP_STG) | XHCI_TRB_IDT | XHCI_TRB_IOC |
	       XHCI_TRB_TRT(trt) | (pcs & 1);
	cx->ep0_idx++;

	/* Data Stage TRB (when len > 0). */
	if (len > 0 && data) {
		u32 *d = cx->rings->ep0_ring[cx->ep0_idx];
		d[0] = (u32)data;
		d[1] = 0;
		d[2] = len;
		d[3] = XHCI_TRB_TYPE(XHCI_TRB_DATA_STG) | XHCI_TRB_ISP | XHCI_TRB_IOC |
		       (data_in ? XHCI_TRB_DIR_IN : 0) | (pcs & 1);
		cx->ep0_idx++;
	}

	/* Status Stage TRB — direction is opposite of data stage. */
	u32 *st = cx->rings->ep0_ring[cx->ep0_idx];
	st[0] = 0; st[1] = 0; st[2] = 0;
	st[3] = XHCI_TRB_TYPE(XHCI_TRB_STATUS_STG) | XHCI_TRB_IOC |
	        (data_in ? 0 : XHCI_TRB_DIR_IN) | (pcs & 1);
	cx->ep0_idx++;

	/* Handle ring wrap — link TRB is at index USBH_TRB_RING_SZ-1.
	 * Per XHCI §4.9.3: update link TRB with CURRENT PCS first, then toggle. */
	if (cx->ep0_idx >= USBH_TRB_RING_SZ - 1) {
		_ring_link_trb(cx->rings->ep0_ring, USBH_TRB_RING_SZ, cx->ep0_pcs);
		cx->ep0_idx = 0;
		cx->ep0_pcs ^= 1;
	}

	/* Ring doorbell: slot_id, target = DCI 1 (EP0). */
	DB(cx->slot_id) = XHCI_DCI_EP0;

	/* Wait for the last TRB's transfer event (status stage). */
	u32 evt[4];
	int res = _evt_wait(evt, USBH_TIMEOUT_US);
	if (res)
		return res;
	/* Drain any preceding data-stage event too (if data stage was queued). */
	if (len > 0 && data) {
		if (XHCI_EVT_TYPE(evt) == XHCI_TRB_EVT_XFER &&
		    XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS &&
		    XHCI_EVT_CC(evt) != XHCI_CC_SHORT_PKT)
			return USB_ERROR_XFER_ERROR;
		res = _evt_wait(evt, USBH_TIMEOUT_US);
		if (res)
			return res;
	}
	if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS && XHCI_EVT_CC(evt) != XHCI_CC_SHORT_PKT)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

/* ---------- bulk transfer ---------------------------------------------- */

int usbh_bulk_out(const void *buf, u32 len, u32 *actual)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u32 *trb = cx->rings->ep1out_ring[cx->ep1out_idx];
	trb[0] = (u32)buf;
	trb[1] = 0;
	trb[2] = len;
	trb[3] = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | (cx->ep1out_pcs & 1);

	if (++cx->ep1out_idx >= USBH_TRB_RING_SZ - 1) {
		_ring_link_trb(cx->rings->ep1out_ring, USBH_TRB_RING_SZ, cx->ep1out_pcs);
		cx->ep1out_idx = 0;
		cx->ep1out_pcs ^= 1;
	}

	DB(cx->slot_id) = XHCI_DCI_EP1_OUT;

	u32 evt[4];
	int res = _evt_wait(evt, USBH_TIMEOUT_US);
	if (res)
		return res;
	if (actual)
		*actual = len - XHCI_EVT_TX_LEN(evt);
	if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS && XHCI_EVT_CC(evt) != XHCI_CC_SHORT_PKT)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

int usbh_bulk_in(void *buf, u32 len, u32 *actual)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u32 *trb = cx->rings->ep1in_ring[cx->ep1in_idx];
	trb[0] = (u32)buf;
	trb[1] = 0;
	trb[2] = len;
	trb[3] = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP |
	         (cx->ep1in_pcs & 1);

	if (++cx->ep1in_idx >= USBH_TRB_RING_SZ - 1) {
		_ring_link_trb(cx->rings->ep1in_ring, USBH_TRB_RING_SZ, cx->ep1in_pcs);
		cx->ep1in_idx = 0;
		cx->ep1in_pcs ^= 1;
	}

	DB(cx->slot_id) = XHCI_DCI_EP1_IN;

	u32 evt[4];
	int res = _evt_wait(evt, USBH_TIMEOUT_US);
	if (res)
		return res;
	if (actual)
		*actual = len - XHCI_EVT_TX_LEN(evt);
	if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS && XHCI_EVT_CC(evt) != XHCI_CC_SHORT_PKT)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

/* ---------- context builders ------------------------------------------- */

static void _build_slot_ctx(u32 *ctx, u8 speed, u8 root_port, u8 ctx_entries)
{
	memset(ctx, 0, 32);
	ctx[0] = ((u32)speed << 20) | ((u32)ctx_entries << 27);
	ctx[1] = (u32)root_port << 16;
}

static void _build_ep_ctx(u32 *ctx, u8 ep_type, u16 max_packet, u32 ring_addr, u8 avg_len_hi)
{
	memset(ctx, 0, 32);
	ctx[1] = ((u32)3 << 1) |             /* CERR=3 */
	         ((u32)ep_type << 3) |
	         ((u32)max_packet << 16);
	ctx[2] = (ring_addr & ~0xFu) | 1;    /* TR Dequeue Ptr + DCS=1 */
	ctx[4] = (avg_len_hi ? 1024u : 8u);  /* avg_trb_length */
}

/* ---------- device enumeration ----------------------------------------- */

/* Issue USB SET_ADDRESS (handled by ADDRESS_DEVICE command). */
static int _enumerate_device(u8 root_port)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	usbh_rings_t *r  = cx->rings;

	/* --- ENABLE_SLOT --- */
	int res = _cmd_enable_slot(&cx->slot_id);
	if (res) {
		EPRINTF("USBH: Enable Slot failed.");
		return res;
	}

	/* --- Build Input Context for ADDRESS_DEVICE (Slot + EP0 only). --- */
	memset(r->in_ctrl, 0, sizeof(r->in_ctrl));
	r->in_ctrl[1] = BIT(0) | BIT(XHCI_DCI_EP0);   /* A[0]=Slot, A[1]=EP0 */

	u16 ep0_max_pkt = (cx->port_speed == XHCI_SPEED_HS) ? 64 : 8;
	_build_slot_ctx(r->in_slot, cx->port_speed, root_port, 1);
	_build_ep_ctx(r->in_ep[XHCI_DCI_EP0], XHCI_EP_CONTROL, ep0_max_pkt,
	              (u32)r->ep0_ring, 0);

	/* Point DCBAA[slot] at the device context. */
	r->dcbaa[cx->slot_id] = (u64)(u32)r->dev_slot;

	/* --- ADDRESS_DEVICE (BSR=false → actually sends SET_ADDRESS on the bus). --- */
	res = _cmd_address_device(cx->slot_id, false);
	if (res) {
		EPRINTF("USBH: Address Device failed.");
		return res;
	}

	return USB_RES_OK;
}

/* Configure bulk endpoints after discovering their addresses from config descriptor. */
static int _configure_bulk_eps(u8 out_ep_addr, u8 in_ep_addr)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	usbh_rings_t *r  = cx->rings;

	memset(r->in_ctrl, 0, sizeof(r->in_ctrl));
	r->in_ctrl[1] = BIT(0) | BIT(XHCI_DCI_EP1_OUT) | BIT(XHCI_DCI_EP1_IN);

	_build_slot_ctx(r->in_slot, cx->port_speed, 1, XHCI_DCI_EP1_IN);
	_build_ep_ctx(r->in_ep[XHCI_DCI_EP1_OUT], XHCI_EP_BULK_OUT, cx->max_packet,
	              (u32)r->ep1out_ring, 1);
	_build_ep_ctx(r->in_ep[XHCI_DCI_EP1_IN],  XHCI_EP_BULK_IN,  cx->max_packet,
	              (u32)r->ep1in_ring,  1);

	int res = _cmd_configure_ep(cx->slot_id);
	if (res)
		EPRINTF("USBH: Configure EP failed.");
	return res;
}

/* Read device descriptor class byte to detect hub (class 0x09) vs other. */
static int _get_dev_class(u8 *dev_class)
{
	/* GET_DESCRIPTOR(Device, index=0, len=18). */
	u8 desc_buf[18];
	usb_ctrl_setup_t setup = {
		.bmRequestType = 0x80,               /* Device-to-Host, Standard, Device */
		.bRequest      = USB_REQUEST_GET_DESCRIPTOR,
		.wValue        = 0x0100,             /* Descriptor Type=Device(1), Index=0 */
		.wIndex        = 0,
		.wLength       = 18,
	};
	int res = _ctrl_xfer(&setup, desc_buf, 18);
	if (!res)
		*dev_class = desc_buf[4];   /* bDeviceClass offset 4 */
	return res;
}

static int _set_configuration(u8 config_val)
{
	usb_ctrl_setup_t setup = {
		.bmRequestType = 0x00,
		.bRequest      = USB_REQUEST_SET_CONFIGURATION,
		.wValue        = config_val,
		.wIndex        = 0,
		.wLength       = 0,
	};
	return _ctrl_xfer(&setup, NULL, 0);
}

/* ---------- port detection and reset ----------------------------------- */

static int _port_wait_connect(u32 timeout_us)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	while (!(OP(XHCI_PORT_SC) & PORT_CCS)) {
		if (!timeout_us--)
			return USB_ERROR_TIMEOUT;
		usleep(1);
	}
	(void)cx;
	return USB_RES_OK;
}

static int _port_reset(void)
{
	/* Clear any pending status change bits, then issue reset. */
	u32 portsc = OP(XHCI_PORT_SC);
	OP(XHCI_PORT_SC) = (portsc & ~PORT_W1C_BITS) | PORT_PR;
	return _wait_op_bits(XHCI_PORT_SC, PORT_PRC, PORT_PRC);
}

/* ---------- top-level init --------------------------------------------- */

int usbh_init(void)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	memset(cx, 0, sizeof(*cx));

	bpmp_clk_rate_relaxed(true);

	/* Disable XUSB device clocks (device and host are mutually exclusive). */
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) = BIT(CLK_U_XUSB_DEV);
	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_CLR) = BIT(CLK_U_XUSB_DEV);

	/* Enable XUSB shared clocks and clear resets. */
	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_W_SET) = BIT(CLK_W_XUSB);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_SET) = BIT(CLK_W_XUSB);
	usleep(2);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB_PADCTL);
	usleep(2);

	/* Route USB2 pad to XUSB. */
	XUSB_PADCTL(XUSB_PADCTL_USB2_PAD_MUX) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_PAD_MUX) &
		 ~(PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_MASK | PADCTL_USB2_PAD_MUX_USB2_OTG_PAD_PORT0_MASK)) |
		PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_XUSB | PADCTL_USB2_PAD_MUX_USB2_OTG_PAD_PORT0_XUSB;

	_usbh_init_phy();

	/* Set port 0 to HOST mode (vs DEV mode used by xusbd.c). */
	XUSB_PADCTL(XUSB_PADCTL_USB2_PORT_CAP) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_PORT_CAP) & ~PADCTL_USB2_PORT_CAP_PORT_0_CAP_MASK) |
		PADCTL_USB2_PORT_CAP_PORT_0_CAP_HOST;

	/* Assert VBUS so downstream device powers on. */
	XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) & ~PADCTL_USB2_VBUS_ID_VBUS_OVR_MASK) |
		PADCTL_USB2_VBUS_ID_VBUS_OVR_EN | PADCTL_USB2_VBUS_ID_VBUS_ON;

	/* Force ID pin to ground so controller sees host role. */
	XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) & ~PADCTL_USB2_VBUS_ID_SRC_MASK) |
		PADCTL_USB2_VBUS_ID_SRC_ID_OVR_EN | PADCTL_USB2_VBUS_ID_OVR_GND;

	XUSB_PADCTL(XUSB_PADCTL_SS_PORT_MAP) &= ~PADCTL_SS_PORT_MAP_PORT0_MASK;
	PMC(APBDEV_PMC_USB_AO) &= 0xFFFFFFF3;
	usleep(1);

	_usbh_init_host_clocks();
	bpmp_clk_rate_relaxed(false);

	/* Enable AHB redirect for IRAM access (rings live there). */
	mc_enable_ahb_redirect();

	/* Enable XUSB host FPCI. */
	XUSB_HOST_CFG(XUSB_HOST_CONFIGURATION) |= HOST_CONFIGURATION_EN_FPCI;
	XUSB_HOST_PCI(XUSB_CFG_1) |= CFG_1_BUS_MASTER | CFG_1_MEMORY_SPACE | CFG_1_IO_SPACE;
	usleep(1);
	XUSB_HOST_PCI(XUSB_CFG_4) = XUSB_HOST_BASE | CFG_4_ADDRESS_TYPE_32_BIT;

	/* Read XHCI capability register to find operational base. */
	u32 cap_len = XUSB_HOST(0) & 0xFF;
	u32 rtsoff  = XUSB_HOST(0x18) & ~0x1F;
	u32 dboff   = XUSB_HOST(0x14) & ~0x3;
	cx->op_base = XUSB_HOST_BASE + cap_len;
	cx->rt_base = XUSB_HOST_BASE + rtsoff + 0x20;   /* +0x20 = interrupter 0 */
	cx->db_base = XUSB_HOST_BASE + dboff;

	/* Reset the host controller. */
	OP(XHCI_OP_USBCMD) |= OP_CMD_HCRST;
	int res = _wait_op_bits(XHCI_OP_USBSTS, OP_STS_HCH, OP_STS_HCH);
	if (res) { EPRINTF("USBH: HC reset timeout."); return res; }
	res = _wait_op_bits(XHCI_OP_USBCMD, OP_CMD_HCRST, 0);
	if (res) { EPRINTF("USBH: HC reset clear timeout."); return res; }

	/* --- Set up ring buffers at XUSB_RING_ADDR. --- */
	cx->rings = (usbh_rings_t *)XUSB_RING_ADDR;
	memset(cx->rings, 0, sizeof(usbh_rings_t));

	/* DCBAA: slot 0 = scratchpad (unused), slots 1+ = device contexts. */
	cx->rings->dcbaa[0] = 0;
	OP(XHCI_OP_DCBAAP_LO) = (u32)cx->rings->dcbaa;
	OP(XHCI_OP_DCBAAP_HI) = 0;

	/* Command ring: 3 usable TRBs + 1 Link. PCS starts at 1. */
	cx->cmd_pcs = 1;
	_ring_link_trb(cx->rings->cmd_ring, USBH_TRB_RING_SZ, cx->cmd_pcs);
	OP(XHCI_OP_CRCR_LO) = (u32)cx->rings->cmd_ring | OP_CRCR_RCS;
	OP(XHCI_OP_CRCR_HI) = 0;

	/* Event ring: 1 segment. */
	cx->evt_ccs = 1;
	cx->rings->erst_lo   = (u32)cx->rings->evt_ring;
	cx->rings->erst_hi   = 0;
	cx->rings->erst_size = USBH_EVT_RING_SZ;
	MMIO_REG32(cx->rt_base, XHCI_RT_ERSTSZ)    = 1;
	MMIO_REG32(cx->rt_base, XHCI_RT_ERSTBA_LO) = (u32)&cx->rings->erst_lo;
	MMIO_REG32(cx->rt_base, XHCI_RT_ERSTBA_HI) = 0;
	MMIO_REG32(cx->rt_base, XHCI_RT_ERDP_LO)   = (u32)cx->rings->evt_ring;
	MMIO_REG32(cx->rt_base, XHCI_RT_ERDP_HI)   = 0;

	/* Transfer rings: PCS starts at 1 for all. Link TRBs initialised. */
	cx->ep0_pcs    = cx->ep1out_pcs = cx->ep1in_pcs = 1;
	_ring_link_trb(cx->rings->ep0_ring,    USBH_TRB_RING_SZ, 1);
	_ring_link_trb(cx->rings->ep1out_ring, USBH_TRB_RING_SZ, 1);
	_ring_link_trb(cx->rings->ep1in_ring,  USBH_TRB_RING_SZ, 1);

	/* Enable MaxSlotsEn = 1 and start the controller. */
	OP(XHCI_OP_CONFIG) = 1;
	OP(XHCI_OP_USBCMD) |= OP_CMD_RUN;
	res = _wait_op_bits(XHCI_OP_USBSTS, OP_STS_HCH, 0);
	if (res) { EPRINTF("USBH: Start timeout."); return res; }

	/* Wait for a device to connect (2-second timeout). */
	usleep(100000);  /* Allow VBUS to stabilise. */
	res = _port_wait_connect(2000000);
	if (res) { return res; }  /* No device — silent return; caller checks usbh_is_ready(). */

	/* Reset the port. */
	res = _port_reset();
	if (res) { EPRINTF("USBH: Port reset timeout."); return res; }
	usleep(10000);

	/* Get port speed from PORTSC.PS[13:10]. */
	u32 ps = (OP(XHCI_PORT_SC) & PORT_PS_MASK) >> PORT_PS_SHIFT;
	cx->port_speed = ps ? ps : XHCI_SPEED_HS;
	cx->max_packet = (cx->port_speed == XHCI_SPEED_HS) ? 512 : 64;

	/* Enumerate: ENABLE_SLOT + ADDRESS_DEVICE. */
	res = _enumerate_device(1);
	if (res) return res;

	/* Read device descriptor class to detect hub (0x09) vs direct MSC. */
	u8 dev_class = 0;
	_get_dev_class(&dev_class);
	if (dev_class == 0x09) {
		/*
		 * Hub present: usbh_hub_enumerate() fully handles downstream
		 * enumeration including SET_CONFIGURATION + CONFIGURE_EP and
		 * sets cx->ready before returning.
		 */
		return usbh_hub_enumerate();
	}

	/* Direct-connect path: SET_CONFIGURATION 1. */
	res = _set_configuration(1);
	if (res) { EPRINTF("USBH: Set Configuration failed."); return res; }

	/* Configure bulk endpoints (EP1-OUT=0x01, EP1-IN=0x81 for standard MSC). */
	res = _configure_bulk_eps(0x01, 0x81);
	if (res) return res;

	cx->ready = true;
	return USB_RES_OK;
}

int usbh_ctrl_xfer(u8 req_type, u8 request, u16 value, u16 index,
                   void *buf, u16 len)
{
	usb_ctrl_setup_t setup = {
		.bmRequestType = req_type,
		.bRequest      = request,
		.wValue        = value,
		.wIndex        = index,
		.wLength       = len,
	};
	return _ctrl_xfer(&setup, buf, len);
}

void usbh_deinit(void)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	if (!cx->op_base) return;

	OP(XHCI_OP_USBCMD) &= ~OP_CMD_RUN;
	_wait_op_bits(XHCI_OP_USBSTS, OP_STS_HCH, OP_STS_HCH);

	/* Power down VBUS. */
	XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) &=
		~(PADCTL_USB2_VBUS_ID_VBUS_OVR_MASK | PADCTL_USB2_VBUS_ID_VBUS_ON);

	/* Reset clocks. */
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) = BIT(CLK_U_XUSB_HOST);
	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_CLR) = BIT(CLK_U_XUSB_HOST);

	memset(cx, 0, sizeof(*cx));
}
