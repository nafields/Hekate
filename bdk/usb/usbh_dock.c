/*
 * Nintendo dock USB hub enable through the BM92T36 USB-PD controller.
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
 * In the dock the console is a power sink and, by Type-C default, the USB
 * device (UFP). The dock's hub only shows up after the console has swapped
 * to the host data role (DFP) and told the dock to switch the hub on with a
 * Nintendo vendor defined message. This mirrors the sequence of the L4T
 * bm92txx driver (CTCaer/switch-l4t-kernel-4.9, drivers/misc/bm92txx.c):
 * RDO, PS_RDY, DR_SWAP, Discover Identity, Enter Nintendo Alt Mode, device
 * state query, LED on, HUB_CONTROL enable. DisplayPort is left alone.
 *
 * Polled: the controller's alert register is read instead of waiting for
 * its interrupt line.
 */

#include <string.h>

#include <usb/usbh.h>
#include <gfx_utils.h>
#include <power/bm92t36.h>
#include <power/bq24193.h>
#include <soc/i2c.h>
#include <soc/timer.h>
#include <utils/types.h>

/* Registers. */
#define ALERT_STATUS_REG    0x02
#define STATUS1_REG         0x03
#define STATUS2_REG         0x04
#define COMMAND_REG         0x05
#define READ_PDOS_SRC_REG   0x08
#define CURRENT_RDO_REG     0x2B
#define SET_RDO_REG         0x30
#define INCOMING_VDM_REG    0x50
#define OUTGOING_VDM_REG    0x60

/* ALERT_STATUS_REG. */
#define ALERT_CMD_DONE      BIT(2)
#define ALERT_VDM_RECEIVED  BIT(11)
#define ALERT_CONTRACT      BIT(12)

/* STATUS1_REG. */
#define STATUS1_FAULT_MASK  (3 << 0)
#define STATUS1_LASTCMD_SHIFT 4
#define STATUS1_LASTCMD_MASK  (7 << STATUS1_LASTCMD_SHIFT)
#define STATUS1_INSERT      BIT(7)
#define STATUS1_DR_SHIFT    8
#define STATUS1_DR_MASK     (3 << STATUS1_DR_SHIFT)
#define STATUS1_SRC_MODE    BIT(12)
#define  DATA_ROLE_UFP      1
#define  DATA_ROLE_DFP      2
#define  LASTCMD_COMPLETE   0

/* STATUS2_REG. */
#define STATUS2_OTG_INSERT  BIT(13)

/* Commands. */
#define PS_RDY_CMD          0x0505
#define SEND_RDO_CMD        0x0707
#define SEND_VDM_CMD        0x1111
#define ACCEPT_VDM_CMD      0x1616
#define DR_SWAP_CMD         0x1818

/* VDM header bytes. */
#define VDM_CMD_DISC_ID     0x01
#define VDM_CMD_ENTER_MODE  0x04
#define VDM_ACK             0x40
#define VDM_STRUCTURED      0x80

/* Nintendo dock. */
#define VID_NINTENDO        0x057E
#define PID_NIN_DOCK        0x2003
#define VDM_ND_REQST        0x00
#define VDM_ND_READ         0
#define VDM_ND_WRITE        1
#define VDM_ND_HOST         1
#define VDM_ND_DOCK         2
#define VDM_ND_BUSY         1
#define VDM_NCMD_LED_CONTROL  0x01
#define VDM_NCMD_DEVICE_STATE 0x16
#define VDM_NCMD_HUB_CONTROL  0x20

/* Dock source PDOs: 5V 0.5A for identification, then 15V. */
#define DOCK_ID_VOLTAGE_MV     5000
#define DOCK_ID_CURRENT_MA     500
#define DOCK_INPUT_VOLTAGE_MV  15000

/* Timeouts (ms). */
#define DOCK_WAIT_CMD_MS   1000
#define DOCK_WAIT_VDM_MS   2000
#define DOCK_WAIT_CONTR_MS 3000

static const u8 vdm_discover_id[6] =
	{ OUTGOING_VDM_REG, 4, VDM_CMD_DISC_ID, VDM_STRUCTURED, 0x00, 0xFF };
static const u8 vdm_enter_nin_alt_mode[6] =
	{ OUTGOING_VDM_REG, 4, VDM_CMD_ENTER_MODE, VDM_STRUCTURED | 1, 0x7E, 0x05 };
static const u8 vdm_query_device[10] =
	{ OUTGOING_VDM_REG, 8, VDM_ND_REQST, 0x00, 0x7E, 0x05, VDM_ND_READ, VDM_ND_HOST, VDM_NCMD_DEVICE_STATE, 0x00 };
static const u8 vdm_hub_enable[10] =
	{ OUTGOING_VDM_REG, 8, VDM_ND_REQST, 0x00, 0x7E, 0x05, VDM_ND_WRITE, VDM_ND_HOST, VDM_NCMD_HUB_CONTROL, 0x00 };
