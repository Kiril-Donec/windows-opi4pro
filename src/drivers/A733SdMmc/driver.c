#include "A733SdMmc.h"
#include "handoff_acpi.h"

/*
 * SDPORT miniport callbacks over the XNU hardware sequence (smhc_hw.c,
 * smhc_request.c). 2.6: IssueRequest only queues the request; a dedicated
 * system thread runs it at PASSIVE_LEVEL (one at a time, polled PIO, so the
 * CPU can be preempted during long transfers) and completes it from a
 * miniport DPC so that SDPORT never re-enters IssueRequest from inside it.
 * Controller interrupts stay masked (IMASK = 0, GCTRL interrupt enable off).
 */

static KDEFERRED_ROUTINE A733CompletionDpc;
static KDEFERRED_ROUTINE A733BusyDpc;
static VOID A733StartWorker(_In_ PA733_SD_EXTENSION Extension);
static VOID A733StopWorker(_In_ PA733_SD_EXTENSION Extension);

static NTSTATUS
A733GetSlotCount(
    _In_ PSD_MINIPORT Miniport,
    _Out_ PUCHAR SlotCount
    )
{
    UNREFERENCED_PARAMETER(Miniport);
    *SlotCount = 1;
    return STATUS_SUCCESS;
}

static VOID
A733GetSlotCapabilities(
    _In_ PVOID PrivateExtension,
    _Out_ PSDPORT_CAPABILITIES Capabilities
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->SpecVersion = 2;
    Capabilities->MaximumOutstandingRequests = 1;
    Capabilities->MaximumBlockSize = 512;
    Capabilities->MaximumBlockCount = A733_MAX_BLOCK_COUNT;
    Capabilities->BaseClockFrequencyKhz = A733_MAX_CARD_KHZ;
    Capabilities->AlignmentRequirement = 3;
    Capabilities->PioTransferMaxThreshold = MAXULONG;
    Capabilities->Supported.HighSpeed = A733_ENABLE_HIGH_SPEED;
    Capabilities->Supported.AutoCmd12 = 1;
    Capabilities->Supported.Voltage33V = 1;
    Capabilities->Supported.DriverTypeB = 1;
    Capabilities->Flags.UsePioForRead = 1;
    Capabilities->Flags.UsePioForWrite = 1;
}

static VOID
A733UnmapShared(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    if (Extension->PioRegisters != NULL) {
        MmUnmapIoSpace((PVOID)Extension->PioRegisters, A733_PIO_REGISTER_LENGTH);
        Extension->PioRegisters = NULL;
    }
    if (Extension->CcuRegisters != NULL) {
        MmUnmapIoSpace((PVOID)Extension->CcuRegisters, A733_CCU_REGISTER_LENGTH);
        Extension->CcuRegisters = NULL;
    }
}

