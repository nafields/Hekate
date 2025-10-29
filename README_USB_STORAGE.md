# USB Mass Storage Host Support

## Overview

Hekate now supports using USB mass storage devices (such as USB flash drives and external SSDs) as storage backends for emuMMC. This allows the Switch to read from (and optionally write to) USB storage devices connected through the USB-C port.

## Requirements

### Hardware
- **Powered USB Hub**: A powered USB hub is strongly recommended to ensure sufficient power delivery to USB storage devices.
- **Compatible USB Storage**: USB 2.0/3.0 flash drives, external hard drives, or SSDs.

### Connection
1. Connect a powered USB hub to the Switch's USB-C port
2. Connect your USB storage device to the hub
3. Boot Hekate with USB storage enabled

## Configuration

### Enabling USB Storage

USB storage support can be enabled through the emuMMC configuration. Add the following to your `emuMMC/emummc.ini`:

```ini
[emummc]
enabled=1
usb_storage=1
usb_write_enable=0  ; Set to 1 to enable writes (use with caution!)
```

### Safety Features

#### Read-Only Mode (Default)
By default, USB storage operates in **read-only mode** for safety. This prevents accidental data corruption and is recommended for most users.

#### Write Mode
Write mode can be enabled by setting `usb_write_enable=1` in the configuration. **Use this with extreme caution**:
- Only enable writes after thorough testing with your specific USB device
- Always backup your data before enabling write mode
- Some USB devices may not work reliably in write mode

## Supported Devices

The USB mass storage implementation supports:
- USB 2.0 and USB 3.0 devices
- Standard Mass Storage Class (MSC) devices
- Devices using Bulk-Only Transport (BOT)
- Standard SCSI command set

### Tested Devices
- USB 2.0/3.0 flash drives (various brands)
- External SSDs with USB enclosures
- External HDDs with powered enclosures

### Known Limitations
- **No USB hub enumeration**: Devices must be connected before boot
- **Single device only**: Only one USB storage device supported at a time
- **No hot-plug**: Device must be connected during boot, replug not supported yet
- **Powered hub required**: Unpowered hubs or direct connections may not provide enough power

## Performance

### Optimization Features
- **Sector Caching**: A 64-sector cache reduces redundant reads
- **Read-Ahead**: Sequential reads are optimized for better performance
- **Bulk Transfer**: Large transfers use optimized bulk transfer mode

### Expected Performance
- Read speeds: 5-20 MB/s (depending on device and interface)
- Write speeds: 3-15 MB/s (when enabled)

## Troubleshooting

### Device Not Detected
1. Ensure the USB hub is powered
2. Check that the device is connected before booting Hekate
3. Try a different USB hub or cable
4. Verify the device works on a PC

### Slow Performance
1. Use a USB 3.0 device for better speeds
2. Ensure the hub supports USB 3.0
3. Try a different USB cable
4. Check device health on a PC

### Write Failures
1. Verify `usb_write_enable=1` is set in configuration
2. Check that the device is not write-protected
3. Ensure the device has sufficient free space
4. Test the device on a PC first

## Advanced Configuration

### Cache Configuration
The sector cache can be configured in `emummc_storage_usb.c`:
```c
#define CACHE_SIZE_SECTORS  64  // Adjust for memory/performance tradeoff
```

### Device Quirks
Some devices may require specific quirks or timeouts. These can be added to the MSC driver based on device vendor/product IDs.

## Technical Details

### Architecture
1. **xHCI Driver** (`bdk/usb/xhci.c`): Handles USB host controller initialization and transfers
2. **MSC Driver** (`bdk/usb/usb_msc_host.c`): Implements USB Mass Storage Class protocol
3. **Storage Integration** (`bootloader/storage/emummc_storage_usb.c`): Provides block device interface

### SCSI Commands Supported
- TEST UNIT READY (0x00)
- REQUEST SENSE (0x03)
- INQUIRY (0x12)
- READ CAPACITY (10) (0x25)
- READ (10) (0x28)
- WRITE (10) (0x2A)

## Safety and Warnings

⚠️ **Important Safety Information**:
- USB storage support is experimental
- Always backup your data before use
- Start with read-only mode for testing
- Only enable writes after extensive testing
- Not all USB devices are compatible
- Data corruption is possible with incompatible devices or unreliable connections

## Future Improvements

Planned enhancements:
- Device hot-plug support
- USB hub enumeration
- Additional device quirks
- Performance optimizations
- Better error recovery
- Support for larger sector sizes (4K native)

## Contributing

If you encounter issues or have device compatibility information, please report:
- Device make/model
- USB hub used
- Error messages or behavior
- Performance measurements

This helps improve compatibility and reliability for all users.
