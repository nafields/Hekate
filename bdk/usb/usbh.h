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

#ifndef _USBH_H_
#define _USBH_H_

#include <utils/types.h>

/* Result code in addition to the usbd.h USB_RES / USB_ERROR codes: bulk pipe STALLed. */
#define USBH_ERROR_STALL         60

/* XHCI standard completion codes (event TRB DW2 bits [31:24]). */
#define XHCI_CC_SUCCESS          1
#define XHCI_CC_DATA_BUF_ERR     2
#define XHCI_CC_BABBLE           3
#define XHCI_CC_USB_XACT_ERR     4
#define XHCI_CC_TRB_ERR          5
#define XHCI_CC_STALL            6
#define XHCI_CC_SHORT_PKT        13

/* XHCI standard TRB types. */
#define XHCI_TRB_NORMAL          1
#define XHCI_TRB_NOOP            8
#define XHCI_TRB_SETUP_STG       2
#define XHCI_TRB_DATA_STG        3
#define XHCI_TRB_STATUS_STG      4
#define XHCI_TRB_LINK            6
#define XHCI_TRB_EN_SLOT         9
#define XHCI_TRB_DIS_SLOT        10
#define XHCI_TRB_ADDR_DEV        11
#define XHCI_TRB_CFG_EP          12
#define XHCI_TRB_EVAL_CTX        13
#define XHCI_TRB_RESET_EP        14
#define XHCI_TRB_STOP_EP         15
#define XHCI_TRB_SET_TR_DEQ      16
#define XHCI_TRB_EVT_XFER        32
#define XHCI_TRB_EVT_CMD         33
#define XHCI_TRB_EVT_PORT        34

/* TRB DW3 field builders. */
#define XHCI_TRB_TYPE(t)         ((t) << 10)
#define XHCI_TRB_SLOT(s)         ((u32)(s) << 24)
#define XHCI_TRB_EP(e)           ((u32)(e) << 16)
#define XHCI_TRB_TRT(t)          ((u32)(t) << 16)  /* Transfer Type for Setup Stage */
#define XHCI_TRB_TC              BIT(1)             /* Toggle Cycle (Link TRB) */
#define XHCI_TRB_ISP             BIT(2)
#define XHCI_TRB_CH              BIT(4)             /* Chain */
#define XHCI_TRB_IOC             BIT(5)
#define XHCI_TRB_IDT             BIT(6)             /* Immediate Data */
#define XHCI_TRB_BSR             BIT(9)             /* Block Set Address Request */
#define XHCI_TRB_DIR_IN          BIT(16)

/* Event TRB field extractors. */
#define XHCI_EVT_CC(trb)         (((trb)[2]) >> 24)
#define XHCI_EVT_TYPE(trb)       (((trb)[3] >> 10) & 0x3F)
#define XHCI_EVT_SLOT(trb)       (((trb)[3]) >> 24)
#define XHCI_EVT_EP(trb)         (((trb)[3] >> 16) & 0x1F)
#define XHCI_EVT_TX_LEN(trb)     ((trb)[2] & 0xFFFFFF)

/* Slot context speed values (also PORTSC.PS values for USB2/USB3 ports). */
#define XHCI_SPEED_FS            1
#define XHCI_SPEED_LS            2
#define XHCI_SPEED_HS            3
#define XHCI_SPEED_SS            4

/* Endpoint types (XHCI ep_type field). */
#define XHCI_EP_BULK_OUT         2
#define XHCI_EP_CONTROL          4
#define XHCI_EP_BULK_IN          6

/* Device Context Index of the default control endpoint. */
#define XHCI_DCI_EP0             1

/* Number of TRBs per ring (last one is always a Link TRB). */
#define USBH_TRB_RING_SZ         16  /* 15 usable + 1 link */
#define USBH_EVT_RING_SZ         32

/* Endpoint contexts tracked (DCI 1..15 -> endpoints up to EP7 IN/OUT). */
#define USBH_CTX_EPS             15
#define USBH_CTX_ENTRIES         (1 + USBH_CTX_EPS)      /* Slot + EPs.       */
#define USBH_CTX_SZ_MAX          64                      /* HCCPARAMS1.CSZ=1. */

/* Device slots: root device (hub or MSC) + one device behind a hub. */
#define USBH_MAX_SLOTS           2

/* Max payload of a single Normal/Data TRB (TRB length field is 17 bits). */
#define USBH_XFER_MAX            SZ_64K
/* A bulk transfer may chain this many TRBs into one TD. */
#define USBH_BULK_TRBS_MAX       8
#define USBH_BULK_XFER_MAX       (USBH_BULK_TRBS_MAX * USBH_XFER_MAX)

/* String info captured during enumeration (ASCII, NUL terminated). */
#define USBH_STR_MAX             32

/*
 * Rings and contexts.  Placed in DRAM at USBH_RING_ADDR (the IRAM XUSB ring
 * area overlaps the IPL image in the bootloader).  Alignment per XHCI:
 * DCBAA/contexts/ERST 64 bytes, rings 16 bytes (64 used throughout).
 */
