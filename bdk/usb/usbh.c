/*
 * USB Host (XHCI) driver for Tegra X1
 *
 * Copyright (c) 2024-2025 Hekate contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Minimal polled xHCI host for the T210 XUSB complex:
 *  - Falcon firmware load (CSB/L2IMEM, mirrors Linux xhci-tegra.c).
 *  - One root port device, optionally one more behind a hub (dock).
 *  - Control transfers + one bulk IN/OUT pair (Mass Storage BOT).
 *
 * The BPMP cache covers DRAM (write-through), so every DMA-written area
 * (event ring, output contexts, IN buffers) is invalidated before reading
 * and producer writes are cleaned before ringing a doorbell.
 */

#include <string.h>

#include <usb/usbh.h>
#include <usb/usbd.h>
#include <usb/usb_t210.h>

#include <gfx_utils.h>
#include <power/regulator_5v.h>
#include <soc/bpmp.h>
#include <soc/clock.h>
#include <soc/fuse.h>
#include <soc/pmc.h>
#include <soc/timer.h>
#include <soc/t210.h>

#include <memory_map.h>

/* XUSB host FPCI config space at +0x8000, IPFS at +0x9000 (T210 TRM). */
#define XUSB_HOST_PCI(off)  MMIO_REG32(XUSB_HOST_BASE + 0x8000, off)
#define XUSB_HOST_CFG(off)  MMIO_REG32(XUSB_HOST_BASE + 0x9000, off)

/* XHCI capability registers (from XUSB_HOST_BASE). */
#define XHCI_CAP_CAPLENGTH  0x00
#define XHCI_CAP_HCSPARAMS1 0x04
#define XHCI_CAP_HCSPARAMS2 0x08
#define XHCI_CAP_HCCPARAMS1 0x10
#define  CAP_HCC_CSZ        BIT(2)   /* 64-byte context size */
#define XHCI_CAP_DBOFF      0x14
#define XHCI_CAP_RTSOFF     0x18

/* XHCI operational register offsets from op_base. */
#define XHCI_OP_USBCMD    0x00
#define  OP_CMD_RUN       BIT(0)
#define  OP_CMD_HCRST     BIT(1)
#define XHCI_OP_USBSTS    0x04
#define  OP_STS_HCH       BIT(0)   /* HC Halted */
#define  OP_STS_HSE       BIT(2)   /* Host System Error */
#define  OP_STS_CNR       BIT(11)  /* Controller Not Ready */
#define  OP_STS_HCE       BIT(12)  /* Host Controller Error */
#define XHCI_OP_PAGESIZE  0x08
#define XHCI_OP_CRCR_LO   0x18
#define  OP_CRCR_RCS      BIT(0)
#define XHCI_OP_CRCR_HI   0x1C
#define XHCI_OP_DCBAAP_LO 0x30
#define XHCI_OP_DCBAAP_HI 0x34
#define XHCI_OP_CONFIG    0x38
#define XHCI_OP_PORTSC(p) (0x400 + 0x10 * ((p) - 1))  /* p is 1-based */
#define  PORT_CCS         BIT(0)   /* Current Connect Status */
#define  PORT_PED         BIT(1)   /* Port Enabled (write 1 to disable!) */
#define  PORT_PR          BIT(4)   /* Port Reset */
#define  PORT_PP          BIT(9)   /* Port Power */
#define  PORT_PS_SHIFT    10
#define  PORT_PS_MASK     (0xF << PORT_PS_SHIFT)
#define  PORT_CSC         BIT(17)  /* Connect Status Change */
#define  PORT_PRC         BIT(21)  /* Port Reset Change */
/* Bits that must not be written back as-is: write-1-to-clear change bits
 * (17..23) plus PED (writing 1 disables the port). */
#define  PORT_RW_HAZARD   (PORT_PED | (0x7F << 17))

/* XHCI runtime register offsets from rt_base (interrupter 0 at +0x20). */
#define XHCI_RT_ERSTSZ    0x28
#define XHCI_RT_ERSTBA_LO 0x30
#define XHCI_RT_ERSTBA_HI 0x34
#define XHCI_RT_ERDP_LO   0x38
#define  RT_ERDP_EHB      BIT(3)
#define XHCI_RT_ERDP_HI   0x3C

/* T210 CAR XUSB clock sources (0x600 host, 0x604 falcon; FS/DEV/SS at
 * 0x608/0x60C/0x610 are in clock.h). */
#define CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_HOST 0x600
#define CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FALCON    0x604

/*
 * Falcon firmware load interface (CSB registers reached through the FPCI
 * CSBRANGE window).  Offsets/bits mirror Linux drivers/usb/host/xhci-tegra.c.
 */
#define XUSB_CFG_ARU_C11_CSBRANGE    0x41C
#define XUSB_CFG_CSB_BASE_ADDR       0x800
#define XUSB_FALC_CPUCTL             0x100
#define  CPUCTL_STARTCPU             BIT(1)
#define  CPUCTL_STATE_HALTED         BIT(4)
#define  CPUCTL_STATE_STOPPED        BIT(5)
#define XUSB_FALC_BOOTVEC            0x104
#define XUSB_FALC_DMACTL             0x10C
#define XUSB_FALC_IMFILLRNG1         0x154
#define XUSB_FALC_IMFILLCTL          0x158
#define XUSB_CSB_MP_ILOAD_ATTR       0x101A00
#define XUSB_CSB_MP_ILOAD_BASE_LO    0x101A04
#define XUSB_CSB_MP_ILOAD_BASE_HI    0x101A08
#define XUSB_CSB_MP_L2IMEMOP_SIZE    0x101A10
#define XUSB_CSB_MP_L2IMEMOP_TRIG    0x101A14
#define  L2IMEMOP_INVALIDATE_ALL     (0x40 << 24)
#define  L2IMEMOP_LOAD_LOCKED_RESULT (0x11 << 24)
#define XUSB_CSB_MP_L2IMEMOP_RESULT  0x101A18
#define  L2IMEMOP_RESULT_VLD         BIT(31)
#define XUSB_CSB_MP_APMAP            0x10181C
#define  APMAP_BOOTPATH              BIT(31)
#define IMEM_BLOCK_SIZE              256

/* Falcon firmware header (256 bytes, precedes the DFI image). */
#define FW_HDR_SIZE                  256
#define FW_HDR_BOOT_CODETAG_OFF      8
#define FW_HDR_BOOT_CODESIZE_OFF     12
#define FW_HDR_FWIMG_LEN_OFF         100

/* USB standard descriptor types. */
#define USB_DESCRIPTOR_DEVICE        1
#define USB_DESCRIPTOR_CONFIGURATION 2
#define USB_DESCRIPTOR_STRING        3
#define USB_DESCRIPTOR_INTERFACE     4
#define USB_DESCRIPTOR_ENDPOINT      5
#define USB_DESCRIPTOR_HUB           0x29

/* USB class codes. */
#define USB_CLASS_MSC                0x08
#define USB_CLASS_HUB                0x09
#define USB_MSC_PROTO_BOT            0x50

/* USB standard feature selectors. */
#define USB_FEATURE_ENDPOINT_HALT    0

/* Hub class requests / features (USB 2.0 §11.24). */
#define HUB_RT_SET_PORT              0x23  /* Host-to-Device, Class, Other */
#define HUB_RT_GET_PORT              0xA3  /* Device-to-Host, Class, Other */
#define HUB_RT_GET_HUB               0xA0  /* Device-to-Host, Class, Device */
#define HUB_FEAT_PORT_RESET          4
#define HUB_FEAT_PORT_POWER          8
#define HUB_FEAT_C_PORT_CONNECTION   16
#define HUB_FEAT_C_PORT_RESET        20
#define HUB_PORT_STAT_CONNECTION     BIT(0)
#define HUB_PORT_STAT_LOW_SPEED      BIT(9)
#define HUB_PORT_STAT_HIGH_SPEED     BIT(10)
#define HUB_PORT_CHG_RESET           BIT(4)

/* Timeouts (ms). */
#define USBH_TIMEOUT_CTRL_MS   1000
#define USBH_TIMEOUT_CMD_MS    1000

static usbh_ctxt_t usbh_ctxt;

usbh_ctxt_t *usbh_get_ctxt(void)      { return &usbh_ctxt; }
bool         usbh_is_initialized(void) { return usbh_ctxt.initialized; }
bool         usbh_is_ready(void)       { return usbh_ctxt.ready; }

