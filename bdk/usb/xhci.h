/*
 * xHCI (USB3 Host Controller Interface) driver for Tegra X1
 *
 * Copyright (c) 2025 Hekate Contributors
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

#ifndef _XHCI_H_
#define _XHCI_H_

#include <utils/types.h>

/* xHCI Host Controller Capability Registers */
#define XHCI_CAP_CAPLENGTH      0x00
#define XHCI_CAP_HCIVERSION     0x02
#define XHCI_CAP_HCSPARAMS1     0x04
#define XHCI_CAP_HCSPARAMS2     0x08
#define XHCI_CAP_HCSPARAMS3     0x0C
#define XHCI_CAP_HCCPARAMS1     0x10
#define XHCI_CAP_DBOFF          0x14
#define XHCI_CAP_RTSOFF         0x18
#define XHCI_CAP_HCCPARAMS2     0x1C

/* xHCI Host Controller Operational Registers (offset from CAPLENGTH) */
#define XHCI_OP_USBCMD          0x00
#define  XHCI_CMD_RUN            BIT(0)
#define  XHCI_CMD_HCRST          BIT(1)
#define  XHCI_CMD_INTE           BIT(2)
#define  XHCI_CMD_HSEE           BIT(3)
#define  XHCI_CMD_EWE            BIT(10)

#define XHCI_OP_USBSTS          0x04
#define  XHCI_STS_HCH            BIT(0)
#define  XHCI_STS_HSE            BIT(2)
#define  XHCI_STS_EINT           BIT(3)
#define  XHCI_STS_PCD            BIT(4)
#define  XHCI_STS_CNR            BIT(11)

#define XHCI_OP_PAGESIZE        0x08
#define XHCI_OP_DNCTRL          0x14
#define XHCI_OP_CRCR_LO         0x18
#define XHCI_OP_CRCR_HI         0x1C
#define  XHCI_CRCR_RCS           BIT(0)
#define  XHCI_CRCR_CS            BIT(1)
#define  XHCI_CRCR_CA            BIT(2)
#define  XHCI_CRCR_CRR           BIT(3)

#define XHCI_OP_DCBAAP_LO       0x30
#define XHCI_OP_DCBAAP_HI       0x34
#define XHCI_OP_CONFIG          0x38
#define  XHCI_CONFIG_SLOTS_MASK  0xFF

#define XHCI_OP_PORTSC(n)       (0x400 + (0x10 * (n)))
#define  XHCI_PORTSC_CCS         BIT(0)
#define  XHCI_PORTSC_PED         BIT(1)
#define  XHCI_PORTSC_PR          BIT(4)
#define  XHCI_PORTSC_PP          BIT(9)
#define  XHCI_PORTSC_SPEED_MASK  (0xF << 10)
#define  XHCI_PORTSC_SPEED_SHIFT 10
#define  XHCI_PORTSC_CSC         BIT(17)
#define  XHCI_PORTSC_PEC         BIT(18)
#define  XHCI_PORTSC_PRC         BIT(21)

/* xHCI TRB Types */
#define XHCI_TRB_NORMAL          1
#define XHCI_TRB_SETUP           2
#define XHCI_TRB_DATA            3
#define XHCI_TRB_STATUS          4
#define XHCI_TRB_LINK            6
#define XHCI_TRB_CMD_ENABLE_SLOT 9
#define XHCI_TRB_CMD_ADDRESS_DEV 11
#define XHCI_TRB_CMD_CONFIG_EP   12
#define XHCI_TRB_EVENT_TRANSFER  32
#define XHCI_TRB_EVENT_CMD_CMPL  33
#define XHCI_TRB_EVENT_PORT_SC   34

/* xHCI Completion Codes */
#define XHCI_CC_SUCCESS          1
#define XHCI_CC_SHORT_PACKET     13

/* USB Port Speed Values */
#define XHCI_SPEED_FULL          1
#define XHCI_SPEED_LOW           2
#define XHCI_SPEED_HIGH          3
#define XHCI_SPEED_SUPER         4

/* Maximum values */
#define XHCI_MAX_SLOTS           32
#define XHCI_MAX_ENDPOINTS       32
#define XHCI_RING_SIZE           256
#define XHCI_EVENT_RING_SIZE     256

/* Transfer Request Block (TRB) */
typedef struct _xhci_trb_t {
	u32 param1;
	u32 param2;
	u32 status;
	u32 control;
} __attribute__((packed)) xhci_trb_t;

/* Event Ring Segment Table Entry */
typedef struct _xhci_erst_entry_t {
	u32 seg_addr_lo;
	u32 seg_addr_hi;
	u32 seg_size;
	u32 rsvd;
} __attribute__((packed)) xhci_erst_entry_t;

/* Slot Context */
typedef struct _xhci_slot_ctx_t {
	u32 info;
	u32 info2;
	u32 tt_info;
	u32 state;
	u32 rsvd[4];
} __attribute__((packed)) xhci_slot_ctx_t;

/* Endpoint Context */
typedef struct _xhci_ep_ctx_t {
	u32 ep_info;
	u32 ep_info2;
	u32 deq_ptr_lo;
	u32 deq_ptr_hi;
	u32 tx_info;
	u32 rsvd[3];
} __attribute__((packed)) xhci_ep_ctx_t;

/* Device Context */
typedef struct _xhci_device_ctx_t {
	xhci_slot_ctx_t slot;
	xhci_ep_ctx_t ep[31];
} __attribute__((packed)) xhci_device_ctx_t;

/* xHCI Controller State */
typedef struct _xhci_controller_t {
	u32 base_addr;
	u32 cap_length;
	u32 op_base;
	u32 runtime_base;
	u32 doorbell_base;
	
	xhci_trb_t *cmd_ring;
	u32 cmd_ring_cycle;
	u32 cmd_ring_enqueue;
	
	xhci_trb_t *event_ring;
	xhci_erst_entry_t *erst;
	u32 event_ring_dequeue;
	u32 event_ring_cycle;
	
	u64 *dcbaap;
	xhci_device_ctx_t *device_ctx[XHCI_MAX_SLOTS];
	
	xhci_trb_t *transfer_rings[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];
	u32 transfer_ring_cycle[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];
	u32 transfer_ring_enqueue[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];
	
	u8 max_slots;
	u8 current_slot;
} xhci_controller_t;

/* Function prototypes */
int xhci_init(xhci_controller_t *xhci);
void xhci_deinit(xhci_controller_t *xhci);
int xhci_port_reset(xhci_controller_t *xhci, u8 port);
int xhci_enumerate_device(xhci_controller_t *xhci, u8 port);
int xhci_control_transfer(xhci_controller_t *xhci, u8 slot, u8 ep, 
                          void *setup, void *data, u32 len);
int xhci_bulk_transfer(xhci_controller_t *xhci, u8 slot, u8 ep, 
                       void *data, u32 len, bool in);

#endif /* _XHCI_H_ */
