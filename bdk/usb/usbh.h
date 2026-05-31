/*
 * USB Host (XHCI) driver for Tegra X1
 *
 * Copyright (c) 2024 Hekate contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef _USBH_H_
#define _USBH_H_

#include <utils/types.h>

/* XHCI standard completion codes (event TRB DW2 bits [31:24]). */
#define XHCI_CC_SUCCESS          1
#define XHCI_CC_SHORT_PKT        13
#define XHCI_CC_STOPPED          26

/* XHCI standard TRB types. */
#define XHCI_TRB_NORMAL          1
#define XHCI_TRB_SETUP_STG       2
#define XHCI_TRB_DATA_STG        3
#define XHCI_TRB_STATUS_STG      4
#define XHCI_TRB_LINK            6
#define XHCI_TRB_EN_SLOT         9
#define XHCI_TRB_ADDR_DEV        11
#define XHCI_TRB_CFG_EP          12
#define XHCI_TRB_EVT_XFER        32
#define XHCI_TRB_EVT_CMD         33
#define XHCI_TRB_EVT_PORT        34

/* TRB DW3 field builders. */
#define XHCI_TRB_TYPE(t)         ((t) << 10)
#define XHCI_TRB_SLOT(s)         ((s) << 24)
#define XHCI_TRB_EP(e)           ((e) << 16)
#define XHCI_TRB_TRT(t)          ((t) << 16)  /* Transfer Type for Setup Stage */
#define XHCI_TRB_IOC             BIT(5)
#define XHCI_TRB_ISP             BIT(2)
#define XHCI_TRB_IDT             BIT(6)        /* Immediate Data */
#define XHCI_TRB_TC              BIT(1)        /* Toggle Cycle (Link TRB) */
#define XHCI_TRB_DIR_IN          BIT(16)

/* Event TRB field extractors. */
#define XHCI_EVT_CC(trb)         (((trb)[2]) >> 24)
#define XHCI_EVT_TYPE(trb)       (((trb)[3] >> 10) & 0x3F)
#define XHCI_EVT_SLOT(trb)       (((trb)[3]) >> 24)
#define XHCI_EVT_EP(trb)         (((trb)[3] >> 16) & 0x1F)
#define XHCI_EVT_CYCLE(trb)      ((trb)[3] & 1)
#define XHCI_EVT_TX_LEN(trb)     ((trb)[2] & 0xFFFFFF)
#define XHCI_EVT_PORT(trb)       (((trb)[0] >> 24) & 0xFF)

/* Slot context speed values (match XHCI spec). */
#define XHCI_SPEED_FS            1
#define XHCI_SPEED_LS            2
#define XHCI_SPEED_HS            3
#define XHCI_SPEED_SS            4

/* Endpoint types (XHCI ep_type field). */
#define XHCI_EP_ISOC_OUT         1
#define XHCI_EP_BULK_OUT         2
#define XHCI_EP_INTR_OUT         3
#define XHCI_EP_CONTROL          4
#define XHCI_EP_ISOC_IN          5
#define XHCI_EP_BULK_IN          6
#define XHCI_EP_INTR_IN          7

/* Device Context Indices. */
#define XHCI_DCI_EP0             1
#define XHCI_DCI_EP1_OUT         2
#define XHCI_DCI_EP1_IN          3

/* Number of TRBs per ring (last one is always a Link TRB). */
#define USBH_TRB_RING_SZ         4  /* 3 usable + 1 link */
#define USBH_EVT_RING_SZ         16

/*
 * Ring buffers and contexts packed into one 1KB block placed at XUSB_RING_ADDR.
 * Every field starts on a 64-byte boundary as required by XHCI.
 */
typedef struct {
	/* +0x000: DCBAA — Device Context Base Address Array (slot 0 + slot 1). */
	u64  dcbaa[8];                       /* 64 bytes */

	/* +0x040: Device Context for slot 1. */
	u32  dev_slot[8];                    /* Slot context, 32 bytes */
	u32  dev_ep[4][8];                   /* EP contexts [0..3], 4×32=128 bytes */
	u8   _pad1[32];                      /* pad to 64-byte boundary */

	/* +0x100: Input Context (Input Ctrl + Slot + 4 EP contexts). */
	u32  in_ctrl[8];                     /* Input Control Context, 32 bytes */
	u32  in_slot[8];                     /* Input Slot Context, 32 bytes */
	u32  in_ep[4][8];                    /* Input EP contexts [0..3], 4×32=128 bytes */

	/* +0x1C0: Command Ring (3 command TRBs + 1 Link TRB). */
	u32  cmd_ring[USBH_TRB_RING_SZ][4]; /* 64 bytes */

	/* +0x200: Event Ring Segment Table (1 entry = 16 bytes, padded to 64). */
	u32  erst_lo;                        /* Segment base address low */
	u32  erst_hi;                        /* Segment base address high */
	u32  erst_size;                      /* Number of TRBs in segment */
	u32  erst_rsvd;
	u8   _pad2[48];

	/* +0x240: Event Ring. */
	u32  evt_ring[USBH_EVT_RING_SZ][4]; /* 16×16=256 bytes */

	/* +0x340: EP0 Transfer Ring (Setup+Data+Status + Link). */
	u32  ep0_ring[USBH_TRB_RING_SZ][4]; /* 64 bytes */

	/* +0x380: EP1-OUT Transfer Ring. */
	u32  ep1out_ring[USBH_TRB_RING_SZ][4];

	/* +0x3C0: EP1-IN Transfer Ring. */
	u32  ep1in_ring[USBH_TRB_RING_SZ][4];
} usbh_rings_t; /* 1024 bytes total */

/* USBH driver context. */
typedef struct {
	usbh_rings_t *rings;

	u32  op_base;      /* Operational register base = HOST_BASE + caplength */
	u32  rt_base;      /* Runtime register base = HOST_BASE + rtsoff */
	u32  db_base;      /* Doorbell array base = HOST_BASE + dboff */

	u8   slot_id;
	u8   port_speed;   /* XHCI_SPEED_* */
	u16  max_packet;   /* 64 (FS) or 512 (HS) for bulk EPs */

	/* Transfer ring state (enqueue index + producer cycle state). */
	u32  cmd_idx;
	u8   cmd_pcs;
	u32  ep0_idx;
	u8   ep0_pcs;
	u32  ep1out_idx;
	u8   ep1out_pcs;
	u32  ep1in_idx;
	u8   ep1in_pcs;

	/* Event ring state (dequeue index + consumer cycle state). */
	u32  evt_idx;
	u8   evt_ccs;

	bool ready;    /* true when MSC device is enumerated and ready */
} usbh_ctxt_t;

/* Public API. */
int  usbh_init(void);
int  usbh_ctrl_xfer(u8 req_type, u8 request, u16 value, u16 index,
                    void *buf, u16 len);
int  usbh_bulk_out(const void *buf, u32 len, u32 *actual);
int  usbh_bulk_in(void *buf, u32 len, u32 *actual);
void usbh_deinit(void);
bool usbh_is_ready(void);

/* Internal helpers used by usbh_msc.c and usbh_hub.c. */
usbh_ctxt_t *usbh_get_ctxt(void);

/* Hub driver entry point — called from usbh_init() when class 0x09 detected. */
int usbh_hub_enumerate(void);

#endif /* _USBH_H_ */