static NTSTATUS
A733Initialize(
    _In_ PVOID PrivateExtension,
    _In_ PHYSICAL_ADDRESS PhysicalBase,
    _In_ PVOID VirtualBase,
    _In_ ULONG Length,
    _In_ BOOLEAN CrashdumpMode
    )
{
    PA733_SD_EXTENSION extension = (PA733_SD_EXTENSION)PrivateExtension;
    PHYSICAL_ADDRESS address;
    NTSTATUS status;

    if (CrashdumpMode) {
        return STATUS_NOT_SUPPORTED;
    }
    if ((VirtualBase == NULL) ||
        (PhysicalBase.QuadPart != A733_SMHC_PHYSICAL_BASE) ||
        (Length < A733_SMHC_REGISTER_LENGTH)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    RtlZeroMemory(extension, sizeof(*extension));
    KeInitializeSpinLock(&extension->StateLock);
    KeInitializeDpc(&extension->CompletionDpc, A733CompletionDpc, extension);
    KeInitializeDpc(&extension->BusyDpc, A733BusyDpc, extension);
    KeInitializeTimer(&extension->BusyTimer);
    extension->Registers = (volatile UCHAR *)VirtualBase;
    extension->CrashdumpMode = CrashdumpMode;

    A733_LOG_INFO("%s XNU-port initialize base=%llx length=%lu irql=%lu high-speed=%u",
                  A733_DRIVER_VERSION_STRING, (ULONGLONG)PhysicalBase.QuadPart, Length, (ULONG)KeGetCurrentIrql(),
                  (ULONG)A733_ENABLE_HIGH_SPEED);

    /* Read-only windows: the CCU for the module clock (as XNU does) and the
     * PIO for the PF bank voltage select / PF6 card detect. Never written. */
    address.QuadPart = A733_CCU_PHYSICAL_BASE;
    extension->CcuRegisters = (volatile UCHAR *)MmMapIoSpaceEx(
        address, A733_CCU_REGISTER_LENGTH, PAGE_READONLY | PAGE_NOCACHE);
    if (extension->CcuRegisters == NULL) {
        extension->CcuRegisters = (volatile UCHAR *)MmMapIoSpaceEx(
            address, A733_CCU_REGISTER_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    }
    address.QuadPart = A733_PIO_PHYSICAL_BASE;
    extension->PioRegisters = (volatile UCHAR *)MmMapIoSpaceEx(
        address, A733_PIO_REGISTER_LENGTH, PAGE_READONLY | PAGE_NOCACHE);
    if (extension->PioRegisters == NULL) {
        extension->PioRegisters = (volatile UCHAR *)MmMapIoSpaceEx(
            address, A733_PIO_REGISTER_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (extension->CcuRegisters == NULL) {
        A733_LOG_ERROR("cannot map CCU read only", 0);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto InitFailed;
    }

    /* Paired UEFI v59.2 proof: informational unless A733_REQUIRE_SDHF. */
    status = A733VerifyFirmwareHandoff(PrivateExtension);
    if (NT_SUCCESS(status)) {
        extension->FirmwareHandoffReady = TRUE;
        A733_LOG_INFO("SDHF firmware handoff record valid", 0);
    } else {
#if A733_REQUIRE_SDHF
        A733_LOG_ERROR("SDHF required but not valid: %08lx", (ULONG)status);
        goto InitFailed;
#else
        A733_LOG_INFO("no valid SDHF (%08lx): non-fatal, using the SMHC0 state left by U-Boot", (ULONG)status);
#endif
    }

    A733SmhcDumpState(extension, "pre-reset");
    status = A733SmhcProbeClock(extension);
    if (!NT_SUCCESS(status)) {
        goto InitFailed;
    }
    if (!A733SmhcCanSignal33(extension)) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto InitFailed;
    }
    status = A733SmhcResetController(extension);
    if (NT_SUCCESS(status)) {
        status = A733SmhcSetBusWidth(extension, SdBusWidth1Bit);
    }
    A733SmhcDumpState(extension, NT_SUCCESS(status) ? "post-reset" : "init-failed");
    if (NT_SUCCESS(status)) {
        A733StartWorker(extension);
        return STATUS_SUCCESS;
    }
InitFailed:
    A733_LOG_ERROR("initialize failed %08lx", (ULONG)status);
    A733UnmapShared(extension);
    return status;
}

/* IMASK is always 0, so the controller never raises its line. Anything
 * pending here is not ours; acknowledge only bits that are unmasked. */
static BOOLEAN
A733Interrupt(
    _In_ PVOID PrivateExtension,
    _Out_ PULONG Events,
    _Out_ PULONG Errors,
    _Out_ PBOOLEAN NotifyCardChange,
    _Out_ PBOOLEAN NotifySdioInterrupt,
    _Out_ PBOOLEAN NotifyTuning
    )
{
    PA733_SD_EXTENSION extension = (PA733_SD_EXTENSION)PrivateExtension;
    ULONG pending;

    *Events = 0;
    *Errors = 0;
    *NotifyCardChange = FALSE;
    *NotifySdioInterrupt = FALSE;
    *NotifyTuning = FALSE;
    if (extension->Registers == NULL) {
        return FALSE;
    }
    pending = A733SmhcRead(extension, SMHC_RINT) & A733SmhcRead(extension, SMHC_IMASK);
    if (pending == 0) {
        return FALSE;
    }
    A733SmhcWrite(extension, SMHC_IMASK, 0);
    A733SmhcWrite(extension, SMHC_RINT, pending);
    return TRUE;
}

static VOID
A733TraceCommand(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ PSDPORT_REQUEST Request,
    _In_ NTSTATUS Status
    )
{
#if A733_TRACE_BRINGUP
    if (Extension->TraceCommandCount < A733_TRACE_COMMAND_LIMIT) {
        Extension->TraceCommandCount++;
        A733_LOG_INFO("#%lu type=%lu cmd=%u arg=%08lx rsp=%lu len=%lu -> %08lx RINT=%08lx RESP=%08lx %08lx %08lx %08lx",
                      Extension->TraceCommandCount, (ULONG)Request->Type,
                      (ULONG)Request->Command.Index, Request->Command.Argument,
                      (ULONG)Request->Command.ResponseType,
                      (Request->Type == SdRequestTypeCommandNoTransfer) ? 0UL : Request->Command.Length,
                      (ULONG)Status, Extension->LastRint,
                      Extension->Response[0], Extension->Response[1],
                      Extension->Response[2], Extension->Response[3]);
    }
#else
    UNREFERENCED_PARAMETER(Extension);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Status);
#endif
}

/* Hand ActiveRequest back to SDPORT from a DPC (never under StateLock). */
static VOID
A733CompletionDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    )
{
    PA733_SD_EXTENSION extension = (PA733_SD_EXTENSION)DeferredContext;
    PSDPORT_REQUEST request;
    NTSTATUS status;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    if (extension == NULL) {
        return;
    }
    KeAcquireSpinLock(&extension->StateLock, &oldIrql);
    request = extension->ActiveRequest;
    status = extension->CompletionStatus;
    extension->ActiveRequest = NULL;
    KeReleaseSpinLock(&extension->StateLock, oldIrql);
    if (request != NULL) {
        request->RequiredEvents = 0;
        request->Status = status;
        SdPortCompleteRequest(request, status);
    }
}

/* DAT0 busy after R1b / writes, polled every 1 ms (XNU waits up to 3 s). */
static VOID
A733BusyDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    )
{
    PA733_SD_EXTENSION extension = (PA733_SD_EXTENSION)DeferredContext;
    PSDPORT_REQUEST request;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN complete = FALSE;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    if (extension == NULL) {
        return;
    }
    KeAcquireSpinLock(&extension->StateLock, &oldIrql);
    request = extension->ActiveRequest;
    if (request != NULL) {
        if (!(A733SmhcRead(extension, SMHC_STATUS) & SMHC_STATUS_CARD_BUSY)) {
            complete = TRUE;
        } else if (A733DeadlinePassed(extension->BusyDeadline)) {
            A733_LOG_ERROR("card busy (DAT0 low) timeout after cmd=%u STATUS=%08lx",
                           (ULONG)request->Command.Index, A733SmhcRead(extension, SMHC_STATUS));
            status = STATUS_IO_TIMEOUT;
            complete = TRUE;
        } else {
            LARGE_INTEGER due;
            due.QuadPart = -A733_BUSY_POLL_100NS;
            (void)KeSetTimer(&extension->BusyTimer, due, &extension->BusyDpc);
        }
        if (complete) {
            extension->ActiveRequest = NULL;
        }
    }
    KeReleaseSpinLock(&extension->StateLock, oldIrql);
    if (complete) {
        request->RequiredEvents = 0;
        request->Status = status;
        SdPortCompleteRequest(request, status);
    }
}

