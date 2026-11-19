#include <PiPei.h>
#include <Library/ArmVirtMemInfoLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>

#define MAX_DESCRIPTORS  8

RETURN_STATUS
EFIAPI
A733VirtMemInfoLibConstructor (VOID)
{
  UINT64 Size = PcdGet64 (PcdSystemMemorySize);
  VOID *Hob = BuildGuidDataHob (&gArmVirtSystemMemorySizeGuid, &Size, sizeof (Size));
  return (Hob == NULL) ? RETURN_OUT_OF_RESOURCES : RETURN_SUCCESS;
}

VOID
ArmVirtGetMemoryMap (OUT ARM_MEMORY_REGION_DESCRIPTOR **VirtualMemoryMap)
{
  ARM_MEMORY_REGION_DESCRIPTOR *Table;
  Table = AllocatePages (EFI_SIZE_TO_PAGES (sizeof (*Table) * MAX_DESCRIPTORS));
  ASSERT (Table != NULL);
  ZeroMem (Table, sizeof (*Table) * MAX_DESCRIPTORS);

  // LPDDR5: 0x40000000..0x13fffffff (4 GiB)
  Table[0].PhysicalBase = PcdGet64 (PcdSystemMemoryBase);
  Table[0].VirtualBase  = Table[0].PhysicalBase;
  Table[0].Length       = PcdGet64 (PcdSystemMemorySize);
  Table[0].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;

  // A733 main MMIO aperture: PIO/CCU/UART/MMC/USB/GMAC/GIC/display/HDMI.
  Table[1].PhysicalBase = 0x02000000ULL;
  Table[1].VirtualBase  = 0x02000000ULL;
  Table[1].Length       = 0x06000000ULL;
  Table[1].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // Always-on / RTC / R_PIO aperture.
  Table[2].PhysicalBase = 0x07000000ULL;
  Table[2].VirtualBase  = 0x07000000ULL;
  Table[2].Length       = 0x01000000ULL;
  Table[2].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // Firmware image, kept executable and read-only after MMU setup.
  Table[3].PhysicalBase = PcdGet64 (PcdFdBaseAddress);
  Table[3].VirtualBase  = Table[3].PhysicalBase;
  Table[3].Length       = FixedPcdGet32 (PcdFvSize) + 0x8000;
  Table[3].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_RO;

  // The HDMI display engine is not cache-coherent with the CPU.  The broad
  // LPDDR5 descriptor above maps this range as write-back RAM, which leaves
  // freshly rendered glyphs in CPU cache lines and produces torn/fragmented
  // text on HDMI.  This later descriptor deliberately overrides the active
  // scan-out buffer with an uncached mapping.  It is slower than write-back,
  // but deterministic and correct for the first hardware port.
  Table[4].PhysicalBase = FixedPcdGet64 (PcdFrameBufferBase);
  Table[4].VirtualBase  = Table[4].PhysicalBase;
  Table[4].Length       = FixedPcdGet32 (PcdFrameBufferSize);
  Table[4].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;

  // CPU PLL block (A55 / A76 / DSU PLLs at 0x08871000..0x08873fff) for
  // A733CpuFreqDxe; outside both MMIO apertures above.
  Table[5].PhysicalBase = 0x08800000ULL;
  Table[5].VirtualBase  = 0x08800000ULL;
  Table[5].Length       = 0x00100000ULL;
  Table[5].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  DEBUG ((
    DEBUG_INFO,
    "A733 MMU: framebuffer %lx..%lx mapped UNCACHED\n",
    Table[4].PhysicalBase,
    Table[4].PhysicalBase + Table[4].Length
    ));

  *VirtualMemoryMap = Table;
}
