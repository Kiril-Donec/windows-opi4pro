#include "A733SdMmc.h"

/*
 * Controller setup, reset and clock: a direct port of smhc_reset(),
 * smhc_update_clk(), smhc_set_clock() and the clock helpers of the XNU
 * driver (ios-a733 bsd/dev/a733_sdmmc.c), which runs this SMHC0 on this
 * board. Only SMHC0 registers are written; CCU and PIO are read only.
 */

ULONG
A733SmhcRead(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ ULONG Offset
    )
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Extension->Registers + Offset));
}

VOID
A733SmhcWrite(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Extension->Registers + Offset), Value);
}

ULONG
A733CcuRead(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ ULONG Offset
    )
{
    if (Extension->CcuRegisters == NULL) {
        return 0;
    }
    return READ_REGISTER_ULONG((volatile ULONG *)(Extension->CcuRegisters + Offset));
}

LONGLONG
A733DeadlineMs(
    _In_ ULONG Milliseconds
    )
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER now = KeQueryPerformanceCounter(&frequency);
    return now.QuadPart + (frequency.QuadPart * (LONGLONG)Milliseconds) / 1000;
}

BOOLEAN
A733DeadlinePassed(
    _In_ LONGLONG Deadline
    )
{
    return KeQueryPerformanceCounter(NULL).QuadPart > Deadline;
}

/* Wait until (reg & mask) == 0 (XNU smhc_wait_clear). */
NTSTATUS
A733WaitRegisterClear(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Milliseconds
    )
{
    LONGLONG deadline = A733DeadlineMs(Milliseconds);

    while (A733SmhcRead(Extension, Offset) & Mask) {
        if (A733DeadlinePassed(deadline)) {
            A733_LOG_ERROR("register clear timeout offset=%03lx mask=%08lx value=%08lx irql=%lu",
                           Offset, Mask, A733SmhcRead(Extension, Offset),
                           (ULONG)KeGetCurrentIrql());
            return STATUS_IO_TIMEOUT;
        }
        SdPortWait(1);
    }
    return STATUS_SUCCESS;
}

/*
 * SMHC0 module clock from the CCU register: parent / (M + 1) / (N + 1), with
 * M [4:0], N [12:8], mux [26:24], gate [31]. 0 for a gated clock or an
 * unknown parent (XNU a733sd_ccu_mod_hz, ccu-sun60iw2.c).
 */
ULONGLONG
A733CcuModuleHz(
    _In_ ULONG CcuClockRegister
    )
{
    static const ULONGLONG parents[] = {
        24000000ULL,    /* sys24M */
        400000000ULL,   /* pll-peri0-400m */
        300000000ULL,   /* pll-peri0-300m */
        400000000ULL,   /* pll-peri1-400m */
        300000000ULL,   /* pll-peri1-300m */
    };
    ULONG mux = (CcuClockRegister >> 24) & 0x7UL;

    if (!(CcuClockRegister & (1UL << 31)) || mux >= RTL_NUMBER_OF(parents)) {
        return 0;
    }
    return parents[mux] / ((CcuClockRegister & 0x1FUL) + 1) /
           (((CcuClockRegister >> 8) & 0x1FUL) + 1);
}

/* 2x timing mode: module / 2 for divider 0, module / (4 * div) otherwise. */
ULONGLONG
A733CardHz(
    _In_ ULONGLONG ModuleHz,
    _In_ ULONG Divider
    )
{
    return Divider ? ModuleHz / (4ULL * Divider) : ModuleHz / 2;
}

/* Smallest divider whose card clock is <= LimitHz, or 256 if none fits. */
ULONG
A733PickDivider(
    _In_ ULONGLONG ModuleHz,
    _In_ ULONGLONG LimitHz
    )
{
    ULONGLONG div;

    if (LimitHz == 0) {
        return SMHC_CLK_DIV_MASK + 1;
    }
    if (ModuleHz / 2 <= LimitHz) {
        return 0;
    }
    div = (ModuleHz + 4 * LimitHz - 1) / (4 * LimitHz);
    return div > SMHC_CLK_DIV_MASK ? SMHC_CLK_DIV_MASK + 1 : (ULONG)div;
}

