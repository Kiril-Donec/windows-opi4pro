/** @file
  ACPI/PSCI/GIC and controlled USB hand-off tables for Allwinner A733.

  v53 keeps the proven EHCI path and exposes xHCI after core-first DWC3
  initialization produces a valid, halted controller. It no longer requires
  the optional UEFI USB stack to enumerate a port before publishing PNP0D10:
  Windows owns controller initialization after ExitBootServices. EHCI1 owns the external
  FE1.1s four-port USB2 hub and uses the standard ACPI PNP0D20 ID with the
  corrected GIC SPI-to-GSIV conversion (SPI 159 -> GSIV 191). DWC3/xHCI2 stays
  exposed as AWIN0030/PNP0D10 on GSIV 187. EHCI is published with _CCA=1 for
  Windows' atomic TD allocator; xHCI keeps the vendor-described _CCA=0.
  DBG2 describes the A733 DesignWare 16550-compatible UART using GAS subtype
  0x12, a 32-bit register stride, and DWORD MMIO accesses for Windows KD.
**/

#include <Uefi.h>

#include <IndustryStandard/Acpi65.h>
#include <IndustryStandard/ArmStdSmc.h>
#include <IndustryStandard/DebugPort2Table.h>
#include <IndustryStandard/SerialPortConsoleRedirectionTable.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/A733XhciReady.h>
#include <Protocol/A733GmacReady.h>

#include <Library/AmlLib/AmlLib.h>
#include <Library/ArmLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#define A733_ACPI_OEM_ID        "ORANGE"
#define A733_ACPI_OEM_REVISION  0x00000004
#define A733_ACPI_CREATOR_ID    SIGNATURE_32 ('E','D','K','2')
#define A733_ACPI_CREATOR_REV   0x00010000

#define A733_GICD_BASE          0x03400000ULL
#define A733_GICR_BASE          0x03460000ULL
#define A733_GICR_LENGTH        0x00100000U
#define A733_GICR_STRIDE        0x00020000ULL
#define A733_UART0_BASE         0x02500000ULL
#define A733_UART0_LENGTH       0x00000400U
#define A733_UART0_GSI          34U
#define A733_UART7_BASE         0x07080000ULL // R_UART0, 40-pin header pins 8/10
#define A733_UART7_GSI          (201U + 32U)  // GIC_SPI 201
#define A733_PIO_BASE            0x02000000U
#define A733_PIO_POW_MOD_SEL     (A733_PIO_BASE + 0x0040U)
#define A733_PIO_PF_BANK_BIT     BIT5

#define A733_EHCI1_BASE         0x04200000U
#define A733_EHCI1_LENGTH       0x00001000U
#define A733_EHCI1_GSI          (159U + 32U)
#define A733_XHCI2_BASE         0x06A00000U
#define A733_XHCI2_LENGTH       0x00100000U
#define A733_XHCI2_GSI          (155U + 32U)  // GIC_SPI 155 -> ACPI GSIV 187
#define A733_SDMMC0_BASE        0x04020000U
#define A733_SDMMC0_LENGTH      0x00001000U
#define A733_SDMMC0_GSI         (161U + 32U)  // GIC_SPI 161 -> ACPI GSIV 193
#define A733_GMAC0_BASE         0x04500000U
#define A733_GMAC0_LENGTH       0x00008000U
#define A733_GMAC0_WRAP_BASE    0x04508000U
#define A733_GMAC0_WRAP_LENGTH  0x00001000U
#define A733_GMAC0_GSI          (172U + 32U)
#define A733_GMAC0_LPI_GSI      (178U + 32U)
#define A733_GMAC0_TX0_GSI      (173U + 32U)
#define A733_GMAC0_RX0_GSI      (174U + 32U)
#define A733_PMU_GSI            23U
#define A733_VGIC_MAINT_GSI     25U

#define GICD_CTLR_OFFSET        0x0000
#define GICD_TYPER_OFFSET       0x0004
#define GICD_IIDR_OFFSET        0x0008
#define GICR_TYPER_OFFSET       0x0008
#define GICR_WAKER_OFFSET       0x0014

// Vendor DTS: level-sensitive, active-low PPIs. It explicitly contains
// arm,no-tick-in-suspend, so do not advertise ALWAYS_ON capability.
#define A733_TIMER_FLAGS  EFI_ACPI_6_5_GTDT_TIMER_FLAG_TIMER_INTERRUPT_POLARITY

#define PPTT_REVISION_ACPI_6_2  0x01

#pragma pack (1)
typedef struct {
  EFI_ACPI_6_5_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER  Header;
  // Variable: GICC[CpuCount], GICD, GICR.
} A733_MADT_HEADER;

typedef struct {
  EFI_ACPI_6_5_PROCESSOR_PROPERTIES_TOPOLOGY_TABLE_HEADER  Header;
  // Variable: package, cluster nodes, core nodes.
} A733_PPTT_HEADER;

typedef struct {
  EFI_ACPI_DEBUG_PORT_2_DESCRIPTION_TABLE          Table;
  EFI_ACPI_DBG2_DEBUG_DEVICE_INFORMATION_STRUCT    Device;
  EFI_ACPI_6_5_GENERIC_ADDRESS_STRUCTURE           BaseAddress;
  UINT32                                           AddressSize;
  CHAR8                                            NamespaceString[2];
} A733_DBG2_TABLE;
#pragma pack ()

STATIC CONST UINT64  mMpidr[8] = {
  0x000ULL, 0x100ULL, 0x200ULL, 0x300ULL,
  0x400ULL, 0x500ULL, 0x600ULL, 0x700ULL
};

STATIC CONST CHAR8  *mCpuNames[8] = {
  "CP00", "CP01", "CP02", "CP03", "CP04", "CP05", "CP06", "CP07"
};

STATIC EFI_EVENT       mReadyToBootEvent;
STATIC EFI_EVENT       mExitBootServicesEvent;
STATIC volatile UINT64 mSecondaryCookie[8];
STATIC A733_XHCI_READY_PROTOCOL *mXhciReady;
STATIC A733_GMAC_READY_PROTOCOL *mGmacReady;