/* ---------- helpers ---------------------------------------------------- */

#define OP(r)     MMIO_REG32(usbh_ctxt.op_base, r)
#define RT(r)     MMIO_REG32(usbh_ctxt.rt_base, r)
#define DB(slot)  MMIO_REG32(usbh_ctxt.db_base, (slot) * 4)

/* Producer writes -> memory before the HC reads them. */
static void _dma_flush(void)      { bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLEAN_WAY,   false); }
/* HC writes -> drop stale cache lines before the CPU reads them. */
static void _dma_invalidate(void) { bpmp_mmu_maintenance(BPMP_MMU_MAINT_INVALID_WAY, false); }

static int _wait_op_bits(u32 reg, u32 mask, u32 val, u32 timeout_ms)
{
	u32 retries = timeout_ms * 100;
	while ((OP(reg) & mask) != val)
	{
		if (!retries--)
			return USB_ERROR_TIMEOUT;
		usleep(10);
	}
	return USB_RES_OK;
}

/* Context entry accessors (entry size depends on HCCPARAMS1.CSZ). */
static u32 *_in_ctx(u32 entry)             { return (u32 *)(usbh_ctxt.rings->in_ctx + entry * usbh_ctxt.ctx_sz); }
static u32 *_dev_ctx(u32 slot, u32 entry)   { return (u32 *)(usbh_ctxt.rings->dev_ctx[slot - 1] + entry * usbh_ctxt.ctx_sz); }

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

bool usbh_fw_loaded(void)
{
	return usbh_ctxt.initialized && _csb_read(XUSB_CSB_MP_ILOAD_BASE_LO) != 0;
}

/*
 * The XUSB host complex is driven by an integrated Falcon microcontroller
 * which implements the xHCI interface.  Unlike the XUSB device controller
 * (pure hardware), host mode does not function until firmware is loaded
 * and the Falcon is booted.  The image is the standard Tegra210 XUSB
 * firmware (L4T tegra21x_xusb_firmware / nvidia/tegra210/xusb.bin).
 */
static int _usbh_load_firmware(const void *fw, u32 fw_size)
{
	/* Firmware already running (e.g. re-init without power cycle). */
	if (_csb_read(XUSB_CSB_MP_ILOAD_BASE_LO))
		return USB_RES_OK;

	if (!fw || fw_size <= FW_HDR_SIZE || fw_size > USBH_FW_BUF_SZ)
		return USB_ERROR_INIT;

	/* The Falcon DFI fetches from this buffer at runtime; it must stay
	 * resident for as long as the controller is in use. */
	u8 *hdr = (u8 *)USBH_FW_BUF_ADDR;
	if ((const u8 *)fw != hdr)
		memcpy(hdr, fw, fw_size);
	_dma_flush();

	u32 boot_codetag, boot_codesize, fwimg_len;
	memcpy(&boot_codetag,  hdr + FW_HDR_BOOT_CODETAG_OFF,  4);
	memcpy(&boot_codesize, hdr + FW_HDR_BOOT_CODESIZE_OFF, 4);
	memcpy(&fwimg_len,     hdr + FW_HDR_FWIMG_LEN_OFF,     4);
	if (!boot_codesize || boot_codetag + boot_codesize > fw_size || fwimg_len > fw_size)
		return USB_ERROR_INIT;  /* Not a XUSB firmware image. */

	u32 tag_blocks  = (boot_codetag  + IMEM_BLOCK_SIZE - 1) / IMEM_BLOCK_SIZE;
	u32 size_blocks = (boot_codesize + IMEM_BLOCK_SIZE - 1) / IMEM_BLOCK_SIZE;

	/* Program firmware image size and DFI location (image after the header),
	 * then load the bootcode into L2IMEM and set the Falcon IMEM autofill. */
	_csb_write(XUSB_CSB_MP_ILOAD_ATTR,    fw_size);
	_csb_write(XUSB_CSB_MP_ILOAD_BASE_HI, 0);
	_csb_write(XUSB_CSB_MP_ILOAD_BASE_LO, USBH_FW_BUF_ADDR + FW_HDR_SIZE);
	_csb_write(XUSB_CSB_MP_APMAP,         APMAP_BOOTPATH);
	_csb_write(XUSB_CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_INVALIDATE_ALL);
	_csb_write(XUSB_CSB_MP_L2IMEMOP_SIZE, ((tag_blocks & 0x3FF) << 8) |
	                                      ((size_blocks & 0xFF) << 24));
	_csb_write(XUSB_CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_LOAD_LOCKED_RESULT);

	_csb_write(XUSB_FALC_IMFILLCTL,  size_blocks);
	_csb_write(XUSB_FALC_IMFILLRNG1, (tag_blocks & 0xFFFF) |
	                                 (((tag_blocks + size_blocks) & 0xFFFF) << 16));
	_csb_write(XUSB_FALC_DMACTL, 0);

	u32 retries = 10000;
	while (!(_csb_read(XUSB_CSB_MP_L2IMEMOP_RESULT) & L2IMEMOP_RESULT_VLD))
	{
		if (!retries--)
		{
			EPRINTF("USBH: L2IMEM load timeout.");
			return USB_ERROR_INIT;
		}
		usleep(10);
	}

	/* Boot the Falcon at the bootcode tag. Readiness is signalled by the
	 * xHCI USBSTS.CNR bit clearing (checked by the caller). */
	_csb_write(XUSB_FALC_BOOTVEC, boot_codetag);
	_csb_write(XUSB_FALC_CPUCTL,  CPUCTL_STARTCPU);

	return USB_RES_OK;
}

/* ---------- PHY init (mirrors _xusb_init_phy in xusbd.c) --------------- */

static void _usbh_init_phy(void)
{
	clock_enable_pllu();

	// Enable IDDQ control by software and disable UTMIPLL IDDQ.
	CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) =
		(CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) & 0xFFFFFFFC) | 1;
	clock_enable_utmipll();
	CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) =
		(CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) & 0xFEFFFFE8) | 0x2000008 | 0x20 | 2;
	usleep(2);

	// OTG PAD0 calibration from fuses.
	u32 fuse_usb_calib = FUSE(FUSE_USB_CALIB);
	XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_0) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_0) & 0xFFFFFFC0) | (fuse_usb_calib & 0x3F);
	XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1) & 0x83FFFF87) |
		((fuse_usb_calib & 0x780) >> 4) |
		((u32)(FUSE(FUSE_USB_CALIB_EXT) << 27) >> 1);

	XUSB_PADCTL(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD0_CTL1) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD0_CTL1) & 0xFFFFFE3F) | 0x80;

	// Disable power down on usb2 port pads.
	XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_0) &= 0xDBFFFFFF;
	XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1) &= 0xFFFFFFFB;
	XUSB_PADCTL(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD0_CTL0) &= 0xFFFFFFFE;
	XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_0) &= 0xFFFFF7FF;
	(void)XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1);

	// USB2 bias pad tracking.
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

/* ---------- power partitions and host clocks --------------------------- */

/*
 * XUSBA (SuperSpeed) and XUSBC (host) are power gated partitions.  Sequence
 * per TRM / tegra_powergate_sequence_power_up(): reset asserted, partition
 * on, clock on, remove clamps, then (later) reset deasserted.
 */
static void _usbh_unpowergate(pmc_power_rail_t rail, u32 clk_enb_set_reg, u32 clk_bit)
{
	pmc_enable_partition(rail, 1);
	CLOCK(clk_enb_set_reg) = BIT(clk_bit);
	usleep(10);

	PMC(APBDEV_PMC_REMOVE_CLAMPING_CMD) = BIT(rail);
	u32 retries = 1000;
	while ((PMC(APBDEV_PMC_REMOVE_CLAMPING_CMD) & BIT(rail)) && retries--)
		usleep(1);
	usleep(10);
}

