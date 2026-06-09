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
#include <mem/heap.h>
#include <mem/mc.h>
#include <power/regulator_5v.h>
#include <soc/bpmp.h>
#include <soc/clock.h>
#include <soc/fuse.h>
#include <soc/pmc.h>
#include <soc/timer.h>
#include <soc/t210.h>
#include <storage/sd.h>

#include <memory_map.h>

/* XUSB host FPCI config space at +0x8000, IPFS at +0x9000 (per T210 TRM). */
#define XUSB_HOST_PCI(off)  MMIO_REG32(XUSB_HOST_BASE + 0x8000, off)
#define XUSB_HOST_CFG(off)  MMIO_REG32(XUSB_HOST_BASE + 0x9000, off)

/* XHCI operational register offsets from op_base. */
#define XHCI_OP_USBCMD   0x00
#define  OP_CMD_RUN      BIT(0)
#define  OP_CMD_HCRST    BIT(1)
#define XHCI_OP_USBSTS   0x04
#define  OP_STS_HCH      BIT(0)   /* HC Halted */
#define  OP_STS_HSE      BIT(2)
#define  OP_STS_EINT     BIT(3)
#define  OP_STS_PCD      BIT(4)
#define  OP_STS_CNR      BIT(11)  /* Controller Not Ready */
#define XHCI_OP_PAGESIZE 0x08
#define XHCI_OP_CRCR_LO  0x18   /* Command Ring Control Register */
#define  OP_CRCR_RCS     BIT(0)  /* Ring Cycle State */
#define XHCI_OP_CRCR_HI  0x1C
#define XHCI_OP_DCBAAP_LO 0x30  /* Device Context Base Address Array Pointer */
#define XHCI_OP_DCBAAP_HI 0x34
#define XHCI_OP_CONFIG   0x38

/* XHCI port registers at op_base + 0x400 (port 0, USB2). */
#define XHCI_PORT_SC     0x400
#define  PORT_CCS        BIT(0)   /* Current Connect Status */
#define  PORT_PED        BIT(1)   /* Port Enabled (write 1 to disable!) */
#define  PORT_PR         BIT(4)   /* Port Reset */
#define  PORT_PS_SHIFT   10
#define  PORT_PS_MASK    (0xF << PORT_PS_SHIFT)
#define  PORT_CSC        BIT(17)  /* Connect Status Change */
#define  PORT_PRC        BIT(21)  /* Port Reset Change */
/* Bits that must not be written back as-is when modifying PORTSC:
 * write-1-to-clear change bits plus PED (writing 1 disables the port). */
#define  PORT_RW_HAZARD  (PORT_PED | PORT_CSC | PORT_PRC | BIT(18) | BIT(19) | \
                          BIT(20) | BIT(22) | BIT(23))

/* XHCI runtime register offsets from rt_base (interrupter 0 set at +0x20). */
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

/* T210 CAR XUSB clock sources (TRM: 0x600 host, 0x604 falcon; FS/DEV/SS
 * already defined in clock.h at 0x608/0x60C/0x610). */
#define CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_HOST 0x600
#define CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FALCON    0x604

/*
 * Falcon firmware load interface (CSB registers reached through the FPCI
 * CSBRANGE window).  Offsets/bits mirror Linux drivers/usb/host/xhci-tegra.c.
 */
#define XUSB_CFG_ARU_C11_CSBRANGE   0x41C
#define XUSB_CFG_CSB_BASE_ADDR      0x800
#define XUSB_FALC_CPUCTL            0x100
#define  CPUCTL_STARTCPU            BIT(1)
#define  CPUCTL_STATE_HALTED        BIT(4)
#define  CPUCTL_STATE_STOPPED       BIT(5)
#define XUSB_FALC_BOOTVEC           0x104
#define XUSB_FALC_DMACTL            0x10C
#define XUSB_FALC_IMFILLRNG1        0x154
#define XUSB_FALC_IMFILLCTL         0x158
#define XUSB_CSB_MP_ILOAD_ATTR      0x101A00
#define XUSB_CSB_MP_ILOAD_BASE_LO   0x101A04
#define XUSB_CSB_MP_ILOAD_BASE_HI   0x101A08
#define XUSB_CSB_MP_L2IMEMOP_SIZE   0x101A10
#define XUSB_CSB_MP_L2IMEMOP_TRIG   0x101A14
#define  L2IMEMOP_INVALIDATE_ALL    (0x40 << 24)
#define  L2IMEMOP_LOAD_LOCKED_RESULT (0x11 << 24)
#define XUSB_CSB_MP_L2IMEMOP_RESULT 0x101A18
#define  L2IMEMOP_RESULT_VLD        BIT(31)
#define XUSB_CSB_MP_APMAP           0x10181C
#define  APMAP_BOOTPATH             BIT(31)
#define IMEM_BLOCK_SIZE             256

