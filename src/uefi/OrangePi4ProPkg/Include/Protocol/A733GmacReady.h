#ifndef A733_GMAC_READY_PROTOCOL_H_
#define A733_GMAC_READY_PROTOCOL_H_

#include <Uefi.h>

#define A733_GMAC_READY_PROTOCOL_GUID \
  { 0xa7330040, 0x4f50, 0x474d, { 0x41, 0x43, 0x52, 0x45, 0x41, 0x44, 0x59, 0x00 } }

typedef struct {
  UINT32   Revision;
  BOOLEAN  PlatformReady;
  UINT32   WrapperConfig;
  UINT32   CoreVersion;
  UINT32   HardwareFeature[4];
  UINT32   CcuMbus;
  UINT32   CcuPhyClock;
  UINT32   CcuBusReset;
} A733_GMAC_READY_PROTOCOL;

extern EFI_GUID  gA733GmacReadyProtocolGuid;

#endif
