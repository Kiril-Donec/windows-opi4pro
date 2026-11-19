#include <PiPei.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>

EFI_STATUS
EFIAPI
PlatformPeim (VOID)
{
  BuildFvHob (PcdGet64 (PcdFvBaseAddress), FixedPcdGet32 (PcdFvSize));

  // BL31/monitor secure region from vendor DTS.
  BuildMemoryAllocationHob (0x48000000ULL, 0x01000000ULL, EfiReservedMemoryType);

  // Preserve the framebuffer inherited from U-Boot/HDMI initialization.
  BuildMemoryAllocationHob (
    FixedPcdGet64 (PcdFrameBufferBase),
    FixedPcdGet32 (PcdFrameBufferSize),
    EfiReservedMemoryType
    );

  DEBUG ((DEBUG_INFO, "A733 PlatformPeim: FV=%lx FB=%lx\n",
          PcdGet64 (PcdFvBaseAddress), FixedPcdGet64 (PcdFrameBufferBase)));
  return EFI_SUCCESS;
}
