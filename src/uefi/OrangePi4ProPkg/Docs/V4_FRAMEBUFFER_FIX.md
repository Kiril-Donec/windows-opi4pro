# Orange Pi 4 Pro EDK2 v4 framebuffer coherence fix

## Physical symptom

V3 reached `UEFI Interactive Shell v2.2` and the `Shell>` prompt, but HDMI
showed fragmented glyphs and stale horizontal pieces while UART text was
correct.

## Root cause

The active vendor framebuffer at `0xBBF11000` is inside LPDDR5. The initial
A733 virtual-memory table mapped all LPDDR5 as write-back cacheable RAM. The
sunxi display engine scans that memory without hardware CPU-cache coherence,
while the stock EDK2 `LcdGraphicsOutputDxe` writes pixels with normal memory
stores and does not clean the AArch64 data cache after every GOP BLT.

## Fix

V4 installs a later, page-aligned MMU descriptor for exactly
`0xBBF11000..0xBC169000` (`0x258000` bytes) using
`ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED`. The remaining 4 GiB RAM
stays write-back cacheable. This trades framebuffer write speed for reliable
scan-out and avoids modifying the generic EDK2 GOP driver.

V4 keeps the compact Shell command set so the firmware still fits the fixed
4 MiB image. The optional `map` command therefore remains absent. V4 does
replace invalid boot discovery policy `0x3F` with the standard
`BDP_CONNECT_ALL` value `2`.

## Still open

The A733 EHCI host controller still fails the generic EDK2 reset/start path.
That is independent from HDMI coherence and remains a later hardware-driver
milestone.
