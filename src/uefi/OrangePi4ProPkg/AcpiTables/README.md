# ACPI status

`Dsdt.asl.draft` records the first device namespace and verified MMIO/IRQ
values. It is deliberately not included in v1 because a Windows-capable ACPI
set must be installed as a consistent group: FADT, MADT/GICv3, GTDT, DSDT,
DBG2 and preferably SPCR. Installing only a DSDT would create a misleading and
potentially unsafe firmware interface.

The next version should use EDK2 `AcpiTableDxe` plus generated table binaries,
with CPU affinities `0x000` through `0x700`, GIC distributor `0x03400000`,
redistributors `0x03460000`, PSCI conduit SMC and timer PPIs from
`HARDWARE_MAP.md`.