/*
 * Run one request on the controller (worker thread at PASSIVE_LEVEL, or
 * inline without a worker) and schedule its completion. Called with
 * ActiveRequest == Request and Executing == 1; clears Executing.
 */
static VOID
A733RunRequest(
    _In_ PA733_SD_EXTENSION extension,
    _Inout_ PSDPORT_REQUEST Request
    )
{
    NTSTATUS status;
    BOOLEAN waitBusy = FALSE;
    KIRQL oldIrql;

    switch (Request->Type) {
    case SdRequestTypeCommandNoTransfer:
    case SdRequestTypeCommandWithTransfer:
        extension->TransferReady = FALSE;
        status = A733SmhcExecute(extension, Request, &waitBusy);
        if (NT_SUCCESS(status) && Request->Type == SdRequestTypeCommandWithTransfer) {
            extension->TransferReady = TRUE;
            extension->TransferIndex = Request->Command.Index;
            extension->TransferArgument = Request->Command.Argument;
            extension->TransferLength = Request->Command.Length;
            extension->TransferDirection = (ULONG)Request->Command.TransferDirection;
            extension->TransferBuffer = Request->Command.DataBuffer;
        }
        A733TraceCommand(extension, Request, status);
        break;
    case SdRequestTypeStartTransfer:
        if (extension->TransferReady &&
            Request->Command.Index == extension->TransferIndex &&
            Request->Command.Argument == extension->TransferArgument &&
            Request->Command.Length == extension->TransferLength &&
            (ULONG)Request->Command.TransferDirection == extension->TransferDirection &&
            (extension->DeferredData || Request->Command.DataBuffer == extension->TransferBuffer)) {
            if (extension->DeferredData) {
                status = A733SmhcFinishDeferredData(extension, Request, &waitBusy);
            } else {
                /* The FIFO was already drained / filled while the command ran. */
                status = STATUS_SUCCESS;
            }
        } else {
            A733_LOG_ERROR("StartTransfer cmd=%u without a matching completed data command (ready=%u)",
                           (ULONG)Request->Command.Index, (ULONG)extension->TransferReady);
            status = STATUS_IO_DEVICE_ERROR;
        }
        extension->TransferReady = FALSE;
        extension->DeferredData = FALSE;
        A733TraceCommand(extension, Request, status);
        break;
    default:
        /* Filtered out by A733IssueRequest; kept for safety. */
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    KeAcquireSpinLock(&extension->StateLock, &oldIrql);
    (void)InterlockedExchange(&extension->Executing, 0);
    if (NT_SUCCESS(status) && waitBusy &&
        (A733SmhcRead(extension, SMHC_STATUS) & SMHC_STATUS_CARD_BUSY)) {
        LARGE_INTEGER due;
        extension->BusyDeadline = A733DeadlineMs(A733_BUSY_WAIT_MS);
        due.QuadPart = -A733_BUSY_POLL_100NS;
        (void)KeSetTimer(&extension->BusyTimer, due, &extension->BusyDpc);
    } else {
        extension->CompletionStatus = status;
        (void)KeInsertQueueDpc(&extension->CompletionDpc, NULL, NULL);
    }
    KeReleaseSpinLock(&extension->StateLock, oldIrql);
}

/* Worker: waits for QueuedRequest and runs it at PASSIVE_LEVEL. */
static KSTART_ROUTINE A733WorkerThread;

static VOID
A733WorkerThread(
    _In_ PVOID Context
    )
{
    PA733_SD_EXTENSION extension = (PA733_SD_EXTENSION)Context;

    (void)KeSetPriorityThread(KeGetCurrentThread(), A733_WORKER_PRIORITY);
    for (;;) {
        PSDPORT_REQUEST request;
        KIRQL oldIrql;

        (void)KeWaitForSingleObject(&extension->WorkEvent, Executive, KernelMode, FALSE, NULL);
        if (InterlockedCompareExchange(&extension->StopWorker, 0, 0) != 0) {
            break;
        }
        KeAcquireSpinLock(&extension->StateLock, &oldIrql);
        request = extension->QueuedRequest;
        extension->QueuedRequest = NULL;
        KeReleaseSpinLock(&extension->StateLock, oldIrql);
        if (request != NULL) {
            A733RunRequest(extension, request);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID
A733StartWorker(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    HANDLE handle;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        A733_LOG_ERROR("Initialize at IRQL %lu: no worker thread, requests run inline",
                       (ULONG)KeGetCurrentIrql());
        return;
    }
    KeInitializeEvent(&Extension->WorkEvent, SynchronizationEvent, FALSE);
    Extension->StopWorker = 0;
    status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                  A733WorkerThread, Extension);
    if (!NT_SUCCESS(status)) {
        A733_LOG_ERROR("PsCreateSystemThread failed %08lx: requests run inline", (ULONG)status);
        return;
    }
    status = ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                       (PVOID *)&Extension->WorkerThread, NULL);
    if (!NT_SUCCESS(status)) {
        /* The thread runs; it cannot be joined at cleanup, but stays usable. */
        Extension->WorkerThread = NULL;
        A733_LOG_ERROR("worker thread reference failed %08lx", (ULONG)status);
    }
    ZwClose(handle);
    A733_LOG_INFO("worker thread started, priority %lu", (ULONG)A733_WORKER_PRIORITY);
}

static VOID
A733StopWorker(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    if (Extension->WorkerThread == NULL) {
        return;
    }
    (void)InterlockedExchange(&Extension->StopWorker, 1);
    (void)KeSetEvent(&Extension->WorkEvent, IO_NO_INCREMENT, FALSE);
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (void)KeWaitForSingleObject(Extension->WorkerThread, Executive, KernelMode, FALSE, NULL);
    }
    ObDereferenceObject(Extension->WorkerThread);
    Extension->WorkerThread = NULL;
}