static void _usbh_init_host_clocks(void)
{
	CLOCK(CLK_RST_CONTROLLER_PLLU_OUTA) |= 1;
	usleep(2);

	// Keep host and SS in reset while powering their partitions.
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_SET) = BIT(CLK_W_XUSB_SS);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) = BIT(CLK_U_XUSB_HOST);

	_usbh_unpowergate(POWER_RAIL_XUSBA, CLK_RST_CONTROLLER_CLK_ENB_W_SET, CLK_W_XUSB_SS);
	_usbh_unpowergate(POWER_RAIL_XUSBC, CLK_RST_CONTROLLER_CLK_ENB_U_SET, CLK_U_XUSB_HOST);

	// Host core clock: PLLP_OUT0 / 4 = 102MHz.
	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_HOST) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_HOST) & 0x1FFFFF00) | (1 << 29) | 6;

	// Falcon clock: PLLP_OUT0 / 2 = 204MHz.
	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FALCON) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FALCON) & 0x1FFFFF00) | (1 << 29) | 2;
	usleep(2);

	// FS clock: FO_48M. SS clock: PLLU_480M / 4 = 120MHz.
	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FS) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FS) & 0x1FFFFFFF) | (2 << 29);
	CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_SS) =
		(CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_SS) & 0x1FFFFF00) | (3 << 29) | 6;
	usleep(2);

	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB_SS);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_CLR) = BIT(CLK_U_XUSB_HOST);
	usleep(2);
}

/* ---------- ring helpers ----------------------------------------------- */

static void _ring_init(usbh_ring_t *r, u32 (*trb)[4])
{
	memset(trb, 0, USBH_TRB_RING_SZ * 16);
	r->trb = trb;
	r->idx = 0;
	r->pcs = 1;
}

/*
 * Claim the next TRB slot on a transfer/command ring.  Returns the TRB
 * pointer and the cycle bit the caller must put in DW3.  Handles the wrap:
 * when the enqueue index reaches the Link TRB, the link is (re)written with
 * the current PCS (XHCI §4.9.3) and the PCS toggles for the next lap.
 */
static u32 *_trb_alloc(usbh_ring_t *r, u32 *cycle)
{
	u32 *trb = r->trb[r->idx];
	*cycle = r->pcs & 1;
	if (++r->idx == USBH_TRB_RING_SZ - 1)
	{
		u32 *ltrb = r->trb[USBH_TRB_RING_SZ - 1];
		ltrb[0] = (u32)r->trb;
		ltrb[1] = 0;
		ltrb[2] = 0;
		ltrb[3] = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC | (r->pcs & 1);
		r->idx = 0;
		r->pcs ^= 1;
	}
	return trb;
}

/* Current enqueue pointer with DCS, as needed by Set TR Dequeue Pointer. */
static u32 _ring_deq_ptr(usbh_ring_t *r)
{
	return ((u32)r->trb[r->idx] & ~0xFu) | (r->pcs & 1);
}

/*
 * Make sure the next n TRBs fit before the Link TRB so that a TD never
 * spans the segment wrap.  The slots left over are filled with No Op TRBs
 * (no IOC: they complete silently).
 */
static void _ring_reserve(usbh_ring_t *r, u32 n)
{
	if (r->idx + n <= USBH_TRB_RING_SZ - 1)
		return;
	while (r->idx != 0)
	{
		u32 cyc;
		u32 *t = _trb_alloc(r, &cyc);
		t[0] = 0; t[1] = 0; t[2] = 0;
		t[3] = XHCI_TRB_TYPE(XHCI_TRB_NOOP) | cyc;
	}
}

/* Poll event ring until an event TRB arrives; copy it out. */
static int _evt_wait_raw(u32 *out_trb, u32 timeout_ms)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u32 *trb = cx->rings->evt_ring[cx->evt_idx];
	u32 retries = timeout_ms * 20;

	while (true)
	{
		_dma_invalidate();
		if ((trb[3] & 1) == (cx->evt_ccs & 1))
			break;
		if (OP(XHCI_OP_USBSTS) & (OP_STS_HCE | OP_STS_HSE))
			return USB_ERROR_INIT;
		if (!retries--)
			return USB_ERROR_TIMEOUT;
		usleep(50);
	}

	out_trb[0] = trb[0]; out_trb[1] = trb[1];
	out_trb[2] = trb[2]; out_trb[3] = trb[3];

	// Advance dequeue pointer; toggle CCS on wrap.
	if (++cx->evt_idx == USBH_EVT_RING_SZ)
	{
		cx->evt_idx = 0;
		cx->evt_ccs ^= 1;
	}

	// Update ERDP to tell the controller we consumed this event.
	RT(XHCI_RT_ERDP_LO) = (u32)cx->rings->evt_ring[cx->evt_idx] | RT_ERDP_EHB;
	RT(XHCI_RT_ERDP_HI) = 0;

	return USB_RES_OK;
}

/*
 * Wait for the next command/transfer event.  Port Status Change Events (the
 * HC posts them unsolicited; port state is tracked via PORTSC) and transfer
 * events for TRBs outside [trb_lo, trb_hi] (left over from an aborted TD)
 * are discarded.
 */
static int _evt_wait(u32 *out_trb, u32 timeout_ms, u32 trb_lo, u32 trb_hi)
{
	while (true)
	{
		int res = _evt_wait_raw(out_trb, timeout_ms);
		if (res)
			return res;

		u32 type = XHCI_EVT_TYPE(out_trb);
		if (type == XHCI_TRB_EVT_PORT)
			continue;
		if (type == XHCI_TRB_EVT_XFER && (out_trb[0] < trb_lo || out_trb[0] > trb_hi))
			continue;
		return USB_RES_OK;
	}
}

/* ---------- command ring operations ------------------------------------ */

static int _cmd_submit(u32 dw0, u32 dw2, u32 dw3, u32 *evt)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u32 cyc;
	u32 *trb = _trb_alloc(&cx->cmd, &cyc);
	trb[0] = dw0; trb[1] = 0; trb[2] = dw2;
	trb[3] = dw3 | cyc;
	_dma_flush();

	// Ring host controller doorbell (slot 0 = command ring).
	DB(0) = 0;

	/* Only the command completion is of interest here; a transfer event
	 * can show up when a Stop Endpoint command retires a pending TD. */
	int res = _evt_wait(evt, USBH_TIMEOUT_CMD_MS, 1, 0);
	if (res)
		return res;
	if (XHCI_EVT_TYPE(evt) != XHCI_TRB_EVT_CMD || XHCI_EVT_CC(evt) != XHCI_CC_SUCCESS)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

static int _cmd_enable_slot(u8 *slot_out)
{
	u32 evt[4];
	int res = _cmd_submit(0, 0, XHCI_TRB_TYPE(XHCI_TRB_EN_SLOT), evt);
	if (res)
		return res;
	*slot_out = XHCI_EVT_SLOT(evt);
	if (!*slot_out || *slot_out > USBH_MAX_SLOTS)
		return USB_ERROR_INIT;
	return USB_RES_OK;
}

/* Commands that take the Input Context: Address Device, Configure Endpoint,
 * Evaluate Context. */
static int _cmd_input_ctx(u8 slot_id, u32 type, u32 flags)
{
	u32 evt[4];
	_dma_flush();
	return _cmd_submit((u32)usbh_ctxt.rings->in_ctx, 0,
	                   XHCI_TRB_SLOT(slot_id) | XHCI_TRB_TYPE(type) | flags, evt);
}

/* ---------- context builders ------------------------------------------- */

static void _build_slot_ctx(u32 *ctx, const usbh_dev_t *d, u8 ctx_entries,
                            u8 hub_ports, u8 hub_ttt)
{
	memset(ctx, 0, 32);
	u32 route = d->hub_port ? (d->hub_port > 15 ? 15 : d->hub_port) : 0;
	ctx[0] = route | ((u32)d->speed << 20) | (hub_ports ? BIT(26) : 0) |
	         ((u32)ctx_entries << 27);
	ctx[1] = ((u32)d->root_port << 16) | ((u32)hub_ports << 24);
	ctx[2] = d->parent_slot | ((u32)(d->parent_slot ? d->hub_port : 0) << 8) |
	         ((u32)hub_ttt << 16);
}

static void _build_ep_ctx(u32 *ctx, u8 ep_type, u16 max_packet, u32 ring_addr, bool bulk)
{
	memset(ctx, 0, 32);
	ctx[1] = ((u32)3 << 1) |             /* CErr = 3                 */
	         ((u32)ep_type << 3) |
	         ((u32)max_packet << 16);
	ctx[2] = (ring_addr & ~0xFu) | 1;    /* TR Dequeue Ptr + DCS = 1 */
	ctx[4] = bulk ? 1024u : 8u;          /* Average TRB Length       */
}