STATIC
BOOLEAN
ShouldExposeXhci2ToWindows (
  VOID
  )
{
  // Live KD on v52 proved CAPLENGTH=0x30, CNR clear and HCHalted while the
  // stricter PlatformReady flag remained false because ConnectFirmwareXhci()
  // did not finish. Hiding the ACPI node in that state left Windows with only
  // PNP0D20/usbehci. The inbox USBXHCI driver needs the valid halted hardware,
  // not successful pre-boot USB enumeration by EDK2.
  return FixedPcdGetBool (PcdExposeXhci2ToWindows) &&
         (mXhciReady != NULL) &&
         mXhciReady->CnrClear &&
         mXhciReady->Halted;
}

VOID
EFIAPI
A733SecondaryEntry (
  VOID
  );

STATIC
UINTN
GetCpuCount (
  VOID
  )
{
  UINTN  Count;

  Count = FixedPcdGet32 (PcdAcpiCpuCount);
  if ((Count == 0) || (Count > ARRAY_SIZE (mMpidr))) {
    Count = ARRAY_SIZE (mMpidr);
  }

  return Count;
}

STATIC
VOID
InitHeader (
  OUT EFI_ACPI_DESCRIPTION_HEADER  *Header,
  IN  UINT32                       Signature,
  IN  UINT32                       Length,
  IN  UINT8                        Revision,
  IN  UINT64                       OemTableId
  )
{
  ZeroMem (Header, sizeof (*Header));
  Header->Signature = Signature;
  Header->Length = Length;
  Header->Revision = Revision;
  CopyMem (Header->OemId, A733_ACPI_OEM_ID, sizeof (Header->OemId));
  Header->OemTableId = OemTableId;
  Header->OemRevision = A733_ACPI_OEM_REVISION;
  Header->CreatorId = A733_ACPI_CREATOR_ID;
  Header->CreatorRevision = A733_ACPI_CREATOR_REV;
}

STATIC
EFI_STATUS
InstallOne (
  IN EFI_ACPI_TABLE_PROTOCOL  *Acpi,
  IN VOID                     *Table,
  IN UINTN                    Size,
  IN CONST CHAR8              *Name
  )
{
  EFI_STATUS  Status;
  UINTN       Key;

  Key = 0;
  Status = Acpi->InstallAcpiTable (Acpi, Table, Size, &Key);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
          "A733 ACPI53: install %a size=%u key=%Lu -> %r\n",
          Name, (UINT32)Size, (UINT64)Key, Status));
  return Status;
}

