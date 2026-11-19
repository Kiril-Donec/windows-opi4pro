#include "precomp.h"

_Use_decl_annotations_
NTSTATUS
GmacEvtPrepareHardware(
    WDFDEVICE device,
    WDFCMRESLIST resourcesRaw,
    WDFCMRESLIST resourcesTranslated)
{
    UNREFERENCED_PARAMETER(resourcesRaw);
    GMAC_ADAPTER *adapter = GmacGetDeviceContext(device)->Adapter;

    NTSTATUS status = GmacMapResources(adapter, resourcesTranslated);
    if (!NT_SUCCESS(status)) {
        GMAC_LOG_ERROR("resource mapping failed %08lx", status);
        return status;
    }

    status = GmacPlatformMap(adapter);
    if (NT_SUCCESS(status)) {
        status = GmacPlatformResetAndConfigure(adapter);
    }
    if (NT_SUCCESS(status)) {
        status = GmacPhyPrepareClocks(adapter);
    }
    if (NT_SUCCESS(status)) {
        status = GmacResetDma(adapter);
    }
    if (NT_SUCCESS(status)) {
        GmacConfigureCore(adapter);
        status = GmacPhyInitialize(adapter);
    }
    if (!NT_SUCCESS(status)) {
        GmacStopCore(adapter);
        GmacPlatformUnmap(adapter);
        GmacUnmapResources(adapter);
        GMAC_LOG_ERROR("hardware prepare failed %08lx", status);
        return status;
    }

    adapter->HardwarePrepared = TRUE;
    GmacPhyPollLink(adapter);
    GMAC_LOG_INFO("hardware prepare complete wrapper=%08lx", READ_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG *>(adapter->Resources.Wrapper)));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
GmacEvtReleaseHardware(
    WDFDEVICE device,
    WDFCMRESLIST resourcesTranslated)
{
    UNREFERENCED_PARAMETER(resourcesTranslated);
    GMAC_ADAPTER *adapter = GmacGetDeviceContext(device)->Adapter;
    GmacStopPolling(adapter);
    adapter->HardwarePrepared = FALSE;
    WdfTimerStop(adapter->LinkTimer, TRUE);
    GmacStopCore(adapter);
    GmacPlatformUnmap(adapter);
    GmacUnmapResources(adapter);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
GmacEvtD0Entry(
    WDFDEVICE device,
    WDF_POWER_DEVICE_STATE previousState)
{
    GMAC_ADAPTER *adapter = GmacGetDeviceContext(device)->Adapter;
    if (adapter->HardwarePrepared) {
        if (previousState != WdfPowerDeviceD3Final) {
            NTSTATUS status = GmacPlatformResetAndConfigure(adapter);
            if (NT_SUCCESS(status)) {
                status = GmacPhyPrepareClocks(adapter);
            }
            if (NT_SUCCESS(status)) {
                status = GmacResetDma(adapter);
            }
            if (!NT_SUCCESS(status)) {
                return status;
            }
            GmacConfigureCore(adapter);
            status = GmacPhyInitialize(adapter);
            if (!NT_SUCCESS(status)) {
                return status;
            }
        }
        WdfTimerStart(adapter->LinkTimer, WDF_REL_TIMEOUT_IN_MS(250U));
#if !GMAC_USE_INTERRUPTS
        GmacStartPolling(adapter);
#endif
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
GmacEvtD0Exit(
    WDFDEVICE device,
    WDF_POWER_DEVICE_STATE targetState)
{
    UNREFERENCED_PARAMETER(targetState);
    GMAC_ADAPTER *adapter = GmacGetDeviceContext(device)->Adapter;
    GmacStopPolling(adapter);
    WdfTimerStop(adapter->LinkTimer, TRUE);
    GmacStopCore(adapter);
    return STATUS_SUCCESS;
}
