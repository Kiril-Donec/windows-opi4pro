/** @file
  Minimal SMBIOS 3.x inventory for Orange Pi 4 Pro / Allwinner A733.
**/

#include <Uefi.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/Smbios.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

STATIC
EFI_STATUS
AddRecord (
  IN  EFI_SMBIOS_PROTOCOL  *Smbios,
  IN  VOID                 *Formatted,
  IN  UINTN                FormattedSize,
  IN  CONST CHAR8          **Strings,
  IN  UINTN                StringCount,
  OUT EFI_SMBIOS_HANDLE    *ResultHandle OPTIONAL
  )
{
  EFI_STATUS               Status;
  EFI_SMBIOS_TABLE_HEADER  *Record;
  EFI_SMBIOS_HANDLE        Handle;
  UINTN                    TotalSize;
  UINTN                    Index;
  CHAR8                    *Cursor;

  TotalSize = FormattedSize + ((StringCount == 0) ? 2 : 1);
  for (Index = 0; Index < StringCount; Index++) {
    TotalSize += AsciiStrLen (Strings[Index]) + 1;
  }

  Record = AllocateZeroPool (TotalSize);
  if (Record == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Record, Formatted, FormattedSize);
  Record->Length = (UINT8)FormattedSize;
  Cursor = (CHAR8 *)Record + FormattedSize;
  for (Index = 0; Index < StringCount; Index++) {
    UINTN Length;
    Length = AsciiStrLen (Strings[Index]);
    CopyMem (Cursor, Strings[Index], Length);
    Cursor += Length + 1;
  }
  *Cursor = '\0';

  Handle = SMBIOS_HANDLE_PI_RESERVED;
  Status = Smbios->Add (Smbios, NULL, &Handle, Record);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
          "A733 SMBIOS10: add type=%u len=%u handle=%04x -> %r\n",
          Record->Type, Record->Length, Handle, Status));
  FreePool (Record);

  if (!EFI_ERROR (Status) && (ResultHandle != NULL)) {
    *ResultHandle = Handle;
  }
  return Status;
}

