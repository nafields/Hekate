/*
 * USB Host Mass Storage Class driver (Bulk-Only Transport, host side)
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

#include <usb/usbh.h>
#include <usb/usbh_msc.h>
#include <usb/usbd.h>   /* USB_RES_* / USB_ERROR_* result codes. */

#include <gfx_utils.h>
#include <soc/timer.h>
#include <memory_map.h>

/* BOT signatures. */
#define CBW_SIGNATURE  0x43425355u  /* 'USBC' */
#define CSW_SIGNATURE  0x53425355u  /* 'USBS' */

/* CSW Status values. */
#define CSW_STAT_GOOD  0
#define CSW_STAT_FAIL  1
#define CSW_STAT_PHASE 2

/* SCSI opcodes needed for host-side MSC. */
#define SC_TEST_UNIT_READY  0x00
#define SC_REQUEST_SENSE    0x03
#define SC_INQUIRY          0x12
#define SC_START_STOP_UNIT  0x1B
#define SC_READ_CAPACITY10  0x25
#define SC_READ10           0x28
#define SC_WRITE10          0x2A

/* SCSI sense keys / additional sense codes we act on. */
#define SENSE_KEY_NOT_READY     0x02
#define ASC_LUN_NOT_READY       0x04
#define ASCQ_INIT_CMD_REQUIRED  0x02

/* Sectors per BOT command. Bounded by the staging buffers and the number
 * of chained TRBs a single transfer may use. */
#define MSC_CHUNK_BYTES     USBH_BULK_XFER_MAX
#define MSC_CHUNK_SECTORS   (MSC_CHUNK_BYTES / USBH_MSC_SECTOR_SZ)

/* Timeouts (ms). Data phases are generous for HDDs waking from standby. */
#define MSC_TIMEOUT_CBW     2000
#define MSC_TIMEOUT_DATA    10000
#define MSC_TIMEOUT_CSW     10000
#define MSC_READY_WAIT_MS   15000

/* Internal result: device returned CSW status FAIL (check sense). */
#define MSC_ERROR_CMD_FAILED 61

/*
 * DMA staging layout. Every transfer goes through DRAM buffers so callers
 * may pass IRAM/stack memory:
 *   USBH_BULK_OUT_BUF_ADDR + 0:     CBW (31 bytes)
 *   USBH_BULK_OUT_BUF_ADDR + 64:    CSW (13 bytes)
 *   USBH_BULK_OUT_BUF_ADDR + 4096:  OUT data
 *   USBH_BULK_IN_BUF_ADDR:          IN data
 */
#define MSC_CBW_ADDR       (USBH_BULK_OUT_BUF_ADDR)
#define MSC_CSW_ADDR       (USBH_BULK_OUT_BUF_ADDR + 64)
#define MSC_OUT_DATA_ADDR  (USBH_BULK_OUT_BUF_ADDR + SZ_4K)
#define MSC_IN_DATA_ADDR   (USBH_BULK_IN_BUF_ADDR)

/* CBW: Command Block Wrapper (31 bytes). */
typedef struct {
	u32 Signature;
	u32 Tag;
	u32 DataTransferLength;
	u8  Flags;     /* Bit 7: 0 = OUT, 1 = IN */
	u8  Lun;
	u8  Length;    /* CDB length */
	u8  CDB[16];
} __attribute__((packed)) usbh_cbw_t;

/* CSW: Command Status Wrapper (13 bytes). */
typedef struct {
	u32 Signature;
	u32 Tag;
	u32 Residue;
	u8  Status;
} __attribute__((packed)) usbh_csw_t;

static usbh_msc_t usbh_msc_dev;
static u32 msc_tag;

usbh_msc_t *usbh_msc_get(void) { return &usbh_msc_dev; }

/* Bulk-Only Mass Storage Reset (class request) + clear both pipes. */
static void _bot_reset_recovery(void)
{
	usbh_ctrl_xfer(0x21, USB_REQUEST_BULK_RESET, 0, usbh_get_ctxt()->iface, NULL, 0);
	msleep(10);
	usbh_bulk_clear_halt(true);
	usbh_bulk_clear_halt(false);
}

