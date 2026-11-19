#include "A733SdMmc.h"

/*
 * One SD command, polled: a port of smhc_xfer() from the XNU driver
 * (ios-a733 bsd/dev/a733_sdmmc.c). Command flags, FIFO handling, the order
 * of the waits (data first, then COMMAND_DONE, then DATA_OVER or
 * AUTO_CMD_DONE, then DAT0 busy) and the error recovery (controller reset +
 * clock re-latch, as U-Boot does) are the same. No SDPORT completion here:
 * driver.c completes the request from a DPC.
 */

static BOOLEAN
A733ResponseHasCrc(
    _In_ SDPORT_RESPONSE_TYPE ResponseType
    )
{
    return ResponseType != SdResponseTypeNone && ResponseType != SdResponseTypeR3 &&
           ResponseType != SdResponseTypeR4;
}

BOOLEAN
A733ResponseHasBusy(
    _In_ SDPORT_RESPONSE_TYPE ResponseType
    )
{
    return ResponseType == SdResponseTypeR1B || ResponseType == SdResponseTypeR5B;
}

static ULONG
A733BuildCommand(
    _In_ PSDPORT_REQUEST Request
    )
{
    PSDPORT_COMMAND command = &Request->Command;
    ULONG value = SMHC_CMD_START | (command->Index & 0x3FUL);

    if (command->Index == 0) {
        value |= SMHC_CMD_SEND_INIT;            /* 74+ clocks before CMD0 */
    }
    if (command->Index == 12 || command->Type == SdCommandTypeAbort) {
        value |= SMHC_CMD_STOP_ABORT;
    }
    if (command->ResponseType != SdResponseTypeNone) {
        value |= SMHC_CMD_RESP_EXPIRE;
    }
    if (command->ResponseType == SdResponseTypeR2) {
        value |= SMHC_CMD_LONG_RESPONSE;
    }
    if (A733ResponseHasCrc(command->ResponseType)) {
        value |= SMHC_CMD_CHECK_CRC;
    }
    if (Request->Type == SdRequestTypeCommandWithTransfer) {
        value |= SMHC_CMD_DATA_EXPIRE | SMHC_CMD_WAIT_PRE_OVER;
        if (command->BlockCount > 1 && command->UseAutoCmd12) {
            value |= SMHC_CMD_AUTO_STOP;
        }
        if (command->TransferDirection == SdTransferDirectionWrite) {
            value |= SMHC_CMD_WRITE;
        }
    }
    return value;
}

