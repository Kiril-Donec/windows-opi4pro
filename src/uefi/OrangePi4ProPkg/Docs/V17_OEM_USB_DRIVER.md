# Orange Pi 4 Pro EDK2 v17 — OEM USB driver handoff

- Exposes only EHCI1 as ACPI\\AWIN0002 with compatible ID PNP0D20.
- Does not replace Microsoft BOOTAA64.EFI.
- Intended for use with the A733UsbGlue ARM64 lower-filter package injected into WinPE.
- SDMMC remains hidden until its native driver exists.
