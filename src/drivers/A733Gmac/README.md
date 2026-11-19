# A733Gmac

Experimental ARM64 NetAdapterCx 2.1 driver for the Orange Pi 4 Pro onboard
Allwinner A733 GMAC and Motorcomm YT8531 PHY.

## Required firmware

The driver binds to `ACPI\AWIN0040`, which is published by the paired
`orangepi4pro_edk2_v59_gmac_diag_flashable.img` firmware after its GMAC preflight
checks succeed. It will not bind on v56.

## Build and package

Run from an elevated PowerShell only when signing or installing is needed:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build-driver.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\sign-package.ps1
```

The signed package is written to `package-0.1.0.5`. The test certificate is
the same certificate already used by the A733SdMmc test driver.

## First hardware test

1. Boot Windows/WinPE with firmware v59.
2. Ensure the test certificate is trusted in Root and TrustedPublisher.
3. Install `A733Gmac.inf` with `pnputil /add-driver A733Gmac.inf /install`.
4. Check `pnputil /enum-devices /instanceid "ACPI\AWIN0040\0" /drivers`.
5. Connect Ethernet and check `Get-NetAdapter` and `ipconfig /all`.

Compilation and signature verification cannot prove DMA, interrupt, MDIO, or
link operation on the physical board. Keep KD attached for the first load.

## v0.1.0.3 changes

- Split the four AWIN0040 interrupt roles: main/LPI are MAC diagnostics only; TX0/RX0 are the only queue-completion paths.
- Require exactly four translated interrupt resources before starting the controller; partial IRQ layouts fail fast instead of enabling per-channel routing with missing lines.
- TX0/RX0 acknowledge only their own W1C DMA status bits, preventing one direction from erasing the other direction's pending completion.
- Share one framework interrupt spin lock across all four WDF interrupt objects so ISR/Enable/Disable mask updates are serialized across CPUs.
- LPI IRQ is acknowledged by reading `GMAC_LPI_CONTROL_STATUS`, matching DWMAC4/5 behavior.
- Program `DMA_MODE.INTM` to mode 1 so the DWMAC4/5 per-direction TX/RX interrupt outputs are routed separately.
- Validate the TX packet ring before `WdfMemoryCreate` and use checked `RtlSizeTMult` for the allocation size.

## v0.1.0.4 changes

- Fixes the reproduced `BUGCODE_NETADAPTER_DRIVER (0x19E)` subcode `0xB` by
  advertising a non-zero multicast address capacity whenever multicast packet
  filtering is supported.

## v0.1.0.5 changes

- Matches the vendor Linux DWMAC4 software-reset budget: polls every 10 ms for
  up to one second instead of declaring a timeout after only 10 ms.
- On a real reset timeout, logs DMA status, GMAC version and the wrapper
  configuration/status/clock registers for the next hardware diagnosis.

## v0.1.0.2 changes

- Creates four KMDF interrupt objects to match AWIN0040 main/LPI/TX0/RX0 resources.
- Logs the translated interrupt resources seen by Windows during PrepareHardware.
- Fixes cleanup when mapping a later MMIO resource fails.
- Serializes GMAC_CONFIGURATION read/modify/write updates with the adapter spin lock.
