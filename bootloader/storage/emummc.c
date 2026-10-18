/*
 * Copyright (c) 2019-2022 CTCaer
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
#include <stdlib.h>

#include <bdk.h>

#include "emummc.h"
#include "usb_storage.h"
#include "../config.h"
#include <libs/fatfs/ff.h>

extern hekate_config h_cfg;
emummc_cfg_t emu_cfg = { 0 };

/*
 * emuMMC always lives on the SD card, since that is where Atmosphere reads
 * it from. While a USB drive is the main storage (volume 0), SD card files
 * are reached through volume 1.
 */
static char *_emummc_sd_path(char *dst, const char *path)
{
	strcpy(dst, usb_storage_sd_volume());
	strcat(dst, path);
	return dst;
}

void emummc_load_cfg()
{
	emu_cfg.enabled = 0;
	emu_cfg.path = NULL;
	emu_cfg.sector = 0;
	emu_cfg.id = 0;
	emu_cfg.file_based_part_size = 0;
	emu_cfg.active_part = 0;
	emu_cfg.fs_ver = 0;
	emu_cfg.usb = 0;
	emu_cfg.usb_atmosphere = 0;
	if (!emu_cfg.nintendo_path)
		emu_cfg.nintendo_path = (char *)malloc(0x200);
	if (!emu_cfg.emummc_file_based_path)
		emu_cfg.emummc_file_based_path = (char *)malloc(0x200);

	emu_cfg.nintendo_path[0] = 0;
	emu_cfg.emummc_file_based_path[0] = 0;

	char ini_path[32];
	LIST_INIT(ini_sections);
	if (ini_parse(&ini_sections, _emummc_sd_path(ini_path, "emuMMC/emummc.ini"), false))
	{
		LIST_FOREACH_ENTRY(ini_sec_t, ini_sec, &ini_sections, link)
		{
			if (ini_sec->type == INI_CHOICE)
			{
				if (strcmp(ini_sec->name, "emummc"))
					continue;

				LIST_FOREACH_ENTRY(ini_kv_t, kv, &ini_sec->kvs, link)
				{
					if (!strcmp("enabled",            kv->key))
						emu_cfg.enabled = atoi(kv->val);
					else if (!strcmp("sector",        kv->key))
						emu_cfg.sector  = strtol(kv->val, NULL, 16);
					else if (!strcmp("id",            kv->key))
						emu_cfg.id      = strtol(kv->val, NULL, 16);
					else if (!strcmp("path",          kv->key))
						emu_cfg.path   = kv->val;
					else if (!strcmp("nintendo_path", kv->key))
						strcpy(emu_cfg.nintendo_path, kv->val);
					else if (!strcmp("usb_enabled",    kv->key))
						emu_cfg.usb = atoi(kv->val);
					else if (!strcmp("usb_atmosphere", kv->key))
						emu_cfg.usb_atmosphere = atoi(kv->val);
				}
				break;
			}
		}
	}
}

bool emummc_set_path(char *path)
{
	FIL fp;
	bool found = false;

	emu_cfg.usb = 0;

	// raw_based: raw SD partition. usb_based: raw image on the USB drive.
	// Both markers hold the start sector.
	_emummc_sd_path(emu_cfg.emummc_file_based_path, path);
	strcat(emu_cfg.emummc_file_based_path, "/raw_based");
	bool raw = !f_open(&fp, emu_cfg.emummc_file_based_path, FA_READ);
	if (!raw)
	{
		_emummc_sd_path(emu_cfg.emummc_file_based_path, path);
		strcat(emu_cfg.emummc_file_based_path, "/usb_based");
		raw = !f_open(&fp, emu_cfg.emummc_file_based_path, FA_READ);
		emu_cfg.usb = raw;
	}

	if (raw)
	{
		if (!f_read(&fp, &emu_cfg.sector, 4, NULL))
			if (emu_cfg.sector)
				found = true;
		f_close(&fp);
	}
	else
	{
		_emummc_sd_path(emu_cfg.emummc_file_based_path, path);
		strcat(emu_cfg.emummc_file_based_path, "/file_based");

		if (!f_stat(emu_cfg.emummc_file_based_path, NULL))
		{
			emu_cfg.sector = 0;
			emu_cfg.path = path;

			found = true;
		}
	}

	if (found)
	{
		emu_cfg.enabled = 1;

		// Get ID from path.
		u32 id_from_path = 0;
		u32 path_size = strlen(path);
		if (path_size >= 4)
			memcpy(&id_from_path, path + path_size - 4, 4);
		emu_cfg.id = id_from_path;

		strcpy(emu_cfg.nintendo_path, path);
		strcat(emu_cfg.nintendo_path, "/Nintendo");
	}

	return found;
}

static int emummc_raw_get_part_off(int part_idx)
{
	switch (part_idx)
	{
	case 0:
		return 2;
	case 1:
		return 0;
	case 2:
		return 1;
	}
	return 2;
}

