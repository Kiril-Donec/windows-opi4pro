# Third-party components

## In `firmware/opi4pro-a733-uefi-v60.img.gz`

| Part | Origin | License |
|---|---|---|
| boot0 (sectors 16–495), U-Boot 2018.07 package / BL31 / SCP (toc1, sectors 32800–65535), device tree in partition 1 | Orange Pi 4 Pro board support images by Shenzhen Xunlong (Orange Pi) / Allwinner, unmodified | U-Boot: GPL-2.0; the other boot blobs are vendor binaries redistributed as found in the public Orange Pi images |
| UEFI (`uefi_stage0.uimg` in partition 1) | built from `src/uefi` on top of TianoCore EDK2 | BSD-2-Clause-Patent |

U-Boot source: published by Orange Pi / Allwinner for the A733 (sun60iw2) BSP.

## In `src/drivers`

- `A733Gmac/netringiterator.h`: from Microsoft Windows-driver-samples (NetAdapterCx), MIT license.

## Not included

Microsoft Windows is not part of this project. The installer uses the `boot.wim` and
`install.wim`/`install.esd` from the ISO that you download from Microsoft, under Microsoft's license.
