#pragma once

#include <ntddk.h>
#include <sdport.h>

/*
 * 2.5.0: hardware layer ported from the XNU SMHC0 driver that works on this
 * board (ios-a733 ref/xnu-base/bsd/dev/a733_sdmmc.c: polled PIO, 4-bit,
 * 25/50 MHz, reads and writes). Same rules:
 *
 *  - The CCU and the pin controller are never written. Vendor U-Boot leaves
 *    SMHC0 running (bus gate + reset deasserted, module clock normally
 *    0x81000003 = pll-peri0-400m / 4 = 100 MHz, PF0-PF5 in SDC0 mode) and
 *    EDK2 does not change it. The driver only READS the CCU to learn the
 *    module clock, exactly like XNU.
 *  - 2x timing mode (NTSR bit 31): card clock = module / 2 for CLKCR div 0,
 *    module / (4 * div) otherwise (vendor sunxi-mmc-v5p3x, U-Boot).
 *  - Vendor phases: <= 26 MHz cmd drive 180, data drive 90, sample phase 0;
 *    High Speed cmd and data drive 180, sample phase 1.
 *  - One command at a time, polled, no IDMAC, no controller interrupts.
 *    A data command moves its whole payload through the FIFO before it is
 *    completed to SDPORT; the later StartTransfer request only reports the
 *    stored result.
 */

/* High Speed (CMD6 group 1 function 1, 50 MHz) was proven by XNU on this
 * board, but the first Windows run stays at Default Speed (25 MHz). */
#ifndef A733_ENABLE_HIGH_SPEED
#define A733_ENABLE_HIGH_SPEED 0
#endif
#if A733_ENABLE_HIGH_SPEED
#define A733_MAX_CARD_KHZ 50000UL
#else
#define A733_MAX_CARD_KHZ 25000UL
#endif
#define A733_HS_PHASE_MIN_HZ 26000000ULL

/* SDHF (paired UEFI v59.2 ACPI proof) is probed and logged. 0 = optional:
 * the current v59 firmware has no SDHF and leaves U-Boot's SMHC0 state,
 * which is what XNU relies on. 1 = refuse to start without a valid SDHF. */
#ifndef A733_REQUIRE_SDHF
#define A733_REQUIRE_SDHF 0
#endif

/* 1 = log every command of the first A733_TRACE_COMMAND_LIMIT and print
 * the bring-up messages at ERROR level so they reach KD without changing
 * Kd_IHVDRIVER_Mask. Failures are always logged (up to a limit). */
#ifndef A733_TRACE_BRINGUP
#define A733_TRACE_BRINGUP 1
#endif
#define A733_TRACE_COMMAND_LIMIT 96U
#define A733_FAILURE_LOG_LIMIT   256U

#define A733_SMHC_PHYSICAL_BASE 0x04020000ULL
#define A733_SMHC_REGISTER_LENGTH 0x1000U
#define A733_CCU_PHYSICAL_BASE    0x02002000ULL
#define A733_CCU_REGISTER_LENGTH  0x1000U
#define A733_PIO_PHYSICAL_BASE    0x02000000ULL
#define A733_PIO_REGISTER_LENGTH  0x600U

/* CCU (sun60iw2, read only): SMHC0 module clock and bus gate / reset. */
#define CCU_SMHC0_CLK_OFFSET   0xD00U
#define CCU_SMHC_BGR_OFFSET    0xD0CU
#define CCU_SMHC0_BGR_GATE     (1UL << 0)
#define CCU_SMHC0_BGR_RESET    (1UL << 16)    /* 1 = deasserted */
#define A733_SMHC_MAX_MODULE_HZ 408000000ULL  /* div 255 keeps ident <= 400 kHz */

/* PIO (type 4 layout, read only): PF bank power mode, PF6 card detect. */
#define A733_PIO_POWER_MODE_OFFSET 0x040U
#define A733_PF_POWER_18V_MASK    (1UL << 5)
#define A733_PF_CFG0_OFFSET       0x300U
#define A733_PF_DATA_OFFSET       0x310U
#define A733_PF6_MASK            (1UL << 6)
#define A733_PF6_FUNCTION_MASK   (0xFUL << 24)

