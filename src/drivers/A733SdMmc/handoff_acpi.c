#include <ntifs.h>
#include "A733SdMmc.h"
#include "handoff_acpi.h"
#include <acpiioct.h>
#include "../Shared/sd_handoff_wire.h"

NTSTATUS A733VerifyFirmwareHandoff(PVOID PrivateExtension)
{
    PSDPORT_SLOT_EXTENSION slot;
    PSD_MINIPORT miniport;
    PDEVICE_OBJECT pdo;
    ACPI_EVAL_INPUT_BUFFER input;
    ULONG storage[64]; /* aligned; output contains exactly one 40-byte Buffer */
    PACPI_EVAL_OUTPUT_BUFFER output = (PACPI_EVAL_OUTPUT_BUFFER)storage;
    IO_STATUS_BLOCK iosb;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;
    ULONG expected = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) +
                     FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + A733_SD_PROOF_SIZE;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || PrivateExtension == NULL)
        return STATUS_INVALID_DEVICE_STATE;
    /* sdport.h defines PrivateExtension as the trailing aligned member of
     * SDPORT_SLOT_EXTENSION; use its port-owned back pointer, never a global. */
    slot = CONTAINING_RECORD(PrivateExtension, SDPORT_SLOT_EXTENSION, PrivateExtension);
    miniport = slot->Miniport;
    if (miniport == NULL || miniport->ConfigurationInfo.BusType != SdBusTypeAcpi ||
        miniport->ConfigurationInfo.DeviceObject == NULL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    pdo = IoGetDeviceAttachmentBaseRef((PDEVICE_OBJECT)miniport->ConfigurationInfo.DeviceObject);
    if (pdo == NULL) return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlZeroMemory(&input, sizeof(input));
    RtlZeroMemory(storage, sizeof(storage));
    RtlZeroMemory(&iosb, sizeof(iosb));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(input.MethodName, "SDHF", 4);
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD, pdo,
                &input, sizeof(input), storage, sizeof(storage), FALSE, &event, &iosb);
    if (irp == NULL) {
        ObDereferenceObject(pdo);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = IoCallDriver(pdo, irp);
    if (status == STATUS_PENDING) {
        /* Stack buffers/event must remain alive until ACPI completes the IRP.
         * Do not return on a timeout with an outstanding IRP using this stack. */
        (void)KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    if (NT_SUCCESS(status)) status = iosb.Status;
    ObDereferenceObject(pdo);
    if (!NT_SUCCESS(status)) {
        A733_LOG_ERROR("SDHF missing or ACPI evaluation failed: %08lx", (ULONG)status);
        return status;
    }
    if (iosb.Information != expected || output->Length != expected ||
        output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || output->Count != 1 ||
        output->Argument[0].Type != ACPI_METHOD_ARGUMENT_BUFFER ||
        output->Argument[0].DataLength != A733_SD_PROOF_SIZE ||
        !A733WireValidate(output->Argument[0].Data, A733_SD_PROOF_SIZE)) {
        A733_LOG_ERROR("SDHF invalid version/length/rails/clock/checksum", 0);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}
