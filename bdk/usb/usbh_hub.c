/*
 * USB Hub class driver (for dock path where SSD is behind a hub)
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

#include <gfx_utils.h>
#include <soc/timer.h>
#include <soc/t210.h>

/* USB Hub class requests. */
#define HUB_REQ_SET_FEATURE  0x03
#define HUB_REQ_GET_STATUS   0x00

/* Hub port features. */
#define HUB_FEAT_PORT_POWER  8
#define HUB_FEAT_PORT_RESET  4

/* bmRequestType for hub port requests. */
#define HUB_PORT_RT_SET  0x23   /* Host-to-Device, Class, Other */
#define HUB_PORT_RT_GET  0xA3   /* Device-to-Host, Class, Other */

/* Port status bits (bytes 0-1 of GET_PORT_STATUS response). */
#define PORT_STAT_CONNECTION BIT(0)

/* Port change bits (bytes 2-3 of GET_PORT_STATUS response). */
#define PORT_CHG_RESET       BIT(4)

/* XHCI operational PORTSC for the root port (used to detect speed of TT device). */
#define XHCI_OP_PORTSC  0x400

static int _hub_set_port_feature(u8 port, u8 feature)
{
	return usbh_ctrl_xfer(HUB_PORT_RT_SET, HUB_REQ_SET_FEATURE,
	                      feature, port, NULL, 0);
}

static int _hub_get_port_status(u8 port, u16 *pstat, u16 *pchg)
{
	u8 resp[4] = {0};
	int res = usbh_ctrl_xfer(HUB_PORT_RT_GET, HUB_REQ_GET_STATUS,
	                         0, port, resp, 4);
	if (!res) {
		*pstat = (u16)resp[0] | ((u16)resp[1] << 8);
		*pchg  = (u16)resp[2] | ((u16)resp[3] << 8);
	}
	return res;
}

static int _hub_get_num_ports(u8 *num_ports)
{
	u8 desc[9] = {0};
	int res = usbh_ctrl_xfer(0xA0, USB_REQUEST_GET_DESCRIPTOR,
	                         0x2900, 0, desc, 9);
	if (!res)
		*num_ports = desc[2];
	return res;
}

/*
 * Called when usbh_init() detects a hub (bDeviceClass == 0x09) on the
 * root port.  Powers hub ports, locates the downstream MSC device, resets
 * it, then enumerates it as slot 2 and configures bulk endpoints.
 * Sets cx->ready = true on success.
 */
int usbh_hub_enumerate(void)
{
	u8 num_ports = 0;
	int res = _hub_get_num_ports(&num_ports);
	if (res || !num_ports) {
		EPRINTF("USBH Hub: Cannot read hub descriptor.");
		return USB_ERROR_XFER_ERROR;
	}

	/* Power all ports. */
	for (u8 p = 1; p <= num_ports; p++)
		_hub_set_port_feature(p, HUB_FEAT_PORT_POWER);
	usleep(100000);

	/* Find the first downstream port with a connected device. */
	u8 dev_port = 0;
	for (u8 p = 1; p <= num_ports; p++) {
		u16 st = 0, ch = 0;
		if (!_hub_get_port_status(p, &st, &ch) && (st & PORT_STAT_CONNECTION)) {
			dev_port = p;
			break;
		}
	}

	if (!dev_port) {
		EPRINTF("USBH Hub: No device on downstream ports.");
		return USB_ERROR_TIMEOUT;
	}

	/* Reset the downstream port and wait for C_PORT_RESET. */
	_hub_set_port_feature(dev_port, HUB_FEAT_PORT_RESET);
	for (int i = 0; i < 200; i++) {
		u16 st = 0, ch = 0;
		_hub_get_port_status(dev_port, &st, &ch);
		if (ch & PORT_CHG_RESET)
			break;
		usleep(10000);
	}
	usleep(10000);

	/*
	 * The downstream device is now attached and reset.  Enumerate it:
	 * ENABLE_SLOT allocates slot 2 (slot 1 = hub), ADDRESS_DEVICE
	 * assigns it USB address 2, then we configure bulk endpoints.
	 *
	 * We reuse the single device context in the ring buffer — this works
	 * because we only ever access one device at a time (the MSC drive).
	 * cx->slot_id is updated to point to the new slot.
	 */
	usbh_ctxt_t *cx = usbh_get_ctxt();

	/* ENABLE_SLOT for downstream device. */
	u8 new_slot = 0;
	/* Send ENABLE_SLOT command via the public control path — not available
	 * directly, so reach the low-level command ring by rebuilding the call.
	 * For now, use the rings directly.  This is an internal coupling that
	 * the single-file split requires.
	 *
	 * Pragmatic approach for hub path: repurpose slot 1 by issuing a
	 * RESET_DEVICE command to clear it, then re-enumerate.
	 */

	/* RESET_DEVICE on slot 1 (the hub) to clear its device context. */
	/* We then call usbh_ctrl_xfer which now targets the hub at slot 1.
	 * After port reset, the downstream device occupies the same TT so
	 * we can re-use slot 1 context for the MSC device with a fresh
	 * ADDRESS_DEVICE (BSR=true clears the address, BSR=false assigns it).
	 *
	 * This works for single-device single-hub topologies.
	 */
	(void)new_slot;
	(void)cx;

	/*
	 * TODO: Full multi-slot hub support requires exposing ENABLE_SLOT
	 * and ADDRESS_DEVICE as public usbh APIs and allocating a second
	 * device context entry in usbh_rings_t.  For now, after the hub
	 * port reset the caller (usbh_init) has already returned, so this
	 * function must complete the entire init.
	 *
	 * Fallback: signal that the hub path is unimplemented.  The user
	 * should use a direct OTG adapter (handheld mode) to bypass the hub.
	 */
	EPRINTF("USBH Hub: Full re-enumeration not yet supported.");
	EPRINTF("USBH Hub: Use direct OTG adapter instead of dock.");
	return USB_ERROR_INIT;
}
