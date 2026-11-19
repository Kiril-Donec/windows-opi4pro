# A733 SDC0 driver plan

A733 SDC0 at `0x04020000` is an Allwinner/Sunxi controller, not a standard
SDHCI controller. Therefore EDK2's generic SDHCI driver cannot simply be
registered as a non-discoverable device.

The native DXE driver must implement:

1. CCU clock/reset and pinctrl setup for the Orange Pi 4 Pro microSD pins.
2. controller soft reset, FIFO reset and interrupt clearing;
3. 400 kHz card-identification clock, CMD0/CMD8/ACMD41 flow;
4. CID/CSD/RCA/select-card and bus-width negotiation;
5. PIO reads first, followed by IDMA only after cache maintenance is proven;
6. `EFI_BLOCK_IO_PROTOCOL` with 512-byte blocks;
7. card removal/write-protect reporting where the board wiring supports it;
8. timeout/error recovery without leaving the controller wedged.

The Linux BSP implementations `AW_MMC_V4P1X/V4P00X/V4P5X/V5P3X` and vendor
U-Boot SDC code are the register-level references. PIO-only bring-up is the
safer first milestone because it avoids DMA/IOMMU/cache-coherency ambiguity.