#define SMHC_GCTRL    0x000U
#define SMHC_CLKCR    0x004U
#define SMHC_TMOUT    0x008U
#define SMHC_WIDTH    0x00CU
#define SMHC_BLKSZ    0x010U
#define SMHC_BYTECNT  0x014U
#define SMHC_CMDR     0x018U
#define SMHC_CARG     0x01CU
#define SMHC_RESP0    0x020U
#define SMHC_RESP1    0x024U
#define SMHC_RESP2    0x028U
#define SMHC_RESP3    0x02CU
#define SMHC_IMASK    0x030U
#define SMHC_RINT     0x038U
#define SMHC_STATUS   0x03CU
#define SMHC_FTRGL    0x040U
#define SMHC_FUNS     0x044U
#define SMHC_DBGC     0x050U
#define SMHC_NTSR     0x05CU
#define SMHC_DMAC     0x080U
#define SMHC_IDST     0x088U
#define SMHC_IDIE     0x08CU
#define SMHC_THLD     0x100U
#define SMHC_DRV_DL   0x140U
#define SMHC_FIFO     0x200U
#define SMHC_VERS     0x300U

#define SMHC_GCTRL_SOFT_RESET    (1UL << 0)
#define SMHC_GCTRL_FIFO_RESET    (1UL << 1)
#define SMHC_GCTRL_DMA_RESET     (1UL << 2)
#define SMHC_GCTRL_RESET_MASK    0x7UL
#define SMHC_GCTRL_INTERRUPT_ENABLE (1UL << 4)
#define SMHC_GCTRL_DMA_ENABLE    (1UL << 5)
#define SMHC_GCTRL_DDR_MODE      (1UL << 10)
#define SMHC_GCTRL_DTIME_UNIT    (1UL << 11)
#define SMHC_GCTRL_ACCESS_DONE_DIRECT (1UL << 30)
#define SMHC_GCTRL_ACCESS_AHB    (1UL << 31)

/* Values the XNU driver (and Linux) write after every controller reset. */
#define SMHC_TIMEOUT_MAX         0xFFFFFFFFUL
#define SMHC_FIFO_TRIGGER_PIO    0x20070008UL  /* burst 8, RX 7, TX 8 */
#define SMHC_DEBUG_DEFAULT       0x00000DEBUL
#define SMHC_FUNS_DEFAULT        0xCEAA0000UL
#define SMHC_THLD_READ_EN        (1UL << 0)
#define SMHC_THLD_WRITE_EN       (1UL << 2)
#define SMHC_THLD_READ_THLD(x)   (((x) & 0xFFFUL) << 16)
#define SMHC_THLD_DEFAULT        (SMHC_THLD_READ_THLD(512UL) | SMHC_THLD_WRITE_EN | SMHC_THLD_READ_EN)

#define SMHC_CLK_DIV_MASK        0xFFUL
#define SMHC_CLK_CARD_ON         (1UL << 16)
#define SMHC_CLK_LOW_POWER       (1UL << 17)
#define SMHC_CLK_MASK_DATA0      (1UL << 31)

#define SMHC_NTSR_2X_TIMING_MODE (1UL << 31)
#define SMHC_NTSR_SAMPLE_MASK    0x330UL       /* cmd [5:4], data [9:8] */
#define SMHC_NTSR_SAMPLE_HS      0x110UL       /* phase 1 for cmd and data */
#define SMHC_DRV_DL_CMD_PH_180   (1UL << 16)
#define SMHC_DRV_DL_DAT_PH_180   (1UL << 17)

#define SMHC_CMD_RESP_EXPIRE     (1UL << 6)
#define SMHC_CMD_LONG_RESPONSE   (1UL << 7)
#define SMHC_CMD_CHECK_CRC       (1UL << 8)
#define SMHC_CMD_DATA_EXPIRE     (1UL << 9)
#define SMHC_CMD_WRITE           (1UL << 10)
#define SMHC_CMD_AUTO_STOP       (1UL << 12)
#define SMHC_CMD_WAIT_PRE_OVER   (1UL << 13)
#define SMHC_CMD_STOP_ABORT      (1UL << 14)
#define SMHC_CMD_SEND_INIT       (1UL << 15)
#define SMHC_CMD_UPCLK_ONLY      (1UL << 21)
#define SMHC_CMD_START           (1UL << 31)