/* ---------- EP0 control transfer --------------------------------------- */

static int  _ep_recover(usbh_dev_t *dev, u8 dci, usbh_ring_t *ring);
static void _ep_abort(usbh_dev_t *dev, u8 dci, usbh_ring_t *ring);

static int _ctrl_xfer_dev(usbh_dev_t *dev, const usb_ctrl_setup_t *setup, void *data, u32 len)
{
	bool data_in = (setup->bmRequestType & 0x80) != 0;
	u32  cyc;
	u32 *t;

	if (!data)
		len = 0;
	if (len > USBH_XFER_MAX)
		return USB_ERROR_XFER_ERROR;

	/* Setup Stage TRB: Immediate Data, TRT 3 = IN data, 2 = OUT data, 0 = none.
	 * IOC only on the final (Status) TRB so exactly one transfer event is
	 * generated per control transfer. */
	u32 trt = (len > 0) ? (data_in ? 3 : 2) : 0;
	_ring_reserve(&dev->ep0, 3);
	u32 *first = dev->ep0.trb[dev->ep0.idx];
	t = _trb_alloc(&dev->ep0, &cyc);
	memcpy(t, setup, 8);     /* DW0+DW1 = 8-byte setup packet */
	t[2] = 8;
	t[3] = XHCI_TRB_TYPE(XHCI_TRB_SETUP_STG) | XHCI_TRB_IDT | XHCI_TRB_TRT(trt) | cyc;

	/* Data Stage TRB (when len > 0).  No IOC/ISP: a short IN packet simply
	 * advances to the Status stage; errors always raise an event. */
	if (len > 0)
	{
		t = _trb_alloc(&dev->ep0, &cyc);
		t[0] = (u32)data;
		t[1] = 0;
		t[2] = len;
		t[3] = XHCI_TRB_TYPE(XHCI_TRB_DATA_STG) | (data_in ? XHCI_TRB_DIR_IN : 0) | cyc;
	}

	/* Status Stage TRB: IN unless the data stage was IN (USB 2.0 §9.3). */
	t = _trb_alloc(&dev->ep0, &cyc);
	t[0] = 0; t[1] = 0; t[2] = 0;
	t[3] = XHCI_TRB_TYPE(XHCI_TRB_STATUS_STG) | XHCI_TRB_IOC |
	       ((len && data_in) ? 0 : XHCI_TRB_DIR_IN) | cyc;
	_dma_flush();

	DB(dev->slot_id) = XHCI_DCI_EP0;

	u32 evt[4];
	int res = _evt_wait(evt, USBH_TIMEOUT_CTRL_MS, (u32)first, (u32)t);
	if (res)
	{
		if (res == USB_ERROR_TIMEOUT)
			_ep_abort(dev, XHCI_DCI_EP0, &dev->ep0);
		return res;
	}

	u32 cc = XHCI_EVT_CC(evt);
	if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PKT)
	{
		/* EP0 is halted on errors; recover so the next request can run. */
		_ep_recover(dev, XHCI_DCI_EP0, &dev->ep0);
		return USB_ERROR_XFER_ERROR;
	}

	if (data_in && len)
		_dma_invalidate();
	return USB_RES_OK;
}

static int _ctrl_req(usbh_dev_t *dev, u8 req_type, u8 request, u16 value, u16 index,
                     void *buf, u16 len)
{
	usb_ctrl_setup_t setup = {
		.bmRequestType = req_type,
		.bRequest      = request,
		.wValue        = value,
		.wIndex        = index,
		.wLength       = len,
	};
	return _ctrl_xfer_dev(dev, &setup, buf, len);
}

int usbh_ctrl_xfer(u8 req_type, u8 request, u16 value, u16 index, void *buf, u16 len)
{
	if (!usbh_ctxt.msc)
		return USB_ERROR_INIT;
	return _ctrl_req(usbh_ctxt.msc, req_type, request, value, index, buf, len);
}

/* ---------- endpoint halt recovery ------------------------------------- */

/*
 * After a STALL (or other error) the xHC halts the endpoint.  Reset Endpoint
 * (TSP = 0 also resets the USB2 data toggle), then move the dequeue pointer
 * to our enqueue position so the ring is usable again.
 */
static int _ep_recover(usbh_dev_t *dev, u8 dci, usbh_ring_t *ring)
{
	u32 evt[4];
	int res = _cmd_submit(0, 0, XHCI_TRB_SLOT(dev->slot_id) | XHCI_TRB_EP(dci) |
	                      XHCI_TRB_TYPE(XHCI_TRB_RESET_EP), evt);
	if (res)
		return res;
	return _cmd_submit(_ring_deq_ptr(ring), 0, XHCI_TRB_SLOT(dev->slot_id) |
	                   XHCI_TRB_EP(dci) | XHCI_TRB_TYPE(XHCI_TRB_SET_TR_DEQ), evt);
}

/* Abandon a TD that never completed (timeout): Stop Endpoint, then move the
 * dequeue pointer past it so the ring stays usable. */
static void _ep_abort(usbh_dev_t *dev, u8 dci, usbh_ring_t *ring)
{
	u32 evt[4];
	_cmd_submit(0, 0, XHCI_TRB_SLOT(dev->slot_id) | XHCI_TRB_EP(dci) |
	            XHCI_TRB_TYPE(XHCI_TRB_STOP_EP), evt);
	_cmd_submit(_ring_deq_ptr(ring), 0, XHCI_TRB_SLOT(dev->slot_id) |
	            XHCI_TRB_EP(dci) | XHCI_TRB_TYPE(XHCI_TRB_SET_TR_DEQ), evt);
}

/* Release a device slot so it can be enabled again (retry, next hub port). */
static void _disable_slot(usbh_dev_t *dev)
{
	u32 evt[4];
	if (!dev->slot_id)
		return;
	_cmd_submit(0, 0, XHCI_TRB_SLOT(dev->slot_id) | XHCI_TRB_TYPE(XHCI_TRB_DIS_SLOT), evt);
	usbh_ctxt.rings->dcbaa[dev->slot_id] = 0;
	dev->slot_id = 0;
}

static void _forget_devices(void)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	cx->ready = false;
	cx->msc   = NULL;
	for (u32 i = 0; i < USBH_MAX_SLOTS; i++)
		_disable_slot(&cx->dev[i]);
	memset(cx->dev, 0, sizeof(cx->dev));
	cx->ndev = 0;
}

int usbh_bulk_clear_halt(bool in)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	if (!cx->ready)
		return USB_ERROR_INIT;

	u8 dci = in ? cx->dci_in : cx->dci_out;
	int res = _ep_recover(cx->msc, dci, in ? &cx->bulk_in : &cx->bulk_out);
	if (res)
		return res;

	/* CLEAR_FEATURE(ENDPOINT_HALT): recipient endpoint, wIndex = EP address. */
	u16 ep_addr = (dci >> 1) | ((dci & 1) ? 0x80 : 0);
	return _ctrl_req(cx->msc, 0x02, USB_REQUEST_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
	                 ep_addr, NULL, 0);
}

/* ---------- bulk transfer ---------------------------------------------- */