/* Falcon firmware header fields (offsets into the image). */
#define FW_HDR_BOOT_CODETAG_OFF     8
#define FW_HDR_BOOT_CODESIZE_OFF    12

#define USBH_FW_PATH  "bootloader/sys/xusbfw.bin"

/* USB standard descriptor types. */
#define USB_DESCRIPTOR_INTERFACE  4
#define USB_DESCRIPTOR_ENDPOINT   5

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

/* ---------- Falcon firmware load --------------------------------------- */

static void _csb_write(u32 addr, u32 val)
{
	XUSB_HOST_PCI(XUSB_CFG_ARU_C11_CSBRANGE) = addr >> 9;
	XUSB_HOST_PCI(XUSB_CFG_CSB_BASE_ADDR + (addr & 0x1FF)) = val;
}

static u32 _csb_read(u32 addr)
{
	XUSB_HOST_PCI(XUSB_CFG_ARU_C11_CSBRANGE) = addr >> 9;
	return XUSB_HOST_PCI(XUSB_CFG_CSB_BASE_ADDR + (addr & 0x1FF));
}

/*
 * The XUSB host complex is driven by an integrated Falcon microcontroller
 * which implements the xHCI interface.  Unlike the XUSB device controller
 * (pure hardware), host mode does not function until firmware is loaded
 * and the Falcon is booted.  The image is the standard Tegra210 XUSB
 * firmware (L4T tegra21x_xusb_firmware), placed on SD by the user.
 */