static NTSTATUS
A733IssueRequest(
    _In_ PVOID PrivateExtension,
    _Inout_ PSDPORT_REQUEST Request
    )
{
    PA733_SD_EXTENSION extension = (PA733_SD_EXTENSION)PrivateExtension;
    BOOLEAN queued = FALSE;
    KIRQL oldIrql;

    if (!extension->RequestIrqlLogged) {
        extension->RequestIrqlLogged = TRUE;
        A733_LOG_INFO("first IssueRequest IRQL=%lu type=%lu worker=%u", (ULONG)KeGetCurrentIrql(),
                      (ULONG)Request->Type, (extension->WorkerThread != NULL) ? 1U : 0U);
    }
    if (Request->Type != SdRequestTypeCommandNoTransfer &&
        Request->Type != SdRequestTypeCommandWithTransfer &&
        Request->Type != SdRequestTypeStartTransfer) {
        A733_LOG_ERROR("unsupported request type=%lu", (ULONG)Request->Type);
        return STATUS_NOT_SUPPORTED;
    }
    KeAcquireSpinLock(&extension->StateLock, &oldIrql);
    if (extension->ActiveRequest != NULL) {
        KeReleaseSpinLock(&extension->StateLock, oldIrql);
        return STATUS_DEVICE_BUSY;
    }
    extension->ActiveRequest = Request;
    (void)InterlockedExchange(&extension->Executing, 1);
    if (extension->WorkerThread != NULL) {
        extension->QueuedRequest = Request;
        queued = TRUE;
    }
    KeReleaseSpinLock(&extension->StateLock, oldIrql);

    if (queued) {
        (void)KeSetEvent(&extension->WorkEvent, IO_NO_INCREMENT, FALSE);
    } else {
        A733RunRequest(extension, Request);
    }
    return STATUS_PENDING;
}

