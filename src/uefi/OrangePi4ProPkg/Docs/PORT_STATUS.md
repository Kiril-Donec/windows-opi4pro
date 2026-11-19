# Port status — v2

| Area | v2 status | Evidence / implementation |
|---|---|---|
| AArch64 handoff | Implemented | fixed load `0x41000000`, early assembly marker |
| SEC/PrePi | Implemented | custom `A733PrePi`, MMU/HOB/DXE handoff |
| LPDDR5 | Inherited | initialized by vendor `boot0`; 4 GiB mapped WB |
| UART0 | Implemented | `0x02500000`, 32-bit register access, stride 4 |
| GICv3 | Implemented | distributor `0x03400000`, redistributor `0x03460000` |
| ARM timer | Implemented | PPI 29/30/27/26; firmware assumes 24 MHz platform counter |
| Reset | Implemented | PSCI through BL31 |
| GOP | Implemented/inherited | 1024×600 XRGB/BGR framebuffer `0xBBF11000` |
| USB1 EHCI | Implemented, untested on board | A733 CCU/reset/PHY/passby sequence + EDK2 EHCI |
| USB keyboard | Included | generic EDK2 USB HID keyboard driver |
| USB storage | Included | generic EDK2 USB mass-storage driver |
| FAT/GPT/MBR | Included | standard EDK2 disk and FAT drivers |
| UEFI Shell | Included | built into FV |
| Boot Manager/BDS | Included | standard EDK2 BDS and menu application |
| Variables | Emulated NV for bring-up | protocol installs without a durable microSD/SPI backend |
| Native SD/MMC | Not implemented | controller is proprietary Sunxi, not SDHCI |
| ACPI | Draft only | required before Windows Boot Manager is a real target |
| Ethernet/audio/Wi-Fi/GPU | Not UEFI v2 targets | Windows drivers come later |
| Direct BL33 package | Not implemented | v2 intentionally uses proven vendor `bootm` chain |

## v2 dispatcher fix

- `PcdDxe` DEPEX: `TRUE`
- `DevicePathDxe` DEPEX: `TRUE`
- DXE APRIORI: `PcdDxe`, then `DevicePathDxe`
- unavailable NV-store-formatted dependency removed
- nested FV left uncompressed for dispatcher diagnosis

## Build verification

- DEBUG EDK2 build: PASS
- RELEASE EDK2 build: PASS
- legacy uImage header and payload CRC: PASS
- flashable-image `/boot.scr` readback comparison: PASS
- flashable-image `/uefi_stage0.uimg` readback comparison: PASS
- physical Orange Pi v2 test: PENDING (v1 reached DxeCore)
