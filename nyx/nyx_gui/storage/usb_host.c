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

/* VBUS already driven from outside (dock or charger)? Then the console is a
 * power sink and must not switch its own 5V onto the connector. */
static bool _usb_host_vbus_present(void)
{
	u8 status = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, BQ24193_Status);
	return (status & (BQ24193_STATUS_VBUS_MASK | BQ24193_STATUS_PG_MASK)) != 0;
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

	const void *fw = NULL;
	u32 fw_size = 0;
	if (!usbh.is_initialized())
	{
		fw_size = _usb_host_load_fw();
		if (!fw_size)
		{
			error = "bootloader/sys/xusbfw.bin is missing!";
			goto out;
		}
		fw = (const void *)USBH_FW_BUF_ADDR;
	}

	int res = usbh.init(fw, fw_size, timeout_ms, !_usb_host_vbus_present());
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
	if (usbh_loaded && usbh.is_initialized())
		usbh.deinit();
}