/* LED: fade 64, time off 0, time on 0, duty 128 (steady green, as L4T sets it). */
static const u8 vdm_led_on[14] =
	{ OUTGOING_VDM_REG, 12, VDM_ND_REQST, 0x00, 0x7E, 0x05, VDM_ND_WRITE, VDM_ND_HOST, VDM_NCMD_LED_CONTROL, 0x00,
	  64, 0, 0, 128 };

static bool _rd(u8 reg, void *buf, u32 len)
{
	memset(buf, 0, len);
	return i2c_recv_buf_big(buf, len, I2C_1, BM92T36_I2C_ADDR, reg) != 0;
}

static u16 _rd16(u8 reg)
{
	u8 b[2];
	_rd(reg, b, 2);
	return b[0] | (b[1] << 8);
}

static bool _wr(const u8 *buf, u32 len)
{
	u8 tmp[32];
	if (len > sizeof(tmp))
		return false;
	memcpy(tmp, buf, len);
	return i2c_send_buf_big(I2C_1, BM92T36_I2C_ADDR, tmp, len) != 0;
}

static bool _cmd(u16 cmd)
{
	u8 b[3] = { COMMAND_REG, cmd & 0xFF, cmd >> 8 };
	return _wr(b, 3);
}

/* Poll the alert register (clear on read) until one of mask is set. */
static u16 _wait_alert(u16 mask, u32 timeout_ms)
{
	u16 acc = 0;
	for (u32 t = 0; t < timeout_ms; t += 5)
	{
		acc |= _rd16(ALERT_STATUS_REG);
		if (acc & mask)
			return acc;
		msleep(5);
	}
	return acc;
}

/* Send a VDM, wait for the reply, accept it and read it into reply[29]. */
static bool _vdm(const u8 *msg, u32 len, u8 *reply)
{
	if (!_wr(msg, len) || !_cmd(SEND_VDM_CMD))
		return false;
	if (!(_wait_alert(ALERT_VDM_RECEIVED, DOCK_WAIT_VDM_MS) & ALERT_VDM_RECEIVED))
		return false;
	if (!_cmd(ACCEPT_VDM_CMD))
		return false;
	if (!(_wait_alert(ALERT_CMD_DONE, DOCK_WAIT_CMD_MS) & ALERT_CMD_DONE))
		return false;
	return _rd(INCOMING_VDM_REG, reply, 29);
}