static int _usbh_load_firmware(void)
{
	/* Firmware already running (e.g. re-init without power cycle). */
	if (_csb_read(XUSB_CSB_MP_ILOAD_BASE_LO))
		return USB_RES_OK;

	u32 fw_size = 0;
	u8 *fw = (u8 *)sd_file_read(USBH_FW_PATH, &fw_size);
	if (!fw || fw_size < SZ_64K || fw_size > USBH_FW_BUF_SZ) {
		EPRINTF("USBH: XUSB firmware missing!\nPlace it at " USBH_FW_PATH);
		if (fw)
			free(fw);
		return USB_ERROR_INIT;
	}

	/* The Falcon DFI fetches from this buffer at runtime; it must stay
	 * resident for as long as the controller is in use. */
	memcpy((void *)USBH_FW_BUF_ADDR, fw, fw_size);
	free(fw);

	u8 *hdr = (u8 *)USBH_FW_BUF_ADDR;
	u32 boot_codetag, boot_codesize;
	memcpy(&boot_codetag,  hdr + FW_HDR_BOOT_CODETAG_OFF,  4);
	memcpy(&boot_codesize, hdr + FW_HDR_BOOT_CODESIZE_OFF, 4);

	u32 tag_blocks  = (boot_codetag  + IMEM_BLOCK_SIZE - 1) / IMEM_BLOCK_SIZE;
	u32 size_blocks = (boot_codesize + IMEM_BLOCK_SIZE - 1) / IMEM_BLOCK_SIZE;

	/* Program firmware image location/size and bootpath, then load the
	 * bootcode into L2IMEM and set up the Falcon IMEM autofill range. */
	_csb_write(XUSB_CSB_MP_ILOAD_ATTR,    fw_size);
	_csb_write(XUSB_CSB_MP_ILOAD_BASE_LO, USBH_FW_BUF_ADDR);
	_csb_write(XUSB_CSB_MP_ILOAD_BASE_HI, 0);
	_csb_write(XUSB_CSB_MP_APMAP,         APMAP_BOOTPATH);
	_csb_write(XUSB_CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_INVALIDATE_ALL);
	_csb_write(XUSB_CSB_MP_L2IMEMOP_SIZE, ((tag_blocks & 0x3FF) << 8) |
	                                      ((size_blocks & 0xFF) << 24));
	_csb_write(XUSB_CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_LOAD_LOCKED_RESULT);

	u32 retries = 100000;
	while (!(_csb_read(XUSB_CSB_MP_L2IMEMOP_RESULT) & L2IMEMOP_RESULT_VLD)) {
		if (!--retries) {
			EPRINTF("USBH: L2IMEM load timeout.");
			return USB_ERROR_INIT;
		}
		usleep(1);
	}

	_csb_write(XUSB_FALC_IMFILLCTL,  size_blocks);
	_csb_write(XUSB_FALC_IMFILLRNG1, (tag_blocks & 0xFFFF) |
	                                 (((tag_blocks + size_blocks) & 0xFFFF) << 16));
	_csb_write(XUSB_FALC_DMACTL, 0);
	usleep(1000);

	/* Boot the Falcon at the bootcode tag and wait for it to settle. */
	_csb_write(XUSB_FALC_BOOTVEC, boot_codetag);
	_csb_write(XUSB_FALC_CPUCTL,  CPUCTL_STARTCPU);

	retries = 200000;
	while (!(_csb_read(XUSB_FALC_CPUCTL) & (CPUCTL_STATE_HALTED | CPUCTL_STATE_STOPPED))) {
		if (!--retries) {
			EPRINTF("USBH: Falcon failed to start.");
			return USB_ERROR_INIT;
		}
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

	/* Host core clock: PLLP for 102MHz (matches device core clocking). */
	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_HOST) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_HOST) & 0x1FFFFF00) | (1 << 29) | 6;

	/* Falcon clock: PLLP for 204MHz. */
	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FALCON) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FALCON) & 0x1FFFFF00) | (1 << 29) | 2;
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
	/* Link TRB at the end of a ring, pointing back to slot 0.
	 * Cycle bit = CURRENT producer cycle so the HC follows it together
	 * with the TRBs written just before it (XHCI §4.9.3). */
	u32 *ltrb = ring[count - 1];
	ltrb[0] = (u32)ring[0];
	ltrb[1] = 0;
	ltrb[2] = 0;
	ltrb[3] = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC | (pcs & 1);
}

/*
 * Claim the next TRB slot on a transfer/command ring.  Returns the TRB
 * pointer and the cycle bit the caller must put in DW3.  Handles the wrap:
 * when the enqueue index reaches the Link TRB, the link is (re)written with
 * the current PCS and the PCS toggles for the next lap.
 */
static u32 *_trb_alloc(u32 (*ring)[4], u32 *idx, u8 *pcs, u8 *cycle)
{
	u32 *trb = ring[*idx];
	*cycle = *pcs & 1;
	if (++(*idx) == USBH_TRB_RING_SZ - 1) {
		_ring_link_trb(ring, USBH_TRB_RING_SZ, *pcs);
		*idx = 0;
		*pcs ^= 1;
	}
	return trb;
}

/* Poll event ring until an event TRB arrives; copy it out. */
static int _evt_wait_raw(u32 *out_trb, u32 timeout_us)
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
	RT(XHCI_RT_ERDP_LO) = deq | RT_ERDP_EHB;
	RT(XHCI_RT_ERDP_HI) = 0;

	return USB_RES_OK;
}

/* Like _evt_wait_raw but discards Port Status Change Events, which the HC
 * posts unsolicited on connect/reset (we track port state via PORTSC). */
static int _evt_wait(u32 *out_trb, u32 timeout_us)
{
	int res;
	do {
		res = _evt_wait_raw(out_trb, timeout_us);
		if (res)
			return res;
	} while (XHCI_EVT_TYPE(out_trb) == XHCI_TRB_EVT_PORT);
	return USB_RES_OK;
}

/* ---------- command ring operations ------------------------------------ */

