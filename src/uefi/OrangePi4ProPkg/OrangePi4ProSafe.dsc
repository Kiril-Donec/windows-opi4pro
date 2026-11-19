[Defines]
  PLATFORM_NAME                  = OrangePi4ProA733
  PLATFORM_GUID                  = 96cd27b7-00fe-49aa-8d12-c089ef762733
  PLATFORM_VERSION               = 16.1
  DSC_SPECIFICATION              = 0x0001001B
  OUTPUT_DIRECTORY               = Build/OrangePi4Pro-AARCH64
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/OrangePi/OrangePi4ProPkg/OrangePi4Pro.fdf
  DEFINE PLAT_XEN                = FALSE
  DEFINE QEMU_PV_VARS            = FALSE
  DEFINE SECURE_BOOT_ENABLE      = FALSE
  DEFINE NETWORK_ENABLE          = FALSE
  DEFINE NETWORK_IP6_ENABLE      = FALSE
  DEFINE NETWORK_HTTP_BOOT_ENABLE = FALSE
  DEFINE NETWORK_SNP_ENABLE      = FALSE
  DEFINE NETWORK_TLS_ENABLE      = FALSE
  DEFINE NETWORK_ALLOW_HTTP_CONNECTIONS = FALSE
  DEFINE NETWORK_ISCSI_ENABLE    = FALSE
  DEFINE NETWORK_PXE_BOOT_ENABLE = FALSE

!include MdePkg/MdeLibs.dsc.inc
!include Platform/OrangePi/OrangePi4ProPkg/OrangePi4ProCommon.dsc.inc

[LibraryClasses.common]
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  ArmVirtMemInfoLib|Platform/OrangePi/OrangePi4ProPkg/Library/A733VirtMemInfoLib/A733VirtMemInfoLib.inf
  ArmPlatformLib|ArmPlatformPkg/Library/ArmPlatformLibNull/ArmPlatformLibNull.inf
  PlatformHookLib|MdeModulePkg/Library/BasePlatformHookLibNull/BasePlatformHookLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  PlatformBootManagerLib|ArmPkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
  LcdHwLib|Platform/OrangePi/OrangePi4ProPkg/Library/A733FrameBufferLcdLib/A733LcdHwLib.inf
  LcdPlatformLib|Platform/OrangePi/OrangePi4ProPkg/Library/A733FrameBufferLcdLib/A733LcdPlatformLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
  CpuMmuLib|UefiCpuPkg/Library/CpuMmuLib/CpuMmuLib.inf
  AcpiHelperLib|DynamicTablesPkg/Library/Common/AcpiHelperLib/AcpiHelperLib.inf
  AmlLib|DynamicTablesPkg/Library/Common/AmlLib/AmlLib.inf

[LibraryClasses.common.SEC]
  PciExpressLib|MdePkg/Library/BasePciExpressLib/BasePciExpressLib.inf
  SerialPortLib|MdeModulePkg/Library/BaseSerialPortLib16550/BaseSerialPortLib16550.inf
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf

[LibraryClasses.common.DXE_CORE]
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf

[PcdsFeatureFlag.common]
  gEmbeddedTokenSpaceGuid.PcdPrePiProduceMemoryTypeInformationHob|TRUE
  gArmPlatformTokenSpaceGuid.PcdGopDisableOnExitBootServices|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutGopSupport|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdTurnOffUsbLegacySupport|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdInstallAcpiSdtProtocol|TRUE

[PcdsFixedAtBuild.common]
  gArmTokenSpaceGuid.PcdUefiShellDefaultBootEnable|TRUE
  gOrangePi4ProTokenSpaceGuid.PcdAcpiCpuCount|8
  gOrangePi4ProTokenSpaceGuid.PcdPsciCpuOnSelfTest|FALSE
  gOrangePi4ProTokenSpaceGuid.PcdExposeEhci1ToWindows|FALSE
  gOrangePi4ProTokenSpaceGuid.PcdExposeSdMmc0ToWindows|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"Orange Pi 4 Pro A733 EDK2 v16 SAFE no OS USB"
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x00010000
  gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize|0x04000000
  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|16
  gArmTokenSpaceGuid.PcdFdSize|0x00800000
  gArmTokenSpaceGuid.PcdFvSize|0x007F8000

  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultDataBits|8
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultParity|1
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultStopBits|1
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0x02500000
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialUseMmio|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialUseHardwareFlowControl|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialDetectCable|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialBaudRate|115200
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialClockRate|24000000
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterStride|4
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterAccessWidth|32
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialLineControl|0x03
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialFifoControl|0x07
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialPciDeviceInfo|{0xFF}
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialExtendedTxFifoSize|0

  gArmTokenSpaceGuid.PcdGicDistributorBase|0x03400000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x03460000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0
  gArmTokenSpaceGuid.PcdGicIrsConfigFrameBase|0
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26
  gArmTokenSpaceGuid.PcdArmArchTimerHypVirtIntrNum|0

  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosVersion|0x0308
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosDocRev|0x00
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosEntryPointProvideMethod|0x3

  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxAuthVariableSize|0x2800
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x40000
  gEfiShellPkgTokenSpaceGuid.PcdShellFileOperationSize|0x20000
  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|3
  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdPciDisableBusEnumeration|TRUE
  gEfiMdePkgTokenSpaceGuid.PcdPciExpressBaseAddress|0xFFFFFFFFFFFFFFFF

[PcdsPatchableInModule.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdPlatformRecoverySupport|FALSE
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x100000000
  gArmTokenSpaceGuid.PcdFdBaseAddress|0x41000000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x41008000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase|0

[PcdsDynamicDefault.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|1024
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|600
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|1024
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|600
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootDiscoveryPolicy|2
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|5

[Components.common]
  Platform/OrangePi/OrangePi4ProPkg/PrePi/A733PrePi.inf {
    <LibraryClasses>
      ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
      PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
      HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
      PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
      MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  }

  MdeModulePkg/Core/Dxe/DxeMain.inf
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  }
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  MdeModulePkg/Universal/Metronome/Metronome.inf
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/VarCheckUefiLib/VarCheckUefiLib.inf
  }

  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf

  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  Platform/OrangePi/OrangePi4ProPkg/Drivers/A733AcpiDxe/A733AcpiDxe.inf
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf
  Platform/OrangePi/OrangePi4ProPkg/Drivers/A733SmbiosDxe/A733SmbiosDxe.inf

  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
  ArmPlatformPkg/Drivers/LcdGraphicsOutputDxe/LcdGraphicsOutputDxe.inf

  Platform/OrangePi/OrangePi4ProPkg/Drivers/A733UsbInitDxe/A733UsbInitDxe.inf
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  MdeModulePkg/Bus/Pci/EhciDxe/EhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf {
    <LibraryClasses>
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf
  MdeModulePkg/Universal/BootManagerPolicyDxe/BootManagerPolicyDxe.inf
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
  MdeModulePkg/Application/BootManagerMenuApp/BootManagerMenuApp.inf
  ShellPkg/Application/Shell/Shell.inf {
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
  }