static int _bulk_xfer(usbh_ring_t *ring, u8 dci, u16 mps, void *buf, u32 len, u32 *actual, u32 timeout_ms)
{
	usbh_ctxt_t *cx = &usbh_ctxt;

	if (!cx->ready)
		return USB_ERROR_INIT;
	if (!len || len > USBH_BULK_XFER_MAX)
		return USB_ERROR_XFER_ERROR;

	/* One TD of up to USBH_BULK_TRBS_MAX chained 64KB TRBs. ISP/IOC only on
	 * the last TRB so exactly one event is generated per TD. */
	u32 ntrbs = (len + USBH_XFER_MAX - 1) / USBH_XFER_MAX;
	_ring_reserve(ring, ntrbs);
	u32 first_idx = ring->idx;
	u32 remaining = len;
	u8 *p = (u8 *)buf;
	for (u32 i = 0; i < ntrbs; i++)
	{
		u32 cyc;
		u32 chunk = MIN(remaining, USBH_XFER_MAX);
		remaining -= chunk;
		u32 td_size = MIN((remaining + mps - 1) / mps, 31);

		u32 *trb = _trb_alloc(ring, &cyc);
		trb[0] = (u32)p;
		trb[1] = 0;
		trb[2] = chunk | (td_size << 17);
		trb[3] = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | cyc |
		         (remaining ? XHCI_TRB_CH : (XHCI_TRB_IOC | XHCI_TRB_ISP));
		p += chunk;
	}
	_dma_flush();

	DB(cx->msc->slot_id) = dci;

	u32 evt[4];
	int res = _evt_wait(evt, timeout_ms, (u32)ring->trb[first_idx],
	                    (u32)ring->trb[first_idx + ntrbs - 1]);
	if (res)
	{
		if (res == USB_ERROR_TIMEOUT)
			_ep_abort(cx->msc, dci, ring);
		return res;
	}

	if (actual)
	{
		/* Bytes before the TRB the event points at, plus what it moved. */
		u32 k = ((evt[0] - (u32)ring->trb[first_idx]) / 16);
		if (k >= ntrbs)
			k = ntrbs - 1;
		u32 trb_len = (k == ntrbs - 1) ? len - k * USBH_XFER_MAX : USBH_XFER_MAX;
		u32 residual = MIN(XHCI_EVT_TX_LEN(evt), trb_len);
		*actual = k * USBH_XFER_MAX + trb_len - residual;
	}

	u32 cc = XHCI_EVT_CC(evt);
	if (cc == XHCI_CC_STALL)
		return USBH_ERROR_STALL;
	if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PKT)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

int usbh_bulk_out(const void *buf, u32 len, u32 *actual, u32 timeout_ms)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	return _bulk_xfer(&cx->bulk_out, cx->dci_out, cx->out_mps, (void *)buf, len, actual, timeout_ms);
}

int usbh_bulk_in(void *buf, u32 len, u32 *actual, u32 timeout_ms)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	int res = _bulk_xfer(&cx->bulk_in, cx->dci_in, cx->in_mps, buf, len, actual, timeout_ms);
	_dma_invalidate();
	return res;
}

/* ---------- device enumeration ----------------------------------------- */

/* ENABLE_SLOT + ADDRESS_DEVICE. dev->speed/root_port/hub_port/parent_slot
 * must be filled in by the caller. */
static int _address_device(usbh_dev_t *dev)
{
	usbh_ctxt_t  *cx = &usbh_ctxt;
	usbh_rings_t *r  = cx->rings;

	int res = _cmd_enable_slot(&dev->slot_id);
	if (res)
	{
		EPRINTF("USBH: Enable Slot failed.");
		return res;
	}

	_ring_init(&dev->ep0, r->ep0_ring[dev->slot_id - 1]);
	memset(_dev_ctx(dev->slot_id, 0), 0, USBH_CTX_ENTRIES * cx->ctx_sz);
	r->dcbaa[dev->slot_id] = (u32)_dev_ctx(dev->slot_id, 0);

	/* EP0 max packet: 64 for HS (and the common FS value), 8 for LS.  The
	 * real FS value is fixed up from the device descriptor afterwards. */
	dev->ep0_mps = (dev->speed == XHCI_SPEED_LS) ? 8 : 64;

	/* Input Context: Add Slot + EP0. Entry 0 = Input Control, 1 = Slot,
	 * 1 + dci = EP context. */
	memset(r->in_ctx, 0, sizeof(r->in_ctx));
	_in_ctx(0)[1] = BIT(0) | BIT(XHCI_DCI_EP0);
	_build_slot_ctx(_in_ctx(1), dev, 1, 0, 0);
	_build_ep_ctx(_in_ctx(1 + XHCI_DCI_EP0), XHCI_EP_CONTROL, dev->ep0_mps,
	              (u32)dev->ep0.trb, false);

	/* ADDRESS_DEVICE (BSR = 0 actually sends SET_ADDRESS on the bus). */
	res = _cmd_input_ctx(dev->slot_id, XHCI_TRB_ADDR_DEV, 0);
	if (res)
	{
		EPRINTF("USBH: Address Device failed.");
		return res;
	}
	usleep(2000); /* SET_ADDRESS recovery time. */
	return USB_RES_OK;
}

static int _get_descriptor(usbh_dev_t *dev, u8 type, u8 index, u16 lang, void *buf, u16 len)
{
	return _ctrl_req(dev, 0x80, USB_REQUEST_GET_DESCRIPTOR, ((u16)type << 8) | index,
	                 lang, buf, len);
}

/* Read the device descriptor; fix EP0 max packet size if it was guessed
 * wrong (FS devices may use 8/16/32); capture identification. */
static int _read_device_descriptor(usbh_dev_t *dev, bool capture_info)
{
	usbh_ctxt_t *cx   = &usbh_ctxt;
	u8          *desc = (u8 *)USBH_BULK_IN_BUF_ADDR;

	int res = _get_descriptor(dev, USB_DESCRIPTOR_DEVICE, 0, 0, desc, 8);
	if (res)
		return res;

	u16 mps0 = desc[7];
	if (mps0 && mps0 != dev->ep0_mps)
	{
		/* Evaluate Context: only EP0 Max Packet Size is evaluated. Copy the
		 * live output EP0 context and patch MPS. */
		_dma_invalidate();
		memset(cx->rings->in_ctx, 0, sizeof(cx->rings->in_ctx));
		_in_ctx(0)[1] = BIT(XHCI_DCI_EP0);
		u32 *ep0 = _in_ctx(1 + XHCI_DCI_EP0);
		memcpy(ep0, _dev_ctx(dev->slot_id, XHCI_DCI_EP0), 32);
		ep0[1] = (ep0[1] & 0xFFFF) | ((u32)mps0 << 16);
		res = _cmd_input_ctx(dev->slot_id, XHCI_TRB_EVAL_CTX, 0);
		if (res)
		{
			EPRINTF("USBH: Evaluate Context failed.");
			return res;
		}
		dev->ep0_mps = mps0;
	}

	res = _get_descriptor(dev, USB_DESCRIPTOR_DEVICE, 0, 0, desc, 18);
	if (res)
		return res;

	dev->dev_class = desc[4];
	if (capture_info)
	{
		cx->bcd_usb = desc[2] | (desc[3] << 8);
		cx->vid     = desc[8] | (desc[9] << 8);
		cx->pid     = desc[10] | (desc[11] << 8);
	}
	return USB_RES_OK;
}

/* Fetch a string descriptor and convert UTF-16LE to ASCII. Non fatal. */
static void _read_string(usbh_dev_t *dev, u8 index, u16 lang, char *out)
{
	u8 *desc = (u8 *)USBH_BULK_IN_BUF_ADDR;
	out[0] = 0;
	if (!index)
		return;

	if (_get_descriptor(dev, USB_DESCRIPTOR_STRING, index, lang, desc, 255))
		return;

	u32 n = 0;
	u32 dlen = desc[0];
	if (dlen > 255)
		dlen = 255;
	for (u32 i = 2; i + 1 < dlen && n < USBH_STR_MAX - 1; i += 2)
	{
		u16 c = desc[i] | (desc[i + 1] << 8);
		out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
	}
	/* Trim trailing spaces. */
	while (n && out[n - 1] == ' ')
		n--;
	out[n] = 0;
}

static void _read_device_strings(usbh_dev_t *dev)
{
	usbh_ctxt_t *cx   = &usbh_ctxt;
	u8          *desc = (u8 *)USBH_BULK_IN_BUF_ADDR;

	/* Indices from the full device descriptor (already fetched). */
	u8 i_mfr = desc[14];
	u8 i_prd = desc[15];

	u16 lang = 0x0409;
	u8  langs[4];
	if (!_get_descriptor(dev, USB_DESCRIPTOR_STRING, 0, 0, desc, 4))
	{
		memcpy(langs, desc, 4);
		if (langs[0] >= 4)
			lang = langs[2] | (langs[3] << 8);
	}

	_read_string(dev, i_mfr, lang, cx->manufacturer);
	_read_string(dev, i_prd, lang, cx->product);
}

/*
 * Fetch the configuration descriptor, record bConfigurationValue and, if
 * find_msc, locate the MSC BOT interface (class 0x08, protocol 0x50) and
 * its bulk endpoint pair.
 */