static int _cmd_submit(u32 dw0, u32 dw1, u32 dw2, u32 dw3, u32 *evt)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u8 cyc;
	u32 *trb = _trb_alloc(cx->rings->cmd_ring, &cx->cmd_idx, &cx->cmd_pcs, &cyc);
	trb[0] = dw0; trb[1] = dw1; trb[2] = dw2;
	trb[3] = dw3 | cyc;

	/* Ring host controller doorbell (slot 0 = command ring). */
	DB(0) = 0;

	int res = _evt_wait(evt, USBH_TIMEOUT_US);
	if (res)
		return res;
	if (XHCI_EVT_TYPE(evt) != XHCI_TRB_EVT_CMD || XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

static int _cmd_enable_slot(u8 *slot_out)
{
	u32 evt[4];
	int res = _cmd_submit(0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_EN_SLOT), evt);
	if (res)
		return res;
	*slot_out = XHCI_EVT_SLOT(evt);
	return USB_RES_OK;
}

static int _cmd_address_device(u8 slot_id, bool bsr)
{
	u32 evt[4];
	u32 dw3 = XHCI_TRB_SLOT(slot_id) | XHCI_TRB_TYPE(XHCI_TRB_ADDR_DEV) |
	          (bsr ? BIT(9) : 0);
	return _cmd_submit((u32)usbh_ctxt.rings->in_ctx, 0, 0, dw3, evt);
}

static int _cmd_configure_ep(u8 slot_id)
{
	u32 evt[4];
	u32 dw3 = XHCI_TRB_SLOT(slot_id) | XHCI_TRB_TYPE(XHCI_TRB_CFG_EP);
	return _cmd_submit((u32)usbh_ctxt.rings->in_ctx, 0, 0, dw3, evt);
}

/* ---------- EP0 control transfer --------------------------------------- */

static int _ctrl_xfer(const usb_ctrl_setup_t *setup, void *data, u32 len)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	bool data_in = (setup->bmRequestType & 0x80) != 0;
	u8   cyc;
	u32 *t;

	if (len > USBH_XFER_MAX)
		return USB_ERROR_XFER_ERROR;

	/* Setup Stage TRB — Immediate Data, TRT 3=IN data, 2=OUT data, 0=none.
	 * IOC only on the final (Status) TRB so exactly one transfer event is
	 * generated per control transfer. */
	u32 trt = (len > 0) ? (data_in ? 3 : 2) : 0;
	t = _trb_alloc(cx->rings->ep0_ring, &cx->ep0_idx, &cx->ep0_pcs, &cyc);
	memcpy(t, setup, 8);     /* DW0+DW1 = 8-byte setup packet */
	t[2] = 8;
	t[3] = XHCI_TRB_TYPE(XHCI_TRB_SETUP_STG) | XHCI_TRB_IDT |
	       XHCI_TRB_TRT(trt) | cyc;

	/* Data Stage TRB (when len > 0).  No IOC/ISP: a short IN packet simply
	 * advances to the Status stage; errors always raise an event. */
	if (len > 0 && data) {
		t = _trb_alloc(cx->rings->ep0_ring, &cx->ep0_idx, &cx->ep0_pcs, &cyc);
		t[0] = (u32)data;
		t[1] = 0;
		t[2] = len;
		t[3] = XHCI_TRB_TYPE(XHCI_TRB_DATA_STG) |
		       (data_in ? XHCI_TRB_DIR_IN : 0) | cyc;
	}

	/* Status Stage TRB — direction is opposite of the data stage. */
	t = _trb_alloc(cx->rings->ep0_ring, &cx->ep0_idx, &cx->ep0_pcs, &cyc);
	t[0] = 0; t[1] = 0; t[2] = 0;
	t[3] = XHCI_TRB_TYPE(XHCI_TRB_STATUS_STG) | XHCI_TRB_IOC |
	       (data_in ? 0 : XHCI_TRB_DIR_IN) | cyc;

	/* Ring doorbell: slot_id, target = DCI 1 (EP0). */
	DB(cx->slot_id) = XHCI_DCI_EP0;

	u32 evt[4];
	int res = _evt_wait(evt, USBH_TIMEOUT_US);
	if (res)
		return res;
	if (XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS && XHCI_EVT_CC(evt) != XHCI_CC_SHORT_PKT)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

/* ---------- bulk transfer ---------------------------------------------- */

