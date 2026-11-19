# A733 hardware map used by v2

These values were recovered from the vendor A733 BSP, previous Stage0/MiniUEFI
experiments and the working Orange Pi 4 Pro boot image.

| Block | Base / value |
|---|---:|
| DRAM | `0x40000000`, size `0x100000000` (4 GiB) |
| EDK2 FD load/entry | `0x41000000` |
| EDK2 firmware volume | `0x41008000` |
| DTB handoff | `0x43000000` |
| BL31 reserved region | `0x48000000`, size `0x01000000` |
| UEFI working RAM | `0x50000000`, size `0x04000000` |
| UART0 | `0x02500000` |
| CCU | `0x02002000` |
| system configuration | `0x03000000` |
| GICv3 distributor | `0x03400000` |
| GICv3 redistributors | `0x03460000` |
| SDC0 / microSD | `0x04020000` |
| USB1 EHCI | `0x04200000` |
| USB1 OHCI | `0x04200400` |
| GMAC0 | `0x04500000` |
| Display Engine | `0x05000000` |
| HDMI | `0x05520000` |
| framebuffer | `0xBBF11000`, `0x00258000` bytes |
| resolution / stride | 1024×600 / 1024 pixels |

Timer PPIs: secure 29, non-secure 30, virtual 27, hypervisor 26.
USB1 EHCI vendor interrupt is SPI 159 (ACPI GSIV 191). SDC0 vendor interrupt
is SPI 161 (ACPI GSIV 193).

The USB initialization sequence in `A733UsbInitDxe` programs CCU registers
`0x05A4`, `0x05C0`, `0x1308`, `0x130C`, `0x1340`, `0x1A00`, system
configuration rescal registers `0x160/0x164`, then USB passby at EHCI+`0x800`.
