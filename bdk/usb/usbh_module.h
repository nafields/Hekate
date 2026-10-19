/*
 * USB Host driver module interface (libsys_usbh.bso)
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

#ifndef _USBH_MODULE_H_
#define _USBH_MODULE_H_

#include <utils/types.h>
#include <soc/pmc.h>
#include <usb/usbh.h>
#include <usb/usbh_msc.h>

/*
 * The XHCI host and mass storage drivers live in a DRAM library loaded with
 * ianos (like minerva), so the size limited IPL only carries the glue.
 * The host fills in the services below, loads the module and the module's
 * entry point fills in the driver entry points.
 */
#define USBH_MODULE_PATH  "bootloader/sys/libsys_usbh.bso"
#define USBH_MODULE_MAGIC 0x48425356 // "USBH" + ABI revision 1.

typedef struct _usbh_module_t
{
	// Services provided by hekate (stateful bdk functions and output).
	void (*usleep)(u32 us);
	void (*msleep)(u32 ms);
	u32  (*get_tmr_us)(void);
	void (*bpmp_mmu_maintenance)(u32 op, bool force);
	void (*bpmp_clk_rate_relaxed)(bool enable);
	int  (*pmc_enable_partition)(pmc_power_rail_t part, u32 enable);
	void (*gpio_write)(u32 port, u32 pins, int high);
	void (*gpio_direction_output)(u32 port, u32 pins, int high);
	u32  (*hw_get_chip_id)(void);
	void (*eputs)(const char *s);

	// Driver entry points provided by the module.
	u32  magic;
	int  (*init)(const void *fw, u32 fw_size, u32 timeout_ms, bool source_vbus);
	void (*deinit)(void);
	bool (*is_initialized)(void);
	bool (*is_ready)(void);
	usbh_ctxt_t *(*get_ctxt)(void);
	int  (*msc_init)(void);
	int  (*msc_read)(u32 sector, u32 count, void *buf);
	int  (*msc_write)(u32 sector, u32 count, const void *buf);
	usbh_msc_t *(*msc_get)(void);
} usbh_module_t;

#endif /* _USBH_MODULE_H_ */
