/*
 * USB mass storage as hekate's main storage.
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

#ifndef _USB_STORAGE_H_
#define _USB_STORAGE_H_

#include <bdk.h>
#include <usb/usbh.h>
#include <usb/usbh_msc.h>

/* Tegra210 XUSB host firmware (L4T tegra21x_xusb_firmware). Must be on the SD card. */
#define USB_STORAGE_FW_PATH "bootloader/sys/xusbfw.bin"

/* Bring up the USB host (driver module + firmware from the SD card) and a
 * mass storage device. Waits up to timeout_ms for a device to be connected.
 * Returns true when the drive is usable. */
bool usb_storage_init(u32 timeout_ms);
bool usb_storage_is_ready(void);

/* Driver state for the info page. Only valid while usb_storage_is_ready(). */
usbh_ctxt_t *usb_storage_ctxt(void);
usbh_msc_t  *usb_storage_msc(void);

/* Block access used by the FatFS disk layer. Return 0 on success. */
int  usb_storage_read(u32 sector, u32 count, void *buf);
int  usb_storage_write(u32 sector, u32 count, const void *buf);

/* Make the USB drive (true) or the SD card (false) volume 0, the storage
 * every "sd:" path is served from. Returns true on success; on failure the
 * SD card stays the main storage. */
bool usb_storage_set_primary(bool enable);
bool usb_storage_is_primary(void);

/* FatFS volume prefix for files that must live on the SD card (emuMMC):
 * "" normally, "1:" while the USB drive is the main storage. */
const char *usb_storage_sd_volume(void);

/* Release volumes, stop the host controller and cut VBUS. */
void usb_storage_end(void);

#endif /* _USB_STORAGE_H_ */