static int _bulk_xfer(u32 (*ring)[4], u32 *idx, u8 *pcs, u8 dci,
                      void *buf, u32 len, u32 *actual)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u8 cyc;

	if (len > USBH_XFER_MAX)
		return USB_ERROR_XFER_ERROR;

	u32 *trb = _trb_alloc(ring, idx, pcs, &cyc);
	trb[0] = (u32)buf;
	trb[1] = 0;
	trb[2] = len;
	trb[3] = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP | cyc;

	DB(cx->slot_id) = dci;

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

int usbh_bulk_out(const void *buf, u32 len, u32 *actual)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	return _bulk_xfer(cx->rings->bulk_out_ring, &cx->out_idx, &cx->out_pcs,
	                  cx->dci_out, (void *)buf, len, actual);
}

int usbh_bulk_in(void *buf, u32 len, u32 *actual)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	return _bulk_xfer(cx->rings->bulk_in_ring, &cx->in_idx, &cx->in_pcs,
	                  cx->dci_in, buf, len, actual);
}

/* ---------- context builders ------------------------------------------- */

static void _build_slot_ctx(u32 *ctx, u8 speed, u8 root_port, u8 ctx_entries)
{
	memset(ctx, 0, 32);
	ctx[0] = ((u32)speed << 20) | ((u32)ctx_entries << 27);
	ctx[1] = (u32)root_port << 16;
}

static void _build_ep_ctx(u32 *ctx, u8 ep_type, u16 max_packet, u32 ring_addr, u8 bulk)
{
	memset(ctx, 0, 32);
	ctx[1] = ((u32)3 << 1) |             /* CERR=3 */
	         ((u32)ep_type << 3) |
	         ((u32)max_packet << 16);
	ctx[2] = (ring_addr & ~0xFu) | 1;    /* TR Dequeue Ptr + DCS=1 */
	ctx[4] = (bulk ? 1024u : 8u);        /* avg_trb_length */
}

/* ---------- device enumeration ----------------------------------------- */

static int _enumerate_device(u8 root_port)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	usbh_rings_t *r  = cx->rings;

	int res = _cmd_enable_slot(&cx->slot_id);
	if (res) {
		EPRINTF("USBH: Enable Slot failed.");
		return res;
	}

	/* Input Context for ADDRESS_DEVICE: add Slot + EP0.
	 * in_ctx[0] = Input Control, [1] = Slot, [1 + dci] = EP context. */
	memset(r->in_ctx, 0, sizeof(r->in_ctx));
	r->in_ctx[0][1] = BIT(0) | BIT(XHCI_DCI_EP0);

	/* EP0 max packet: 64 for HS (and the common FS value); 8 for LS. */
	u16 ep0_max_pkt = (cx->port_speed == XHCI_SPEED_LS) ? 8 : 64;
	_build_slot_ctx(r->in_ctx[1], cx->port_speed, root_port, 1);
	_build_ep_ctx(r->in_ctx[1 + XHCI_DCI_EP0], XHCI_EP_CONTROL, ep0_max_pkt,
	              (u32)r->ep0_ring, 0);

	/* Point DCBAA[slot] at the device context. */
	r->dcbaa[cx->slot_id] = (u64)(u32)r->dev_ctx;

	/* ADDRESS_DEVICE (BSR=false → actually sends SET_ADDRESS on the bus). */
	res = _cmd_address_device(cx->slot_id, false);
	if (res)
		EPRINTF("USBH: Address Device failed.");
	return res;
}

/* Configure the bulk endpoints discovered in the config descriptor. */
static int _configure_bulk_eps(void)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	usbh_rings_t *r  = cx->rings;

	u8 max_dci = (cx->dci_in > cx->dci_out) ? cx->dci_in : cx->dci_out;

	memset(r->in_ctx, 0, sizeof(r->in_ctx));
	r->in_ctx[0][1] = BIT(0) | BIT(cx->dci_out) | BIT(cx->dci_in);

	_build_slot_ctx(r->in_ctx[1], cx->port_speed, 1, max_dci);
	_build_ep_ctx(r->in_ctx[1 + cx->dci_out], XHCI_EP_BULK_OUT, cx->out_mps,
	              (u32)r->bulk_out_ring, 1);
	_build_ep_ctx(r->in_ctx[1 + cx->dci_in],  XHCI_EP_BULK_IN,  cx->in_mps,
	              (u32)r->bulk_in_ring,  1);

	int res = _cmd_configure_ep(cx->slot_id);
	if (res)
		EPRINTF("USBH: Configure EP failed.");
	return res;
}

