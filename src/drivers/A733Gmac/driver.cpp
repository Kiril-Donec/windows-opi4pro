#include "precomp.h"

EXTERN_C DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD GmacEvtDeviceAdd;

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT driverObject,
    PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, GmacEvtDeviceAdd);
    config.DriverPoolTag = GMAC_POOL_TAG;
    return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS
GmacEvtDeviceAdd(
    WDFDRIVER driver,
    PWDFDEVICE_INIT deviceInit)
{
    UNREFERENCED_PARAMETER(driver);

    NTSTATUS status = NetDeviceInitConfig(deviceInit);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDevicePrepareHardware = GmacEvtPrepareHardware;
    power.EvtDeviceReleaseHardware = GmacEvtReleaseHardware;
    power.EvtDeviceD0Entry = GmacEvtD0Entry;
    power.EvtDeviceD0Exit = GmacEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &power);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, GMAC_DEVICE);
    WDFDEVICE device = WDF_NO_HANDLE;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfDeviceSetAlignmentRequirement(device, FILE_64_BYTE_ALIGNMENT);
    return GmacCreateAdapter(device);
}
