/*
 * USB Mass Storage emuMMC integration for Hekate
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

#ifndef EMUMMC_STORAGE_USB_H
#define EMUMMC_STORAGE_USB_H

#include <bdk.h>

/* Initialize USB mass storage for emuMMC */
int emummc_storage_usb_init(void);

/* Clean up USB mass storage */
void emummc_storage_usb_end(void);

/* Read sectors from USB storage */
int emummc_storage_usb_read(u32 sector, u32 num_sectors, void *buf);

/* Write sectors to USB storage (requires explicit enable) */
int emummc_storage_usb_write(u32 sector, u32 num_sectors, void *buf);

/* Enable/disable write operations (default: disabled for safety) */
void emummc_storage_usb_enable_write(bool enable);

/* Check if USB storage is initialized */
bool emummc_storage_usb_is_initialized(void);

/* Get USB storage capacity in sectors */
u64 emummc_storage_usb_get_capacity(void);

#endif /* EMUMMC_STORAGE_USB_H */