static int _get_dev_class(u8 *dev_class)
{
	/* GET_DESCRIPTOR(Device, len=18) into the DMA-safe IN buffer. */
	u8 *desc = (u8 *)USBH_BULK_IN_BUF_ADDR;
	usb_ctrl_setup_t setup = {
		.bmRequestType = 0x80,
		.bRequest      = USB_REQUEST_GET_DESCRIPTOR,
		.wValue        = 0x0100,
		.wIndex        = 0,
		.wLength       = 18,
	};
	int res = _ctrl_xfer(&setup, desc, 18);
	if (!res)
		*dev_class = desc[4];   /* bDeviceClass */
	return res;
}

/*
 * Fetch the configuration descriptor and locate the MSC BOT interface
 * (class 0x08, protocol 0x50) and its bulk endpoint pair.  Fills cfg_val,
 * dci_out/in and out_mps/in_mps.
 */
static int _parse_msc_config(void)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u8 *cfg = (u8 *)USBH_BULK_IN_BUF_ADDR;
	usb_ctrl_setup_t setup = {
		.bmRequestType = 0x80,
		.bRequest      = USB_REQUEST_GET_DESCRIPTOR,
		.wValue        = 0x0200,   /* Configuration descriptor, index 0 */
		.wIndex        = 0,
		.wLength       = 9,
	};

	int res = _ctrl_xfer(&setup, cfg, 9);
	if (res)
		return res;

	u16 total = cfg[2] | (cfg[3] << 8);
	if (total < 9)
		return USB_ERROR_XFER_ERROR;
	if (total > 512)
		total = 512;

	setup.wLength = total;
	res = _ctrl_xfer(&setup, cfg, total);
	if (res)
		return res;

	cx->cfg_val = cfg[5];
	cx->dci_out = 0;
	cx->dci_in  = 0;

	bool msc_iface = false;
	u32 i = 0;
	while (i + 1 < total) {
		u8 dlen  = cfg[i];
		u8 dtype = cfg[i + 1];
		if (!dlen || i + dlen > total)
			break;

		if (dtype == USB_DESCRIPTOR_INTERFACE) {
			/* bInterfaceClass 0x08 (MSC), bInterfaceProtocol 0x50 (BOT). */
			msc_iface = (cfg[i + 5] == 0x08) && (cfg[i + 7] == 0x50);
		} else if (dtype == USB_DESCRIPTOR_ENDPOINT && msc_iface) {
			u8  addr = cfg[i + 2];
			u8  attr = cfg[i + 3] & 3;
			u16 mps  = cfg[i + 4] | (cfg[i + 5] << 8);
			u8  dci  = ((addr & 0xF) * 2) + ((addr & 0x80) ? 1 : 0);
			if (attr == 2 && dci <= USBH_CTX_EPS) {  /* Bulk */
				if (addr & 0x80) {
					cx->dci_in = dci;
					cx->in_mps = mps;
				} else {
					cx->dci_out = dci;
					cx->out_mps = mps;
				}
			}
		}
		i += dlen;
	}

	if (!cx->dci_out || !cx->dci_in) {
		EPRINTF("USBH: No MSC BOT interface found.");
		return USB_ERROR_XFER_ERROR;
	}
	return USB_RES_OK;
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
	while (!(OP(XHCI_PORT_SC) & PORT_CCS)) {
		if (!timeout_us--)
			return USB_ERROR_TIMEOUT;
		usleep(1);
	}
	return USB_RES_OK;
}

static int _port_reset(void)
{
	u32 portsc = OP(XHCI_PORT_SC);
	OP(XHCI_PORT_SC) = (portsc & ~PORT_RW_HAZARD) | PORT_PR;
	int res = _wait_op_bits(XHCI_PORT_SC, PORT_PRC, PORT_PRC);
	if (res)
		return res;

	/* Acknowledge the reset/connect change bits. */
	portsc = OP(XHCI_PORT_SC);
	OP(XHCI_PORT_SC) = (portsc & ~PORT_RW_HAZARD) | PORT_PRC | PORT_CSC;
	return USB_RES_OK;
}