STATIC
EFI_STATUS
AddMmioAcpiDevice (
  IN AML_OBJECT_NODE_HANDLE  Scope,
  IN CONST CHAR8             *Name,
  IN CONST CHAR8             *HardwareId,
  IN CONST CHAR8             *CompatibleId OPTIONAL,
  IN UINT64                  Uid,
  IN UINT32                  Base,
  IN UINT32                  Length,
  IN BOOLEAN                 CacheCoherent,
  IN UINT32                  Interrupt
  )
{
  EFI_STATUS              Status;
  AML_OBJECT_NODE_HANDLE  Device;
  AML_OBJECT_NODE_HANDLE  Crs;
  UINT32                  Irq;

  Device = NULL;
  Crs = NULL;
  Irq = Interrupt;

  Status = AmlCodeGenDevice (Name, Scope, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenNameString ("_HID", HardwareId, Device, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CompatibleId != NULL) {
    Status = AmlCodeGenNameString ("_CID", CompatibleId, Device, NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = AmlCodeGenNameInteger ("_UID", Uid, Device, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenNameInteger ("_STA", 0x0F, Device, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Windows' inbox ARM64 EHCI miniport uses LSE atomics in its common-buffer TD
  // allocator. Mapping that pool as Device memory (_CCA=0) raises a synchronous
  // external abort at EHCI_AllocTd. Publish EHCI as coherent so Windows maps its
  // software-visible TD pool as Normal memory; keep other devices non-coherent.
  Status = AmlCodeGenNameInteger ("_CCA", CacheCoherent ? 1 : 0, Device, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenNameResourceTemplate ("_CRS", Device, &Crs);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenRdMemory32Fixed (TRUE, Base, Length, Crs, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // AmlLib booleans are EdgeTriggered, ActiveLow, Shared. The vendor DTS
  // specifies level-high and this controller is not shared.
  return AmlCodeGenRdInterrupt (
           TRUE,   // ResourceConsumer
           FALSE,  // EdgeTriggered=FALSE -> level triggered
           FALSE,  // ActiveLow=FALSE -> active high
           FALSE,  // Shared=FALSE -> exclusive
           &Irq,
           1,
           Crs,
           NULL
           );
}

STATIC
EFI_STATUS
AddGmacAcpiDevice (
  IN AML_OBJECT_NODE_HANDLE  Scope
  )
{
  EFI_STATUS              Status;
  AML_OBJECT_NODE_HANDLE  Device;
  AML_OBJECT_NODE_HANDLE  Crs;
  UINT32                  Irqs[4];

  Device = NULL;
  Crs = NULL;
  Irqs[0] = A733_GMAC0_GSI;
  Irqs[1] = A733_GMAC0_LPI_GSI;
  Irqs[2] = A733_GMAC0_TX0_GSI;
  Irqs[3] = A733_GMAC0_RX0_GSI;

  Status = AmlCodeGenDevice ("ETH0", Scope, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenNameString ("_HID", "AWIN0040", Device, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = AmlCodeGenNameInteger ("_UID", 0, Device, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = AmlCodeGenNameInteger ("_STA", 0x0F, Device, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = AmlCodeGenNameInteger ("_CCA", 0, Device, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = AmlCodeGenNameResourceTemplate ("_CRS", Device, &Crs);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = AmlCodeGenRdMemory32Fixed (TRUE, A733_GMAC0_BASE, A733_GMAC0_LENGTH, Crs, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = AmlCodeGenRdMemory32Fixed (TRUE, A733_GMAC0_WRAP_BASE, A733_GMAC0_WRAP_LENGTH, Crs, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // AmlCodeGenRdInterrupt with IrqCount > 1 overflows the stack and generates an invalid
  // AML Extended Interrupt descriptor because AmlResourceDataCodeGen.c uses a fixed-size
  // EFI_ACPI_EXTENDED_INTERRUPT_DESCRIPTOR struct on stack.
  // Generate each interrupt as an individual Extended Interrupt resource descriptor.
  for (UINTN Index = 0; Index < ARRAY_SIZE (Irqs); Index++) {
    Status = AmlCodeGenRdInterrupt (
               TRUE,   // ResourceConsumer
               FALSE,  // EdgeTriggered=FALSE -> level triggered
               FALSE,  // ActiveLow=FALSE -> active high
               FALSE,  // Shared=FALSE -> exclusive
               &Irqs[Index],
               1,
               Crs,
               NULL
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
BuildCpuDsdt (
  OUT EFI_ACPI_DESCRIPTION_HEADER  **Dsdt
  )
{
  EFI_STATUS              Status;
  AML_ROOT_NODE_HANDLE    Root;
  AML_OBJECT_NODE_HANDLE  Scope;
  AML_OBJECT_NODE_HANDLE  Cpu;
  UINTN                   Index;
  UINTN                   CpuCount;

  Root = NULL;
  Scope = NULL;
  Cpu = NULL;
  *Dsdt = NULL;
  CpuCount = GetCpuCount ();

  // A733 uses the NCAT3 PIO power-select register at PIO + 0x40.
  // Linux BIAS_VOLTAGE_PIO_POW_MODE_SEL semantics are: a set bank bit
  // selects the <=1.8 V I/O bias mode; a clear bit selects the >1.8 V mode.
  // PF is the SD/MMC0 bank. Log only: do not switch it blindly in firmware.
  {
    UINT32 PowSel;
    PowSel = MmioRead32 (A733_PIO_POW_MOD_SEL);
    DEBUG ((DEBUG_INFO,
            "A733 ACPI59: PIO_POW_MOD_SEL=%08x PF(SMHC0)=%a\n",
            PowSel,
            ((PowSel & A733_PIO_PF_BANK_BIT) != 0U) ? "<=1.8V" : ">1.8V"));
  }

  DEBUG ((DEBUG_INFO,
          "A733 ACPI59: GMAC GSIVs main=%u lpi=%u tx0=%u rx0=%u\n",
          A733_GMAC0_GSI, A733_GMAC0_LPI_GSI,
          A733_GMAC0_TX0_GSI, A733_GMAC0_RX0_GSI));

  Status = AmlCodeGenDefinitionBlock (
             "DSDT",
             A733_ACPI_OEM_ID,
             "OPI4PRO ",
             A733_ACPI_OEM_REVISION,
             &Root
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenScope ("\\_SB_", Root, &Scope);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  for (Index = 0; Index < CpuCount; Index++) {
    Cpu = NULL;
    Status = AmlCodeGenDevice (mCpuNames[Index], Scope, &Cpu);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    Status = AmlCodeGenNameString ("_HID", "ACPI0007", Cpu, NULL);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    Status = AmlCodeGenNameInteger ("_UID", Index, Cpu, NULL);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    Status = AmlCodeGenNameInteger ("_STA", 0x0F, Cpu, NULL);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  // The board's external FE1.1s four-port USB2 hub is physically enumerated by
  // EHCI1 in UEFI. Earlier EHCI ACPI experiments used raw SPI 159 as an ACPI
  // interrupt number; ACPI GSIVs for SPIs include the 32 SGI/PPI entries, so
  // the correct interrupt is 191. Publish the standard embedded EHCI ID so the
  // Microsoft inbox usbehci.sys driver can bind without modifying Windows.
  if (FixedPcdGetBool (PcdExposeEhci1ToWindows)) {
    Status = AddMmioAcpiDevice (
               Scope, "EH01", "PNP0D20", NULL, 1,
               A733_EHCI1_BASE, A733_EHCI1_LENGTH, TRUE, A733_EHCI1_GSI);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  // Publish DWC3/xHCI2 whenever the core has a valid capability window and is
  // halted with CNR clear. Windows will perform the runtime host-controller
  // initialization even when EDK2 did not enumerate a pre-boot USB device.
  if (ShouldExposeXhci2ToWindows ()) {
    Status = AddMmioAcpiDevice (
               Scope, "XHC2", "AWIN0030", "PNP0D10", 2,
               A733_XHCI2_BASE, A733_XHCI2_LENGTH, FALSE, A733_XHCI2_GSI);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  // The Allwinner MMC v5p3x controller is not SDHCI-compatible. A733SdMmc is a
  // dedicated Windows ARM64 SDPORT miniport for this AWIN0001 device. ACPI GSIV
  // includes the 32 SGI/PPI entries, hence raw GIC SPI 161 is published as 193.
  if (FixedPcdGetBool (PcdExposeSdMmc0ToWindows)) {
    Status = AddMmioAcpiDevice (
               Scope, "SDC0", "AWIN0001", NULL, 0,
               A733_SDMMC0_BASE, A733_SDMMC0_LENGTH, FALSE, A733_SDMMC0_GSI);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  // The v59 GMAC board driver owns shared pins/clocks/resets. Expose only the
  // DWMAC and wrapper windows after its readback probe succeeds.
  if ((mGmacReady != NULL) && mGmacReady->PlatformReady) {
    Status = AddGmacAcpiDevice (Scope);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "A733 ACPI59: AddGmacAcpiDevice failed: %r\n", Status));
      Print (L"\r\n[WARNING] A733 GMAC ACPI init failed: %r\r\n", Status);
      Print (L"Continuing boot without onboard Ethernet in 5 seconds...\r\n");
      for (UINTN Sec = 5; Sec > 0; Sec--) {
        Print (L"Continuing in %u...\r\n", Sec);
        gBS->Stall (1000000);
      }
    }
  } else if ((mGmacReady != NULL) && !mGmacReady->PlatformReady) {
    DEBUG ((DEBUG_WARN, "A733 ACPI59: GMAC preflight probe failed on hardware\n"));
    Print (L"\r\n[WARNING] A733 GMAC preflight probe failed on hardware!\r\n");
    Print (L"Continuing boot without onboard Ethernet in 5 seconds...\r\n");
    for (UINTN Sec = 5; Sec > 0; Sec--) {
      Print (L"Continuing in %u...\r\n", Sec);
      gBS->Stall (1000000);
    }
  }

  Status = AmlSerializeDefinitionBlock (Root, Dsdt);
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO,
            "A733 ACPI53: generated AML DSDT size=%u CPUs=%u EHCI1=%u EHCI-CCA=1 EHCI-GSIV=%u XHCI2=%u XHCI-GSIV=%u SDC0=%u\n",
            (*Dsdt)->Length, (UINT32)CpuCount,
            FixedPcdGetBool (PcdExposeEhci1ToWindows) ? 1U : 0U,
            A733_EHCI1_GSI,
            ShouldExposeXhci2ToWindows () ? 1U : 0U,
            A733_XHCI2_GSI,
            FixedPcdGetBool (PcdExposeSdMmc0ToWindows) ? 1U : 0U));
  }

Done:
  if (Root != NULL) {
    AmlDeleteTree (Root);
  }

  if (EFI_ERROR (Status) && (*Dsdt != NULL)) {
    FreePool (*Dsdt);
    *Dsdt = NULL;
  }

  return Status;
}

STATIC
EFI_STATUS
BuildMadt (
  OUT VOID   **Table,
  OUT UINTN  *TableSize
  )
{
  A733_MADT_HEADER                          *Madt;
  EFI_ACPI_6_5_GIC_STRUCTURE               *Gicc;
  EFI_ACPI_6_5_GIC_DISTRIBUTOR_STRUCTURE   *Gicd;
  EFI_ACPI_6_5_GICR_STRUCTURE              *Gicr;
  UINT8                                     *Cursor;
  UINTN                                     CpuCount;
  UINTN                                     Index;
  UINTN                                     Size;

  CpuCount = GetCpuCount ();
  Size = sizeof (*Madt) + (CpuCount * sizeof (*Gicc)) + sizeof (*Gicd) + sizeof (*Gicr);
  Madt = AllocateZeroPool (Size);
  if (Madt == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  InitHeader (&Madt->Header.Header,
              EFI_ACPI_6_5_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE,
              (UINT32)Size,
              EFI_ACPI_6_5_MULTIPLE_APIC_DESCRIPTION_TABLE_REVISION,
              SIGNATURE_64 ('O','P','I','M','A','D','T',' '));
  Madt->Header.Flags = 0;

  Cursor = (UINT8 *)(Madt + 1);
  Gicc = (EFI_ACPI_6_5_GIC_STRUCTURE *)Cursor;
  for (Index = 0; Index < CpuCount; Index++) {
    Gicc[Index].Type = EFI_ACPI_6_5_GIC;
    Gicc[Index].Length = sizeof (EFI_ACPI_6_5_GIC_STRUCTURE);
    Gicc[Index].CPUInterfaceNumber = 0;
    Gicc[Index].AcpiProcessorUid = (UINT32)Index;
    Gicc[Index].Flags = EFI_ACPI_6_5_GIC_ENABLED;
    Gicc[Index].ParkingProtocolVersion = 0;
    Gicc[Index].PerformanceInterruptGsiv = A733_PMU_GSI;
    Gicc[Index].ParkedAddress = 0;
    Gicc[Index].PhysicalBaseAddress = 0;
    Gicc[Index].GICV = 0;
    Gicc[Index].GICH = 0;
    Gicc[Index].VGICMaintenanceInterrupt = A733_VGIC_MAINT_GSI;
    Gicc[Index].GICRBaseAddress = 0;
    Gicc[Index].MPIDR = mMpidr[Index];
    Gicc[Index].ProcessorPowerEfficiencyClass = (Index < 6) ? 0 : 1;
  }

  Cursor += CpuCount * sizeof (*Gicc);
  Gicd = (EFI_ACPI_6_5_GIC_DISTRIBUTOR_STRUCTURE *)Cursor;
  Gicd->Type = EFI_ACPI_6_5_GICD;
  Gicd->Length = sizeof (*Gicd);
  Gicd->GicId = 0;
  Gicd->PhysicalBaseAddress = A733_GICD_BASE;
  Gicd->SystemVectorBase = 0;
  Gicd->GicVersion = EFI_ACPI_6_5_GIC_V3;

  Cursor += sizeof (*Gicd);
  Gicr = (EFI_ACPI_6_5_GICR_STRUCTURE *)Cursor;
  Gicr->Type = EFI_ACPI_6_5_GICR;
  Gicr->Length = sizeof (*Gicr);
  Gicr->DiscoveryRangeBaseAddress = A733_GICR_BASE;
  Gicr->DiscoveryRangeLength = A733_GICR_LENGTH;

  *Table = Madt;
  *TableSize = Size;
  return EFI_SUCCESS;
}

STATIC
VOID
InitPpttProcessorNode (
  OUT EFI_ACPI_6_5_PPTT_STRUCTURE_PROCESSOR  *Node,
  IN  BOOLEAN                                PhysicalPackage,
  IN  BOOLEAN                                ProcessorIdValid,
  IN  BOOLEAN                                IsLeaf,
  IN  UINT32                                 Parent,
  IN  UINT32                                 ProcessorId
  )
{
  ZeroMem (Node, sizeof (*Node));
  Node->Type = EFI_ACPI_6_5_PPTT_TYPE_PROCESSOR;
  Node->Length = sizeof (*Node);
  Node->Flags.PhysicalPackage = PhysicalPackage ? 1 : 0;
  Node->Flags.AcpiProcessorIdValid = ProcessorIdValid ? 1 : 0;
  Node->Flags.ProcessorIsAThread = 0;
  Node->Flags.NodeIsALeaf = IsLeaf ? 1 : 0;
  Node->Flags.IdenticalImplementation = 0;
  Node->Parent = Parent;
  Node->AcpiProcessorId = ProcessorId;
  Node->NumberOfPrivateResources = 0;
}

STATIC
EFI_STATUS
BuildPptt (
  OUT VOID   **Table,
  OUT UINTN  *TableSize
  )
{
  A733_PPTT_HEADER                            *Pptt;
  EFI_ACPI_6_5_PPTT_STRUCTURE_PROCESSOR       *Nodes;
  UINTN                                       CpuCount;
  UINTN                                       ClusterCount;
  UINTN                                       NodeCount;
  UINTN                                       Size;
  UINTN                                       Index;
  UINT32                                      PackageOffset;
  UINT32                                      LittleClusterOffset;
  UINT32                                      BigClusterOffset;
  UINT32                                      ParentOffset;

  CpuCount = GetCpuCount ();
  ClusterCount = (CpuCount > 6) ? 2 : 1;
  NodeCount = 1 + ClusterCount + CpuCount;
  Size = sizeof (*Pptt) + (NodeCount * sizeof (*Nodes));
  Pptt = AllocateZeroPool (Size);
  if (Pptt == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  InitHeader (&Pptt->Header.Header,
              EFI_ACPI_6_5_PROCESSOR_PROPERTIES_TOPOLOGY_TABLE_STRUCTURE_SIGNATURE,
              (UINT32)Size,
              PPTT_REVISION_ACPI_6_2,
              SIGNATURE_64 ('O','P','I','P','P','T','T',' '));

  Nodes = (EFI_ACPI_6_5_PPTT_STRUCTURE_PROCESSOR *)(Pptt + 1);
  PackageOffset = sizeof (*Pptt);
  LittleClusterOffset = PackageOffset + sizeof (*Nodes);
  BigClusterOffset = LittleClusterOffset + sizeof (*Nodes);

  InitPpttProcessorNode (&Nodes[0], TRUE, FALSE, FALSE, 0, 0);
  InitPpttProcessorNode (&Nodes[1], FALSE, FALSE, FALSE, PackageOffset, 0);
  if (ClusterCount == 2) {
    InitPpttProcessorNode (&Nodes[2], FALSE, FALSE, FALSE, PackageOffset, 0);
  }

  for (Index = 0; Index < CpuCount; Index++) {
    ParentOffset = ((ClusterCount == 2) && (Index >= 6)) ? BigClusterOffset : LittleClusterOffset;
    InitPpttProcessorNode (
      &Nodes[1 + ClusterCount + Index],
      FALSE,
      TRUE,
      TRUE,
      ParentOffset,
      (UINT32)Index
      );
  }

  *Table = Pptt;
  *TableSize = Size;
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
ChecksumOk (
  IN CONST EFI_ACPI_DESCRIPTION_HEADER  *Header
  )
{
  return (Header != NULL) && (CalculateSum8 ((CONST UINT8 *)Header, Header->Length) == 0);
}

STATIC
UINTN
CountBytes (
  IN CONST UINT8  *Buffer,
  IN UINTN        BufferSize,
  IN CONST UINT8  *Pattern,
  IN UINTN        PatternSize
  )
{
  UINTN  Offset;
  UINTN  Count;

  if ((PatternSize == 0) || (BufferSize < PatternSize)) {
    return 0;
  }

  Count = 0;
  for (Offset = 0; Offset <= BufferSize - PatternSize; Offset++) {
    if (CompareMem (Buffer + Offset, Pattern, PatternSize) == 0) {
      Count++;
    }
  }

  return Count;
}

STATIC
VOID
ValidatePublishedTables (
  VOID
  )
{
  EFI_ACPI_DESCRIPTION_HEADER                   *Dsdt;
  EFI_ACPI_6_5_FIXED_ACPI_DESCRIPTION_TABLE     *Fadt;
  EFI_ACPI_DESCRIPTION_HEADER                   *Madt;
  EFI_ACPI_6_5_GENERIC_TIMER_DESCRIPTION_TABLE  *Gtdt;
  EFI_ACPI_DESCRIPTION_HEADER                   *Spcr;
  EFI_ACPI_DESCRIPTION_HEADER                   *Dbg2;
  EFI_ACPI_DESCRIPTION_HEADER                   *Pptt;
  UINTN                                         HidCount;
  UINTN                                         EhciCount;
  UINTN                                         XhciCount;
  UINTN                                         SdCount;

  Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)EfiLocateFirstAcpiTable (EFI_ACPI_6_5_DIFFERENTIATED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE);
  Fadt = (EFI_ACPI_6_5_FIXED_ACPI_DESCRIPTION_TABLE *)EfiLocateFirstAcpiTable (
                                                            EFI_ACPI_6_5_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE);
  Madt = (EFI_ACPI_DESCRIPTION_HEADER *)EfiLocateFirstAcpiTable (EFI_ACPI_6_5_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE);
  Gtdt = (EFI_ACPI_6_5_GENERIC_TIMER_DESCRIPTION_TABLE *)EfiLocateFirstAcpiTable (
                                                              EFI_ACPI_6_5_GENERIC_TIMER_DESCRIPTION_TABLE_SIGNATURE);
  Spcr = (EFI_ACPI_DESCRIPTION_HEADER *)EfiLocateFirstAcpiTable (EFI_ACPI_6_5_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_SIGNATURE);
  Dbg2 = (EFI_ACPI_DESCRIPTION_HEADER *)EfiLocateFirstAcpiTable (EFI_ACPI_6_5_DEBUG_PORT_2_TABLE_SIGNATURE);
  Pptt = (EFI_ACPI_DESCRIPTION_HEADER *)EfiLocateFirstAcpiTable (EFI_ACPI_6_5_PROCESSOR_PROPERTIES_TOPOLOGY_TABLE_STRUCTURE_SIGNATURE);

  HidCount = (Dsdt == NULL) ? 0 : CountBytes ((CONST UINT8 *)Dsdt, Dsdt->Length,
                                               (CONST UINT8 *)"ACPI0007", 8);
  EhciCount = (Dsdt == NULL) ? 0 : CountBytes ((CONST UINT8 *)Dsdt, Dsdt->Length,
                                                (CONST UINT8 *)"PNP0D20", 8);
  XhciCount = (Dsdt == NULL) ? 0 : CountBytes ((CONST UINT8 *)Dsdt, Dsdt->Length,
                                                (CONST UINT8 *)"PNP0D10", 8);
  SdCount = (Dsdt == NULL) ? 0 : CountBytes ((CONST UINT8 *)Dsdt, Dsdt->Length,
                                              (CONST UINT8 *)"AWIN0001", 8);
  DEBUG ((DEBUG_INFO,
          "A733 ACPI53 SELFTEST: CPUs=%u DSDT=%a CPU-HID=%u EHCI-HID=%u XHCI-CID=%u SD-HID=%u FADT=%a MADT=%a GTDT=%a SPCR=%a DBG2=%a PPTT=%a\n",
          (UINT32)GetCpuCount (),
          ChecksumOk (Dsdt) ? "OK" : "BAD",
          (UINT32)HidCount,
          (UINT32)EhciCount,
          (UINT32)XhciCount,
          (UINT32)SdCount,
          ChecksumOk ((EFI_ACPI_DESCRIPTION_HEADER *)Fadt) ? "OK" : "BAD",
          ChecksumOk (Madt) ? "OK" : "BAD",
          ChecksumOk ((EFI_ACPI_DESCRIPTION_HEADER *)Gtdt) ? "OK" : "BAD",
          ChecksumOk (Spcr) ? "OK" : "BAD",
          ChecksumOk (Dbg2) ? "OK" : "BAD",
          ChecksumOk (Pptt) ? "OK" : "BAD"));
  DEBUG ((DEBUG_INFO,
          "A733 ACPI53 SELFTEST: FADT flags=%08x armboot=%04x GTDT flags=%08x/%08x/%08x/%08x\n",
          (Fadt != NULL) ? Fadt->Flags : 0,
          (Fadt != NULL) ? Fadt->ArmBootArch : 0,
          (Gtdt != NULL) ? Gtdt->SecurePL1TimerFlags : 0,
          (Gtdt != NULL) ? Gtdt->NonSecurePL1TimerFlags : 0,
          (Gtdt != NULL) ? Gtdt->VirtualTimerFlags : 0,
          (Gtdt != NULL) ? Gtdt->NonSecurePL2TimerFlags : 0));
}

STATIC
INT64
PsciCall (
  IN UINT64  Function,
  IN UINT64  Arg1,
  IN UINT64  Arg2,
  IN UINT64  Arg3
  )
{
  ARM_SMC_ARGS  Args;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = Function;
  Args.Arg1 = Arg1;
  Args.Arg2 = Arg2;
  Args.Arg3 = Arg3;
  ArmCallSmc (&Args);
  return (INT64)Args.Arg0;
}

STATIC
VOID
DumpPsciFeatures (
  VOID
  )
{
  STATIC CONST UINT32  FunctionIds[] = {
    ARM_SMC_ID_PSCI_CPU_ON_AARCH64,
    ARM_SMC_ID_PSCI_CPU_OFF,
    ARM_SMC_ID_PSCI_AFFINITY_INFO_AARCH64,
    ARM_SMC_ID_PSCI_SYSTEM_RESET,
    ARM_SMC_ID_PSCI_SYSTEM_OFF
  };
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (FunctionIds); Index++) {
    DEBUG ((DEBUG_INFO,
            "A733 HAL19: PSCI_FEATURES fn=%08x -> %Ld\n",
            FunctionIds[Index],
            PsciCall (ARM_SMC_ID_PSCI_FEATURES, FunctionIds[Index], 0, 0)));
  }
}

STATIC
VOID
TestSecondaryCpus (
  VOID
  )
{
  UINTN   Index;
  UINTN   Poll;
  INT64   Result;
  UINT64  State;

  if (!FixedPcdGetBool (PcdPsciCpuOnSelfTest)) {
    DEBUG ((DEBUG_INFO, "A733 HAL19: PSCI CPU_ON round-trip disabled for this image\n"));
    return;
  }

  WriteBackDataCacheRange ((VOID *)(UINTN)A733SecondaryEntry, 64);
  for (Index = 1; Index < ARRAY_SIZE (mMpidr); Index++) {
    mSecondaryCookie[Index] = 0;
    WriteBackDataCacheRange ((VOID *)&mSecondaryCookie[Index], sizeof (mSecondaryCookie[Index]));

    Result = PsciCall (
               ARM_SMC_ID_PSCI_CPU_ON_AARCH64,
               mMpidr[Index],
               (UINT64)(UINTN)A733SecondaryEntry,
               (UINT64)(UINTN)&mSecondaryCookie[Index]
               );

    for (Poll = 0; (Poll < 1000) && (mSecondaryCookie[Index] == 0); Poll++) {
      InvalidateDataCacheRange ((VOID *)&mSecondaryCookie[Index], sizeof (mSecondaryCookie[Index]));
      gBS->Stall (100);
    }

    State = (UINT64)PsciCall (ARM_SMC_ID_PSCI_AFFINITY_INFO_AARCH64, mMpidr[Index], 0, 0);
    DEBUG ((DEBUG_INFO,
            "A733 HAL19: CPU_ON test CPU%u MPIDR=%Lx ret=%Ld cookie=%Lu (1=entered/off,2=CPU_OFF-returned) final-affinity=%Ld wait-us=%u\n",
            (UINT32)Index,
            mMpidr[Index],
            Result,
            mSecondaryCookie[Index],
            (INT64)State,
            (UINT32)(Poll * 100)));
  }
}

STATIC
VOID
DumpHalState (
  VOID
  )
{
  UINT64  Counter0;
  UINT64  Counter1;
  UINT64  GicrTyper;
  UINTN   Index;

  Counter0 = ArmReadCntPct ();
  gBS->Stall (1000);
  Counter1 = ArmReadCntPct ();

  DEBUG ((DEBUG_INFO,
          "A733 HAL19: CurrentEL=0x%Lx MPIDR=0x%Lx CNTFRQ=%u CNTPCT-delta-1ms=%Lu\n",
          (UINT64)ArmReadCurrentEL (), ArmReadMpidr (), ArmReadCntFrq (), Counter1 - Counter0));
  DEBUG ((DEBUG_INFO,
          "A733 HAL19: GICD CTLR=%08x TYPER=%08x IIDR=%08x\n",
          MmioRead32 (A733_GICD_BASE + GICD_CTLR_OFFSET),
          MmioRead32 (A733_GICD_BASE + GICD_TYPER_OFFSET),
          MmioRead32 (A733_GICD_BASE + GICD_IIDR_OFFSET)));

  for (Index = 0; Index < ARRAY_SIZE (mMpidr); Index++) {
    GicrTyper = MmioRead64 (A733_GICR_BASE + (Index * A733_GICR_STRIDE) + GICR_TYPER_OFFSET);
    DEBUG ((DEBUG_INFO,
            "A733 HAL19: GICR[%u] TYPER=%016Lx WAKER=%08x affinity=%08x last=%u\n",
            (UINT32)Index,
            GicrTyper,
            MmioRead32 (A733_GICR_BASE + (Index * A733_GICR_STRIDE) + GICR_WAKER_OFFSET),
            (UINT32)(GicrTyper >> 32),
            (UINT32)((GicrTyper >> 4) & 1)));
  }

  DEBUG ((DEBUG_INFO, "A733 HAL19: PSCI_VERSION=%08x\n",
          (UINT32)PsciCall (ARM_SMC_ID_PSCI_VERSION, 0, 0, 0)));
  DumpPsciFeatures ();
  TestSecondaryCpus ();
}

STATIC
VOID
EFIAPI
ReadyToBootCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DEBUG ((DEBUG_INFO, "A733 HAL19: ReadyToBoot reached\n"));
  ValidatePublishedTables ();
  DumpHalState ();
}

STATIC
VOID
EFIAPI
ExitBootServicesCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DEBUG ((DEBUG_ERROR,
          "A733 HAL19: ExitBootServices handoff GICD_CTLR=%08x CNTP_CTL=%Lx CNTV_CTL=%Lx\n",
          MmioRead32 (A733_GICD_BASE + GICD_CTLR_OFFSET),
          ArmReadCntpCtl (),
          ArmReadCntvCtl ()));
}

EFI_STATUS
EFIAPI
A733AcpiDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                                      Status;
  EFI_ACPI_TABLE_PROTOCOL                         *Acpi;
  EFI_ACPI_DESCRIPTION_HEADER                     *Dsdt;
  EFI_ACPI_6_5_FIXED_ACPI_DESCRIPTION_TABLE       Fadt;
  VOID                                            *Madt;
  UINTN                                           MadtSize;
  EFI_ACPI_6_5_GENERIC_TIMER_DESCRIPTION_TABLE    Gtdt;
  EFI_ACPI_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE  Spcr;
  A733_DBG2_TABLE                                 Dbg2;
  VOID                                            *Pptt;
  UINTN                                           PpttSize;

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&Acpi);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LocateProtocol (&gA733XhciReadyProtocolGuid, NULL, (VOID **)&mXhciReady);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  DEBUG ((ShouldExposeXhci2ToWindows () ? DEBUG_INFO : DEBUG_WARN,
          "A733 ACPI53: XHCI ready protocol platform=%u windows=%u pma=%u cnr=%u halted=%u uefi=%u port=%u\n",
          mXhciReady->PlatformReady ? 1U : 0U,
          ShouldExposeXhci2ToWindows () ? 1U : 0U,
          mXhciReady->PmaReady ? 1U : 0U,
          mXhciReady->CnrClear ? 1U : 0U,
          mXhciReady->Halted ? 1U : 0U,
          mXhciReady->UefiDriverReady ? 1U : 0U,
          mXhciReady->PortConnected ? 1U : 0U));

  Status = gBS->LocateProtocol (&gA733GmacReadyProtocolGuid, NULL, (VOID **)&mGmacReady);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733 ACPI59: GMAC ready protocol not found: %r\n", Status));
    mGmacReady = NULL;
  } else {
    DEBUG ((mGmacReady->PlatformReady ? DEBUG_INFO : DEBUG_ERROR,
            "A733 ACPI59: GMAC ready=%u wrapper=%08x version=%08x features=%08x/%08x/%08x/%08x\n",
            mGmacReady->PlatformReady ? 1U : 0U,
            mGmacReady->WrapperConfig, mGmacReady->CoreVersion,
            mGmacReady->HardwareFeature[0], mGmacReady->HardwareFeature[1],
            mGmacReady->HardwareFeature[2], mGmacReady->HardwareFeature[3]));
  }

  Dsdt = NULL;
  Status = BuildCpuDsdt (&Dsdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = InstallOne (Acpi, Dsdt, Dsdt->Length, "DSDT-CPU");
  FreePool (Dsdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Fadt, sizeof (Fadt));
  InitHeader (&Fadt.Header,
              EFI_ACPI_6_5_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE,
              sizeof (Fadt),
              EFI_ACPI_6_5_FIXED_ACPI_DESCRIPTION_TABLE_REVISION,
              SIGNATURE_64 ('O','P','I','F','A','D','T',' '));
  Fadt.PreferredPmProfile = EFI_ACPI_6_5_PM_PROFILE_APPLIANCE_PC;
  Fadt.IaPcBootArch = EFI_ACPI_6_5_MSI_NOT_SUPPORTED;
  Fadt.Flags = EFI_ACPI_6_5_WBINVD | EFI_ACPI_6_5_SLP_BUTTON | EFI_ACPI_6_5_HW_REDUCED_ACPI;
  Fadt.ArmBootArch = EFI_ACPI_6_5_ARM_PSCI_COMPLIANT;
  Fadt.MinorVersion = EFI_ACPI_6_5_FIXED_ACPI_DESCRIPTION_TABLE_MINOR_REVISION;
  Status = InstallOne (Acpi, &Fadt, sizeof (Fadt), "FADT");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Madt = NULL;
  Status = BuildMadt (&Madt, &MadtSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = InstallOne (Acpi, Madt, MadtSize, "MADT-GICv3-v15");
  FreePool (Madt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Gtdt, sizeof (Gtdt));
  InitHeader (&Gtdt.Header,
              EFI_ACPI_6_5_GENERIC_TIMER_DESCRIPTION_TABLE_SIGNATURE,
              sizeof (Gtdt),
              EFI_ACPI_6_5_GENERIC_TIMER_DESCRIPTION_TABLE_REVISION,
              SIGNATURE_64 ('O','P','I','G','T','D','T',' '));
  Gtdt.CntControlBasePhysicalAddress = MAX_UINT64;
  Gtdt.SecurePL1TimerGSIV = 29;
  Gtdt.SecurePL1TimerFlags = A733_TIMER_FLAGS;
  Gtdt.NonSecurePL1TimerGSIV = 30;
  Gtdt.NonSecurePL1TimerFlags = A733_TIMER_FLAGS;
  Gtdt.VirtualTimerGSIV = 27;
  Gtdt.VirtualTimerFlags = A733_TIMER_FLAGS;
  Gtdt.NonSecurePL2TimerGSIV = 26;
  Gtdt.NonSecurePL2TimerFlags = A733_TIMER_FLAGS;
  Gtdt.CntReadBasePhysicalAddress = MAX_UINT64;
  Gtdt.PlatformTimerCount = 0;
  Gtdt.PlatformTimerOffset = 0;
  Gtdt.VirtualPL2TimerGSIV = 0;
  Gtdt.VirtualPL2TimerFlags = 0;
  Status = InstallOne (Acpi, &Gtdt, sizeof (Gtdt), "GTDT-v15");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Spcr, sizeof (Spcr));
  InitHeader (&Spcr.Header,
              EFI_ACPI_6_5_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_SIGNATURE,
              sizeof (Spcr),
              EFI_ACPI_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_REVISION,
              SIGNATURE_64 ('O','P','I','S','P','C','R',' '));
  Spcr.InterfaceType = EFI_ACPI_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_INTERFACE_TYPE_16550_WITH_GAS;
  Spcr.BaseAddress.AddressSpaceId = EFI_ACPI_6_5_SYSTEM_MEMORY;
  Spcr.BaseAddress.RegisterBitWidth = 32;
  Spcr.BaseAddress.AccessSize = EFI_ACPI_6_5_DWORD;
  // v60: PcdA733SpcrUart = 7 moves the Windows headless console to UART7 so that
  // KD (DBG2, UART0) and SAC can run at the same time.
  Spcr.BaseAddress.Address = (FixedPcdGet32 (PcdA733SpcrUart) == 7U) ? A733_UART7_BASE : A733_UART0_BASE;
  Spcr.InterruptType = EFI_ACPI_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_INTERRUPT_TYPE_GIC;
  Spcr.GlobalSystemInterrupt = (FixedPcdGet32 (PcdA733SpcrUart) == 7U) ? A733_UART7_GSI : A733_UART0_GSI;
  // v60: SPCR has no code above 115200; 0 means "as configured by firmware".
  Spcr.BaudRate = (FixedPcdGet32 (PcdSerialBaudRate) == 115200U) ?
                  EFI_ACPI_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_BAUD_RATE_115200 : 0;
  Spcr.Parity = EFI_ACPI_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_PARITY_NO_PARITY;
  Spcr.StopBits = EFI_ACPI_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_STOP_BITS_1;
  Spcr.TerminalType = EFI_ACPI_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_TERMINAL_TYPE_VT_UTF8;
  Spcr.PciDeviceId = 0xFFFF;
  Spcr.PciVendorId = 0xFFFF;
  Status = InstallOne (Acpi, &Spcr, sizeof (Spcr), "SPCR");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Dbg2, sizeof (Dbg2));
  InitHeader (&Dbg2.Table.Header,
              EFI_ACPI_6_5_DEBUG_PORT_2_TABLE_SIGNATURE,
              sizeof (Dbg2),
              EFI_ACPI_DEBUG_PORT_2_TABLE_REVISION,
              SIGNATURE_64 ('O','P','I','D','B','G','2',' '));
  Dbg2.Table.OffsetDbgDeviceInfo = sizeof (Dbg2.Table);
  Dbg2.Table.NumberDbgDeviceInfo = 1;

  Dbg2.Device.Revision = EFI_ACPI_DBG2_DEBUG_DEVICE_INFORMATION_STRUCT_REVISION;
  Dbg2.Device.Length = sizeof (Dbg2.Device) +
                       sizeof (Dbg2.BaseAddress) +
                       sizeof (Dbg2.AddressSize) +
                       sizeof (Dbg2.NamespaceString);
  Dbg2.Device.NumberofGenericAddressRegisters = 1;
  Dbg2.Device.NameSpaceStringLength = sizeof (Dbg2.NamespaceString);
  Dbg2.Device.NameSpaceStringOffset = sizeof (Dbg2.Device) +
                                      sizeof (Dbg2.BaseAddress) +
                                      sizeof (Dbg2.AddressSize);
  Dbg2.Device.PortType = EFI_ACPI_DBG2_PORT_TYPE_SERIAL;
  Dbg2.Device.PortSubtype = EFI_ACPI_DBG2_PORT_SUBTYPE_SERIAL_16550_WITH_GAS;
  Dbg2.Device.BaseAddressRegisterOffset = sizeof (Dbg2.Device);
  Dbg2.Device.AddressSizeOffset = sizeof (Dbg2.Device) +
                                  sizeof (Dbg2.BaseAddress);

  Dbg2.BaseAddress.AddressSpaceId = EFI_ACPI_6_5_SYSTEM_MEMORY;
  Dbg2.BaseAddress.RegisterBitWidth = 32;
  Dbg2.BaseAddress.RegisterBitOffset = 0;
  Dbg2.BaseAddress.AccessSize = EFI_ACPI_6_5_DWORD;
  Dbg2.BaseAddress.Address = A733_UART0_BASE;
  Dbg2.AddressSize = A733_UART0_LENGTH;
  Dbg2.NamespaceString[0] = '.';
  Dbg2.NamespaceString[1] = '\0';

  Status = InstallOne (Acpi, &Dbg2, sizeof (Dbg2), "DBG2");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Pptt = NULL;
  Status = BuildPptt (&Pptt, &PpttSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = InstallOne (Acpi, Pptt, PpttSize, "PPTT-topology");
  FreePool (Pptt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EfiCreateEventReadyToBootEx (TPL_CALLBACK, ReadyToBootCallback, NULL, &mReadyToBootEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                             ExitBootServicesCallback, NULL, &mExitBootServicesEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO,
          "A733 ACPI53: installed CPUs=%u EHCI1=%u EHCI-CCA=1 EHCI-GSIV=%u XHCI2=%u XHCI-GSIV=%u SDC0=%u DSDT/FADT/MADT/GTDT/SPCR/DBG2/PPTT; GICR range=1MiB\n",
          (UINT32)GetCpuCount (),
          FixedPcdGetBool (PcdExposeEhci1ToWindows) ? 1U : 0U,
          A733_EHCI1_GSI,
          ShouldExposeXhci2ToWindows () ? 1U : 0U,
          A733_XHCI2_GSI,
          FixedPcdGetBool (PcdExposeSdMmc0ToWindows) ? 1U : 0U));
  return EFI_SUCCESS;
}
