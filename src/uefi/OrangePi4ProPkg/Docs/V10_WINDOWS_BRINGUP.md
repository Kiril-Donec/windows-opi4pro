# v10 Windows bring-up

v10 keeps the physically proven v9 EHCI path and adds the first ACPI/SMBIOS
platform description required by ARM64 operating-system loaders.

Installed ACPI 6.5 tables:
- DSDT: minimal empty AML namespace for the first boot-library experiment
- FADT: Hardware Reduced ACPI + PSCI (SMC)
- MADT: 8 A733 CPU interfaces, GICv3 distributor and redistributor range
- GTDT: secure/non-secure/virtual EL1 and EL2 architectural timers
- SPCR: UART0 at 0x02500000, GIC SPI 2, 115200 8N1

Installed SMBIOS records describe Orange Pi 4 Pro, Allwinner A733, eight cores,
and 4096 MB onboard LPDDR5.

The DXE page converter now prints the requested type/attributes when an image
asks for a fixed address outside physical DRAM (which begins at 0x40000000).
The high-frequency EHCI common-buffer allocation trace was demoted to verbose.
