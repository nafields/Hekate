/*
 * USB Host Mass Storage Class driver header
 *
 * Copyright (c) 2024 Hekate contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef _USBH_MSC_H_
#define _USBH_MSC_H_

#include <utils/types.h>

typedef struct {
	u32  num_sectors;
	bool ready;
} usbh_msc_t;

usbh_msc_t *usbh_msc_get(void);
int  usbh_msc_init(void);
int  usbh_msc_read(u32 sector, u32 count, void *buf);
int  usbh_msc_write(u32 sector, u32 count, void *buf);
u32  usbh_msc_get_sector_count(void);

#endif /* _USBH_MSC_H_ */