static VOID
A733GetResponse(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_COMMAND Command,
    _Out_ PVOID ResponseBuffer
    )
{
    PA733_SD_EXTENSION extension = (PA733_SD_EXTENSION)PrivateExtension;
    PULONG response = (PULONG)ResponseBuffer;

    RtlZeroMemory(ResponseBuffer, SDPORT_MAX_RESPONSE_LENGTH);
    if (Command->ResponseType == SdResponseTypeR2) {
        /* SMHC RESP0..3 hold R2 bits 31:0 .. 127:96 (XNU). SDPORT expects
         * the SDHCI layout: bits 127:8, CRC byte dropped. */
        response[0] = (extension->Response[0] >> 8) | (extension->Response[1] << 24);
        response[1] = (extension->Response[1] >> 8) | (extension->Response[2] << 24);
        response[2] = (extension->Response[2] >> 8) | (extension->Response[3] << 24);
        response[3] = extension->Response[3] >> 8;
    } else if (Command->ResponseType != SdResponseTypeNone) {
        response[0] = extension->Response[0];
    }
}

/* Never called: the ISR reports no events. */
static VOID
A733RequestDpc(
    _In_ PVOID PrivateExtension,
    _Inout_ PSDPORT_REQUEST Request,
    _In_ ULONG Events,
    _In_ ULONG Errors
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Events);
    UNREFERENCED_PARAMETER(Errors);
}

/* Polled controller: event masks stay off. */
static VOID
A733ToggleEvents(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask,
    _In_ BOOLEAN Enable
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    UNREFERENCED_PARAMETER(EventMask);
    UNREFERENCED_PARAMETER(Enable);
}

static VOID
A733ClearEvents(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    UNREFERENCED_PARAMETER(EventMask);
}

/* Reserve the controller for a bus operation: no request may be active. */
static BOOLEAN
A733ControllerIdle(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    KIRQL oldIrql;
    BOOLEAN idle;

    KeAcquireSpinLock(&Extension->StateLock, &oldIrql);
    idle = (Extension->ActiveRequest == NULL) ? TRUE : FALSE;
    KeReleaseSpinLock(&Extension->StateLock, oldIrql);
    return idle;
}

