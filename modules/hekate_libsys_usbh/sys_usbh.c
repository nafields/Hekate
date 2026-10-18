/*
 * USB Host driver module for hekate (libsys_usbh.bso)
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

/*
 * Wraps bdk/usb/usbh.c and bdk/usb/usbh_msc.c into a position independent
 * DRAM library.  The bdk services those drivers use are provided here as
 * shims that call back into the hekate binary that loaded the module, so
 * no driver state is duplicated.
 */

#include <stddef.h>

#include <module.h>
#include <soc/bpmp.h>
#include <soc/clock.h>
#include <soc/gpio.h>
#include <soc/hw_init.h>
#include <soc/pmc.h>
#include <soc/timer.h>
#include <soc/t210.h>
#include <usb/usbh.h>
#include <usb/usbh_msc.h>
#include <usb/usbh_module.h>
#include <utils/types.h>

static bdkParams_t   bdk;
static usbh_module_t *host;

/* ---------- bdk service shims ------------------------------------------- */

void usleep(u32 us) { host->usleep(us); }
void msleep(u32 ms) { host->msleep(ms); }
u32  get_tmr_us()   { return host->get_tmr_us(); }

void bpmp_mmu_maintenance(u32 op, bool force) { host->bpmp_mmu_maintenance(op, force); }
void bpmp_clk_rate_relaxed(bool enable)       { host->bpmp_clk_rate_relaxed(enable); }

int  pmc_enable_partition(pmc_power_rail_t part, u32 enable) { return host->pmc_enable_partition(part, enable); }

void gpio_write(u32 port, u32 pins, int high)            { host->gpio_write(port, pins, high); }
void gpio_direction_output(u32 port, u32 pins, int high) { host->gpio_direction_output(port, pins, high); }

u32  hw_get_chip_id() { return host->hw_get_chip_id(); }

void gfx_eputs(const char *s) { host->eputs(s); }

void *memcpy(void *dst, const void *src, size_t len)
{
	bdk->memcpy(dst, (void *)src, len);
	return dst;
}

void *memset(void *dst, int val, size_t len)
{
	bdk->memset(dst, val, len);
	return dst;
}

/* Mirrors bdk/soc/clock.c. Stateless register sequences. */
void clock_enable_pllu()
{
	// Configure PLLU.
	CLOCK(CLK_RST_CONTROLLER_PLLU_MISC) |= BIT(29); // Disable reference clock.
	u32 pllu_cfg = (CLOCK(CLK_RST_CONTROLLER_PLLU_BASE) & 0xFFE00000) | BIT(24) | (1 << 16) | (0x19 << 8) | 2;
	CLOCK(CLK_RST_CONTROLLER_PLLU_BASE) = pllu_cfg;
	CLOCK(CLK_RST_CONTROLLER_PLLU_BASE) = pllu_cfg | PLLCX_BASE_ENABLE; // Enable.

	// Wait for PLL to stabilize.
	u32 timeout = get_tmr_us() + 1300;
	while (!(CLOCK(CLK_RST_CONTROLLER_PLLU_BASE) & PLLCX_BASE_LOCK)) // PLL_LOCK.
		if (get_tmr_us() > timeout)
			break;
	usleep(10);

	// Enable PLLU USB/HSIC/ICUSB/48M.
	CLOCK(CLK_RST_CONTROLLER_PLLU_BASE) |= 0x2E00000;
}

void clock_enable_utmipll()
{
	// Set UTMIPLL dividers and config based on OSC and enable it to 960 MHz.
	CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG0) = (CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG0) & 0xFF0000FF) | (25 << 16) | (1 << 8); // 38.4Mhz * (25 / 1) = 960 MHz.
	CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) = (CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) & 0xFF00003F) | (24 << 18); // Set delay count for 38.4Mhz osc crystal.
	CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG1) = (CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG1) &  0x7FFA000) | (1 << 15) | 375;

	// Wait for UTMIPLL to stabilize.
	u32 retries = 10; // Wait 20us
	while (!(CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) & UTMIPLL_LOCK) && retries)
	{
		usleep(1);
		retries--;
	}
}

/* ---------- module entry point ------------------------------------------ */

void _usbh_module_init(usbh_module_t *mod, bdkParams_t params)
{
	bdk  = params;
	host = mod;

	mod->init           = usbh_init;
	mod->deinit         = usbh_deinit;
	mod->is_initialized = usbh_is_initialized;
	mod->is_ready       = usbh_is_ready;
	mod->get_ctxt       = usbh_get_ctxt;
	mod->msc_init       = usbh_msc_init;
	mod->msc_read       = usbh_msc_read;
	mod->msc_write      = usbh_msc_write;
	mod->msc_get        = usbh_msc_get;
	mod->magic          = USBH_MODULE_MAGIC;
}
