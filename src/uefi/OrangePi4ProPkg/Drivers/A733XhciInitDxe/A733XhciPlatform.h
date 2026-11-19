#ifndef A733_XHCI_PLATFORM_H_
#define A733_XHCI_PLATFORM_H_

#include <Uefi.h>

#define A733_XHCI_BASE  0x06A00000U
#define A733_XHCI_SIZE  0x00100000U

EFI_STATUS
A733XhciPlatformRecoverController (
  VOID
  );

BOOLEAN
A733XhciPlatformPmaReady (
  VOID
  );

EFI_STATUS
A733XhciPlatformInit (
  VOID
  );

VOID
A733XhciPlatformDump (
  IN CONST CHAR8  *Stage
  );

#endif