typedef struct {
	/* Device Context Base Address Array (slot 0 = scratchpad array). */
	u64  dcbaa[USBH_MAX_SLOTS + 1]                              __attribute__((aligned(64)));

	/* Output Device Contexts: Slot + EP contexts (DCI 1..15) per slot. */
	u8   dev_ctx[USBH_MAX_SLOTS][USBH_CTX_ENTRIES * USBH_CTX_SZ_MAX] __attribute__((aligned(64)));

	/* Input Context: Input Control + Slot + EP contexts (DCI 1..15).
	 * Entry for DCI n lives at index 1 + n. */
	u8   in_ctx[(USBH_CTX_ENTRIES + 1) * USBH_CTX_SZ_MAX]       __attribute__((aligned(64)));

	/* Command Ring. */
	u32  cmd_ring[USBH_TRB_RING_SZ][4]                          __attribute__((aligned(64)));

	/* Event Ring Segment Table (1 entry) and Event Ring. */
	u32  erst[4]                                                __attribute__((aligned(64)));
	u32  evt_ring[USBH_EVT_RING_SZ][4]                          __attribute__((aligned(64)));

	/* Transfer rings: EP0 control per slot, bulk OUT, bulk IN. */
	u32  ep0_ring[USBH_MAX_SLOTS][USBH_TRB_RING_SZ][4]          __attribute__((aligned(64)));
	u32  bulk_out_ring[USBH_TRB_RING_SZ][4]                     __attribute__((aligned(64)));
	u32  bulk_in_ring[USBH_TRB_RING_SZ][4]                      __attribute__((aligned(64)));
} usbh_rings_t;

/* Producer side state of a command/transfer ring. */
typedef struct {
	u32 (*trb)[4];
	u32  idx;   /* Enqueue index.         */
	u8   pcs;   /* Producer Cycle State.  */
} usbh_ring_t;

/* A USB device we have addressed (root device, or one behind a hub). */
typedef struct {
	u8   slot_id;
	u8   speed;        /* XHCI_SPEED_*                                   */
	u8   root_port;    /* 1-based root hub port.                         */
	u8   hub_port;     /* 0: on root port. Else downstream hub port.     */
	u8   parent_slot;  /* Hub slot for FS/LS devices behind a HS hub.    */
	u8   dev_class;    /* bDeviceClass.                                  */
	u16  ep0_mps;
	usbh_ring_t ep0;
} usbh_dev_t;

/* USBH driver context. */
typedef struct {
	usbh_rings_t *rings;

	u32  op_base;      /* Operational register base = HOST_BASE + caplength */
	u32  rt_base;      /* Runtime register base = HOST_BASE + rtsoff        */
	u32  db_base;      /* Doorbell array base = HOST_BASE + dboff           */
	u32  ctx_sz;       /* Context entry size: 32 or 64 bytes.               */
	u8   max_ports;    /* HCSPARAMS1.MaxPorts.                              */

	usbh_ring_t cmd;
	usbh_ring_t bulk_out;
	usbh_ring_t bulk_in;

	/* Event ring state (dequeue index + consumer cycle state). */
	u32  evt_idx;
	u8   evt_ccs;

	/* Devices: [0] = root port device, [1] = device behind hub. */
	usbh_dev_t dev[USBH_MAX_SLOTS];
	u8   ndev;
	usbh_dev_t *msc;   /* The mass storage device we talk to. */

	/* Bulk endpoint info parsed from the MSC config descriptor. */
	u8   cfg_val;      /* bConfigurationValue.   */
	u8   iface;        /* bInterfaceNumber.      */
	u8   dci_out;      /* DCI of bulk OUT.        */
	u8   dci_in;       /* DCI of bulk IN.         */
	u16  out_mps;      /* wMaxPacketSize OUT.     */
	u16  in_mps;       /* wMaxPacketSize IN.      */

	/* Device identification for the info page. */
	u16  vid;
	u16  pid;
	u16  bcd_usb;
	char manufacturer[USBH_STR_MAX];
	char product[USBH_STR_MAX];

	bool initialized;  /* Host controller is running.                       */
	bool ready;        /* MSC device enumerated; bulk endpoints configured. */
} usbh_ctxt_t;

/*
 * Bring up the XUSB host controller and enumerate a mass storage device on
 * the root port, directly or behind one hub tier (dock).
 *
 * fw/fw_size:  Tegra210 XUSB Falcon firmware image. May be NULL if the
 *              firmware is already running (re-init without power cycle).
 * timeout_ms:  How long to wait for a device to connect.
 * source_vbus: Drive 5V onto VBUS (OTG adapter, bus powered device). Must
 *              be false when VBUS is already present (dock, charger).
 *
 * Returns USB_RES_OK when the MSC bulk pipes are configured. On
 * USB_ERROR_TIMEOUT no device was connected (controller stays up so that a
 * later call can retry quickly).
 */
int  usbh_init(const void *fw, u32 fw_size, u32 timeout_ms, bool source_vbus);
void usbh_deinit(void);

bool usbh_is_initialized(void);
bool usbh_is_ready(void);
bool usbh_fw_loaded(void);
usbh_ctxt_t *usbh_get_ctxt(void);

/* Transfers to the enumerated MSC device. Return USB_RES_* / USB_ERROR_*. */
int  usbh_ctrl_xfer(u8 req_type, u8 request, u16 value, u16 index, void *buf, u16 len);
int  usbh_bulk_out(const void *buf, u32 len, u32 *actual, u32 timeout_ms);
int  usbh_bulk_in(void *buf, u32 len, u32 *actual, u32 timeout_ms);

/* Recover a halted (STALLed) bulk pipe: Reset EP + Set TR Dequeue + CLEAR_FEATURE. */
int  usbh_bulk_clear_halt(bool in);

#endif /* _USBH_H_ */
