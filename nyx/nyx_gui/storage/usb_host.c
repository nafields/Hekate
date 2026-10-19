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

#include <string.h>

#include <bdk.h>

#include <ianos/ianos.h>
#include <libs/fatfs/ff.h>
#include <usb/usbh_module.h>

#include "usb_host.h"

/* The XHCI/MSC drivers: the same DRAM library hekate uses. */
static usbh_module_t usbh;
static bool usbh_loaded = false;
static u32  usbh_fw_size = 0; // XUSB firmware resident in USBH_FW_BUF.

static bool _usb_host_load_module(void)
{
	if (usbh_loaded)
		return true;

	memset(&usbh, 0, sizeof(usbh));
	usbh.usleep                = usleep;
	usbh.msleep                = msleep;
	usbh.get_tmr_us            = get_tmr_us;
	usbh.bpmp_mmu_maintenance  = bpmp_mmu_maintenance;
	usbh.bpmp_clk_rate_relaxed = bpmp_clk_rate_relaxed;
	usbh.pmc_enable_partition  = pmc_enable_partition;
	usbh.gpio_write            = gpio_write;
	usbh.gpio_direction_output = gpio_direction_output;
	usbh.hw_get_chip_id        = hw_get_chip_id;
	usbh.i2c_send_buf_big      = i2c_send_buf_big;
	usbh.i2c_recv_buf_big      = i2c_recv_buf_big;
	usbh.eputs                 = gfx_eputs;

	if (!ianos_loader(USBH_MODULE_PATH, DRAM_LIB, &usbh) || usbh.magic != USBH_MODULE_MAGIC)
		return false;

	usbh_loaded = true;
	return true;
}

/* Read the XUSB firmware from the SD card into its resident DRAM buffer. */
static u32 _usb_host_load_fw(void)
{
	FIL fp;
	if (f_open(&fp, USB_HOST_FW_PATH, FA_READ))
		return 0;

	u32 size = f_size(&fp);
	if (size > USBH_FW_BUF_SZ || f_read(&fp, (void *)USBH_FW_BUF_ADDR, size, NULL))
		size = 0;
	f_close(&fp);

	return size;
}

bool usb_host_is_ready(void)
{
	return usbh_loaded && usbh.is_ready() && usbh.msc_get()->ready;
}

bool usb_host_init(u32 timeout_ms, const char **error_out)
{
	const char *error = NULL;

	if (usb_host_is_ready())
		return true;

	if (!_usb_host_load_module())
	{
		error = "bootloader/sys/libsys_usbh.bso is missing!";
		goto out;
	}

	// The firmware stays resident: the module reloads it on a host restart.
	if (!usbh_fw_size)
		usbh_fw_size = _usb_host_load_fw();
	if (!usbh_fw_size)
	{
		error = "bootloader/sys/xusbfw.bin is missing!";
		goto out;
	}

	// VBUS policy, dock handshake and host restarts live in the module.
	int res = usbh.probe((const void *)USBH_FW_BUF_ADDR, usbh_fw_size, timeout_ms);
	if (res == USB_ERROR_TIMEOUT)
		error = "No USB drive connected.";
	else if (res)
		error = "USB host init failed.";
	else if (usbh.msc_init())
		error = "USB drive is not usable.";

out:
	if (error_out)
		*error_out = error;
	return !error;
}

usbh_ctxt_t *usb_host_ctxt(void) { return usbh.get_ctxt(); }
usbh_msc_t  *usb_host_msc(void)  { return usbh.msc_get(); }

int usb_host_read(u32 sector, u32 count, void *buf)
{
	return usbh_loaded ? usbh.msc_read(sector, count, buf) : 1;
}

int usb_host_write(u32 sector, u32 count, const void *buf)
{
	return usbh_loaded ? usbh.msc_write(sector, count, buf) : 1;
}

void usb_host_end(void)
{
	if (usbh_loaded)
	{
		if (usbh.is_initialized())
			usbh.deinit();
		usbh.probe_reset();
	}
}