VOID
A733SmhcDumpState(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ PCSTR Tag
    )
{
    A733_LOG_INFO("%s CCU_CLK=%08lx CCU_BGR=%08lx VERS=%08lx GCTRL=%08lx CLKCR=%08lx TMOUT=%08lx WIDTH=%08lx",
                  Tag,
                  A733CcuRead(Extension, CCU_SMHC0_CLK_OFFSET),
                  A733CcuRead(Extension, CCU_SMHC_BGR_OFFSET),
                  A733SmhcRead(Extension, SMHC_VERS),
                  A733SmhcRead(Extension, SMHC_GCTRL),
                  A733SmhcRead(Extension, SMHC_CLKCR),
                  A733SmhcRead(Extension, SMHC_TMOUT),
                  A733SmhcRead(Extension, SMHC_WIDTH));
    A733_LOG_INFO("%s IMASK=%08lx RINT=%08lx STATUS=%08lx FTRGL=%08lx FUNS=%08lx NTSR=%08lx THLD=%08lx DRV_DL=%08lx",
                  Tag,
                  A733SmhcRead(Extension, SMHC_IMASK),
                  A733SmhcRead(Extension, SMHC_RINT),
                  A733SmhcRead(Extension, SMHC_STATUS),
                  A733SmhcRead(Extension, SMHC_FTRGL),
                  A733SmhcRead(Extension, SMHC_FUNS),
                  A733SmhcRead(Extension, SMHC_NTSR),
                  A733SmhcRead(Extension, SMHC_THLD),
                  A733SmhcRead(Extension, SMHC_DRV_DL));
}

/*
 * Read the module clock U-Boot left (XNU a733_sd_card_init). Fails, like
 * XNU, when the bus clock is gated / in reset or the rate is unusable;
 * nothing is reprogrammed.
 */