/* ---------- scratchpad buffers ------------------------------------------ */

static int _setup_scratchpad(void)
{
	usbh_ctxt_t *cx = &usbh_ctxt;

	/* HCSPARAMS2: Max Scratchpad Bufs Hi [25:21], Lo [31:27]. */
	u32 hcs2 = XUSB_HOST(0x08);
	u32 num  = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
	if (!num)
		return USB_RES_OK;

	u32 pgsz = (OP(XHCI_OP_PAGESIZE) & 0xFFFF) << 12;
	if (!pgsz || ((u64)(num + 1) * pgsz + 4096) > USBH_SCRATCHPAD_SZ) {
		EPRINTF("USBH: Scratchpad too large.");
		return USB_ERROR_INIT;
	}

	/* Array of page pointers, then the pages themselves (page-aligned). */
	u64 *sp_array = (u64 *)USBH_SCRATCHPAD_ADDR;
	u32  pages    = USBH_SCRATCHPAD_ADDR + pgsz;
	memset(sp_array, 0, 4096);
	for (u32 i = 0; i < num; i++) {
		memset((void *)(pages + i * pgsz), 0, pgsz);
		sp_array[i] = pages + i * pgsz;
	}
	cx->rings->dcbaa[0] = (u64)(u32)sp_array;
	return USB_RES_OK;
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

	/* Assert VBUS override and force ID to ground so the controller sees
	 * the host role.  OVR field must be cleared explicitly: GND = 0. */
	XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) &
		 ~(PADCTL_USB2_VBUS_ID_VBUS_OVR_MASK | PADCTL_USB2_VBUS_ID_SRC_MASK |
		   PADCTL_USB2_VBUS_ID_OVR_MASK)) |
		PADCTL_USB2_VBUS_ID_VBUS_OVR_EN | PADCTL_USB2_VBUS_ID_VBUS_ON |
		PADCTL_USB2_VBUS_ID_SRC_ID_OVR_EN | PADCTL_USB2_VBUS_ID_OVR_GND;

	XUSB_PADCTL(XUSB_PADCTL_SS_PORT_MAP) &= ~PADCTL_SS_PORT_MAP_PORT0_MASK;
	PMC(APBDEV_PMC_USB_AO) &= 0xFFFFFFF3;
	usleep(1);

	/* Source 5V onto VBUS for bus-powered devices (handheld/OTG path;
	 * Icosa only — in dock the hub supplies downstream power). */
	regulator_5v_enable(REGULATOR_5V_ALL);
	regulator_5v_usb_src_enable(true);

	_usbh_init_host_clocks();
	bpmp_clk_rate_relaxed(false);

	/* Enable AHB redirect for IRAM access (rings live there). */
	mc_enable_ahb_redirect();

	/* Enable XUSB host IPFS and configure FPCI BAR0. */
	XUSB_HOST_CFG(XUSB_HOST_CONFIGURATION) |= HOST_CONFIGURATION_EN_FPCI;
	XUSB_HOST_PCI(XUSB_CFG_1) |= CFG_1_BUS_MASTER | CFG_1_MEMORY_SPACE | CFG_1_IO_SPACE;
	usleep(1);
	XUSB_HOST_PCI(XUSB_CFG_4) = XUSB_HOST_BASE | CFG_4_ADDRESS_TYPE_32_BIT;
	usleep(1);

	/* Load and boot the Falcon firmware — the xHCI interface is dead
	 * until the firmware is running. */
	int res = _usbh_load_firmware();
	if (res)
		return res;

	/* Read XHCI capability registers to find register group bases. */
	u32 cap_len = XUSB_HOST(0) & 0xFF;
	u32 dboff   = XUSB_HOST(0x14) & ~0x3;
	u32 rtsoff  = XUSB_HOST(0x18) & ~0x1F;
	cx->op_base = XUSB_HOST_BASE + cap_len;
	cx->rt_base = XUSB_HOST_BASE + rtsoff;
	cx->db_base = XUSB_HOST_BASE + dboff;

	/* Wait for the controller (firmware) to become ready. */
	res = _wait_op_bits(XHCI_OP_USBSTS, OP_STS_CNR, 0);
	if (res) { EPRINTF("USBH: Controller not ready."); return res; }

	/* Reset the host controller. */
	OP(XHCI_OP_USBCMD) |= OP_CMD_HCRST;
	res = _wait_op_bits(XHCI_OP_USBCMD, OP_CMD_HCRST, 0);
	if (res) { EPRINTF("USBH: HC reset timeout."); return res; }
	res = _wait_op_bits(XHCI_OP_USBSTS, OP_STS_CNR, 0);
	if (res) { EPRINTF("USBH: HC not ready after reset."); return res; }

	/* --- Set up ring buffers at XUSB_RING_ADDR. --- */
	cx->rings = (usbh_rings_t *)XUSB_RING_ADDR;
	memset(cx->rings, 0, sizeof(usbh_rings_t));

	/* Scratchpad buffers (DCBAA[0]) if the firmware requests them. */
	res = _setup_scratchpad();
	if (res)
		return res;

	OP(XHCI_OP_DCBAAP_LO) = (u32)cx->rings->dcbaa;
	OP(XHCI_OP_DCBAAP_HI) = 0;

	/* Command ring. PCS starts at 1. */
	cx->cmd_pcs = 1;
	OP(XHCI_OP_CRCR_LO) = (u32)cx->rings->cmd_ring | OP_CRCR_RCS;
	OP(XHCI_OP_CRCR_HI) = 0;

	/* Event ring: 1 segment. */
	cx->evt_ccs = 1;
	cx->rings->erst[0] = (u32)cx->rings->evt_ring;
	cx->rings->erst[1] = 0;
	cx->rings->erst[2] = USBH_EVT_RING_SZ;
	cx->rings->erst[3] = 0;
	RT(XHCI_RT_ERSTSZ)    = 1;
	RT(XHCI_RT_ERDP_LO)   = (u32)cx->rings->evt_ring;
	RT(XHCI_RT_ERDP_HI)   = 0;
	RT(XHCI_RT_ERSTBA_LO) = (u32)cx->rings->erst;
	RT(XHCI_RT_ERSTBA_HI) = 0;

	/* Transfer rings: PCS starts at 1 for all. */
	cx->ep0_pcs = cx->out_pcs = cx->in_pcs = 1;

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

	if (!(OP(XHCI_PORT_SC) & PORT_PED)) {
		EPRINTF("USBH: Port not enabled after reset.");
		return USB_ERROR_INIT;
	}

	/* Get port speed from PORTSC.PS[13:10]. */
	u32 ps = (OP(XHCI_PORT_SC) & PORT_PS_MASK) >> PORT_PS_SHIFT;
	cx->port_speed = ps ? ps : XHCI_SPEED_HS;

	/* Enumerate: ENABLE_SLOT + ADDRESS_DEVICE on root port 1. */
	res = _enumerate_device(1);
	if (res) return res;

	/* Read device descriptor class to detect hub (0x09) vs direct MSC. */
	u8 dev_class = 0;
	res = _get_dev_class(&dev_class);
	if (res) { EPRINTF("USBH: Device descriptor failed."); return res; }
	if (dev_class == 0x09) {
		/* Hub: usbh_hub_enumerate() owns the rest of the init. */
		return usbh_hub_enumerate();
	}

	/* Find the MSC interface and its bulk endpoints. */
	res = _parse_msc_config();
	if (res) return res;

	res = _set_configuration(cx->cfg_val);
	if (res) { EPRINTF("USBH: Set Configuration failed."); return res; }

	res = _configure_bulk_eps();
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
	regulator_5v_usb_src_enable(false);
	regulator_5v_disable(REGULATOR_5V_ALL);
	XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) &=
		~(PADCTL_USB2_VBUS_ID_VBUS_OVR_MASK | PADCTL_USB2_VBUS_ID_VBUS_ON);

	/* Reset clocks. */
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) = BIT(CLK_U_XUSB_HOST);
	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_CLR) = BIT(CLK_U_XUSB_HOST);

	memset(cx, 0, sizeof(*cx));
}
