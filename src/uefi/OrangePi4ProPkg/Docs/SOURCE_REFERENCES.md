# Source references used during the port

- Orange Pi vendor U-Boot for `sun60iw2` / A733.
- Orange Pi vendor Linux 6.6 BSP branch for `sun60iw2`.
- Radxa A733 BSP where useful for cross-checking peripherals.
- TianoCore EDK2 packages: MdePkg, MdeModulePkg, ArmPkg, ArmPlatformPkg,
  EmbeddedPkg, FatPkg and ShellPkg.
- Previous project artifacts:
  - Orange Pi 4 Pro UEFI Stage0 v13;
  - MiniUEFI v86 source;
  - EDK2 bootstrap/port v1 experiments;
  - confirmed 1024×600 framebuffer and A733 USB BSP sequence.

Vendor binary components are not redistributed in the source-only package.
The flashable test images retain binary `boot0`, BL31/monitor, SCP and vendor
U-Boot from the user's previously proven Stage0 v13 image.