EFI_STATUS
EFIAPI
A733SmbiosDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_SMBIOS_PROTOCOL  *Smbios;
  EFI_SMBIOS_HANDLE    MemoryArrayHandle;
  SMBIOS_TABLE_TYPE0   Type0;
  SMBIOS_TABLE_TYPE1   Type1;
  SMBIOS_TABLE_TYPE4   Type4;
  SMBIOS_TABLE_TYPE16  Type16;
  SMBIOS_TABLE_TYPE17  Type17;
  SMBIOS_TABLE_TYPE19  Type19;
  SMBIOS_TABLE_TYPE32  Type32;

  CONST CHAR8 *Type0Strings[] = { "EDK II", "Orange Pi 4 Pro EDK2 v16", "07/30/2026" };
  CONST CHAR8 *Type1Strings[] = { "Orange Pi", "Orange Pi 4 Pro", "A733", "Development Board", "OPI4PRO-A733", "Orange Pi 4 Pro" };
  CONST CHAR8 *Type4Strings[] = { "SoC", "Allwinner", "A733 (6x Cortex-A55 + 2x Cortex-A76)", "Unknown", "Unknown", "A733" };
  CONST CHAR8 *Type17Strings[] = { "Onboard LPDDR5", "System Board", "Orange Pi", "Unknown", "Unknown", "LPDDR5 4GB" };

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733 SMBIOS10: protocol missing: %r\n", Status));
    return Status;
  }

  ZeroMem (&Type0, sizeof (Type0));
  Type0.Hdr.Type = SMBIOS_TYPE_BIOS_INFORMATION;
  Type0.Vendor = 1;
  Type0.BiosVersion = 2;
  Type0.BiosReleaseDate = 3;
  Type0.BiosSize = 0x7F; // 8 MiB in 64 KiB units
  Type0.BIOSCharacteristicsExtensionBytes[1] = BIT3; // UEFI specification supported
  Type0.SystemBiosMajorRelease = 1;
  Type0.SystemBiosMinorRelease = 0;
  Status = AddRecord (Smbios, &Type0, sizeof (Type0), Type0Strings, ARRAY_SIZE (Type0Strings), NULL);
  if (EFI_ERROR (Status)) return Status;

  ZeroMem (&Type1, sizeof (Type1));
  Type1.Hdr.Type = SMBIOS_TYPE_SYSTEM_INFORMATION;
  Type1.Manufacturer = 1;
  Type1.ProductName = 2;
  Type1.Version = 3;
  Type1.SerialNumber = 4;
  Type1.Uuid.Data1 = 0xA7334F50;
  Type1.Uuid.Data2 = 0x4934;
  Type1.Uuid.Data3 = 0x5052;
  Type1.Uuid.Data4[0] = 0x4F;
  Type1.Uuid.Data4[1] = 0x41;
  Type1.Uuid.Data4[2] = 0x37;
  Type1.Uuid.Data4[3] = 0x33;
  Type1.Uuid.Data4[4] = 0x33;
  Type1.Uuid.Data4[5] = 0x55;
  Type1.Uuid.Data4[6] = 0x45;
  Type1.Uuid.Data4[7] = 0x46;
  Type1.WakeUpType = SystemWakeupTypePowerSwitch;
  Type1.SKUNumber = 5;
  Type1.Family = 6;
  Status = AddRecord (Smbios, &Type1, sizeof (Type1), Type1Strings, ARRAY_SIZE (Type1Strings), NULL);
  if (EFI_ERROR (Status)) return Status;

  ZeroMem (&Type4, sizeof (Type4));
  Type4.Hdr.Type = SMBIOS_TYPE_PROCESSOR_INFORMATION;
  Type4.Socket = 1;
  Type4.ProcessorType = CentralProcessor;
  Type4.ProcessorFamily = ProcessorFamilyIndicatorFamily2;
  Type4.ProcessorManufacturer = 2;
  Type4.ProcessorVersion = 3;
  Type4.MaxSpeed = 2000;
  Type4.CurrentSpeed = 1014;
  Type4.Status = 0x41; // enabled and populated
  Type4.ProcessorUpgrade = ProcessorUpgradeNone;
  Type4.L1CacheHandle = 0xFFFF;
  Type4.L2CacheHandle = 0xFFFF;
  Type4.L3CacheHandle = 0xFFFF;
  Type4.SerialNumber = 4;
  Type4.AssetTag = 5;
  Type4.PartNumber = 6;
  Type4.CoreCount = 8;
  Type4.EnabledCoreCount = 8;
  Type4.ThreadCount = 8;
  Type4.ProcessorCharacteristics = BIT2 | BIT3 | BIT5;
  Type4.ProcessorFamily2 = ProcessorFamilyARMv8;
  Type4.CoreCount2 = 8;
  Type4.EnabledCoreCount2 = 8;
  Type4.ThreadCount2 = 8;
  Type4.ThreadEnabled = 8;
  Status = AddRecord (Smbios, &Type4, sizeof (Type4), Type4Strings, ARRAY_SIZE (Type4Strings), NULL);
  if (EFI_ERROR (Status)) return Status;

  ZeroMem (&Type16, sizeof (Type16));
  Type16.Hdr.Type = SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
  Type16.Location = MemoryArrayLocationSystemBoard;
  Type16.Use = MemoryArrayUseSystemMemory;
  Type16.MemoryErrorCorrection = MemoryErrorCorrectionNone;
  Type16.MaximumCapacity = 4 * 1024 * 1024; // KB
  Type16.MemoryErrorInformationHandle = 0xFFFE;
  Type16.NumberOfMemoryDevices = 1;
  Status = AddRecord (Smbios, &Type16, sizeof (Type16), NULL, 0, &MemoryArrayHandle);
  if (EFI_ERROR (Status)) return Status;

  ZeroMem (&Type17, sizeof (Type17));
  Type17.Hdr.Type = SMBIOS_TYPE_MEMORY_DEVICE;
  Type17.MemoryArrayHandle = MemoryArrayHandle;
  Type17.MemoryErrorInformationHandle = 0xFFFE;
  Type17.TotalWidth = 64;
  Type17.DataWidth = 64;
  Type17.Size = 4096;
  Type17.FormFactor = MemoryFormFactorRowOfChips;
  Type17.DeviceLocator = 1;
  Type17.BankLocator = 2;
  Type17.MemoryType = MemoryTypeLpddr5;
  Type17.Speed = 3600;
  Type17.Manufacturer = 3;
  Type17.SerialNumber = 4;
  Type17.AssetTag = 5;
  Type17.PartNumber = 6;
  Type17.ConfiguredMemoryClockSpeed = 3600;
  Type17.VolatileSize = 4ULL * 1024 * 1024 * 1024;
  Type17.LogicalSize = Type17.VolatileSize;
  Status = AddRecord (Smbios, &Type17, sizeof (Type17), Type17Strings, ARRAY_SIZE (Type17Strings), NULL);
  if (EFI_ERROR (Status)) return Status;

  ZeroMem (&Type19, sizeof (Type19));
  Type19.Hdr.Type = SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
  Type19.StartingAddress = 0x00100000; // 0x40000000 / 1KB
  Type19.EndingAddress = 0x004FFFFF;   // 0x13FFFFFFFF / 1KB
  Type19.MemoryArrayHandle = MemoryArrayHandle;
  Type19.PartitionWidth = 1;
  Status = AddRecord (Smbios, &Type19, sizeof (Type19), NULL, 0, NULL);
  if (EFI_ERROR (Status)) return Status;

  ZeroMem (&Type32, sizeof (Type32));
  Type32.Hdr.Type = SMBIOS_TYPE_SYSTEM_BOOT_INFORMATION;
  Type32.BootStatus = BootInformationStatusNoError;
  Status = AddRecord (Smbios, &Type32, sizeof (Type32), NULL, 0, NULL);
  if (EFI_ERROR (Status)) return Status;

  DEBUG ((DEBUG_INFO, "A733 SMBIOS10: installed platform/CPU/4GB LPDDR5 inventory\n"));
  return EFI_SUCCESS;
}
