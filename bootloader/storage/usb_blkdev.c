/*
 * USB Block Device — thin wrapper presenting usbh_msc as a block-read interface
 * matching the sdmmc_storage_read() convention used by emummc.c.
 *
 * Copyright (c) 2024 Hekate contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <usb/usbh.h>
#include <usb/usbh_msc.h>

#include "usb_blkdev.h"

static usb_blkdev_t usb_blkdev_inst;

usb_blkdev_t *usb_blkdev_get(void) { return &usb_blkdev_inst; }

int usb_blkdev_init(usb_blkdev_t *dev)
{
	int res = usbh_msc_init();
	if (res)
		return res;
	dev->sector_count = usbh_msc_get_sector_count();
	return 0;
}

/*
 * Read count 512-byte sectors starting at sector from the USB drive.
 * Applies the LBA offset stored in dev->sector_start.
 */
int usb_blkdev_read(usb_blkdev_t *dev, u32 sector, u32 count, void *buf)
{
	return usbh_msc_read(sector + dev->sector_start, count, buf);
}