#define SMHC_RINT_RESP_ERROR     (1UL << 1)
#define SMHC_RINT_COMMAND_DONE   (1UL << 2)
#define SMHC_RINT_DATA_OVER      (1UL << 3)
#define SMHC_RINT_RESP_CRC       (1UL << 6)
#define SMHC_RINT_DATA_CRC       (1UL << 7)
#define SMHC_RINT_RESP_TIMEOUT   (1UL << 8)
#define SMHC_RINT_DATA_TIMEOUT   (1UL << 9)
#define SMHC_RINT_FIFO_RUN       (1UL << 11)
#define SMHC_RINT_HARD_WLOCK     (1UL << 12)
#define SMHC_RINT_START_BIT      (1UL << 13)
#define SMHC_RINT_AUTO_CMD_DONE  (1UL << 14)
#define SMHC_RINT_END_BIT        (1UL << 15)
#define SMHC_RINT_SDIO_INTERRUPT (1UL << 16)
#define SMHC_RINT_ERROR_MASK     0xBBC2UL     /* same set as XNU RINT_ERRORS */

#define SMHC_STATUS_FIFO_EMPTY   (1UL << 2)
#define SMHC_STATUS_FIFO_FULL    (1UL << 3)
#define SMHC_STATUS_CARD_PRESENT (1UL << 8)
#define SMHC_STATUS_CARD_BUSY    (1UL << 9)
#define SMHC_STATUS_FIFO_LEVEL(s) (((s) >> 17) & 0x3FFFUL)

/* Timeouts, milliseconds (XNU values). They only matter when the hardware
 * misbehaves: the controller reports a response timeout after 255 card
 * clocks. */
#define A733_CMD_START_WAIT_MS   100UL
#define A733_RESET_WAIT_MS       250UL
#define A733_UPDATE_CLOCK_WAIT_MS 250UL
#define A733_FIFO_RESET_WAIT_MS  10UL
#define A733_CMD_DONE_WAIT_MS    1000UL
#define A733_READ_DONE_WAIT_MS   500UL
#define A733_WRITE_DONE_WAIT_MS  3000UL
#define A733_BUSY_WAIT_MS        3000UL
#define A733_BUSY_POLL_100NS     10000LL       /* 1 ms busy poll timer */

/* Worker thread priority: above normal application threads (8), below the
 * real-time range, like other storage worker threads. */
#ifndef A733_WORKER_PRIORITY
#define A733_WORKER_PRIORITY     12
#endif
#define A733_DRIVER_VERSION_STRING "2.6.0.0"

/* Advertised to sdbus. sdbus does not always honour it (format sends 2048
 * blocks = 1 MiB writes), so A733ValidateRequest accepts up to
 * A733_ACCEPT_BLOCK_COUNT: BYTECNT is 32 bits and PIO has no size limit. */
#define A733_MAX_BLOCK_COUNT     2048U
#define A733_ACCEPT_BLOCK_COUNT  65535U

#define A733_LOG_ERROR(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "A733SdMmc: " fmt "\n", __VA_ARGS__)
#if A733_TRACE_BRINGUP
#define A733_LOG_INFO(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "A733SdMmc: " fmt "\n", __VA_ARGS__)
#else
#define A733_LOG_INFO(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "A733SdMmc: " fmt "\n", __VA_ARGS__)
#endif

