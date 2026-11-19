# Orange Pi 4 Pro A733 EDK2 v11 — CPU/HAL ACPI fix

Scope: only the early Windows ARM64 HAL path.  The proven EHCI1 USB 2.0,
USB hub, keyboard, mass-storage, FAT, GOP/framebuffer, SMBIOS, low-DMA and
vendor boot0/BL31/U-Boot chain are intentionally unchanged from v10.

## v10 hardware result

Windows ARM64 progressed past bootmgfw.efi, displayed the Windows logo, loaded
the kernel, and stopped with HAL_INITIALIZATION_FAILED (0x5C).

## Concrete v11 corrections

1. Real AML DSDT generated with the upstream EDK2 DynamicTablesPkg AmlLib.
   It contains \\_SB.CP00 through CP07, each with:
   - _HID = "ACPI0007"
   - _UID = 0 through 7
   - _STA = 0x0F

2. GICv3 MADT correction:
   - CPUInterfaceNumber is zero for every GICC entry because A733 uses GICv3
     without GICv2 compatibility mode.
   - Eight GICC entries remain ordered CPU0 first.
   - AcpiProcessorUid 0..7 maps to MPIDR 0x000, 0x100, ... 0x700.
   - GICD 0x03400000 and GICR 0x03460000 + 0xFF004 are unchanged from BSP DTS.

3. GTDT correction:
   - Secure PL1 GSIV 29
   - Non-secure PL1 GSIV 30
   - Virtual timer GSIV 27
   - Non-secure PL2 GSIV 26
   - Virtual PL2 timer is now zero because the A733 DTS exposes four timer
     interrupts and the EDK2 FDT parser defaults Virtual PL2 to zero on systems
     without a described ARMv8.1 VHE timer.

4. Runtime diagnostics in DEBUG build:
   - DSDT/FADT/MADT/GTDT/SPCR lookup and checksum status
   - DSDT ACPI0007 count
   - FADT XDsdt
   - MADT GICC field validation
   - CurrentEL, MPIDR_EL1, CNTFRQ_EL0 and 1 ms CNTPCT delta
   - GICD_CTLR/TYPER/IIDR
   - eight GICR_TYPER values
   - PSCI version and affinity state for all eight MPIDRs
   - ReadyToBoot and ExitBootServices markers

## Important expected UART lines

    A733 ACPI11: generated AML DSDT ... CPUs=8 HID=ACPI0007 UID=0..7
    A733 ACPI11: installed real CPU DSDT + FADT + fixed MADT + 4-timer GTDT + SPCR
    A733 HAL11: ReadyToBoot reached
    A733 ACPI11 SELFTEST: ...
    A733 HAL11: CurrentEL=...
    A733 HAL11: GICR[0] ... GICR[7] ...
    A733 HAL11: PSCI_VERSION ...
    A733 HAL11: ExitBootServices signaled - OS kernel handoff

Physical behavior beyond the already-proven v10 path is not claimed until the
board is tested.
