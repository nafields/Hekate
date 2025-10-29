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

#include <string.h>
#include <usb/usb_msc_host.h>
#include <usb/xhci.h>

/* Helper function to send CBW and receive CSW */
static int _msc_bot_transaction(usb_msc_device_t *dev, u8 *cb, u8 cb_len,
                                void *data, u32 data_len, bool data_in) {
	int ret;
	msc_cbw_t cbw;
	msc_csw_t csw;
	
	/* Prepare CBW */
	memset(&cbw, 0, sizeof(cbw));
	cbw.signature = MSC_BOT_CBW_SIGNATURE;
	cbw.tag = dev->tag_counter++;
	cbw.data_transfer_length = data_len;
	cbw.flags = data_in ? MSC_BOT_DIR_IN : MSC_BOT_DIR_OUT;
	cbw.lun = dev->lun;
	cbw.cb_length = cb_len;
	memcpy(cbw.cb, cb, cb_len);
	
	/* Send CBW */
	ret = xhci_bulk_transfer(dev->xhci, dev->slot_id, dev->ep_out,
	                         &cbw, MSC_BOT_CBW_SIZE, false);
	if (ret < 0)
		return ret;
	
	/* Data phase (if any) */
	if (data && data_len > 0) {
		ret = xhci_bulk_transfer(dev->xhci, dev->slot_id,
		                         data_in ? dev->ep_in : dev->ep_out,
		                         data, data_len, data_in);
		if (ret < 0)
			return ret;
	}
	
	/* Receive CSW */
	ret = xhci_bulk_transfer(dev->xhci, dev->slot_id, dev->ep_in,
	                         &csw, MSC_BOT_CSW_SIZE, true);
	if (ret < 0)
		return ret;
	
	/* Validate CSW */
	if (csw.signature != MSC_BOT_CSW_SIGNATURE)
		return -1;
	if (csw.tag != cbw.tag)
		return -1;
	if (csw.status != MSC_CSW_STATUS_PASSED)
		return -1;
	
	return 0;
}

int usb_msc_init(usb_msc_device_t *dev, xhci_controller_t *xhci, u8 slot_id) {
	if (!dev || !xhci)
		return -1;
	
	memset(dev, 0, sizeof(usb_msc_device_t));
	dev->xhci = xhci;
	dev->slot_id = slot_id;
	dev->tag_counter = 1;
	dev->sector_size = 512; /* Default, will be updated by READ CAPACITY */
	
	/* Endpoint addresses should be determined during enumeration */
	/* For now, use common defaults: EP1 OUT, EP1 IN */
	dev->ep_out = 1;
	dev->ep_in = 1;
	
	return 0;
}

int usb_msc_test_unit_ready(usb_msc_device_t *dev) {
	u8 cb[6] = {0};
	cb[0] = SCSI_CMD_TEST_UNIT_READY;
	
	return _msc_bot_transaction(dev, cb, sizeof(cb), NULL, 0, false);
}

int usb_msc_inquiry(usb_msc_device_t *dev, void *data, u32 len) {
	u8 cb[6] = {0};
	cb[0] = SCSI_CMD_INQUIRY;
	cb[4] = (u8)len; /* Allocation length */
	
	return _msc_bot_transaction(dev, cb, sizeof(cb), data, len, true);
}

int usb_msc_read_capacity(usb_msc_device_t *dev) {
	u8 cb[10] = {0};
	u8 data[8];
	int ret;
	
	cb[0] = SCSI_CMD_READ_CAPACITY_10;
	
	ret = _msc_bot_transaction(dev, cb, sizeof(cb), data, sizeof(data), true);
	if (ret < 0)
		return ret;
	
	/* Parse response */
	u32 last_lba = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
	u32 block_size = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
	
	dev->num_sectors = (u64)last_lba + 1;
	dev->sector_size = block_size;
	dev->ready = true;
	
	return 0;
}

int usb_msc_read_sectors(usb_msc_device_t *dev, u32 lba, u32 count, void *buffer) {
	u8 cb[10] = {0};
	
	if (!dev->ready)
		return -1;
	
	cb[0] = SCSI_CMD_READ_10;
	cb[2] = (lba >> 24) & 0xFF;
	cb[3] = (lba >> 16) & 0xFF;
	cb[4] = (lba >> 8) & 0xFF;
	cb[5] = lba & 0xFF;
	cb[7] = (count >> 8) & 0xFF;
	cb[8] = count & 0xFF;
	
	u32 data_len = count * dev->sector_size;
	return _msc_bot_transaction(dev, cb, sizeof(cb), buffer, data_len, true);
}

int usb_msc_write_sectors(usb_msc_device_t *dev, u32 lba, u32 count, void *buffer) {
	u8 cb[10] = {0};
	
	if (!dev->ready || dev->write_protected)
		return -1;
	
	cb[0] = SCSI_CMD_WRITE_10;
	cb[2] = (lba >> 24) & 0xFF;
	cb[3] = (lba >> 16) & 0xFF;
	cb[4] = (lba >> 8) & 0xFF;
	cb[5] = lba & 0xFF;
	cb[7] = (count >> 8) & 0xFF;
	cb[8] = count & 0xFF;
	
	u32 data_len = count * dev->sector_size;
	return _msc_bot_transaction(dev, cb, sizeof(cb), buffer, data_len, false);
}

int usb_msc_request_sense(usb_msc_device_t *dev, void *data, u32 len) {
	u8 cb[6] = {0};
	cb[0] = SCSI_CMD_REQUEST_SENSE;
	cb[4] = (u8)len; /* Allocation length */
	
	return _msc_bot_transaction(dev, cb, sizeof(cb), data, len, true);
}
