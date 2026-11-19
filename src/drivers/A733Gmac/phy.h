#pragma once

NTSTATUS GmacMdioRead(_In_ GMAC_ADAPTER *adapter, _In_ UINT8 reg, _Out_ UINT16 *value);
NTSTATUS GmacMdioWrite(_In_ GMAC_ADAPTER *adapter, _In_ UINT8 reg, _In_ UINT16 value);
NTSTATUS GmacPhyPrepareClocks(_In_ GMAC_ADAPTER *adapter);
NTSTATUS GmacPhyInitialize(_In_ GMAC_ADAPTER *adapter);
VOID GmacPhyPollLink(_In_ GMAC_ADAPTER *adapter);
