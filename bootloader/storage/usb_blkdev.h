/*
 * USB Block Device header
 *
 * Copyright (c) 2024 Hekate contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef _USB_BLKDEV_H_
#define _USB_BLKDEV_H_

#include <utils/types.h>

typedef struct {
	u32 sector_start;   /* LBA offset on USB drive where emuMMC begins */
	u32 sector_count;   /* Total sectors on USB drive */
} usb_blkdev_t;

usb_blkdev_t *usb_blkdev_get(void);
int usb_blkdev_init(usb_blkdev_t *dev);
int usb_blkdev_read(usb_blkdev_t *dev, u32 sector, u32 count, void *buf);

#endif /* _USB_BLKDEV_H_ */
