# USB emuMMC: what Atmosphere needs

Status: hekate side complete on this branch, untested on hardware. Atmosphere side implemented on the `claude/usb-emummc-backend` branch of the Atmosphere fork (emummc USB backend, exosphere type 3, loader `usb_emummc_patches`), untested on hardware; the `usb` sysmodule patches are still to be written per Horizon version.

## Goal

Boot Atmosphere from a USB SSD/HDD in the dock: flip the RCM toggle, inject hekate with a USB-C injector, move the console into the dock, hekate finds the drive through the dock hub, reads boot files and the emuMMC from it and launches exosphere and Atmosphere on it. The SD card stays as a boot stub: the XUSB host firmware (`bootloader/sys/xusbfw.bin`) and the driver module (`bootloader/sys/libsys_usbh.bso`) must be read from it before any USB device exists.

## What exists today

| Piece | State |
| --- | --- |
| XHCI host + mass storage driver, one hub tier (dock) as a DRAM module | built, spec-reviewed, untested |
| hekate boots configs, payloads, Nyx and HOS boot files from the drive (`usbstorage=1`) | built |
| 45 s wait for the drive with prompt, VOL- to skip; 5V only when no external VBUS | built |
| USB emuMMC config (`usb_enabled=1`, `usb_based`), exosphere type 3, hekate reads package1 from it | built |
| Nyx copies sysMMC or the active SD emuMMC to the drive | built |
| Atmosphere reads the emuMMC from the drive | built (Atmosphere branch), untested |

Without any Atmosphere change the flow works with a normal SD emuMMC. With a USB emuMMC hekate refuses to launch HOS until `usb_atmosphere=1` is set in `emummc.ini`.

## Contract passed to Atmosphere

Exosphere `emummc_config_t`, filled like an SD raw partition emuMMC:

| Field | Value |
| --- | --- |
| `base_cfg.magic` | `EFS0` |
| `base_cfg.type` | `3` (`emuMMC_USB`; 1 = SD partition, 2 = SD files) |
| `base_cfg.id`, `base_cfg.fs_ver` | as today |
| `partition_cfg.start_sector` | LBA of BOOT0 on the USB drive, 512-byte sectors |
| `nintendo_path` | folder on the SD card, as today |

Image layout from `start_sector`: BOOT0 (0x2000 sectors), BOOT1 at +0x2000, GPP at +0x4000. Nyx places it 16 MiB after the drive's last MBR partition and claims the space with a type `0xE0` MBR entry. The drive is the first USB mass storage device reachable, directly or through one hub. USB 2.0 High Speed only.

## Why stock Atmosphere cannot use it

- `emummc` knows `emuMMC_EMMC = 0`, `emuMMC_SD_Raw = 1`, `emuMMC_SD_File = 2`. Raw reads go through its own copy of the SD driver (`sdmmc_storage_read(&sd_storage, ...)`), file reads through its own FatFS mount of the SD card. Any other type ends in a fatal.
- It runs inside the FS process, before the `usb` sysmodule exists; `usb` is itself loaded from the emuMMC. It must drive the hardware directly.
- There is one usable USB controller (XUSB). The `usb` sysmodule resets it when it starts.

## Design of the emummc USB backend

Port hekate's driver into the emummc module and have emummc own XUSB for the session. XHCI covers Erista and Mariko; the Erista-only legacy USB1 EHCI controller would avoid the firmware blob but still needs the pad mux and `usb` handled.

| Component | Source | Port notes |
| --- | --- | --- |
| XHCI host | `bdk/usb/usbh.c` (~1,300 lines, polled) | MMIO via KIP capabilities: XUSB_HOST `0x70090000` (64 KiB, FPCI at +0x8000, IPFS at +0x9000), XUSB_PADCTL `0x7009F000`, CAR `0x60006000`, PMC `0x7000E400`, FUSE `0x7000F800`, GPIO. Replace `bpmp_mmu_maintenance` with `dc cvac`/`dc civac`, `usleep`/`msleep` with `svcSleepThread`. |
| Mass storage | `bdk/usb/usbh_msc.c` (~350 lines) | keep 512 KiB chained transfers |
| Dispatch | `emummc_ctx.h`, `emummc.c` | add `emuMMC_USB_Raw = 3`; `EMMC_StoragePartitionOffset = start_sector`; third branch in `emummc_read_write_inner()`: `usb_msc_rw(offset + active_partition * BOOT_PARTITION_SIZE + sector)`; same for the nand patrol check |
| Firmware | `bootloader/sys/xusbfw.bin` on SD | read with emummc's FatFS into a resident contiguous 128 KiB buffer at first use |
| DMA memory | static 4 KiB aligned buffers + `svcQueryPhysicalAddress` | as the SD driver; keep each buffer under 64 KiB |
| Locking | mutex per BOT transaction | FS is multithreaded; HDD spin-up timeouts up to 10 s |

