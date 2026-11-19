# Orange Pi 4 Pro EDK2 v16 — EHCI1 clean Windows handoff

v15 proved that Windows consumes the ACPI USB nodes, but exposing uninitialized
EHCI0 and EHCI1 together caused WHEA_UNCORRECTABLE_ERROR (0x124).

v16 changes:
- EHCI0 is never exposed.
- SDC0 is hidden until an AWIN0001 Windows ARM64 driver exists.
- USB-handoff build exposes only proven EHCI1 as PNP0D20.
- At ExitBootServices, EhciDxe disables interrupts and schedules, halts and
  resets EHCI1, then restores A733 clocks, PHY, pass-by and PHY tuning while
  leaving the standard EHCI core halted for Windows to initialize.
- SAFE build publishes no USB/MMC ACPI device and serves as a regression check.
