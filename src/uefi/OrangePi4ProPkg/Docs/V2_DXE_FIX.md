# v2 DXE dispatcher fix

The first hardware log proved that SEC, PrePi, the firmware volume and DxeCore
all ran on the A733, but no architectural DXE driver could dispatch.

## Root cause

`PcdDxe` was linked with the normal protocol-backed `DxePcdLib`. Its generated
DEPEX therefore required `gEfiPcdProtocolGuid`, even though `PcdDxe` is the
module that installs that protocol. This created a circular bootstrap
dependency. The protocol-based DevicePath library introduced a second early
bootstrap dependency. `VariableRuntimeDxe` also pulled in an NV-store formatted
protocol that this bring-up platform does not provide yet.

## Fixes

- Link `PcdDxe` with `BasePcdLibNull` and the plain `UefiDevicePathLib`.
- Link `DevicePathDxe` with `BasePcdLibNull` and the plain
  `UefiDevicePathLib`.
- Use emulated non-volatile variables for bring-up and remove the unavailable
  NV-store-formatted dependency.
- Put only `PcdDxe` and `DevicePathDxe` in the DXE APRIORI file.
- Keep the nested firmware volume uncompressed while debugging dispatcher
  startup.
- Enable `DEBUG_DISPATCH` logging.

The expected v2 UART marker is:

```
[A733 EDK2 v2] SEC entered at 0x41000000
```
