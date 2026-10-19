/*
 * USB mass storage host access for Nyx.
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

#ifndef _NYX_USB_HOST_H_
#define _NYX_USB_HOST_H_

#include <bdk.h>
#include <usb/usbh.h>
#include <usb/usbh_msc.h>

/* Tegra210 XUSB host firmware (L4T tegra21x_xusb_firmware). Must be on the SD card. */
#define USB_HOST_FW_PATH "bootloader/sys/xusbfw.bin"

/*
 * Bring up the USB host (driver module and firmware from the SD card, which
 * must be mounted) and a mass storage device. Waits up to timeout_ms for a
 * device. Returns true when the drive is usable. error_out (optional)
 * receives a short reason when it is not.
 */
bool usb_host_init(u32 timeout_ms, const char **error_out);
bool usb_host_is_ready(void);

usbh_ctxt_t *usb_host_ctxt(void);
usbh_msc_t  *usb_host_msc(void);

/* 512-byte sector access. Return 0 on success. */
int  usb_host_read(u32 sector, u32 count, void *buf);
int  usb_host_write(u32 sector, u32 count, const void *buf);

/* Stop the host controller and cut VBUS. */
void usb_host_end(void);

#endif /* _NYX_USB_HOST_H_ */
