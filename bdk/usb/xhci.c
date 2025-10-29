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

#include <string.h>
#include <usb/xhci.h>
#include <soc/t210.h>
#include <soc/clock.h>
#include <soc/pmc.h>
#include <soc/timer.h>
#include <mem/heap.h>
#include <utils/util.h>

#define XHCI_TIMEOUT_MS 1000

static inline u32 xhci_read32(u32 addr) {
	return *(volatile u32 *)addr;
}

static inline void xhci_write32(u32 addr, u32 val) {
	*(volatile u32 *)addr = val;
}

static void _xhci_wait_for_bit(u32 addr, u32 mask, bool set, u32 timeout_ms) {
	u32 timeout = get_tmr_ms() + timeout_ms;
	while (get_tmr_ms() < timeout) {
		u32 val = xhci_read32(addr);
		if (set) {
			if (val & mask)
				return;
		} else {
			if (!(val & mask))
				return;
		}
		usleep(10);
	}
}

static int _xhci_init_clocks() {
	// Enable XUSB host clock
	clock_enable_xusb_host();
	
	// Enable XUSB SS (SuperSpeed) clock
	clock_enable_xusb_ss();
	
	// Enable XUSB FS (FullSpeed) clock  
	clock_enable_xusb_fs();
	
	return 0;
}

static int _xhci_init_power() {
	// Power on XUSB partitions
	pmc_enable_partition(POWER_RAIL_XUSBA, 1);
	pmc_enable_partition(POWER_RAIL_XUSBB, 1);
	pmc_enable_partition(POWER_RAIL_XUSBC, 1);
	
	return 0;
}

static int _xhci_reset_controller(xhci_controller_t *xhci) {
	u32 cmd_addr = xhci->op_base + XHCI_OP_USBCMD;
	u32 sts_addr = xhci->op_base + XHCI_OP_USBSTS;
	
	// Stop controller if running
	u32 cmd = xhci_read32(cmd_addr);
	if (cmd & XHCI_CMD_RUN) {
		cmd &= ~XHCI_CMD_RUN;
		xhci_write32(cmd_addr, cmd);
		_xhci_wait_for_bit(sts_addr, XHCI_STS_HCH, true, XHCI_TIMEOUT_MS);
	}
	
	// Reset controller
	cmd = xhci_read32(cmd_addr);
	cmd |= XHCI_CMD_HCRST;
	xhci_write32(cmd_addr, cmd);
	
	// Wait for reset to complete
	_xhci_wait_for_bit(cmd_addr, XHCI_CMD_HCRST, false, XHCI_TIMEOUT_MS);
	_xhci_wait_for_bit(sts_addr, XHCI_STS_CNR, false, XHCI_TIMEOUT_MS);
	
	return 0;
}

static int _xhci_init_rings(xhci_controller_t *xhci) {
	// Allocate command ring
	xhci->cmd_ring = (xhci_trb_t *)calloc(XHCI_RING_SIZE, sizeof(xhci_trb_t));
	if (!xhci->cmd_ring)
		return -1;
	
	xhci->cmd_ring_cycle = 1;
	xhci->cmd_ring_enqueue = 0;
	
	// Set link TRB at end of command ring
	xhci_trb_t *link = &xhci->cmd_ring[XHCI_RING_SIZE - 1];
	link->param1 = (u32)xhci->cmd_ring;
	link->param2 = 0;
	link->control = (XHCI_TRB_LINK << 10) | BIT(1); // Toggle cycle bit
	
	// Allocate event ring
	xhci->event_ring = (xhci_trb_t *)calloc(XHCI_EVENT_RING_SIZE, sizeof(xhci_trb_t));
	if (!xhci->event_ring)
		return -1;
	
	xhci->event_ring_cycle = 1;
	xhci->event_ring_dequeue = 0;
	
	// Allocate event ring segment table
	xhci->erst = (xhci_erst_entry_t *)calloc(1, sizeof(xhci_erst_entry_t));
	if (!xhci->erst)
		return -1;
	
	xhci->erst[0].seg_addr_lo = (u32)xhci->event_ring;
	xhci->erst[0].seg_addr_hi = 0;
	xhci->erst[0].seg_size = XHCI_EVENT_RING_SIZE;
	
	// Allocate device context base address array
	xhci->dcbaap = (u64 *)calloc(XHCI_MAX_SLOTS + 1, sizeof(u64));
	if (!xhci->dcbaap)
		return -1;
	
	return 0;
}

static int _xhci_program_registers(xhci_controller_t *xhci) {
	// Program max device slots
	u32 config_addr = xhci->op_base + XHCI_OP_CONFIG;
	xhci_write32(config_addr, xhci->max_slots & XHCI_CONFIG_SLOTS_MASK);
	
	// Program device context base address array pointer
	u32 dcbaap_lo_addr = xhci->op_base + XHCI_OP_DCBAAP_LO;
	u32 dcbaap_hi_addr = xhci->op_base + XHCI_OP_DCBAAP_HI;
	xhci_write32(dcbaap_lo_addr, (u32)xhci->dcbaap);
	xhci_write32(dcbaap_hi_addr, 0);
	
	// Program command ring control register
	u32 crcr_lo_addr = xhci->op_base + XHCI_OP_CRCR_LO;
	u32 crcr_hi_addr = xhci->op_base + XHCI_OP_CRCR_HI;
	xhci_write32(crcr_lo_addr, ((u32)xhci->cmd_ring & ~0x3F) | XHCI_CRCR_RCS);
	xhci_write32(crcr_hi_addr, 0);
	
	// Program event ring (runtime registers)
	u32 erst_size_addr = xhci->runtime_base + 0x28;
	u32 erst_addr_lo = xhci->runtime_base + 0x30;
	u32 erst_addr_hi = xhci->runtime_base + 0x34;
	u32 erdp_lo_addr = xhci->runtime_base + 0x38;
	u32 erdp_hi_addr = xhci->runtime_base + 0x3C;
	
	xhci_write32(erst_size_addr, 1); // 1 segment
	xhci_write32(erst_addr_lo, (u32)xhci->erst);
	xhci_write32(erst_addr_hi, 0);
	xhci_write32(erdp_lo_addr, (u32)xhci->event_ring);
	xhci_write32(erdp_hi_addr, 0);
	
	return 0;
}