static int _recv_csw(usbh_csw_t *csw)
{
	u32 actual = 0;
	int res = usbh_bulk_in(csw, 13, &actual, MSC_TIMEOUT_CSW);
	if (res == USBH_ERROR_STALL)
	{
		/* BOT §6.7.2: clear the halt and retry the CSW once. */
		usbh_bulk_clear_halt(true);
		res = usbh_bulk_in(csw, 13, &actual, MSC_TIMEOUT_CSW);
	}
	if (res || actual != 13)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

/*
 * Send a CBW, run the optional data phase, then receive the CSW.
 * data_in: true = device to host. Returns 0, MSC_ERROR_CMD_FAILED when the
 * device reports the command failed, or a USB_ERROR_* code.
 */
static int _bot_transfer(const u8 *cdb, u8 cdb_len, void *buf, u32 data_len, bool data_in)
{
	usbh_cbw_t *cbw = (usbh_cbw_t *)MSC_CBW_ADDR;
	usbh_csw_t *csw = (usbh_csw_t *)MSC_CSW_ADDR;
	u32 actual = 0;
	int res;

	if (data_len > MSC_CHUNK_BYTES)
		return USB_ERROR_XFER_ERROR;

	memset(cbw, 0, sizeof(*cbw));
	cbw->Signature          = CBW_SIGNATURE;
	cbw->Tag                = ++msc_tag;
	cbw->DataTransferLength = data_len;
	cbw->Flags              = data_in ? 0x80 : 0x00;
	cbw->Lun                = 0;
	cbw->Length             = cdb_len;
	memcpy(cbw->CDB, cdb, cdb_len);

	/* Phase 1: CBW. */
	res = usbh_bulk_out(cbw, 31, &actual, MSC_TIMEOUT_CBW);
	if (res == USBH_ERROR_STALL)
	{
		_bot_reset_recovery();
		return USB_ERROR_XFER_ERROR;
	}
	if (res || actual != 31)
		return USB_ERROR_XFER_ERROR;

	/* Phase 2: data, staged through DRAM. A STALL here is the device's way
	 * of ending the data phase early; clear it and go on to the CSW. */
	u32 got = 0;
	if (data_len && buf)
	{
		if (data_in)
		{
			res = usbh_bulk_in((void *)MSC_IN_DATA_ADDR, data_len, &got, MSC_TIMEOUT_DATA);
			if (res == USBH_ERROR_STALL)
				usbh_bulk_clear_halt(true);
			else if (res)
				return res;
			if (got > data_len)
				got = data_len;
			memcpy(buf, (void *)MSC_IN_DATA_ADDR, got);
		}
		else
		{
			memcpy((void *)MSC_OUT_DATA_ADDR, buf, data_len);
			res = usbh_bulk_out((void *)MSC_OUT_DATA_ADDR, data_len, &got, MSC_TIMEOUT_DATA);
			if (res == USBH_ERROR_STALL)
				usbh_bulk_clear_halt(false);
			else if (res)
				return res;
		}
	}

	/* Phase 3: CSW. */
	res = _recv_csw(csw);
	if (res)
	{
		_bot_reset_recovery();
		return res;
	}
	if (csw->Signature != CSW_SIGNATURE || csw->Tag != msc_tag || csw->Status == CSW_STAT_PHASE)
	{
		_bot_reset_recovery();
		return USB_ERROR_XFER_ERROR;
	}
	if (csw->Status == CSW_STAT_FAIL)
		return MSC_ERROR_CMD_FAILED;
	/* A successful fixed-length command must deliver all of its data. */
	if (data_len && buf && data_in && got != data_len)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

/* REQUEST SENSE: clears pending Unit Attention and tells us why a command failed. */
static void _request_sense(void)
{
	u8 cdb[6]  = { SC_REQUEST_SENSE, 0, 0, 0, 18, 0 };
	u8 resp[18];
	memset(resp, 0, sizeof(resp));
	if (!_bot_transfer(cdb, 6, resp, 18, true))
	{
		usbh_msc_dev.sense_key  = resp[2] & 0xF;
		usbh_msc_dev.sense_asc  = resp[12];
		usbh_msc_dev.sense_ascq = resp[13];
	}
}

static int _start_unit(void)
{
	u8 cdb[6] = { SC_START_STOP_UNIT, 0, 0, 0, 1 /* START */, 0 };
	return _bot_transfer(cdb, 6, NULL, 0, false);
}

/* Poll TEST UNIT READY until the medium is ready (handles power-on Unit
 * Attention and drives that need an explicit START UNIT to spin up). */
static int _test_unit_ready(void)
{
	u8 cdb[6] = { SC_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
	for (u32 waited = 0; waited < MSC_READY_WAIT_MS; waited += 100)
	{
		int res = _bot_transfer(cdb, 6, NULL, 0, false);
		if (!res)
			return USB_RES_OK;
		if (res != MSC_ERROR_CMD_FAILED)
			return res;

		_request_sense();
		if (usbh_msc_dev.sense_key == SENSE_KEY_NOT_READY &&
			usbh_msc_dev.sense_asc == ASC_LUN_NOT_READY &&
			usbh_msc_dev.sense_ascq == ASCQ_INIT_CMD_REQUIRED)
			_start_unit();

		msleep(100);
	}
	return USB_ERROR_TIMEOUT;
}

static void _copy_trimmed(char *dst, const u8 *src, u32 len)
{
	u32 n = 0;
	for (u32 i = 0; i < len; i++)
		dst[n++] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
	while (n && dst[n - 1] == ' ')
		n--;
	dst[n] = 0;
}

/* INQUIRY: confirm a direct access block device and capture its identity. */
static int _inquiry(void)
{
	u8 cdb[6] = { SC_INQUIRY, 0, 0, 0, 36, 0 };
	u8 resp[36];
	memset(resp, 0, sizeof(resp));
	int res = _bot_transfer(cdb, 6, resp, 36, true);
	if (res)
		return res;

	/* Peripheral device type (bits [4:0] of byte 0): 0 = direct access. */
	if ((resp[0] & 0x1F) != 0)
		return USB_ERROR_XFER_ERROR;

	usbh_msc_dev.removable = (resp[1] & 0x80) != 0;
	_copy_trimmed(usbh_msc_dev.vendor,   resp + 8,  8);
	_copy_trimmed(usbh_msc_dev.product,  resp + 16, 16);
	_copy_trimmed(usbh_msc_dev.revision, resp + 32, 4);
	return USB_RES_OK;
}

/* READ CAPACITY(10). Only 512-byte sectors are supported (FatFS FF_MAX_SS). */
static int _read_capacity(void)
{
	u8 cdb[10] = { SC_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	u8 resp[8];
	int res = _bot_transfer(cdb, 10, resp, 8, true);
	if (res)
		return res;

	/* Response: 4 bytes last LBA + 4 bytes block length (big-endian). */
	u32 last_lba   = ((u32)resp[0] << 24) | ((u32)resp[1] << 16) | ((u32)resp[2] << 8) | resp[3];
	u32 block_size = ((u32)resp[4] << 24) | ((u32)resp[5] << 16) | ((u32)resp[6] << 8) | resp[7];

	if (block_size != USBH_MSC_SECTOR_SZ)
	{
		EPRINTF("USBH MSC: Only 512 byte sectors supported.");
		return USB_ERROR_XFER_ERROR;
	}

	/* 0xFFFFFFFF means > 2 TiB (READ CAPACITY(16) territory). FatFS here is
	 * 32-bit LBA anyway, so expose the first 2 TiB. */
	usbh_msc_dev.capacity_clamped = (last_lba == 0xFFFFFFFF);
	usbh_msc_dev.num_sectors = usbh_msc_dev.capacity_clamped ? 0xFFFFFFFF : last_lba + 1;
	return USB_RES_OK;
}

int usbh_msc_init(void)
{
	memset(&usbh_msc_dev, 0, sizeof(usbh_msc_dev));
	if (!usbh_is_ready())
		return USB_ERROR_INIT;

	int res = _test_unit_ready();
	if (res) { EPRINTF("USBH MSC: Device not ready.");   return res; }

	res = _inquiry();
	if (res) { EPRINTF("USBH MSC: Inquiry failed.");     return res; }

	res = _read_capacity();
	if (res) { EPRINTF("USBH MSC: Read Capacity failed."); return res; }

	usbh_msc_dev.ready = true;
	return USB_RES_OK;
}

/* READ(10)/WRITE(10): transfer count 512-byte sectors starting at sector. */
static int _msc_rw(u32 sector, u32 count, u8 *buf, bool write)
{
	if (!usbh_msc_dev.ready)
		return USB_ERROR_INIT;
	if (!count || sector + count < sector || sector + count > usbh_msc_dev.num_sectors)
		return USB_ERROR_XFER_ERROR;

	while (count)
	{
		u32 chunk = MIN(count, MSC_CHUNK_SECTORS);

		u8 cdb[10] = { 0 };
		cdb[0] = write ? SC_WRITE10 : SC_READ10;
		cdb[2] = (sector >> 24) & 0xFF;
		cdb[3] = (sector >> 16) & 0xFF;
		cdb[4] = (sector >>  8) & 0xFF;
		cdb[5] =  sector        & 0xFF;
		cdb[7] = (chunk >> 8) & 0xFF;
		cdb[8] =  chunk       & 0xFF;

		int res = USB_ERROR_XFER_ERROR;
		for (u32 retry = 0; retry < 3 && res; retry++)
		{
			res = _bot_transfer(cdb, 10, buf, chunk * USBH_MSC_SECTOR_SZ, !write);
			if (res == MSC_ERROR_CMD_FAILED)
				_request_sense();
			else if (res && !usbh_is_ready())
				break;
		}
		if (res)
			return res;

		sector += chunk;
		count  -= chunk;
		buf    += chunk * USBH_MSC_SECTOR_SZ;
	}
	return USB_RES_OK;
}

int usbh_msc_read(u32 sector, u32 count, void *buf)
{
	return _msc_rw(sector, count, (u8 *)buf, false);
}

int usbh_msc_write(u32 sector, u32 count, const void *buf)
{
	return _msc_rw(sector, count, (u8 *)buf, true);
}

u32 usbh_msc_get_sector_count(void)
{
	return usbh_msc_dev.num_sectors;
}