static int _parse_config(usbh_dev_t *dev, bool find_msc)
{
	usbh_ctxt_t *cx  = &usbh_ctxt;
	u8          *cfg = (u8 *)USBH_BULK_IN_BUF_ADDR;

	int res = _get_descriptor(dev, USB_DESCRIPTOR_CONFIGURATION, 0, 0, cfg, 9);
	if (res)
		return res;

	u16 total = cfg[2] | (cfg[3] << 8);
	if (total < 9)
		return USB_ERROR_XFER_ERROR;
	if (total > 1024)
		total = 1024;

	res = _get_descriptor(dev, USB_DESCRIPTOR_CONFIGURATION, 0, 0, cfg, total);
	if (res)
		return res;

	cx->cfg_val = cfg[5];
	if (!find_msc)
		return USB_RES_OK;

	cx->dci_out = 0;
	cx->dci_in  = 0;

	bool msc_iface = false;
	u32  i = 0;
	while (i + 1 < total)
	{
		u8 dlen  = cfg[i];
		u8 dtype = cfg[i + 1];
		if (!dlen || i + dlen > total)
			break;

		if (dtype == USB_DESCRIPTOR_INTERFACE && dlen >= 9)
		{
			/* Stop at the first complete BOT interface (alt setting 0). */
			if (msc_iface && cx->dci_out && cx->dci_in)
				break;
			msc_iface = (cfg[i + 3] == 0) && (cfg[i + 5] == USB_CLASS_MSC) &&
			            (cfg[i + 7] == USB_MSC_PROTO_BOT);
			if (msc_iface)
				cx->iface = cfg[i + 2];
			cx->dci_out = 0;
			cx->dci_in  = 0;
		}
		else if (dtype == USB_DESCRIPTOR_ENDPOINT && dlen >= 7 && msc_iface)
		{
			u8  addr = cfg[i + 2];
			u8  attr = cfg[i + 3] & 3;
			u16 mps  = cfg[i + 4] | (cfg[i + 5] << 8);
			u8  dci  = ((addr & 0xF) * 2) + ((addr & 0x80) ? 1 : 0);
			if (attr == 2 && dci >= 2 && dci <= USBH_CTX_EPS && mps)  /* Bulk */
			{
				if (addr & 0x80)
				{
					if (!cx->dci_in) { cx->dci_in = dci; cx->in_mps = mps; }
				}
				else
				{
					if (!cx->dci_out) { cx->dci_out = dci; cx->out_mps = mps; }
				}
			}
		}
		i += dlen;
	}

	if (!msc_iface || !cx->dci_out || !cx->dci_in)
	{
		EPRINTF("USBH: No mass storage (BOT) interface found.");
		return USB_ERROR_XFER_ERROR;
	}
	return USB_RES_OK;
}

static int _set_configuration(usbh_dev_t *dev, u8 config_val)
{
	int res = _ctrl_req(dev, 0x00, USB_REQUEST_SET_CONFIGURATION, config_val, 0, NULL, 0);
	usleep(10000);
	return res;
}

/* Configure the bulk endpoints discovered in the config descriptor. */
static int _configure_bulk_eps(usbh_dev_t *dev)
{
	usbh_ctxt_t  *cx = &usbh_ctxt;
	usbh_rings_t *r  = cx->rings;

	u8 max_dci = (cx->dci_in > cx->dci_out) ? cx->dci_in : cx->dci_out;

	_ring_init(&cx->bulk_out, r->bulk_out_ring);
	_ring_init(&cx->bulk_in,  r->bulk_in_ring);

	memset(r->in_ctx, 0, sizeof(r->in_ctx));
	_in_ctx(0)[1] = BIT(0) | BIT(cx->dci_out) | BIT(cx->dci_in);
	_build_slot_ctx(_in_ctx(1), dev, max_dci, 0, 0);
	_build_ep_ctx(_in_ctx(1 + cx->dci_out), XHCI_EP_BULK_OUT, cx->out_mps, (u32)r->bulk_out_ring, true);
	_build_ep_ctx(_in_ctx(1 + cx->dci_in),  XHCI_EP_BULK_IN,  cx->in_mps,  (u32)r->bulk_in_ring,  true);

	int res = _cmd_input_ctx(dev->slot_id, XHCI_TRB_CFG_EP, 0);
	if (res)
		EPRINTF("USBH: Configure Endpoint failed.");
	return res;
}

/* Enumerate dev (already addressed) as the mass storage device. */
static int _setup_msc_device(usbh_dev_t *dev)
{
	usbh_ctxt_t *cx = &usbh_ctxt;

	int res = _parse_config(dev, true);
	if (res)
		return res;

	res = _set_configuration(dev, cx->cfg_val);
	if (res)
	{
		EPRINTF("USBH: Set Configuration failed.");
		return res;
	}

	res = _configure_bulk_eps(dev);
	if (res)
		return res;

	cx->msc   = dev;
	cx->ready = true;
	return USB_RES_OK;
}

/* ---------- hub (one tier, e.g. the dock) ------------------------------- */

static int _hub_port_feature(usbh_dev_t *hub, u8 request, u8 feature, u8 port)
{
	return _ctrl_req(hub, HUB_RT_SET_PORT, request, feature, port, NULL, 0);
}

static int _hub_port_status(usbh_dev_t *hub, u8 port, u16 *status, u16 *change)
{
	u8 *resp = (u8 *)USBH_BULK_IN_BUF_ADDR;
	int res = _ctrl_req(hub, HUB_RT_GET_PORT, USB_REQUEST_GET_STATUS, 0, port, resp, 4);
	if (!res)
	{
		*status = resp[0] | (resp[1] << 8);
		*change = resp[2] | (resp[3] << 8);
	}
	return res;
}

/* Reset a downstream hub port and address the device behind it as dev. */
static int _hub_attach_port(usbh_dev_t *hub, u8 port, usbh_dev_t *dev)
{
	u16 st = 0, ch = 0;

	/* Debounce, then reset the port and wait for C_PORT_RESET. */
	msleep(100);
	_hub_port_feature(hub, USB_REQUEST_CLEAR_FEATURE, HUB_FEAT_C_PORT_CONNECTION, port);
	int res = _hub_port_feature(hub, USB_REQUEST_SET_FEATURE, HUB_FEAT_PORT_RESET, port);
	if (res)
		return res;
	bool reset_done = false;
	for (u32 i = 0; i < 100; i++)
	{
		msleep(10);
		if (!_hub_port_status(hub, port, &st, &ch) && (ch & HUB_PORT_CHG_RESET))
		{
			reset_done = true;
			break;
		}
	}
	if (!reset_done || !(st & HUB_PORT_STAT_CONNECTION))
	{
		EPRINTF("USBH Hub: Port reset failed.");
		return USB_ERROR_INIT;
	}
	_hub_port_feature(hub, USB_REQUEST_CLEAR_FEATURE, HUB_FEAT_C_PORT_RESET, port);
	msleep(10);

	memset(dev, 0, sizeof(*dev));
	dev->root_port = hub->root_port;
	dev->hub_port  = port;
	if (st & HUB_PORT_STAT_HIGH_SPEED)
		dev->speed = XHCI_SPEED_HS;
	else if (st & HUB_PORT_STAT_LOW_SPEED)
		dev->speed = XHCI_SPEED_LS;
	else
		dev->speed = XHCI_SPEED_FS;
	/* FS/LS behind a HS hub goes through the hub's Transaction Translator. */
	if (hub->speed == XHCI_SPEED_HS && dev->speed != XHCI_SPEED_HS)
		dev->parent_slot = hub->slot_id;

	res = _address_device(dev);
	if (res)
		return res;

	res = _read_device_descriptor(dev, true);
	if (res)
		EPRINTF("USBH Hub: Device descriptor failed.");
	return res;
}

/*
 * The root device is a hub (dock): configure it, power its ports and try the
 * connected downstream ports in turn until a mass storage device is found.
 */
