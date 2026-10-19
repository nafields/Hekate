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

#include <string.h>

#include <bdk.h>

#include <ianos/ianos.h>
#include <libs/fatfs/ff.h>
#include <usb/usbh_module.h>

#include "usb_storage.h"

/* The XHCI/MSC drivers: a DRAM library loaded from the SD card. */
static usbh_module_t usbh;
static bool usbh_loaded = false;
static bool usbh_sourcing_vbus = false; // Our 5V is on the connector.

/* The SD card as FatFS volume 1 while the USB drive holds volume 0. */
static FATFS sd_card_fs;
static bool  sd_card_fs_mounted = false;

static bool _usb_storage_load_module(void)
{
	if (usbh_loaded)
		return true;

	if (sd_is_usb_backend() || !sd_mount())
		return false;

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
	{
		EPRINTF("USB: " USBH_MODULE_PATH " missing!\nUpdate bootloader folder!");
		return false;
	}

	usbh_loaded = true;
	return true;
}

/* Read the XUSB firmware from the SD card into its resident DRAM buffer. */
static u32 _usb_storage_load_fw(void)
{
	FIL fp;
	u32 size = 0;

	if (f_open(&fp, USB_STORAGE_FW_PATH, FA_READ))
	{
		EPRINTF("USB: " USB_STORAGE_FW_PATH " missing!\n(Tegra210 XUSB firmware)");
		return 0;
	}

	size = f_size(&fp);
	if (size > USBH_FW_BUF_SZ || f_read(&fp, (void *)USBH_FW_BUF_ADDR, size, NULL))
	{
		EPRINTF("USB: Failed to read XUSB firmware.");
		size = 0;
	}
	f_close(&fp);

	return size;
}

bool usb_storage_is_ready(void)
{
	return usbh_loaded && usbh.is_ready() && usbh.msc_get()->ready;
}

/* VBUS already driven from outside (dock or charger)? Then the console is a
 * power sink and must not switch its own 5V onto the connector. */
static bool _usb_storage_vbus_present(void)
{
	u8 status = i2c_recv_byte(I2C_1, BQ24193_I2C_ADDR, BQ24193_Status);
	return (status & (BQ24193_STATUS_VBUS_MASK | BQ24193_STATUS_PG_MASK)) != 0;
}

bool usb_storage_init(u32 timeout_ms)
{
	if (usb_storage_is_ready())
		return true;

	// Driver module and controller firmware can only come from the SD card.
	if (!_usb_storage_load_module())
		return false;

	const void *fw = NULL;
	u32 fw_size = 0;
	if (!usbh.is_initialized())
	{
		fw_size = _usb_storage_load_fw();
		if (!fw_size)
			return false;
		fw = (const void *)USBH_FW_BUF_ADDR;
	}

	bool source_vbus = !_usb_storage_vbus_present();
	if (usbh.init(fw, fw_size, timeout_ms, source_vbus))
		return false;

	usbh_sourcing_vbus = source_vbus;
	return !usbh.msc_init();
}

bool usb_storage_wait(u32 timeout_ms)
{
	if (usb_storage_is_ready())
		return true;

	gfx_printf("%kConnect a USB drive or dock the console...%k\n(VOL- to skip)\n",
		TXT_CLR_CYAN_L, TXT_CLR_DEFAULT);

	for (u32 waited = 0; waited < timeout_ms; waited += 1000)
	{
		// Docked or charger plugged in while we were sourcing 5V: restart the
		// host without it before probing again.
		if (usbh_loaded && usbh.is_initialized() && usbh_sourcing_vbus && _usb_storage_vbus_present())
			usbh.deinit();

		if (usb_storage_init(1000))
			return true;
		if (btn_read_vol() & BTN_VOL_DOWN)
			break;
		// No driver module or firmware: nothing to wait for.
		if (!usbh_loaded || !usbh.is_initialized())
			break;
		gfx_puts(".");
	}
	gfx_puts("\n");

	return false;
}

usbh_ctxt_t *usb_storage_ctxt(void) { return usbh.get_ctxt(); }
usbh_msc_t  *usb_storage_msc(void)  { return usbh.msc_get(); }

int usb_storage_read(u32 sector, u32 count, void *buf)
{
	return usbh_loaded ? usbh.msc_read(sector, count, buf) : 1;
}

int usb_storage_write(u32 sector, u32 count, const void *buf)
{
	return usbh_loaded ? usbh.msc_write(sector, count, buf) : 1;
}

bool usb_storage_is_primary(void)
{
	return sd_is_usb_backend();
}

static void _usb_storage_unmount_sd_card(void)
{
	if (sd_card_fs_mounted)
		f_mount(NULL, "1:", 1);
	sd_card_fs_mounted = false;
}

bool usb_storage_set_primary(bool enable)
{
	if (enable == sd_is_usb_backend())
		return true;

	if (enable)
	{
		// The drive must already be up; do not wait for a hot plug here.
		if (!usb_storage_init(0))
			return false;

		sd_set_usb_backend(true);
		if (!sd_mount())
		{
			// No usable FAT volume on the drive. Back to the SD card.
			sd_set_usb_backend(false);
			sd_mount();
			return false;
		}
		return true;
	}

	_usb_storage_unmount_sd_card();
	sd_set_usb_backend(false);
	return sd_mount();
}

const char *usb_storage_sd_volume(void)
{
	if (!sd_is_usb_backend())
		return "";

	if (!sd_card_fs_mounted && f_mount(&sd_card_fs, "1:", 1) == FR_OK)
		sd_card_fs_mounted = true;

	return "1:";
}

void usb_storage_end(void)
{
	if (sd_is_usb_backend())
	{
		sd_unmount();
		_usb_storage_unmount_sd_card();
		sd_set_usb_backend(false);
	}

	if (usbh_loaded && usbh.is_initialized())
		usbh.deinit();
	usbh_sourcing_vbus = false;
}
