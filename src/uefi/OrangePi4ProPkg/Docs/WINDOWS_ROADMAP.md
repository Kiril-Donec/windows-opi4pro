# Path from UEFI v2 to Windows ARM64

UEFI v2 proves the real EDK2 architecture and provides the environment needed
to continue. Windows is not expected to boot from this version yet.

## Firmware v2 requirements

1. Install ACPI 6.x tables: RSDP/XSDT, FADT, DSDT, MADT/GICv3, GTDT and DBG2.
2. Describe all eight CPUs with PSCI and correct GICC affinity values.
3. Add native A733 SDC0 Block I/O so EDK2 and Windows Setup can access microSD.
4. Add persistent variables, preferably SPI NOR; otherwise use a protected
   microSD firmware-variable partition with wear-aware updates.
5. Stop relying on inherited HDMI by programming Display Engine/TCON/HDMI.
6. Repack EDK2 as BL33 inside `boot_package.fex` and remove U-Boot from runtime.

## First Windows drivers

- A733 SD host miniport: required to keep the system disk available after
  `ExitBootServices`.
- KMDOD/display-only driver: uses the display controller/framebuffer without
  PowerVR acceleration.
- reset/power/thermal support through ACPI.
- DWMAC/STMMAC-derived NDIS Ethernet driver.

PowerVR and AIC8800 Wi-Fi remain later projects and are not blockers for the
first Windows desktop.