int emummc_storage_init_mmc()
{
	FILINFO fno;
	emu_cfg.active_part = 0;

	// Always init eMMC even when in emuMMC. eMMC is needed from the emuMMC driver anyway.
	if (!emmc_initialize(false))
		return 2;

	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		return 0;

	if (emu_cfg.usb)
	{
		// Raw image on the USB drive. BOOT0 + BOOT1 + GPP must fit on it.
		if (!emu_cfg.sector || !usb_storage_init(3000))
		{
			EPRINTF("Failed to init USB emuMMC drive.");
			goto out;
		}
		// The GPP may be smaller than the eMMC (resized source); only require
		// BOOT0/BOOT1 and the start of GPP to be on the drive.
		u32 usb_sectors = usb_storage_msc()->num_sectors;
		if (emu_cfg.sector + 2 * 0x2000 >= usb_sectors)
		{
			EPRINTF("USB emuMMC is outside the drive.");
			goto out;
		}
		return 0;
	}

	if (!sd_mount())
		goto out;

	if (!emu_cfg.sector)
	{
		_emummc_sd_path(emu_cfg.emummc_file_based_path, emu_cfg.path);
		strcat(emu_cfg.emummc_file_based_path, "/eMMC");

		if (f_stat(emu_cfg.emummc_file_based_path, &fno))
		{
			EPRINTF("Failed to open eMMC folder.");
			goto out;
		}
		f_chmod(emu_cfg.emummc_file_based_path, AM_ARC, AM_ARC);

		strcat(emu_cfg.emummc_file_based_path, "/00");
		if (f_stat(emu_cfg.emummc_file_based_path, &fno))
		{
			EPRINTF("Failed to open emuMMC rawnand.");
			goto out;
		}
		emu_cfg.file_based_part_size = fno.fsize >> 9;
	}

	return 0;

out:
	return 1;
}

int emummc_storage_end()
{
	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		emmc_end();
	else
		sd_end();

	return 1;
}

int emummc_storage_read(u32 sector, u32 num_sectors, void *buf)
{
	FIL fp;
	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		return sdmmc_storage_read(&emmc_storage, sector, num_sectors, buf);
	else if (emu_cfg.sector)
	{
		sector += emu_cfg.sector;
		sector += emummc_raw_get_part_off(emu_cfg.active_part) * 0x2000;
		if (emu_cfg.usb) // usb_storage returns 0 on success.
			return !usb_storage_read(sector, num_sectors, buf);
		return sdmmc_storage_read(&sd_storage, sector, num_sectors, buf);
	}
	else
	{
		if (!emu_cfg.active_part)
		{
			u32 file_part = sector / emu_cfg.file_based_part_size;
			sector = sector % emu_cfg.file_based_part_size;
			if (file_part >= 10)
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 2, 10);
			else
			{
				emu_cfg.emummc_file_based_path[strlen(emu_cfg.emummc_file_based_path) - 2] = '0';
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 1, 10);
			}
		}
		if (f_open(&fp, emu_cfg.emummc_file_based_path, FA_READ))
		{
			EPRINTF("Failed to open emuMMC image.");
			return 0;
		}
		f_lseek(&fp, (u64)sector << 9);
		if (f_read(&fp, buf, (u64)num_sectors << 9, NULL))
		{
			EPRINTF("Failed to read emuMMC image.");
			f_close(&fp);
			return 0;
		}

		f_close(&fp);
		return 1;
	}

	return 1;
}

int emummc_storage_write(u32 sector, u32 num_sectors, void *buf)
{
	FIL fp;
	if (!emu_cfg.enabled || h_cfg.emummc_force_disable)
		return sdmmc_storage_write(&emmc_storage, sector, num_sectors, buf);
	else if (emu_cfg.sector)
	{
		sector += emu_cfg.sector;
		sector += emummc_raw_get_part_off(emu_cfg.active_part) * 0x2000;
		if (emu_cfg.usb) // usb_storage returns 0 on success.
			return !usb_storage_write(sector, num_sectors, buf);
		return sdmmc_storage_write(&sd_storage, sector, num_sectors, buf);
	}
	else
	{
		if (!emu_cfg.active_part)
		{
			u32 file_part = sector / emu_cfg.file_based_part_size;
			sector = sector % emu_cfg.file_based_part_size;
			if (file_part >= 10)
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 2, 10);
			else
			{
				emu_cfg.emummc_file_based_path[strlen(emu_cfg.emummc_file_based_path) - 2] = '0';
				itoa(file_part, emu_cfg.emummc_file_based_path + strlen(emu_cfg.emummc_file_based_path) - 1, 10);
			}
		}

		if (f_open(&fp, emu_cfg.emummc_file_based_path, FA_WRITE))
			return 0;

		f_lseek(&fp, (u64)sector << 9);
		if (f_write(&fp, buf, (u64)num_sectors << 9, NULL))
		{
			f_close(&fp);
			return 0;
		}

		f_close(&fp);
		return 1;
	}
}

int emummc_storage_set_mmc_partition(u32 partition)
{
	emu_cfg.active_part = partition;
	emmc_set_partition(partition);

	if (!emu_cfg.enabled || h_cfg.emummc_force_disable || emu_cfg.sector)
		return 1;
	else
	{
		_emummc_sd_path(emu_cfg.emummc_file_based_path, emu_cfg.path);
		strcat(emu_cfg.emummc_file_based_path, "/eMMC");

		switch (partition)
		{
		case 0:
			strcat(emu_cfg.emummc_file_based_path, "/00");
			break;
		case 1:
			strcat(emu_cfg.emummc_file_based_path, "/BOOT0");
			break;
		case 2:
			strcat(emu_cfg.emummc_file_based_path, "/BOOT1");
			break;
		}

		return 1;
	}

	return 1;
}
