#include <Uefi.h>
#include <Library/LcdHwLib.h>
#include <Library/PcdLib.h>
EFI_STATUS LcdIdentify (VOID) { return EFI_SUCCESS; }
EFI_STATUS LcdInitialize (EFI_PHYSICAL_ADDRESS FrameBaseAddress) {
  return (FrameBaseAddress == FixedPcdGet64 (PcdFrameBufferBase)) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}
EFI_STATUS LcdSetMode (IN UINT32 ModeNumber) { return (ModeNumber == 0) ? EFI_SUCCESS : EFI_UNSUPPORTED; }
VOID LcdShutdown (VOID) { }