int xhci_init(xhci_controller_t *xhci) {
	if (!xhci)
		return -1;
	
	memset(xhci, 0, sizeof(xhci_controller_t));
	
	// Set base address
	xhci->base_addr = XUSB_HOST_BASE;
	
	// Initialize clocks and power
	_xhci_init_clocks();
	_xhci_init_power();
	
	// Read capability registers
	xhci->cap_length = xhci_read32(xhci->base_addr + XHCI_CAP_CAPLENGTH) & 0xFF;
	xhci->op_base = xhci->base_addr + xhci->cap_length;
	
	u32 dboff = xhci_read32(xhci->base_addr + XHCI_CAP_DBOFF) & ~0x3;
	xhci->doorbell_base = xhci->base_addr + dboff;
	
	u32 rtsoff = xhci_read32(xhci->base_addr + XHCI_CAP_RTSOFF) & ~0x1F;
	xhci->runtime_base = xhci->base_addr + rtsoff;
	
	// Get max device slots
	u32 hcsparams1 = xhci_read32(xhci->base_addr + XHCI_CAP_HCSPARAMS1);
	xhci->max_slots = (hcsparams1 >> 24) & 0xFF;
	if (xhci->max_slots > XHCI_MAX_SLOTS)
		xhci->max_slots = XHCI_MAX_SLOTS;
	
	// Reset controller
	_xhci_reset_controller(xhci);
	
	// Initialize rings and data structures
	if (_xhci_init_rings(xhci) < 0)
		return -1;
	
	// Program registers
	_xhci_program_registers(xhci);
	
	// Enable interrupts (optional for polling mode)
	u32 cmd_addr = xhci->op_base + XHCI_OP_USBCMD;
	u32 cmd = xhci_read32(cmd_addr);
	cmd |= XHCI_CMD_INTE;
	xhci_write32(cmd_addr, cmd);
	
	// Start controller
	cmd |= XHCI_CMD_RUN;
	xhci_write32(cmd_addr, cmd);
	
	// Wait for controller to be ready
	u32 sts_addr = xhci->op_base + XHCI_OP_USBSTS;
	_xhci_wait_for_bit(sts_addr, XHCI_STS_HCH, false, XHCI_TIMEOUT_MS);
	
	return 0;
}

void xhci_deinit(xhci_controller_t *xhci) {
	if (!xhci)
		return;
	
	// Stop controller
	u32 cmd_addr = xhci->op_base + XHCI_OP_USBCMD;
	u32 cmd = xhci_read32(cmd_addr);
	cmd &= ~XHCI_CMD_RUN;
	xhci_write32(cmd_addr, cmd);
	
	// Free allocated memory
	if (xhci->cmd_ring)
		free(xhci->cmd_ring);
	if (xhci->event_ring)
		free(xhci->event_ring);
	if (xhci->erst)
		free(xhci->erst);
	if (xhci->dcbaap)
		free(xhci->dcbaap);
	
	// Free device contexts
	for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
		if (xhci->device_ctx[i])
			free(xhci->device_ctx[i]);
	}
	
	// Free transfer rings
	for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
		for (int j = 0; j < XHCI_MAX_ENDPOINTS; j++) {
			if (xhci->transfer_rings[i][j])
				free(xhci->transfer_rings[i][j]);
		}
	}
}

int xhci_port_reset(xhci_controller_t *xhci, u8 port) {
	if (!xhci || port == 0)
		return -1;
	
	u32 portsc_addr = xhci->op_base + XHCI_OP_PORTSC(port - 1);
	
	// Issue port reset
	u32 portsc = xhci_read32(portsc_addr);
	portsc |= XHCI_PORTSC_PR;
	xhci_write32(portsc_addr, portsc);
	
	// Wait for reset completion
	_xhci_wait_for_bit(portsc_addr, XHCI_PORTSC_PR, false, XHCI_TIMEOUT_MS);
	
	// Check if port is enabled
	portsc = xhci_read32(portsc_addr);
	if (!(portsc & XHCI_PORTSC_PED))
		return -1;
	
	return 0;
}

// Placeholder implementations for enumeration and transfers
// These will be implemented in subsequent phases

int xhci_enumerate_device(xhci_controller_t *xhci, u8 port) {
	// TODO: Implement device enumeration
	return -1;
}

int xhci_control_transfer(xhci_controller_t *xhci, u8 slot, u8 ep, 
                          void *setup, void *data, u32 len) {
	// TODO: Implement control transfers
	return -1;
}

int xhci_bulk_transfer(xhci_controller_t *xhci, u8 slot, u8 ep, 
                       void *data, u32 len, bool in) {
	// TODO: Implement bulk transfers
	return -1;
}