static int _hub_enumerate(usbh_dev_t *hub, u32 timeout_ms)
{
	usbh_ctxt_t *cx   = &usbh_ctxt;
	u8          *desc = (u8 *)USBH_BULK_IN_BUF_ADDR;

	/* A hub must be configured before class requests work. */
	int res = _parse_config(hub, false);
	if (!res)
		res = _set_configuration(hub, cx->cfg_val);
	if (res)
	{
		EPRINTF("USBH Hub: Configuration failed.");
		return res;
	}

	/* Hub descriptor: bNbrPorts, wHubCharacteristics (TT think time), bPwrOn2PwrGood. */
	res = _ctrl_req(hub, HUB_RT_GET_HUB, USB_REQUEST_GET_DESCRIPTOR, USB_DESCRIPTOR_HUB << 8, 0, desc, 9);
	if (res || !desc[2])
	{
		EPRINTF("USBH Hub: Cannot read hub descriptor.");
		return USB_ERROR_XFER_ERROR;
	}
	u8  nports = MIN(desc[2], 31);
	u8  ttt    = (desc[3] >> 5) & 3;
	u32 pwr_ms = (u32)desc[5] * 2 + 100;

	/* Tell the xHC this slot is a hub (Hub bit, port count, TT think time).
	 * This goes through Configure Endpoint with only the slot context added. */
	memset(cx->rings->in_ctx, 0, sizeof(cx->rings->in_ctx));
	_in_ctx(0)[1] = BIT(0);
	_build_slot_ctx(_in_ctx(1), hub, 1, nports, hub->speed == XHCI_SPEED_HS ? ttt : 0);
	res = _cmd_input_ctx(hub->slot_id, XHCI_TRB_CFG_EP, 0);
	if (res)
	{
		EPRINTF("USBH Hub: Slot update failed.");
		return res;
	}

	/* Power all ports and wait for power good. */
	for (u8 p = 1; p <= nports; p++)
		_hub_port_feature(hub, USB_REQUEST_SET_FEATURE, HUB_FEAT_PORT_POWER, p);
	msleep(pwr_ms);

	/* Other peripherals (keyboards, etc.) may share the hub: try every
	 * connected port once, keep polling for new connections until timeout. */
	usbh_dev_t *dev = &cx->dev[1];
	u32 tried = 0;
	for (u32 waited = 0; ; waited += 100)
	{
		for (u8 p = 1; p <= nports; p++)
		{
			u16 st = 0, ch = 0;
			if ((tried & BIT(p)) || _hub_port_status(hub, p, &st, &ch) || !(st & HUB_PORT_STAT_CONNECTION))
				continue;
			tried |= BIT(p);

			cx->ndev = 2;
			res = _hub_attach_port(hub, p, dev);
			if (!res && dev->dev_class != USB_CLASS_HUB)
			{
				_read_device_strings(dev);
				if (!_setup_msc_device(dev))
					return USB_RES_OK;
			}
			_disable_slot(dev);
			cx->ndev = 1;
		}
		if (waited >= timeout_ms)
			break;
		msleep(100);
	}

	EPRINTF("USBH Hub: No mass storage device on the hub.");
	return USB_ERROR_TIMEOUT;
}

/* ---------- root port detection and reset ------------------------------ */

/* Wait for a connect on any root port; returns the 1-based port or 0. */
static u8 _port_wait_connect(u32 timeout_ms)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	u32 waited = 0;
	while (true)
	{
		for (u8 p = 1; p <= cx->max_ports; p++)
			if (OP(XHCI_OP_PORTSC(p)) & PORT_CCS)
				return p;
		if (waited >= timeout_ms)
			return 0;
		msleep(10);
		waited += 10;
	}
}

static int _port_reset(u8 port)
{
	u32 reg = XHCI_OP_PORTSC(port);
	u32 portsc = OP(reg);
	OP(reg) = (portsc & ~PORT_RW_HAZARD) | PORT_PR;
	int res = _wait_op_bits(reg, PORT_PRC, PORT_PRC, 500);
	if (res)
		return res;

	/* Acknowledge the reset/connect change bits. */
	portsc = OP(reg);
	OP(reg) = (portsc & ~PORT_RW_HAZARD) | PORT_PRC | PORT_CSC;
	msleep(10);

	if (!(OP(reg) & PORT_PED))
		return USB_ERROR_INIT;
	return USB_RES_OK;
}

/* ---------- scratchpad buffers ------------------------------------------ */

static int _setup_scratchpad(void)
{
	usbh_ctxt_t *cx = &usbh_ctxt;

	/* HCSPARAMS2: Max Scratchpad Bufs Hi [25:21], Lo [31:27]. */
	u32 hcs2 = XUSB_HOST(XHCI_CAP_HCSPARAMS2);
	u32 num  = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
	cx->rings->dcbaa[0] = 0;
	if (!num)
		return USB_RES_OK;

	/* PAGESIZE bit n set -> page size 2^(n+12). */
	u32 pgsz = (OP(XHCI_OP_PAGESIZE) & 0xFFFF) << 12;
	if (!pgsz || ((u64)(num + 1) * pgsz) > USBH_SCRATCHPAD_SZ)
	{
		EPRINTF("USBH: Scratchpad too large.");
		return USB_ERROR_INIT;
	}

	/* Array of page pointers, then the pages themselves (page-aligned). */
	u64 *sp_array = (u64 *)USBH_SCRATCHPAD_ADDR;
	u32  pages    = USBH_SCRATCHPAD_ADDR + pgsz;
	memset(sp_array, 0, pgsz);
	for (u32 i = 0; i < num; i++)
	{
		memset((void *)(pages + i * pgsz), 0, pgsz);
		sp_array[i] = pages + i * pgsz;
	}
	cx->rings->dcbaa[0] = (u32)sp_array;
	return USB_RES_OK;
}

/* ---------- controller bring-up ---------------------------------------- */

