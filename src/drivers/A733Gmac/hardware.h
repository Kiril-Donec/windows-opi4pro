#pragma once

typedef struct _GMAC_RESOURCES {
    PUCHAR Mac;
    ULONG MacLength;
    PUCHAR Wrapper;
    ULONG WrapperLength;
    ULONG InterruptCount;
} GMAC_RESOURCES;

UINT32 GmacRead32(_In_ const GMAC_ADAPTER *adapter, _In_ ULONG offset);
VOID GmacWrite32(_In_ const GMAC_ADAPTER *adapter, _In_ ULONG offset, _In_ UINT32 value);
VOID GmacUpdateConfigurationBits(
    _In_ GMAC_ADAPTER *adapter,
    _In_ UINT32 clearMask,
    _In_ UINT32 setMask);
NTSTATUS GmacMapResources(_Inout_ GMAC_ADAPTER *adapter, _In_ WDFCMRESLIST translated);
VOID GmacUnmapResources(_Inout_ GMAC_ADAPTER *adapter);
NTSTATUS GmacResetDma(_In_ GMAC_ADAPTER *adapter);
VOID GmacProgramAddress(_In_ GMAC_ADAPTER *adapter);
VOID GmacConfigureCore(_In_ GMAC_ADAPTER *adapter);
VOID GmacStopCore(_In_ GMAC_ADAPTER *adapter);
VOID GmacUpdateLinkMode(_In_ GMAC_ADAPTER *adapter, _In_ ULONG speedMbps, _In_ BOOLEAN fullDuplex);
