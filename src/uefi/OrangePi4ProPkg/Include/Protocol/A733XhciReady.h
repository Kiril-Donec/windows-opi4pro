#ifndef A733_XHCI_READY_PROTOCOL_H_
#define A733_XHCI_READY_PROTOCOL_H_

#include <Uefi.h>

#define A733_XHCI_READY_PROTOCOL_GUID \
  { 0x5b82f798, 0x1e6b, 0x4f51, { 0xa7, 0x33, 0x58, 0x48, 0x43, 0x49, 0x52, 0x44 } }

typedef struct {
  UINT32   Revision;
  BOOLEAN  PmaReady;
  BOOLEAN  CnrClear;
  BOOLEAN  Halted;
  BOOLEAN  UefiDriverReady;
  BOOLEAN  PortConnected;
  BOOLEAN  PlatformReady;
} A733_XHCI_READY_PROTOCOL;

extern EFI_GUID  gA733XhciReadyProtocolGuid;

#endif