static u32 _le32(const u8 *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static int _dock_enable(void);

static bool dock_done = false;    /* Handshake attempted this session.  */
static bool sourcing_vbus = false; /* Our 5V is on the connector.        */

/* VBUS already driven from outside (dock or charger)? Then the console is a
 * power sink and must not switch its own 5V onto the connector. */
bool usbh_vbus_present(void)
{
	u8 status = 0;
	i2c_recv_buf_big(&status, 1, I2C_1, BQ24193_I2C_ADDR, BQ24193_Status);
	return (status & (BQ24193_STATUS_VBUS_MASK | BQ24193_STATUS_PG_MASK)) != 0;
}

int usbh_probe(const void *fw, u32 fw_size, u32 timeout_ms)
{
	bool external = usbh_vbus_present();

	/* Docked or charger plugged in while we were sourcing 5V: restart the
	 * host without it before probing again. */
	if (usbh_is_initialized() && sourcing_vbus && external)
		usbh_deinit();

	/* Docked: take the host data role and switch the dock's hub on first. */
	if (external && !dock_done)
	{
		dock_done = true;
		usbh_dock_enable();
	}

	int res = usbh_init(fw, fw_size, timeout_ms, !external);
	if (!res)
		sourcing_vbus = !external;
	return res;
}

void usbh_probe_reset(void)
{
	dock_done = false;
	sourcing_vbus = false;
}

int usbh_dock_enable(void)
{
	int res = _dock_enable();

	/* Reported from here to keep the hekate binary small. */
	if (res == USBH_DOCK_OK)
		gfx_eputs("Dock: USB hub enabled.\n");
	else if (res == USBH_DOCK_OLD_FW)
		gfx_eputs("Dock: old firmware, hub may stay off.\n");
	else if (res == USBH_DOCK_ERROR)
		gfx_eputs("Dock: hub enable failed.\n");

	return res;
}

static int _dock_enable(void)
{
	u8  vdm[29];
	u8  pdos[29];
	u16 st1 = _rd16(STATUS1_REG);
	u16 st2 = _rd16(STATUS2_REG);

	/* Nothing plugged, or an OTG adapter: the console already is the host. */
	if (!(st1 & STATUS1_INSERT))
		return USBH_DOCK_NONE;
	if ((st1 & STATUS1_SRC_MODE) || (st2 & STATUS2_OTG_INSERT))
		return USBH_DOCK_NONE;
	if (st1 & STATUS1_FAULT_MASK)
		return USBH_DOCK_ERROR;

	(void)_rd16(ALERT_STATUS_REG); /* Drop stale alerts. */

	/* Identify the Nintendo dock by its source PDOs: 5V 0.5A, then 15V. */
	bool nin_pdos = false;
	u32  dock_amp = 0;
	if (_rd(READ_PDOS_SRC_REG, pdos, 29) && pdos[0] / 4 == 2)
	{
		u32 pdo0 = _le32(pdos + 1), pdo1 = _le32(pdos + 5);
		u32 v0 = ((pdo0 >> 10) & 0x3FF) * 50, a0 = (pdo0 & 0x3FF) * 10;
		u32 v1 = ((pdo1 >> 10) & 0x3FF) * 50;
		dock_amp = pdo1 & 0x3FF;
		nin_pdos = (pdo1 >> 30) == 0 && v0 == DOCK_ID_VOLTAGE_MV && a0 == DOCK_ID_CURRENT_MA &&
		           v1 == DOCK_INPUT_VOLTAGE_MV;
	}

	/* Dock: make sure the 15V contract is in place (object 2). */
	if (nin_pdos)
	{
		u8 rdo[5];
		_rd(CURRENT_RDO_REG, rdo, 5);
		if ((_le32(rdo + 1) >> 28) != 2)
		{
			u32 r = dock_amp | (dock_amp << 10) | BIT(25) /* USB comms */ | (2u << 28);
			u8 msg[6] = { SET_RDO_REG, 4, r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF, r >> 24 };
			if (!_wr(msg, 6) || !_cmd(SEND_RDO_CMD))
				return USBH_DOCK_ERROR;
			if (!(_wait_alert(ALERT_CONTRACT, DOCK_WAIT_CONTR_MS) & ALERT_CONTRACT))
				return USBH_DOCK_ERROR;
		}
	}

	/* Power supply ready, then take the host data role if we do not have it. */
	if (!_cmd(PS_RDY_CMD) || !(_wait_alert(ALERT_CMD_DONE, DOCK_WAIT_CMD_MS) & ALERT_CMD_DONE))
		return USBH_DOCK_ERROR;

	st1 = _rd16(STATUS1_REG);
	if (((st1 & STATUS1_DR_MASK) >> STATUS1_DR_SHIFT) == DATA_ROLE_UFP)
	{
		if (!_cmd(DR_SWAP_CMD) || !(_wait_alert(ALERT_CMD_DONE, 2000) & ALERT_CMD_DONE))
			return USBH_DOCK_ERROR;
		st1 = _rd16(STATUS1_REG);
		if (((st1 & STATUS1_LASTCMD_MASK) >> STATUS1_LASTCMD_SHIFT) != LASTCMD_COMPLETE)
			return USBH_DOCK_ERROR;
	}
	if (((st1 & STATUS1_DR_MASK) >> STATUS1_DR_SHIFT) != DATA_ROLE_DFP)
		return USBH_DOCK_ERROR;

	/* Who is on the other end? Third party docks are done at this point. */
	if (!_vdm(vdm_discover_id, sizeof(vdm_discover_id), vdm))
		return USBH_DOCK_GENERIC;
	u16 vid = vdm[5] | (vdm[6] << 8);
	u16 pid = vdm[11] | (vdm[12] << 8);
	if (vid != VID_NINTENDO || pid != PID_NIN_DOCK)
		return USBH_DOCK_GENERIC;

	/* Nintendo dock: enter its alt mode. */
	if (!_vdm(vdm_enter_nin_alt_mode, sizeof(vdm_enter_nin_alt_mode), vdm) ||
		vdm[1] != (VDM_ACK | VDM_CMD_ENTER_MODE) || vdm[2] != (VDM_STRUCTURED | 1) ||
		vdm[3] != 0x7E || vdm[4] != 0x05)
		return USBH_DOCK_ERROR;

	/* Device state (informational; bit 1 of byte 11 set means a dock
	 * firmware without hub control). */
	bool old_fw = false;
	if (_vdm(vdm_query_device, sizeof(vdm_query_device), vdm) &&
		vdm[6] == VDM_ND_DOCK && vdm[7] == VDM_NCMD_DEVICE_STATE + 1)
		old_fw = (vdm[11] & 0x02) != 0;

	/* Green LED on, then give the hub time to power up. */
	_vdm(vdm_led_on, sizeof(vdm_led_on), vdm);
	msleep(500);

	/* Switch the hub on; the dock answers busy while it is starting. */
	for (u32 retry = 0; retry < 10; retry++)
	{
		if (!_vdm(vdm_hub_enable, sizeof(vdm_hub_enable), vdm))
			return USBH_DOCK_ERROR;
		if (vdm[6] == VDM_ND_DOCK && vdm[7] == VDM_NCMD_HUB_CONTROL + 1 && (vdm[5] & VDM_ND_BUSY))
		{
			msleep(250);
			continue;
		}
		break;
	}

	return old_fw ? USBH_DOCK_OLD_FW : USBH_DOCK_OK;
}