Resolved while implementing: (1) mesosphere lists XUSB_HOST among the SMMU managed clients and parks it on a blocking ASID at boot, so the emummc module creates a device address space, attaches it to `DeviceName_XusbHost` and maps one ~800 KiB DMA block (rings, scratchpad, firmware, 256 KiB BOT staging) with `svcMapDeviceAddressSpaceAligned`; every address handed to the controller (TRBs, DCBAA, Falcon `ILOAD_BASE`) is a device address. (2) The PMC is not mappable from userland on 2.0.0+; partition power and clamp removal go through the secure monitor's register access SMC (`APBDEV_PMC_USB_AO` was added to its whitelist). (3) FS closes the eMMC controller on sleep; the module stops the host then and brings it back (firmware reload included) on the next access, so resume works while the drive stays connected.

Still to verify on hardware: `pcv`/`usb` must not gate XUSB clocks or the XUSBA/XUSBC partitions, and the per-version `usb` sysmodule patches.

## The usb sysmodule

Patch the `usb` sysmodule (`0100000000000006`) with Atmosphere exefs IPS patches (`atmosphere/exefs_patches/<name>/<build id>.ips`) so its XUSB host/device bring-up returns early. One patch per Horizon version. Lost in HOS: PC connection (MTP, USB homebrew tools) and USB peripherals in the dock. Kept: charging/PD, HDMI, Joy-Cons, Wi-Fi, Bluetooth.

## hekate side for the dock flow

- 45 s wait with prompt and VOL- skip at boot (`usbstorage=1`) and before launching HOS with a USB emuMMC.
- BQ24193 status checked before host init: no 5V boost when VBUS/PG is present; if docked mid-wait, the host restarts without sourcing.
- Dock hub as one tier, every connected port probed until a mass storage device is found.
- Dock handshake (`bdk/usb/usbh_dock.c`, run once per session when external VBUS is present, before the host is started). Resolved by reading the L4T `bm92txx` driver (CTCaer switch-l4t-kernel-4.9): the dock's USB hub does not enumerate on its own. In the dock the console is a USB-PD sink and, by Type-C default, the USB device (UFP). The BM92T36 PD controller does nothing beyond the power contract by itself, so the host has to drive it over I2C: request the 15 V contract if it is not already in place, `PS_RDY`, `DR_SWAP` to become the host (DFP), then Nintendo's vendor defined messages: Discover Identity (VID 0x057E, PID 0x2003), Enter Nintendo alt mode (SVID 0x7E05), device state query, LED on, and `HUB_CONTROL` enable, retried while the dock answers busy. Only after that message does the hub power up. DisplayPort is not entered. hekate polls the controller's alert register instead of using its interrupt line. An OTG adapter (`OTG_INSERT` / source mode) and third party PD sources (role swap only) are told apart from the dock. Dock firmware without hub control is reported on screen.

Remaining hardware questions: whether the charger reports dock VBUS early enough, HDD spin-up on an OTG adapter, and whether the dock accepts the sequence from a cold hekate without the HOS-side timing.

## Plan

1. Handheld bring-up with an OTG adapter: USB Drive info page, mount, launch a payload.
2. Same in the dock (verifies the BM92T36 dock handshake and the hub enable).
3. Full flow with a normal SD emuMMC; Nyx copy to a scratch drive; hekate reads package1 from it.
4. emuMMC fork: port drivers, type 3, firmware from SD, SMMU answer; boot with `usb` patched out.
5. `usb` exefs patches per Horizon version.
6. Resume, removal, spin-up, write workloads; then `usb_atmosphere=1`.
