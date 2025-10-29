/*
 * USB Mass Storage Class (MSC) Host Driver for Tegra X1
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

#ifndef _USB_MSC_HOST_H_
#define _USB_MSC_HOST_H_

#include <utils/types.h>
#include <usb/xhci.h>

/* USB Mass Storage Class defines */
#define MSC_CLASS               0x08
#define MSC_SUBCLASS_SCSI       0x06
#define MSC_PROTOCOL_BOT        0x50

/* Bulk-Only Transport (BOT) defines */
#define MSC_BOT_CBW_SIGNATURE   0x43425355  /* "USBC" */
#define MSC_BOT_CSW_SIGNATURE   0x53425355  /* "USBS" */

#define MSC_BOT_CBW_SIZE        31
#define MSC_BOT_CSW_SIZE        13

#define MSC_BOT_DIR_OUT         0x00
#define MSC_BOT_DIR_IN          0x80

/* CSW Status codes */
#define MSC_CSW_STATUS_PASSED   0x00
#define MSC_CSW_STATUS_FAILED   0x01
#define MSC_CSW_STATUS_PHASE_ERR 0x02

/* SCSI Command codes */
#define SCSI_CMD_TEST_UNIT_READY    0x00
#define SCSI_CMD_REQUEST_SENSE      0x03
#define SCSI_CMD_INQUIRY            0x12
#define SCSI_CMD_READ_CAPACITY_10   0x25
#define SCSI_CMD_READ_10            0x28
#define SCSI_CMD_WRITE_10           0x2A
#define SCSI_CMD_READ_CAPACITY_16   0x9E
#define SCSI_CMD_READ_12            0xA8
#define SCSI_CMD_WRITE_12           0xAA

/* SCSI sense keys */
#define SCSI_SENSE_NO_SENSE         0x00
#define SCSI_SENSE_NOT_READY        0x02
#define SCSI_SENSE_MEDIUM_ERROR     0x03
#define SCSI_SENSE_UNIT_ATTENTION   0x06

/* Command Block Wrapper (CBW) */
typedef struct _msc_cbw_t {
	u32 signature;
	u32 tag;
	u32 data_transfer_length;
	u8  flags;
	u8  lun;
	u8  cb_length;
	u8  cb[16];
} __attribute__((packed)) msc_cbw_t;

/* Command Status Wrapper (CSW) */
typedef struct _msc_csw_t {
	u32 signature;
	u32 tag;
	u32 data_residue;
	u8  status;
} __attribute__((packed)) msc_csw_t;

/* USB MSC Device Context */
typedef struct _usb_msc_device_t {
	xhci_controller_t *xhci;
	u8 slot_id;
	u8 ep_in;
	u8 ep_out;
	u8 lun;
	u32 tag_counter;
	
	/* Device information */
	u64 num_sectors;
	u32 sector_size;
	bool ready;
	bool write_protected;
} usb_msc_device_t;

/* Function prototypes */
int usb_msc_init(usb_msc_device_t *dev, xhci_controller_t *xhci, u8 slot_id);
int usb_msc_test_unit_ready(usb_msc_device_t *dev);
int usb_msc_inquiry(usb_msc_device_t *dev, void *data, u32 len);
int usb_msc_read_capacity(usb_msc_device_t *dev);
int usb_msc_read_sectors(usb_msc_device_t *dev, u32 lba, u32 count, void *buffer);
int usb_msc_write_sectors(usb_msc_device_t *dev, u32 lba, u32 count, void *buffer);
int usb_msc_request_sense(usb_msc_device_t *dev, void *data, u32 len);

#endif /* _USB_MSC_HOST_H_ */