typedef struct _A733_SD_EXTENSION {
    volatile UCHAR *Registers;
    volatile UCHAR *CcuRegisters;     /* mapped read only */
    volatile UCHAR *PioRegisters;     /* mapped read only */

    /* Clock state, all derived from the CCU readback. */
    ULONGLONG ModuleHz;
    ULONG ClockDivider;
    ULONGLONG CardHz;
    BOOLEAN HighSpeedTiming;
    BOOLEAN CardClockEnabled;
    ULONG WidthRegister;              /* restored after a controller reset */

    BOOLEAN CrashdumpMode;
    BOOLEAN FirmwareHandoffReady;     /* valid SDHF seen (informational) */
    BOOLEAN CardDetectLogged;
    BOOLEAN RequestIrqlLogged;

    ULONG Response[4];
    ULONG LastRint;                   /* RINT that ended the last command */
    ULONG TraceCommandCount;
    ULONG FailureLogCount;

    /* Request ownership. Only <= DISPATCH_LEVEL code takes StateLock; the
     * ISR never does (it has nothing to do: IMASK stays 0). */
    KSPIN_LOCK StateLock;
    PSDPORT_REQUEST ActiveRequest;
    NTSTATUS CompletionStatus;
    KDPC CompletionDpc;
    KTIMER BusyTimer;
    KDPC BusyDpc;
    LONGLONG BusyDeadline;            /* performance counter ticks */

    /* Set while IssueRequest runs a command on the controller; a reset from
     * another CPU is refused meanwhile. */
    volatile LONG Executing;

    /* The data of a CommandWithTransfer is normally moved during the
     * command; the following StartTransfer must name the same request.
     * DeferredData: SDPORT gave no DataBuffer with the command, so the FIFO
     * is serviced in StartTransfer instead. */
    /* 2.6: requests run on a dedicated system thread at PASSIVE_LEVEL, so
     * a 1 MiB PIO transfer (~50-100 ms) is preemptible instead of holding a
     * CPU at the IRQL SDPORT calls IssueRequest with. QueuedRequest is set
     * by IssueRequest under StateLock and taken by the worker. Without a
     * worker (thread creation failed) requests run inline as in 2.5. */
    PETHREAD WorkerThread;
    KEVENT WorkEvent;                 /* synchronization (auto-reset) */
    volatile LONG StopWorker;
    PSDPORT_REQUEST QueuedRequest;

    BOOLEAN TransferReady;
    BOOLEAN DeferredData;
    ULONG DeferredCmdr;
    ULONG TransferIndex;
    ULONG TransferArgument;
    ULONG TransferLength;
    ULONG TransferDirection;
    PVOID TransferBuffer;
} A733_SD_EXTENSION, *PA733_SD_EXTENSION;

DRIVER_INITIALIZE DriverEntry;

ULONG A733SmhcRead(_In_ PA733_SD_EXTENSION Extension, _In_ ULONG Offset);
VOID A733SmhcWrite(_In_ PA733_SD_EXTENSION Extension, _In_ ULONG Offset, _In_ ULONG Value);
ULONG A733CcuRead(_In_ PA733_SD_EXTENSION Extension, _In_ ULONG Offset);
LONGLONG A733DeadlineMs(_In_ ULONG Milliseconds);
BOOLEAN A733DeadlinePassed(_In_ LONGLONG Deadline);
NTSTATUS A733WaitRegisterClear(_In_ PA733_SD_EXTENSION Extension, _In_ ULONG Offset,
                               _In_ ULONG Mask, _In_ ULONG Milliseconds);

ULONGLONG A733CcuModuleHz(_In_ ULONG CcuClockRegister);
ULONGLONG A733CardHz(_In_ ULONGLONG ModuleHz, _In_ ULONG Divider);
ULONG A733PickDivider(_In_ ULONGLONG ModuleHz, _In_ ULONGLONG LimitHz);

NTSTATUS A733SmhcProbeClock(_In_ PA733_SD_EXTENSION Extension);
NTSTATUS A733SmhcResetController(_In_ PA733_SD_EXTENSION Extension);
NTSTATUS A733SmhcUpdateClock(_In_ PA733_SD_EXTENSION Extension);
NTSTATUS A733SmhcReset(_In_ PA733_SD_EXTENSION Extension, _In_ SDPORT_RESET_TYPE ResetType);
NTSTATUS A733SmhcSetClock(_In_ PA733_SD_EXTENSION Extension, _In_ ULONG FrequencyKhz);
NTSTATUS A733SmhcSetBusWidth(_In_ PA733_SD_EXTENSION Extension, _In_ SDPORT_BUS_WIDTH Width);
NTSTATUS A733SmhcStatusFromRint(_In_ ULONG Rint);
BOOLEAN A733SmhcCanSignal33(_In_ PA733_SD_EXTENSION Extension);
VOID A733SmhcDumpState(_In_ PA733_SD_EXTENSION Extension, _In_ PCSTR Tag);

NTSTATUS A733SmhcExecute(_In_ PA733_SD_EXTENSION Extension, _Inout_ PSDPORT_REQUEST Request,
                         _Out_ PBOOLEAN WaitBusy);
NTSTATUS A733SmhcFinishDeferredData(_In_ PA733_SD_EXTENSION Extension, _Inout_ PSDPORT_REQUEST Request,
                                    _Out_ PBOOLEAN WaitBusy);
BOOLEAN A733ResponseHasBusy(_In_ SDPORT_RESPONSE_TYPE ResponseType);