static NTSTATUS
A733IssueBusOperation(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_BUS_OPERATION Operation
    )
{
    PA733_SD_EXTENSION extension = (PA733_SD_EXTENSION)PrivateExtension;

    switch (Operation->Type) {
    case SdResetHw:
    case SdResetHost:
        {
            PSDPORT_REQUEST aborted;
            NTSTATUS abortedStatus = STATUS_IO_DEVICE_ERROR;
            NTSTATUS status;
            KIRQL oldIrql;

            /* A request can only still be ours in its DAT0 busy wait or with
             * its completion DPC queued. A timer / completion DPC already
             * running on another CPU waits for StateLock and then finds no
             * ActiveRequest, so the request is completed exactly once here. */
            KeAcquireSpinLock(&extension->StateLock, &oldIrql);
            if (InterlockedCompareExchange(&extension->Executing, 0, 0) != 0) {
                KeReleaseSpinLock(&extension->StateLock, oldIrql);
                return STATUS_DEVICE_BUSY;
            }
            (void)KeCancelTimer(&extension->BusyTimer);
            aborted = extension->ActiveRequest;
            if (aborted != NULL && KeRemoveQueueDpc(&extension->CompletionDpc)) {
                abortedStatus = extension->CompletionStatus;
            }
            extension->ActiveRequest = NULL;
            extension->TransferReady = FALSE;
            extension->DeferredData = FALSE;
            KeReleaseSpinLock(&extension->StateLock, oldIrql);

            status = A733SmhcReset(extension, Operation->Parameters.ResetType);
            A733_LOG_INFO("bus reset op=%lu type=%lu -> %08lx",
                          (ULONG)Operation->Type, (ULONG)Operation->Parameters.ResetType, (ULONG)status);
            if (aborted != NULL) {
                aborted->RequiredEvents = 0;
                aborted->Status = abortedStatus;
                SdPortCompleteRequest(aborted, abortedStatus);
            }
            return status;
        }
    case SdSetClock:
        if (!A733ControllerIdle(extension)) return STATUS_DEVICE_BUSY;
        return A733SmhcSetClock(extension, Operation->Parameters.FrequencyKhz);
    case SdSetBusWidth:
        if (!A733ControllerIdle(extension)) return STATUS_DEVICE_BUSY;
        return A733SmhcSetBusWidth(extension, Operation->Parameters.BusWidth);
    case SdSetPower:
        /* The slot rail belongs to the board (PMIC set up by U-Boot). */
        A733_LOG_INFO("power %u", (ULONG)Operation->Parameters.PowerEnabled);
        if (!Operation->Parameters.PowerEnabled) {
            if (!A733ControllerIdle(extension)) return STATUS_DEVICE_BUSY;
            return A733SmhcSetClock(extension, 0);
        }
        return A733SmhcCanSignal33(extension) ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
    case SdSetVoltage:
        A733_LOG_INFO("bus voltage %lu", (ULONG)Operation->Parameters.Voltage);
        if (Operation->Parameters.Voltage == SdBusVoltageUndefined) return STATUS_SUCCESS;
        if (Operation->Parameters.Voltage == SdBusVoltageOff) {
            /* The rail is the board's; only the card clock can be stopped. */
            if (!A733ControllerIdle(extension)) return STATUS_DEVICE_BUSY;
            return A733SmhcSetClock(extension, 0);
        }
        if (Operation->Parameters.Voltage == SdBusVoltage33) {
            return A733SmhcCanSignal33(extension) ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        A733_LOG_ERROR("unsupported bus voltage=%lu", (ULONG)Operation->Parameters.Voltage);
        return STATUS_NOT_SUPPORTED;
    case SdSetSignalingVoltage:
        if (Operation->Parameters.SignalingVoltage == SdignalingVoltageUndefined) return STATUS_SUCCESS;
        if (Operation->Parameters.SignalingVoltage == SdSignalingVoltage33) {
            return A733SmhcCanSignal33(extension) ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        A733_LOG_ERROR("unsupported signaling voltage=%lu", (ULONG)Operation->Parameters.SignalingVoltage);
        return STATUS_NOT_SUPPORTED;
    case SdSetBusSpeed:
        A733_LOG_INFO("bus speed %lu", (ULONG)Operation->Parameters.BusSpeed);
        switch (Operation->Parameters.BusSpeed) {
        case SdBusSpeedUndefined:
        case SdBusSpeedNormal:
        case SdBusSpeedSDR12:
            return STATUS_SUCCESS;
#if A733_ENABLE_HIGH_SPEED
        case SdBusSpeedHigh:
        case SdBusSpeedSDR25:
            /* The phases follow the clock chosen by the next SetClock. */
            return STATUS_SUCCESS;
#endif
        default:
            return STATUS_NOT_SUPPORTED;
        }
    case SdSetDriverType:
        return (Operation->Parameters.DriverType == SdDriverTypeUndefined ||
                Operation->Parameters.DriverType == SdDriverTypeB) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    case SdSetDriveStrength:
        return (Operation->Parameters.DriveStrength == SdDriveStrengthUndefined) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    case SdSetPresetValue:
        return Operation->Parameters.PresetValueEnabled ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
    case SdSetBlockGapInterrupt:
        return Operation->Parameters.BlockGapIntEnabled ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
    case SdExecuteTuning:
        return STATUS_NOT_SUPPORTED;
    default:
        A733_LOG_ERROR("unsupported bus operation=%lu", (ULONG)Operation->Type);
        return STATUS_NOT_SUPPORTED;
    }
}

/* Fixed card (ACPI has no GPIO interrupt for PF6). PF6 is only logged. */
static BOOLEAN
A733GetCardDetectState(
    _In_ PVOID PrivateExtension
    )
{
    PA733_SD_EXTENSION extension = (PA733_SD_EXTENSION)PrivateExtension;

    if (!extension->CardDetectLogged) {
        extension->CardDetectLogged = TRUE;
        if (extension->PioRegisters != NULL) {
            ULONG cfg = READ_REGISTER_ULONG((volatile ULONG *)(extension->PioRegisters + A733_PF_CFG0_OFFSET));
            ULONG dat = READ_REGISTER_ULONG((volatile ULONG *)(extension->PioRegisters + A733_PF_DATA_OFFSET));
            A733_LOG_INFO("card detect: reporting present; PF_CFG0=%08lx PF_DAT=%08lx (PF6 %s) SMHC STATUS=%08lx",
                          cfg, dat, (dat & A733_PF6_MASK) ? "high" : "low",
                          A733SmhcRead(extension, SMHC_STATUS));
        }
    }
    return TRUE;
}

static BOOLEAN
A733GetWriteProtectState(
    _In_ PVOID PrivateExtension
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    return FALSE;
}

static VOID
A733Cleanup(
    _In_ PSD_MINIPORT Miniport
    )
{
    UCHAR slotIndex;

    if (Miniport == NULL) {
        return;
    }
    for (slotIndex = 0; slotIndex < Miniport->SlotCount; ++slotIndex) {
        PSDPORT_SLOT_EXTENSION slotExtension = Miniport->SlotExtensionList[slotIndex];
        PA733_SD_EXTENSION extension;

        if (slotExtension == NULL) {
            continue;
        }
        extension = (PA733_SD_EXTENSION)slotExtension->PrivateExtension;
        A733StopWorker(extension);
        (void)KeCancelTimer(&extension->BusyTimer);
        (void)KeRemoveQueueDpc(&extension->CompletionDpc);
        (void)KeRemoveQueueDpc(&extension->BusyDpc);
        if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
            KeFlushQueuedDpcs();
        }
        A733UnmapShared(extension);
    }
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    SDPORT_INITIALIZATION_DATA initializationData;

    RtlZeroMemory(&initializationData, sizeof(initializationData));
    initializationData.StructureSize = sizeof(initializationData);
    initializationData.GetSlotCount = A733GetSlotCount;
    initializationData.GetSlotCapabilities = A733GetSlotCapabilities;
    initializationData.Interrupt = A733Interrupt;
    initializationData.IssueRequest = A733IssueRequest;
    initializationData.GetResponse = A733GetResponse;
    initializationData.RequestDpc = A733RequestDpc;
    initializationData.ToggleEvents = A733ToggleEvents;
    initializationData.ClearEvents = A733ClearEvents;
    initializationData.Initialize = A733Initialize;
    initializationData.IssueBusOperation = A733IssueBusOperation;
    initializationData.GetCardDetectState = A733GetCardDetectState;
    initializationData.GetWriteProtectState = A733GetWriteProtectState;
    initializationData.Cleanup = A733Cleanup;
    initializationData.PrivateExtensionSize = sizeof(A733_SD_EXTENSION);
    initializationData.CrashdumpSupported = FALSE;

    return SdPortInitialize(DriverObject, RegistryPath, &initializationData);
}
