#pragma once

typedef struct _GMAC_PLATFORM_RESOURCES {
    PUCHAR Ccu;
    ULONG CcuLength;
} GMAC_PLATFORM_RESOURCES;

NTSTATUS GmacPlatformMap(_Inout_ GMAC_ADAPTER *adapter);
VOID GmacPlatformUnmap(_Inout_ GMAC_ADAPTER *adapter);
NTSTATUS GmacPlatformResetAndConfigure(_In_ GMAC_ADAPTER *adapter);