NTSTATUS
A733SmhcProbeClock(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    ULONG clk = A733CcuRead(Extension, CCU_SMHC0_CLK_OFFSET);
    ULONG bgr = A733CcuRead(Extension, CCU_SMHC_BGR_OFFSET);
    ULONG identDiv;

    A733_LOG_INFO("CCU SMHC0_CLK=%08lx BGR=%08lx (read only)", clk, bgr);
    if ((bgr & (CCU_SMHC0_BGR_GATE | CCU_SMHC0_BGR_RESET)) !=
        (CCU_SMHC0_BGR_GATE | CCU_SMHC0_BGR_RESET)) {
        A733_LOG_ERROR("SMHC0 bus clock gated or in reset (BGR=%08lx); firmware did not leave it running", bgr);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Extension->ModuleHz = A733CcuModuleHz(clk);
    if (Extension->ModuleHz == 0 || Extension->ModuleHz > A733_SMHC_MAX_MODULE_HZ) {
        A733_LOG_ERROR("unusable SMHC0 module clock %llu Hz (CCU_CLK=%08lx)", Extension->ModuleHz, clk);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    identDiv = A733PickDivider(Extension->ModuleHz, 400000ULL);
    if (identDiv > SMHC_CLK_DIV_MASK) {
        identDiv = SMHC_CLK_DIV_MASK;
    }
    A733_LOG_INFO("module clock %llu Hz; ident div %lu (%llu Hz), 25 MHz div %lu (%llu Hz), 50 MHz div %lu (%llu Hz)",
                  Extension->ModuleHz,
                  identDiv, A733CardHz(Extension->ModuleHz, identDiv),
                  A733PickDivider(Extension->ModuleHz, 25000000ULL),
                  A733CardHz(Extension->ModuleHz, A733PickDivider(Extension->ModuleHz, 25000000ULL)),
                  A733PickDivider(Extension->ModuleHz, 50000000ULL),
                  A733CardHz(Extension->ModuleHz, A733PickDivider(Extension->ModuleHz, 50000000ULL)));
    return STATUS_SUCCESS;
}

/* Polled operation, CPU (AHB) FIFO access, no IDMAC (XNU smhc_gctrl_pio). */
static VOID
A733SmhcGctrlPio(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    ULONG v = A733SmhcRead(Extension, SMHC_GCTRL);
    v &= ~(SMHC_GCTRL_INTERRUPT_ENABLE | SMHC_GCTRL_DMA_ENABLE | SMHC_GCTRL_DDR_MODE |
           SMHC_GCTRL_DTIME_UNIT | SMHC_GCTRL_ACCESS_DONE_DIRECT);
    v |= SMHC_GCTRL_ACCESS_AHB;
    A733SmhcWrite(Extension, SMHC_GCTRL, v);
}

/*
 * Soft, FIFO and DMA reset, then the polled PIO setup (XNU smhc_reset).
 * CLKCR and NTSR survive; the caller re-latches the clock if it was on.
 */
NTSTATUS
A733SmhcResetController(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    NTSTATUS status;

    A733SmhcWrite(Extension, SMHC_GCTRL,
                  A733SmhcRead(Extension, SMHC_GCTRL) | SMHC_GCTRL_RESET_MASK);
    status = A733WaitRegisterClear(Extension, SMHC_GCTRL, SMHC_GCTRL_RESET_MASK, A733_RESET_WAIT_MS);
    A733SmhcGctrlPio(Extension);
    A733SmhcWrite(Extension, SMHC_DMAC, 0);
    A733SmhcWrite(Extension, SMHC_IDIE, 0);
    A733SmhcWrite(Extension, SMHC_IDST, MAXULONG);
    A733SmhcWrite(Extension, SMHC_IMASK, 0);
    A733SmhcWrite(Extension, SMHC_RINT, MAXULONG);
    A733SmhcWrite(Extension, SMHC_TMOUT, SMHC_TIMEOUT_MAX);
    A733SmhcWrite(Extension, SMHC_FTRGL, SMHC_FIFO_TRIGGER_PIO);
    A733SmhcWrite(Extension, SMHC_DBGC, SMHC_DEBUG_DEFAULT);
    A733SmhcWrite(Extension, SMHC_FUNS, SMHC_FUNS_DEFAULT);
    A733SmhcWrite(Extension, SMHC_THLD, SMHC_THLD_DEFAULT);
    return status;
}

/* Latch CLKCR into the card clock domain; no command reaches the card. */
NTSTATUS
A733SmhcUpdateClock(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    NTSTATUS status;

    A733SmhcWrite(Extension, SMHC_CMDR,
                  SMHC_CMD_START | SMHC_CMD_UPCLK_ONLY | SMHC_CMD_WAIT_PRE_OVER);
    status = A733WaitRegisterClear(Extension, SMHC_CMDR, SMHC_CMD_START, A733_UPDATE_CLOCK_WAIT_MS);
    A733SmhcWrite(Extension, SMHC_RINT,
                  A733SmhcRead(Extension, SMHC_RINT) & ~SMHC_RINT_SDIO_INTERRUPT);
    if (!NT_SUCCESS(status)) {
        A733_LOG_ERROR("clock update timeout CLKCR=%08lx CMDR=%08lx RINT=%08lx STATUS=%08lx",
                       A733SmhcRead(Extension, SMHC_CLKCR), A733SmhcRead(Extension, SMHC_CMDR),
                       A733SmhcRead(Extension, SMHC_RINT), A733SmhcRead(Extension, SMHC_STATUS));
    }
    return status;
}

/*
 * Card clock off, new divider, 2x timing mode with the vendor phases, clock
 * on (XNU smhc_set_clock). DATA0 is masked only while the clock changes.
 */
static NTSTATUS
A733SmhcProgramClock(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ ULONG Divider,
    _In_ BOOLEAN HighSpeed
    )
{
    ULONG v = A733SmhcRead(Extension, SMHC_CLKCR);
    ULONG n;
    ULONG d;
    NTSTATUS status;

    v &= ~(SMHC_CLK_CARD_ON | SMHC_CLK_LOW_POWER | SMHC_CLK_MASK_DATA0);
    A733SmhcWrite(Extension, SMHC_CLKCR, v | SMHC_CLK_MASK_DATA0);
    status = A733SmhcUpdateClock(Extension);
    if (!NT_SUCCESS(status)) {
        A733SmhcWrite(Extension, SMHC_CLKCR, A733SmhcRead(Extension, SMHC_CLKCR) & ~SMHC_CLK_MASK_DATA0);
        return status;
    }
    v = (v & ~SMHC_CLK_DIV_MASK) | (Divider & SMHC_CLK_DIV_MASK);
    A733SmhcWrite(Extension, SMHC_CLKCR, v | SMHC_CLK_MASK_DATA0);

    n = A733SmhcRead(Extension, SMHC_NTSR) & ~SMHC_NTSR_SAMPLE_MASK;
    A733SmhcWrite(Extension, SMHC_NTSR,
                  n | SMHC_NTSR_2X_TIMING_MODE | (HighSpeed ? SMHC_NTSR_SAMPLE_HS : 0));
    d = A733SmhcRead(Extension, SMHC_DRV_DL) | SMHC_DRV_DL_CMD_PH_180;
    A733SmhcWrite(Extension, SMHC_DRV_DL,
                  HighSpeed ? (d | SMHC_DRV_DL_DAT_PH_180) : (d & ~SMHC_DRV_DL_DAT_PH_180));

    A733SmhcWrite(Extension, SMHC_CLKCR, v | SMHC_CLK_CARD_ON | SMHC_CLK_MASK_DATA0);
    status = A733SmhcUpdateClock(Extension);
    A733SmhcWrite(Extension, SMHC_CLKCR, A733SmhcRead(Extension, SMHC_CLKCR) & ~SMHC_CLK_MASK_DATA0);
    return status;
}

static NTSTATUS
A733SmhcClockOff(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    ULONG v = A733SmhcRead(Extension, SMHC_CLKCR);
    NTSTATUS status;

    v &= ~(SMHC_CLK_CARD_ON | SMHC_CLK_LOW_POWER | SMHC_CLK_MASK_DATA0);
    A733SmhcWrite(Extension, SMHC_CLKCR, v | SMHC_CLK_MASK_DATA0);
    status = A733SmhcUpdateClock(Extension);
    A733SmhcWrite(Extension, SMHC_CLKCR, A733SmhcRead(Extension, SMHC_CLKCR) & ~SMHC_CLK_MASK_DATA0);
    return status;
}

/* SDPORT reset of any type: the full XNU / U-Boot recovery sequence. */
NTSTATUS
A733SmhcReset(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ SDPORT_RESET_TYPE ResetType
    )
{
    NTSTATUS status = A733SmhcResetController(Extension);

    A733SmhcWrite(Extension, SMHC_WIDTH, Extension->WidthRegister);
    if (Extension->CardClockEnabled) {
        NTSTATUS clockStatus = A733SmhcUpdateClock(Extension);
        if (NT_SUCCESS(status)) {
            status = clockStatus;
        }
    }
    if (!NT_SUCCESS(status)) {
        A733_LOG_ERROR("controller reset type=%lu failed %08lx", (ULONG)ResetType, (ULONG)status);
    }
    return status;
}

NTSTATUS
A733SmhcSetClock(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ ULONG FrequencyKhz
    )
{
    ULONGLONG limitHz;
    ULONG div;
    ULONGLONG cardHz;
    BOOLEAN highSpeed;
    BOOLEAN wasOff;
    NTSTATUS status;

    if (Extension->ModuleHz == 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (FrequencyKhz == 0) {
        status = A733SmhcClockOff(Extension);
        Extension->CardClockEnabled = FALSE;
        A733_LOG_INFO("card clock off: %08lx", (ULONG)status);
        return status;
    }
    limitHz = (ULONGLONG)min(FrequencyKhz, A733_MAX_CARD_KHZ) * 1000ULL;
    div = A733PickDivider(Extension->ModuleHz, limitHz);
    if (div > SMHC_CLK_DIV_MASK) {
        div = SMHC_CLK_DIV_MASK;
    }
    cardHz = A733CardHz(Extension->ModuleHz, div);
    highSpeed = (cardHz > A733_HS_PHASE_MIN_HZ) ? TRUE : FALSE;
    wasOff = Extension->CardClockEnabled ? FALSE : TRUE;
    status = A733SmhcProgramClock(Extension, div, highSpeed);
    Extension->CardClockEnabled = NT_SUCCESS(status) ? TRUE : FALSE;
    if (NT_SUCCESS(status) && wasOff) {
        SdPortWait(1000);       /* XNU: 1 ms of clock before the first command */
    }
    if (NT_SUCCESS(status)) {
        Extension->ClockDivider = div;
        Extension->CardHz = cardHz;
        Extension->HighSpeedTiming = highSpeed;
    }
    A733_LOG_INFO("clock request %lu kHz -> div %lu card %llu Hz (module %llu Hz) %s phases, CLKCR=%08lx NTSR=%08lx DRV_DL=%08lx status=%08lx",
                  FrequencyKhz, div, cardHz, Extension->ModuleHz,
                  highSpeed ? "HS" : "DS",
                  A733SmhcRead(Extension, SMHC_CLKCR), A733SmhcRead(Extension, SMHC_NTSR),
                  A733SmhcRead(Extension, SMHC_DRV_DL), (ULONG)status);
    return status;
}

NTSTATUS
A733SmhcSetBusWidth(
    _In_ PA733_SD_EXTENSION Extension,
    _In_ SDPORT_BUS_WIDTH Width
    )
{
    ULONG value;

    switch (Width) {
    case SdBusWidthUndefined:
        /* SDPORT passes Undefined during host setup: stay on 1 bit. */
    case SdBusWidth1Bit:
        value = 0;
        break;
    case SdBusWidth4Bit:
        value = 1;
        break;
    default:
        A733_LOG_ERROR("unsupported bus width=%lu", (ULONG)Width);
        return STATUS_NOT_SUPPORTED;
    }
    Extension->WidthRegister = value;
    A733SmhcWrite(Extension, SMHC_WIDTH, value);
    A733_LOG_INFO("bus width %lu -> WIDTH=%lu", (ULONG)Width, value);
    return STATUS_SUCCESS;
}

NTSTATUS
A733SmhcStatusFromRint(
    _In_ ULONG Rint
    )
{
    if ((Rint & (SMHC_RINT_RESP_TIMEOUT | SMHC_RINT_DATA_TIMEOUT)) != 0) {
        return STATUS_IO_TIMEOUT;
    }
    if ((Rint & (SMHC_RINT_RESP_CRC | SMHC_RINT_DATA_CRC)) != 0) {
        return STATUS_CRC_ERROR;
    }
    if ((Rint & SMHC_RINT_ERROR_MASK) != 0) {
        return STATUS_IO_DEVICE_ERROR;
    }
    return STATUS_SUCCESS;
}

/* PIO+0x40 bit 5 set = PF bank biased for <= 1.8 V: never claim 3.3 V then.
 * XNU does not check; the pads stay as U-Boot set them (3.3 V). */
BOOLEAN
A733SmhcCanSignal33(
    _In_ PA733_SD_EXTENSION Extension
    )
{
    ULONG select;

    if (Extension->PioRegisters == NULL) {
        return TRUE;
    }
    select = READ_REGISTER_ULONG((volatile ULONG *)(Extension->PioRegisters +
                                                    A733_PIO_POWER_MODE_OFFSET));
    if (select & A733_PF_POWER_18V_MASK) {
        A733_LOG_ERROR("PF I/O bank is in <=1.8V mode; 3.3V signaling not safe: POW_MOD_SEL=%08lx", select);
        return FALSE;
    }
    return TRUE;
}
