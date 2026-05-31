/*
 * USB Host Mass Storage Class driver (BOT protocol, host side)
 *
 * Copyright (c) 2024 Hekate contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <string.h>

#include <usb/usbh.h>
#include <usb/usbh_msc.h>

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

/* SCSI opcodes needed for host-side read-only MSC. */
#define SC_TEST_UNIT_READY  0x00
#define SC_INQUIRY          0x12
#define SC_READ_CAPACITY10  0x25
#define SC_READ10           0x28

/* CBW: Command Block Wrapper (31 bytes). */
typedef struct {
	u32 Signature;
	u32 Tag;
	u32 DataTransferLength;
	u8  Flags;     /* Bit 7: 0=OUT, 1=IN */
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

usbh_msc_t *usbh_msc_get(void) { return &usbh_msc_dev; }

static u32 _next_tag(void)
{
	static u32 tag = 0;
	return ++tag;
}

/* Send a CBW, optional data phase, then receive CSW.
 * data_dir: 0=OUT (host→device), 1=IN (device→host).
 * For IN transfers, buf is filled with up to data_len bytes received.
 * Returns 0 on success. */
/*
 * Buffer layout:
 *   USBH_BULK_OUT_BUF_ADDR: CBW (31 bytes)
 *   USBH_BULK_IN_BUF_ADDR:  data RX area (up to USBH_BULK_BUF_SZ)
 *   CSW is received into a small local aligned buffer.
 */
static int _bot_transfer(const u8 *cdb, u8 cdb_len, void *buf, u32 data_len,
                         int data_dir)
{
	usbh_cbw_t *cbw = (usbh_cbw_t *)USBH_BULK_OUT_BUF_ADDR;
	/* CSW sits in the first 16 bytes of the OUT buffer after CBW is sent. */
	usbh_csw_t *csw = (usbh_csw_t *)(USBH_BULK_OUT_BUF_ADDR + 64);
	u32 tag;

	memset(cbw, 0, sizeof(*cbw));
	cbw->Signature          = CBW_SIGNATURE;
	cbw->Tag                = tag = _next_tag();
	cbw->DataTransferLength = data_len;
	cbw->Flags              = data_dir ? 0x80 : 0x00;
	cbw->Lun                = 0;
	cbw->Length             = cdb_len;
	memcpy(cbw->CDB, cdb, cdb_len);

	/* Phase 1: send CBW. */
	u32 actual = 0;
	int res = usbh_bulk_out(cbw, 31, &actual);
	if (res || actual != 31)
		return USB_ERROR_XFER_ERROR;

	/* Phase 2: data (optional). Data IN is staged through BULK_IN buffer. */
	if (data_len && buf) {
		if (data_dir) {
			res = usbh_bulk_in((void *)USBH_BULK_IN_BUF_ADDR, data_len, &actual);
			if (res)
				return res;
			memcpy(buf, (void *)USBH_BULK_IN_BUF_ADDR, actual);
		} else {
			res = usbh_bulk_out(buf, data_len, &actual);
			if (res)
				return res;
		}
	}

	/* Phase 3: receive CSW. */
	res = usbh_bulk_in(csw, 13, &actual);
	if (res || actual != 13)
		return USB_ERROR_XFER_ERROR;
	if (csw->Signature != CSW_SIGNATURE || csw->Tag != tag)
		return USB_ERROR_XFER_ERROR;
	if (csw->Status != CSW_STAT_GOOD)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

/* Poll TEST UNIT READY until device is ready or timeout. */
static int _test_unit_ready(void)
{
	u8 cdb[6] = { SC_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
	for (int i = 0; i < 30; i++) {
		if (!_bot_transfer(cdb, 6, NULL, 0, 0))
			return USB_RES_OK;
		usleep(100000);  /* 100ms between retries */
	}
	return USB_ERROR_TIMEOUT;
}

/* INQUIRY to confirm device is a valid direct-access block device. */
static int _inquiry(void)
{
	u8 cdb[6]     = { SC_INQUIRY, 0, 0, 0, 36, 0 };
	u8 resp[36];
	int res = _bot_transfer(cdb, 6, resp, 36, 1);
	if (res)
		return res;
	/* Peripheral device type (bits[4:0] of byte 0) must be 0 (direct access). */
	if ((resp[0] & 0x1F) != 0)
		return USB_ERROR_XFER_ERROR;
	return USB_RES_OK;
}

/* READ CAPACITY(10) — fills sector_count and sector_size in the MSC context. */
static int _read_capacity(void)
{
	u8  cdb[10] = { SC_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	u8  resp[8];
	int res = _bot_transfer(cdb, 10, resp, 8, 1);
	if (res)
		return res;

	/* Response: 4 bytes LBA + 4 bytes block length (big-endian). */
	u32 last_lba   = ((u32)resp[0] << 24) | ((u32)resp[1] << 16) |
	                 ((u32)resp[2] <<  8) |  (u32)resp[3];
	u32 block_size = ((u32)resp[4] << 24) | ((u32)resp[5] << 16) |
	                 ((u32)resp[6] <<  8) |  (u32)resp[7];

	if (block_size != 512)
		return USB_ERROR_XFER_ERROR;  /* Only 512-byte sectors supported. */

	usbh_msc_dev.num_sectors = last_lba + 1;
	return USB_RES_OK;
}

int usbh_msc_init(void)
{
	memset(&usbh_msc_dev, 0, sizeof(usbh_msc_dev));

	int res = _test_unit_ready();
	if (res) { EPRINTF("USBH MSC: Device not ready."); return res; }

	res = _inquiry();
	if (res) { EPRINTF("USBH MSC: Inquiry failed."); return res; }

	res = _read_capacity();
	if (res) { EPRINTF("USBH MSC: Read Capacity failed."); return res; }

	usbh_msc_dev.ready = true;
	return USB_RES_OK;
}

/* READ(10): read count 512-byte sectors starting at sector. */
int usbh_msc_read(u32 sector, u32 count, void *buf)
{
	if (!usbh_msc_dev.ready)
		return USB_ERROR_INIT;

	/* Process in chunks that fit in USBH_BULK_BUF_SZ (1MB = 2048 sectors). */
	u8 *dst = (u8 *)buf;
	while (count) {
		u32 chunk = count;
		if (chunk > (USBH_BULK_BUF_SZ / 512))
			chunk = USBH_BULK_BUF_SZ / 512;

		u8 cdb[10];
		cdb[0] = SC_READ10;
		cdb[1] = 0;
		cdb[2] = (sector >> 24) & 0xFF;
		cdb[3] = (sector >> 16) & 0xFF;
		cdb[4] = (sector >>  8) & 0xFF;
		cdb[5] =  sector        & 0xFF;
		cdb[6] = 0;
		cdb[7] = (chunk >> 8) & 0xFF;
		cdb[8] =  chunk       & 0xFF;
		cdb[9] = 0;

		int res = _bot_transfer(cdb, 10, dst, chunk * 512, 1);
		if (res)
			return res;

		sector += chunk;
		count  -= chunk;
		dst    += chunk * 512;
	}
	return USB_RES_OK;
}

u32 usbh_msc_get_sector_count(void)
{
	return usbh_msc_dev.num_sectors;
}
