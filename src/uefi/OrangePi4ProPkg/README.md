# Orange Pi 4 Pro (Allwinner A733) EDK2 UEFI — bring-up v2

This is the first **real EDK2 firmware port** for the Orange Pi 4 Pro A733
created from the previously proven Stage0/MiniUEFI work. It is not an EFI
application: the image contains SEC/PrePi, HOB construction, MMU setup, DXE
Core, BDS, UEFI Shell and platform DXE drivers.

## What v2 contains

- fixed-load AArch64 SEC/PrePi entry at `0x41000000`;
- earliest UART marker before MMU/HOB/DXE initialization;
- 4 GiB LPDDR5 memory map and a dedicated 64 MiB UEFI working region;
- GICv3 and ARM architectural timer drivers;
- PSCI reset path through BL31;
- DesignWare/16550 UART0 console at `0x02500000`;
- GOP exposing the already initialized 1024×600 framebuffer at `0xBBF11000`;
- A733 USB1 clock/reset/PHY setup and non-discoverable EHCI registration;
- EDK2 EHCI, USB bus, USB keyboard and USB mass-storage drivers;
- GPT/MBR partition parsing, Disk I/O and FAT filesystem support;
- graphical/text console, Boot Manager, setup browser and UEFI Shell;
- DEBUG and RELEASE builds;
- legacy U-Boot `uImage` wrapper compatible with the proven Stage0 v13
  `bootm` handoff;
- tool to produce a ready-to-flash 64 MiB microSD image.

## v2 DXE dispatcher repair

The first board test of v1 reached DxeCore but stopped because `PcdDxe` was
linked against the protocol-backed PCD library and therefore waited for the
very protocol it was supposed to install. v2 breaks that circular dependency,
uses bootstrap-safe libraries for `PcdDxe` and `DevicePathDxe`, enables
emulated variables, and places the two bootstrap drivers in DXE APRIORI.
Static FV inspection confirms both now have `TRUE` DEPEX.

## Important limits of v2

The host build is verified, and the files inside the flashable images were
read back from ext4 and compared byte-for-byte. The firmware has **not yet
been executed on the physical board in this build environment**.

This first bring-up version still inherits several things from the vendor
boot chain:

- `boot0` initializes LPDDR5;
- BL31/monitor provides PSCI and the EL transition;
- vendor U-Boot initializes HDMI and loads the EDK2 uImage/DTB;
- GOP exposes that inherited framebuffer rather than programming Display
  Engine/TCON/HDMI from a cold state.

Not implemented yet:

- native A733 SD/MMC Block I/O inside EDK2;
- ACPI table installation required for Windows;
- persistent UEFI variables in SPI/eMMC/microSD;
- OHCI/USB3 and independent USB power-switch handling;
- cold display initialization without `screen_char`;
- direct `boot0 -> BL31 -> EDK2 as BL33` boot package with U-Boot removed;
- Windows ARM64 device drivers.

Therefore v2 is a **DXE-dispatch-fixed EDK2 bring-up firmware**, not yet the
final Windows-ready firmware.

## First test

Flash the DEBUG image. UART at 115200 8N1 should show first:

```text
[A733 EDK2 v2] SEC entered at 0x41000000
```

After that, EDK2 DEBUG messages should appear as PrePi creates HOBs and enters
DXE. HDMI should remain at 1024×600 and eventually display the EDK2 console or
Shell. See `Docs/TESTING.md` for recovery and checkpoints.

## v4 hardware update

The active HDMI framebuffer is mapped uncached to keep CPU rendering coherent with the non-coherent sunxi display scan-out.
