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

#include <string.h>
#include "emummc_storage_usb.h"
#include <usb/xhci.h>
#include <usb/usb_msc_host.h>
#include <mem/heap.h>
#include <soc/timer.h>

/* Configuration flags */
#define USB_STORAGE_READ_ONLY       1
#define USB_STORAGE_REQUIRE_HUB     1
#define USB_STORAGE_CACHE_ENABLED   1

/* Sector cache configuration */
#define CACHE_SIZE_SECTORS  64
#define CACHE_INVALID       0xFFFFFFFF

typedef struct _sector_cache_t {
	u32 start_lba;
	u32 count;
	u8 *data;
} sector_cache_t;

/* Global USB storage state */
static xhci_controller_t g_xhci;
static usb_msc_device_t g_msc_dev;
static sector_cache_t g_cache;
static bool g_usb_storage_initialized = false;
static bool g_usb_storage_write_enabled = false;

static void _cache_init(void) {
	g_cache.start_lba = CACHE_INVALID;
	g_cache.count = 0;
	if (!g_cache.data) {
		g_cache.data = (u8 *)malloc(CACHE_SIZE_SECTORS * 512);
	}
}

static void _cache_invalidate(void) {
	g_cache.start_lba = CACHE_INVALID;
	g_cache.count = 0;
}

static int _cache_read(u32 lba, u32 count, void *buffer) {
	/* Check if request is in cache */
	if (g_cache.start_lba != CACHE_INVALID &&
	    lba >= g_cache.start_lba &&
	    (lba + count) <= (g_cache.start_lba + g_cache.count)) {
		u32 offset = (lba - g_cache.start_lba) * 512;
		memcpy(buffer, g_cache.data + offset, count * 512);
		return 0;
	}
	return -1;
}

static void _cache_update(u32 lba, u32 count, void *buffer) {
	if (!g_cache.data || count > CACHE_SIZE_SECTORS)
		return;
	
	g_cache.start_lba = lba;
	g_cache.count = count;
	memcpy(g_cache.data, buffer, count * 512);
}

int emummc_storage_usb_init(void) {
	int ret;
	
	if (g_usb_storage_initialized)
		return 0;
	
	/* Initialize xHCI controller */
	ret = xhci_init(&g_xhci);
	if (ret < 0)
		return ret;
	
	/* TODO: Check for powered hub requirement */
	
	/* Reset and enumerate device on port 1 */
	ret = xhci_port_reset(&g_xhci, 1);
	if (ret < 0)
		goto cleanup;
	
	ret = xhci_enumerate_device(&g_xhci, 1);
	if (ret < 0)
		goto cleanup;
	
	/* Initialize MSC device */
	ret = usb_msc_init(&g_msc_dev, &g_xhci, g_xhci.current_slot);
	if (ret < 0)
		goto cleanup;
	
	/* Test if device is ready */
	ret = usb_msc_test_unit_ready(&g_msc_dev);
	if (ret < 0) {
		/* Device might need time to spin up */
		usleep(1000000); /* 1 second */
		ret = usb_msc_test_unit_ready(&g_msc_dev);
		if (ret < 0)
			goto cleanup;
	}
	
	/* Read device capacity */
	ret = usb_msc_read_capacity(&g_msc_dev);
	if (ret < 0)
		goto cleanup;
	
	/* Initialize cache */
	_cache_init();
	
	g_usb_storage_initialized = true;
	
	/* Start in read-only mode for safety */
	g_usb_storage_write_enabled = false;
	
	return 0;
	
cleanup:
	xhci_deinit(&g_xhci);
	return ret;
}

void emummc_storage_usb_end(void) {
	if (!g_usb_storage_initialized)
		return;
	
	_cache_invalidate();
	if (g_cache.data) {
		free(g_cache.data);
		g_cache.data = NULL;
	}
	
	xhci_deinit(&g_xhci);
	g_usb_storage_initialized = false;
}

int emummc_storage_usb_read(u32 sector, u32 num_sectors, void *buf) {
	int ret;
	
	if (!g_usb_storage_initialized)
		return -1;
	
	/* Try cache first */
	if (_cache_read(sector, num_sectors, buf) == 0)
		return 0;
	
	/* Read from device */
	ret = usb_msc_read_sectors(&g_msc_dev, sector, num_sectors, buf);
	if (ret < 0)
		return ret;
	
	/* Update cache with read data (for small reads) */
	if (num_sectors <= CACHE_SIZE_SECTORS) {
		_cache_update(sector, num_sectors, buf);
	}
	
	return 0;
}

int emummc_storage_usb_write(u32 sector, u32 num_sectors, void *buf) {
	if (!g_usb_storage_initialized)
		return -1;
	
	/* Check if writes are enabled */
	if (!g_usb_storage_write_enabled)
		return -1; /* Read-only mode */
	
	/* Invalidate cache on write */
	_cache_invalidate();
	
	return usb_msc_write_sectors(&g_msc_dev, sector, num_sectors, buf);
}

void emummc_storage_usb_enable_write(bool enable) {
	g_usb_storage_write_enabled = enable;
}

bool emummc_storage_usb_is_initialized(void) {
	return g_usb_storage_initialized;
}

u64 emummc_storage_usb_get_capacity(void) {
	if (!g_usb_storage_initialized)
		return 0;
	return g_msc_dev.num_sectors;
}