static NTSTATUS
A733ValidateRequest(
    _In_ PSDPORT_REQUEST Request
    )
{
    PSDPORT_COMMAND command = &Request->Command;

    if (command->ResponseType == SdResponseTypeUndefined) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->Type != SdRequestTypeCommandWithTransfer) {
        return STATUS_SUCCESS;
    }
    if (command->TransferMethod != SdTransferMethodPio) {
        A733_LOG_ERROR("cmd=%u: transfer method %lu, only PIO is supported",
                       (ULONG)command->Index, (ULONG)command->TransferMethod);
        return STATUS_NOT_SUPPORTED;
    }
    if (command->BlockSize == 0 || command->BlockSize > 512U ||
        command->BlockCount == 0 || command->BlockCount > A733_ACCEPT_BLOCK_COUNT ||
        command->Length != (ULONG)command->BlockSize * command->BlockCount ||
        (command->TransferDirection != SdTransferDirectionRead &&
         command->TransferDirection != SdTransferDirectionWrite) ||
        command->UseAutoCmd23) {
        A733_LOG_ERROR("cmd=%u: bad transfer blksz=%u count=%u len=%lu dir=%lu acmd23=%u",
                       (ULONG)command->Index, (ULONG)command->BlockSize,
                       (ULONG)command->BlockCount, command->Length,
                       (ULONG)command->TransferDirection, (ULONG)command->UseAutoCmd23);
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

/* Wait for Done in RINT; any error bit ends the wait (XNU smhc_wait_rint). */
static NTSTATUS
A733WaitRint(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ ULONG Done,
    _In_ ULONG Milliseconds
    )
{
    LONGLONG deadline = A733DeadlineMs(Milliseconds);

    for (;;) {
        ULONG r = A733SmhcRead(Extension, SMHC_RINT);
        if (r & SMHC_RINT_ERROR_MASK) {
            Extension->LastRint = r;
            return A733SmhcStatusFromRint(r);
        }
        if (r & Done) {
            return STATUS_SUCCESS;
        }
        if (A733DeadlinePassed(deadline)) {
            Extension->LastRint = r;
            return STATUS_IO_TIMEOUT;
        }
    }
}

/*
 * Drain Length bytes from the FIFO. STATUS holds the fill level, so read
 * that many words without polling again (XNU smhc_pio_read).
 */
static NTSTATUS
A733PioRead(
    _In_ PA733_SD_EXTENSION Extension,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _In_ LONGLONG Deadline
    )
{
    ULONG offset = 0;

    while (offset < Length) {
        ULONG s = A733SmhcRead(Extension, SMHC_STATUS);
        ULONG n;
        if (s & SMHC_STATUS_FIFO_EMPTY) {
            ULONG r = A733SmhcRead(Extension, SMHC_RINT);
            if (r & SMHC_RINT_ERROR_MASK) {
                Extension->LastRint = r;
                return A733SmhcStatusFromRint(r);
            }
            if (A733DeadlinePassed(Deadline)) {
                Extension->LastRint = r;
                A733_LOG_ERROR("PIO read timeout at %lu/%lu STATUS=%08lx RINT=%08lx",
                               offset, Length, s, r);
                return STATUS_IO_TIMEOUT;
            }
            continue;
        }
        n = SMHC_STATUS_FIFO_LEVEL(s);
        if (n == 0) {
            n = (s & SMHC_STATUS_FIFO_FULL) ? 32 : 1;
        }
        while (n-- && offset < Length) {
            ULONG value = A733SmhcRead(Extension, SMHC_FIFO);
            ULONG chunk = min((ULONG)sizeof(ULONG), Length - offset);
            RtlCopyMemory(Buffer + offset, &value, chunk);
            offset += chunk;
        }
    }
    return STATUS_SUCCESS;
}

/* Feed Length bytes, checking FIFO_FULL before each word (XNU / U-Boot). */
static NTSTATUS
A733PioWrite(
    _In_ PA733_SD_EXTENSION Extension,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length,
    _In_ LONGLONG Deadline
    )
{
    ULONG offset = 0;

    while (offset < Length) {
        ULONG value = 0;
        ULONG chunk;
        if (A733SmhcRead(Extension, SMHC_STATUS) & SMHC_STATUS_FIFO_FULL) {
            ULONG r = A733SmhcRead(Extension, SMHC_RINT);
            if (r & SMHC_RINT_ERROR_MASK) {
                Extension->LastRint = r;
                return A733SmhcStatusFromRint(r);
            }
            if (A733DeadlinePassed(Deadline)) {
                Extension->LastRint = r;
                A733_LOG_ERROR("PIO write timeout at %lu/%lu RINT=%08lx", offset, Length, r);
                return STATUS_IO_TIMEOUT;
            }
            continue;
        }
        chunk = min((ULONG)sizeof(ULONG), Length - offset);
        RtlCopyMemory(&value, Buffer + offset, chunk);
        A733SmhcWrite(Extension, SMHC_FIFO, value);
        offset += chunk;
    }
    return STATUS_SUCCESS;
}

static VOID
A733Recover(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    (void)A733SmhcResetController(Extension);
    A733SmhcWrite(Extension, SMHC_WIDTH, Extension->WidthRegister);
    if (Extension->CardClockEnabled) {
        (void)A733SmhcUpdateClock(Extension);
    }
}

/*
 * Run Request (CommandNoTransfer or CommandWithTransfer) to the end of its
 * data phase. *WaitBusy tells the caller to wait for DAT0 release (R1b, or
 * any write: card programming) before completing the request.
 */
NTSTATUS
A733SmhcExecute(
    _In_ PA733_SD_EXTENSION Extension,
    _Inout_ PSDPORT_REQUEST Request,
    _Out_ PBOOLEAN WaitBusy
    )
{
    PSDPORT_COMMAND command = &Request->Command;
    BOOLEAN data = (Request->Type == SdRequestTypeCommandWithTransfer) ? TRUE : FALSE;
    BOOLEAN write = (data && command->TransferDirection == SdTransferDirectionWrite) ? TRUE : FALSE;
    BOOLEAN deferred = (data && command->DataBuffer == NULL) ? TRUE : FALSE;
    ULONG nblk = data ? command->BlockCount : 0;
    ULONG cmd;
    NTSTATUS status;

    *WaitBusy = FALSE;
    Extension->DeferredData = FALSE;
    Extension->LastRint = 0;
    Extension->Response[0] = 0;
    Extension->Response[1] = 0;
    Extension->Response[2] = 0;
    Extension->Response[3] = 0;

    status = A733ValidateRequest(Request);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (!Extension->CardClockEnabled) {
        A733_LOG_ERROR("cmd=%u issued with the card clock off", (ULONG)command->Index);
    }
    if (!NT_SUCCESS(A733WaitRegisterClear(Extension, SMHC_CMDR, SMHC_CMD_START, A733_CMD_START_WAIT_MS))) {
        A733_LOG_ERROR("controller still busy with a previous command; resetting", 0);
        A733Recover(Extension);
    }

    cmd = A733BuildCommand(Request);
    A733SmhcWrite(Extension, SMHC_RINT, MAXULONG);
    if (data) {
        A733SmhcWrite(Extension, SMHC_GCTRL,
                      A733SmhcRead(Extension, SMHC_GCTRL) | SMHC_GCTRL_FIFO_RESET);
        (void)A733WaitRegisterClear(Extension, SMHC_GCTRL, SMHC_GCTRL_FIFO_RESET, A733_FIFO_RESET_WAIT_MS);
        A733SmhcWrite(Extension, SMHC_BLKSZ, command->BlockSize);
        A733SmhcWrite(Extension, SMHC_BYTECNT, command->Length);
    }
    A733SmhcWrite(Extension, SMHC_CARG, command->Argument);
    A733SmhcWrite(Extension, SMHC_CMDR, cmd);

    status = STATUS_SUCCESS;
    if (deferred) {
        /* Fallback only: SDPORT gave no buffer yet. Take the response now,
         * leave DATA_OVER / AUTO_CMD_DONE in RINT for StartTransfer. */
        if (Extension->TraceCommandCount < A733_TRACE_COMMAND_LIMIT) {
            A733_LOG_ERROR("cmd=%u len=%lu: no DataBuffer at command time; FIFO serviced in StartTransfer",
                           (ULONG)command->Index, command->Length);
        }
        status = A733WaitRint(Extension, SMHC_RINT_COMMAND_DONE, A733_CMD_DONE_WAIT_MS);
        if (NT_SUCCESS(status)) {
            Extension->Response[0] = A733SmhcRead(Extension, SMHC_RESP0);
            Extension->LastRint = A733SmhcRead(Extension, SMHC_RINT);
            A733SmhcWrite(Extension, SMHC_RINT, SMHC_RINT_COMMAND_DONE);
            Extension->DeferredData = TRUE;
            Extension->DeferredCmdr = cmd;
            return STATUS_SUCCESS;
        }
    } else if (write) {
        status = A733PioWrite(Extension, command->DataBuffer, command->Length,
                              A733DeadlineMs(1000UL + 5UL * nblk));
    } else if (data) {
        status = A733PioRead(Extension, command->DataBuffer, command->Length,
                             A733DeadlineMs(1000UL + 2UL * nblk));
    }
    if (NT_SUCCESS(status)) {
        status = A733WaitRint(Extension, SMHC_RINT_COMMAND_DONE, A733_CMD_DONE_WAIT_MS);
    }
    if (NT_SUCCESS(status) && data) {
        status = A733WaitRint(Extension,
                              (cmd & SMHC_CMD_AUTO_STOP) ? SMHC_RINT_AUTO_CMD_DONE : SMHC_RINT_DATA_OVER,
                              write ? A733_WRITE_DONE_WAIT_MS : A733_READ_DONE_WAIT_MS);
    }
    if (NT_SUCCESS(status)) {
        Extension->Response[0] = A733SmhcRead(Extension, SMHC_RESP0);
        if (command->ResponseType == SdResponseTypeR2) {
            Extension->Response[1] = A733SmhcRead(Extension, SMHC_RESP1);
            Extension->Response[2] = A733SmhcRead(Extension, SMHC_RESP2);
            Extension->Response[3] = A733SmhcRead(Extension, SMHC_RESP3);
        }
        Extension->LastRint = A733SmhcRead(Extension, SMHC_RINT);
        *WaitBusy = (write || (!data && A733ResponseHasBusy(command->ResponseType))) ? TRUE : FALSE;
    } else {
        if (Extension->LastRint == 0) {
            Extension->LastRint = A733SmhcRead(Extension, SMHC_RINT);
        }
        if (Extension->FailureLogCount < A733_FAILURE_LOG_LIMIT &&
            !(command->Index == 8 && status == STATUS_IO_TIMEOUT)) {
            Extension->FailureLogCount++;
            A733_LOG_ERROR("cmd=%u arg=%08lx CMDR=%08lx failed %08lx RINT=%08lx STATUS=%08lx CLKCR=%08lx card=%llu Hz",
                           (ULONG)command->Index, command->Argument, cmd, (ULONG)status,
                           Extension->LastRint, A733SmhcRead(Extension, SMHC_STATUS),
                           A733SmhcRead(Extension, SMHC_CLKCR), Extension->CardHz);
        }
        A733Recover(Extension);
    }
    A733SmhcWrite(Extension, SMHC_RINT, MAXULONG);
    return status;
}

/*
 * StartTransfer half of the fallback path: move the data now, then wait for
 * DATA_OVER / AUTO_CMD_DONE exactly like the normal path.
 */
NTSTATUS
A733SmhcFinishDeferredData(
    _In_ PA733_SD_EXTENSION Extension,
    _Inout_ PSDPORT_REQUEST Request,
    _Out_ PBOOLEAN WaitBusy
    )
{
    PSDPORT_COMMAND command = &Request->Command;
    BOOLEAN write = (command->TransferDirection == SdTransferDirectionWrite) ? TRUE : FALSE;
    ULONG nblk = command->BlockCount;
    ULONG cmd = Extension->DeferredCmdr;
    NTSTATUS status;

    *WaitBusy = FALSE;
    Extension->DeferredData = FALSE;
    Extension->LastRint = 0;
    if (command->DataBuffer == NULL) {
        A733_LOG_ERROR("StartTransfer cmd=%u still has no DataBuffer", (ULONG)command->Index);
        A733Recover(Extension);
        return STATUS_INVALID_PARAMETER;
    }
    if (write) {
        status = A733PioWrite(Extension, command->DataBuffer, command->Length,
                              A733DeadlineMs(1000UL + 5UL * nblk));
    } else {
        status = A733PioRead(Extension, command->DataBuffer, command->Length,
                             A733DeadlineMs(1000UL + 2UL * nblk));
    }
    if (NT_SUCCESS(status)) {
        status = A733WaitRint(Extension,
                              (cmd & SMHC_CMD_AUTO_STOP) ? SMHC_RINT_AUTO_CMD_DONE : SMHC_RINT_DATA_OVER,
                              write ? A733_WRITE_DONE_WAIT_MS : A733_READ_DONE_WAIT_MS);
    }
    if (NT_SUCCESS(status)) {
        Extension->LastRint = A733SmhcRead(Extension, SMHC_RINT);
        *WaitBusy = write;
    } else {
        if (Extension->LastRint == 0) {
            Extension->LastRint = A733SmhcRead(Extension, SMHC_RINT);
        }
        if (Extension->FailureLogCount < A733_FAILURE_LOG_LIMIT) {
            Extension->FailureLogCount++;
            A733_LOG_ERROR("deferred data cmd=%u len=%lu failed %08lx RINT=%08lx STATUS=%08lx",
                           (ULONG)command->Index, command->Length, (ULONG)status,
                           Extension->LastRint, A733SmhcRead(Extension, SMHC_STATUS));
        }
        A733Recover(Extension);
    }
    A733SmhcWrite(Extension, SMHC_RINT, MAXULONG);
    return status;
}