static int _usbh_hc_init(const void *fw, u32 fw_size, bool source_vbus)
{
	usbh_ctxt_t *cx = &usbh_ctxt;

	bpmp_clk_rate_relaxed(true);

	/* Disable XUSB device controller (device and host are mutually exclusive). */
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) = BIT(CLK_U_XUSB_DEV);
	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_CLR) = BIT(CLK_U_XUSB_DEV);

	/* Enable XUSB shared clock and clear XUSB / PADCTL resets. */
	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_W_SET) = BIT(CLK_W_XUSB);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_SET) = BIT(CLK_W_XUSB);
	usleep(2);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB_PADCTL);
	usleep(2);

	/* Route USB2 pad 0 to XUSB. */
	XUSB_PADCTL(XUSB_PADCTL_USB2_PAD_MUX) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_PAD_MUX) &
		 ~(PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_MASK | PADCTL_USB2_PAD_MUX_USB2_OTG_PAD_PORT0_MASK)) |
		PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_XUSB | PADCTL_USB2_PAD_MUX_USB2_OTG_PAD_PORT0_XUSB;

	_usbh_init_phy();

	/* USB2 port 0 in HOST mode (xusbd.c uses DEV). */
	XUSB_PADCTL(XUSB_PADCTL_USB2_PORT_CAP) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_PORT_CAP) & ~PADCTL_USB2_PORT_CAP_PORT_0_CAP_MASK) |
		PADCTL_USB2_PORT_CAP_PORT_0_CAP_HOST;

	/* Override VBUS as present and ID as grounded so the controller takes
	 * the host role.  The OVR field must be cleared explicitly (GND = 0). */
	XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) =
		(XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) &
		 ~(PADCTL_USB2_VBUS_ID_VBUS_OVR_MASK | PADCTL_USB2_VBUS_ID_SRC_MASK |
		   PADCTL_USB2_VBUS_ID_OVR_MASK)) |
		PADCTL_USB2_VBUS_ID_VBUS_OVR_EN | PADCTL_USB2_VBUS_ID_VBUS_ON |
		PADCTL_USB2_VBUS_ID_SRC_ID_OVR_EN | PADCTL_USB2_VBUS_ID_OVR_GND;

	XUSB_PADCTL(XUSB_PADCTL_SS_PORT_MAP) &= ~PADCTL_SS_PORT_MAP_PORT0_MASK;
	PMC(APBDEV_PMC_USB_AO) &= 0xFFFFFFF3;
	usleep(1);

	/* Source 5V onto VBUS for bus-powered devices (OTG adapter in handheld
	 * mode). Never while VBUS is already driven from outside (dock, charger):
	 * the caller checks the charger's VBUS status and decides. */
	if (source_vbus)
	{
		regulator_5v_enable(REGULATOR_5V_ALL);
		regulator_5v_usb_src_enable(true);
	}

	_usbh_init_host_clocks();
	bpmp_clk_rate_relaxed(false);

	/* Enable XUSB host IPFS, program FPCI BAR0 and enable bus mastering. */
	XUSB_HOST_CFG(XUSB_HOST_CONFIGURATION) |= HOST_CONFIGURATION_EN_FPCI;
	usleep(20);
	XUSB_HOST_PCI(XUSB_CFG_4) = XUSB_HOST_BASE | CFG_4_ADDRESS_TYPE_32_BIT;
	usleep(200);
	XUSB_HOST_PCI(XUSB_CFG_1) |= CFG_1_BUS_MASTER | CFG_1_MEMORY_SPACE | CFG_1_IO_SPACE;
	usleep(10);

	/* Load and boot the Falcon firmware. The xHCI interface is dead until
	 * the firmware is running. */
	int res = _usbh_load_firmware(fw, fw_size);
	if (res)
	{
		EPRINTF("USBH: XUSB firmware missing or invalid!");
		return res;
	}

	/* Capability registers: register group bases and context size. */
	u32 cap_len = XUSB_HOST(XHCI_CAP_CAPLENGTH) & 0xFF;
	u32 dboff   = XUSB_HOST(XHCI_CAP_DBOFF)  & ~0x3u;
	u32 rtsoff  = XUSB_HOST(XHCI_CAP_RTSOFF) & ~0x1Fu;
	cx->op_base   = XUSB_HOST_BASE + cap_len;
	cx->rt_base   = XUSB_HOST_BASE + rtsoff;
	cx->db_base   = XUSB_HOST_BASE + dboff;
	cx->ctx_sz    = (XUSB_HOST(XHCI_CAP_HCCPARAMS1) & CAP_HCC_CSZ) ? 64 : 32;
	cx->max_ports = XUSB_HOST(XHCI_CAP_HCSPARAMS1) >> 24;
	if (!cap_len || cap_len == 0xFF || !cx->max_ports)
	{
		EPRINTF("USBH: Controller registers unreadable.");
		return USB_ERROR_INIT;
	}

	/* Wait for the firmware to bring the controller up (USBSTS.CNR clear). */
	res = _wait_op_bits(XHCI_OP_USBSTS, OP_STS_CNR, 0, 500);
	if (res)
	{
		EPRINTF("USBH: Controller not ready (firmware).");
		return res;
	}

	/* Reset the host controller. */
	OP(XHCI_OP_USBCMD) = (OP(XHCI_OP_USBCMD) & ~OP_CMD_RUN) | OP_CMD_HCRST;
	res = _wait_op_bits(XHCI_OP_USBCMD, OP_CMD_HCRST, 0, 500);
	if (!res)
		res = _wait_op_bits(XHCI_OP_USBSTS, OP_STS_CNR, 0, 500);
	if (res)
	{
		EPRINTF("USBH: HC reset timeout.");
		return res;
	}

	/* Rings, contexts and scratchpad. */
	cx->rings = (usbh_rings_t *)USBH_RING_ADDR;
	memset(cx->rings, 0, sizeof(usbh_rings_t));
	res = _setup_scratchpad();
	if (res)
		return res;

	OP(XHCI_OP_DCBAAP_LO) = (u32)cx->rings->dcbaa;
	OP(XHCI_OP_DCBAAP_HI) = 0;

	_ring_init(&cx->cmd, cx->rings->cmd_ring);
	OP(XHCI_OP_CRCR_LO) = (u32)cx->rings->cmd_ring | OP_CRCR_RCS;
	OP(XHCI_OP_CRCR_HI) = 0;

	/* Event ring: 1 segment. ERSTBA is written last (arms the ring). */
	cx->evt_idx = 0;
	cx->evt_ccs = 1;
	cx->rings->erst[0] = (u32)cx->rings->evt_ring;
	cx->rings->erst[1] = 0;
	cx->rings->erst[2] = USBH_EVT_RING_SZ;
	cx->rings->erst[3] = 0;
	_dma_flush();
	RT(XHCI_RT_ERSTSZ)    = 1;
	RT(XHCI_RT_ERDP_LO)   = (u32)cx->rings->evt_ring;
	RT(XHCI_RT_ERDP_HI)   = 0;
	RT(XHCI_RT_ERSTBA_LO) = (u32)cx->rings->erst;
	RT(XHCI_RT_ERSTBA_HI) = 0;

	/* Enable our slots and start the controller. */
	OP(XHCI_OP_CONFIG) = USBH_MAX_SLOTS;
	OP(XHCI_OP_USBCMD) |= OP_CMD_RUN;
	res = _wait_op_bits(XHCI_OP_USBSTS, OP_STS_HCH, 0, 500);
	if (res)
	{
		EPRINTF("USBH: Start timeout.");
		return res;
	}

	/* Power all root ports (no-op where PP is hardwired). */
	for (u8 p = 1; p <= cx->max_ports; p++)
	{
		u32 reg = XHCI_OP_PORTSC(p);
		u32 portsc = OP(reg);
		if (!(portsc & PORT_PP))
			OP(reg) = (portsc & ~PORT_RW_HAZARD) | PORT_PP;
	}
	msleep(20);

	cx->initialized = true;
	return USB_RES_OK;
}

/* ---------- public API -------------------------------------------------- */

int usbh_init(const void *fw, u32 fw_size, u32 timeout_ms, bool source_vbus)
{
	usbh_ctxt_t *cx = &usbh_ctxt;
	int res;

	if (cx->ready)
		return USB_RES_OK;

	if (!cx->initialized)
	{
		memset(cx, 0, sizeof(*cx));
		res = _usbh_hc_init(fw, fw_size, source_vbus);
		if (res)
		{
			usbh_deinit();
			return res;
		}
		msleep(100); /* Let VBUS stabilise before looking for a device. */
	}

	/* Wait for a device on any root port. T210: ports 1-4 USB3, 5-8 USB2. */
	u8 port = _port_wait_connect(timeout_ms);
	if (!port)
		return USB_ERROR_TIMEOUT;  /* No device: controller stays up for a retry. */

	/* Debounce (USB 2.0 TATTDB), then reset the port. */
	msleep(100);
	res = _port_reset(port);
	if (res)
	{
		EPRINTF("USBH: Port reset failed.");
		return res;
	}

	/* Root device: speed from PORTSC.PS (1 FS, 2 LS, 3 HS, 4 SS). Slots from
	 * an earlier attempt are released first. */
	_forget_devices();
	usbh_dev_t *dev = &cx->dev[0];
	cx->ndev = 1;
	u32 ps = (OP(XHCI_OP_PORTSC(port)) & PORT_PS_MASK) >> PORT_PS_SHIFT;
	dev->speed     = ps ? ps : XHCI_SPEED_HS;
	dev->root_port = port;

	res = _address_device(dev);
	if (res)
		return res;

	res = _read_device_descriptor(dev, true);
	if (res)
	{
		EPRINTF("USBH: Device descriptor failed.");
		return res;
	}
	_read_device_strings(dev);

	/* Hub (dock) or direct mass storage device. */
	if (dev->dev_class == USB_CLASS_HUB)
		return _hub_enumerate(dev, timeout_ms);

	return _setup_msc_device(dev);
}

void usbh_deinit(void)
{
	usbh_ctxt_t *cx = &usbh_ctxt;

	if (cx->op_base && !(OP(XHCI_OP_USBSTS) & OP_STS_CNR))
	{
		/* Drop port power so the device sees a clean disconnect, then halt. */
		for (u8 p = 1; p <= cx->max_ports; p++)
			OP(XHCI_OP_PORTSC(p)) = OP(XHCI_OP_PORTSC(p)) & ~(PORT_RW_HAZARD | PORT_PP);
		OP(XHCI_OP_USBCMD) &= ~OP_CMD_RUN;
		_wait_op_bits(XHCI_OP_USBSTS, OP_STS_HCH, OP_STS_HCH, 100);
	}

	/* Power down VBUS and drop the host role overrides. */
	regulator_5v_usb_src_enable(false);
	regulator_5v_disable(REGULATOR_5V_ALL);
	XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) &=
		~(PADCTL_USB2_VBUS_ID_VBUS_OVR_MASK | PADCTL_USB2_VBUS_ID_VBUS_ON);

	/* Hold host and SS in reset with clocks off. Partitions stay powered. */
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) = BIT(CLK_U_XUSB_HOST);
	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_CLR) = BIT(CLK_U_XUSB_HOST);
	CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_SET) = BIT(CLK_W_XUSB_SS);
	CLOCK(CLK_RST_CONTROLLER_CLK_ENB_W_CLR) = BIT(CLK_W_XUSB_SS);

	memset(cx, 0, sizeof(*cx));
}
