/** @file
  A733 microSD (SMHC0) block device for UEFI: the XNU driver that works on
  this board (ios-a733 bsd/dev/a733_sdmmc.c) ported to EFI_BLOCK_IO.

  Needed so that UEFI (and Windows Boot Manager / winload, which read the
  OS through the firmware's Block I/O) can see the card: Windows installed
  on the microSD cannot boot otherwise. The Windows SDPORT miniport
  A733SdMmc 2.5.0 resets the controller and re-identifies the card after
  ExitBootServices.

  Same rules as XNU: CCU and pins are never written (U-Boot left SMHC0 at
  0x81000003 = 100 MHz with PF0-PF5 in SDC0 mode); polled PIO, no IDMAC, no
  interrupts; 2x timing mode with the vendor phases; Default Speed 25 MHz,
  4-bit bus. Reads use CMD17/CMD18 with auto CMD12, writes CMD24/CMD25 and
  then wait for DAT0 release.
**/

#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define SMHC_BASE            0x04020000UL
#define CCU_BASE             0x02002000UL
#define CCU_SMHC0_CLK        0xd00
#define CCU_SMHC0_BGR        0xd0c

#define SMHC_GCTRL           0x000
#define SMHC_CLKCR           0x004
#define SMHC_TMOUT           0x008
#define SMHC_WIDTH           0x00c
#define SMHC_BLKSZ           0x010
#define SMHC_BCNTR           0x014
#define SMHC_CMDR            0x018
#define SMHC_CARG            0x01c
#define SMHC_RESP0           0x020
#define SMHC_IMASK           0x030
#define SMHC_RINTR           0x038
#define SMHC_STAS            0x03c
#define SMHC_FTRGL           0x040
#define SMHC_FUNS            0x044
#define SMHC_DBGC            0x050
#define SMHC_NTSR            0x05c
#define SMHC_DMAC            0x080
#define SMHC_IDIE            0x08c
#define SMHC_THLD            0x100
#define SMHC_DRV_DL          0x140
#define SMHC_FIFO            0x200

#define GCTRL_HW_RESET       0x7u
#define GCTRL_FIFO_RESET     BIT1
#define GCTRL_INT_ENABLE     BIT4
#define GCTRL_DMA_ENABLE     BIT5
#define GCTRL_DDR_MODE       BIT10
#define GCTRL_DTIME_UNIT     BIT11
#define GCTRL_ACCESS_DONE_DIRECT BIT30
#define GCTRL_ACCESS_BY_AHB  BIT31

#define CLKCR_DIV_MASK       0xffu
#define CLKCR_CARD_CLOCK_ON  BIT16
#define CLKCR_LOW_POWER_ON   BIT17
#define CLKCR_MASK_DATA0     BIT31

#define CMD_RESP_EXPIRE      BIT6
#define CMD_LONG_RESPONSE    BIT7
#define CMD_CHECK_RESPONSE_CRC BIT8
#define CMD_DATA_EXPIRE      BIT9
#define CMD_WRITE            BIT10
#define CMD_SEND_AUTO_STOP   BIT12
#define CMD_WAIT_PRE_OVER    BIT13
#define CMD_STOP_ABORT       BIT14
#define CMD_SEND_INIT_SEQ    BIT15
#define CMD_UPCLK_ONLY       BIT21
#define CMD_START            BIT31

#define RINT_COMMAND_DONE    BIT2
#define RINT_DATA_OVER       BIT3
#define RINT_RESP_TIMEOUT    BIT8
#define RINT_DATA_TIMEOUT    BIT9
#define RINT_AUTO_CMD_DONE   BIT14
#define RINT_SDIO_INTERRUPT  BIT16
#define RINT_ERRORS          0xbbc2u

#define STAS_FIFO_EMPTY      BIT2
#define STAS_FIFO_FULL       BIT3
#define STAS_CARD_DATA_BUSY  BIT9
#define STAS_FIFO_LEVEL(s)   (((s) >> 17) & 0x3fffu)

#define NTSR_2X_TIMING_MODE  BIT31
#define NTSR_SAMPLE_PHASE_MASK 0x330u
#define DRV_DL_CMD_DRV_PH_SEL BIT16
#define DRV_DL_DAT_DRV_PH_SEL BIT17

#define RT_PRESENT           0x1u
#define RT_LONG              0x2u
#define RT_CRC               0x4u
#define RT_BUSY              0x8u
#define RT_R1                (RT_PRESENT | RT_CRC)
#define RT_R1B               (RT_R1 | RT_BUSY)
#define RT_R2                (RT_PRESENT | RT_LONG | RT_CRC)
#define RT_R3                RT_PRESENT

#define R1_STATE(r)          (((r) >> 9) & 0xfu)
#define R1_STATE_TRAN        4u
#define R1_STATE_DATA        5u
#define R1_STATE_RCV         6u
#define R1_ERRORS            0xfdf80000u

#define SD_SECTOR            512u
#define SD_MAX_BLOCKS        256u

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} A733_SD_DEVICE_PATH;

STATIC A733_SD_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0xa7330001, 0x5344, 0x4d4d, { 0x43, 0x30, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EFI_BLOCK_IO_MEDIA     mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo;
STATIC EFI_HANDLE             mHandle;
STATIC UINT64                 mModHz;
STATIC UINT32                 mIdentDiv, mXferDiv;
STATIC UINT32                 mRca;
STATIC BOOLEAN                mHighCapacity;
STATIC UINT64                 mSectors;
STATIC UINT32                 mLastRint;
STATIC UINT32                 mBounce[SD_SECTOR / 4];

STATIC UINT32 SdRd (UINT32 Off) { return MmioRead32 (SMHC_BASE + Off); }
STATIC VOID   SdWr (UINT32 Off, UINT32 V) { MmioWrite32 (SMHC_BASE + Off, V); }

/* -------------------------------------------------- helpers (XNU) */

STATIC
UINT64
CcuModHz (
  IN UINT32  Reg
  )
{
  STATIC CONST UINT64  Parents[] = { 24000000ULL, 400000000ULL, 300000000ULL, 400000000ULL, 300000000ULL };
  UINT32               Mux = (Reg >> 24) & 7;

  if (!(Reg & BIT31) || Mux >= ARRAY_SIZE (Parents)) {
    return 0;
  }
  return DivU64x32 (DivU64x32 (Parents[Mux], (Reg & 0x1f) + 1), ((Reg >> 8) & 0x1f) + 1);
}

STATIC UINT64 CardHz (UINT64 Mod, UINT32 Div) { return Div ? DivU64x32 (Mod, 4 * Div) : DivU64x32 (Mod, 2); }

STATIC
UINT32
PickDiv (
  IN UINT64  Mod,
  IN UINT64  Limit
  )
{
  UINT64  D;

  if (DivU64x32 (Mod, 2) <= Limit) {
    return 0;
  }
  D = DivU64x64Remainder (Mod + 4 * Limit - 1, 4 * Limit, NULL);
  return D > CLKCR_DIV_MASK ? CLKCR_DIV_MASK : (UINT32)D;
}

STATIC
UINT32
RBits (
  IN CONST UINT32  R[4],
  IN UINTN         Lsb,
  IN UINTN         Width
  )
{
  UINTN   W = Lsb / 32, S = Lsb % 32;
  UINT64  V = R[W] >> S;

  if (S + Width > 32 && W < 3) {
    V |= LShiftU64 (R[W + 1], 32 - S);
  }
  return (UINT32)(V & (LShiftU64 (1, Width) - 1));
}

STATIC
UINT64
CsdSectors (
  IN CONST UINT32  Csd[4]
  )
{
  UINT64  CSize;
  UINTN   Mult, BlLen;

  switch (RBits (Csd, 126, 2)) {
    case 0:
      CSize = RBits (Csd, 62, 12);
      Mult = RBits (Csd, 47, 3);
      BlLen = RBits (Csd, 80, 4);
      if (BlLen < 9 || BlLen > 11) {
        return 0;
      }
      return RShiftU64 (LShiftU64 (CSize + 1, Mult + 2 + BlLen), 9);
    case 1:
      return ((UINT64)RBits (Csd, 48, 22) + 1) * 1024;
    default:
      return 0;
  }
}

/* --------------------------------------------- controller (XNU) */

STATIC
EFI_STATUS
WaitClear (
  IN UINT32  Reg,
  IN UINT32  Mask,
  IN UINT32  Ms
  )
{
  UINT32  Us;

  for (Us = 0; Us < Ms * 1000; Us++) {
    if ((SdRd (Reg) & Mask) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SmhcReset (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      V;

  SdWr (SMHC_GCTRL, SdRd (SMHC_GCTRL) | GCTRL_HW_RESET);
  Status = WaitClear (SMHC_GCTRL, GCTRL_HW_RESET, 250);
  V = SdRd (SMHC_GCTRL);
  V &= ~(GCTRL_INT_ENABLE | GCTRL_DMA_ENABLE | GCTRL_DDR_MODE | GCTRL_DTIME_UNIT | GCTRL_ACCESS_DONE_DIRECT);
  SdWr (SMHC_GCTRL, V | GCTRL_ACCESS_BY_AHB);
  SdWr (SMHC_DMAC, 0);
  SdWr (SMHC_IDIE, 0);
  SdWr (SMHC_IMASK, 0);
  SdWr (SMHC_RINTR, 0xffffffffu);
  SdWr (SMHC_TMOUT, 0xffffffffu);
  SdWr (SMHC_FTRGL, 0x20070008u);
  SdWr (SMHC_DBGC, 0xdeb);
  SdWr (SMHC_FUNS, 0xceaa0000u);
  SdWr (SMHC_THLD, (SD_SECTOR << 16) | BIT2 | BIT0);
  return Status;
}

STATIC
EFI_STATUS
SmhcUpdateClk (
  VOID
  )
{
  EFI_STATUS  Status;

  SdWr (SMHC_CMDR, CMD_START | CMD_UPCLK_ONLY | CMD_WAIT_PRE_OVER);
  Status = WaitClear (SMHC_CMDR, CMD_START, 250);
  SdWr (SMHC_RINTR, SdRd (SMHC_RINTR) & ~RINT_SDIO_INTERRUPT);
  return Status;
}

STATIC
EFI_STATUS
SmhcSetClock (
  IN UINT32  Div
  )
{
  UINT32      V = SdRd (SMHC_CLKCR), N, D;
  EFI_STATUS  Status;

  V &= ~(CLKCR_CARD_CLOCK_ON | CLKCR_LOW_POWER_ON | CLKCR_MASK_DATA0);
  SdWr (SMHC_CLKCR, V | CLKCR_MASK_DATA0);
  Status = SmhcUpdateClk ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  V = (V & ~CLKCR_DIV_MASK) | (Div & CLKCR_DIV_MASK);
  SdWr (SMHC_CLKCR, V | CLKCR_MASK_DATA0);
  N = SdRd (SMHC_NTSR) & ~NTSR_SAMPLE_PHASE_MASK;
  SdWr (SMHC_NTSR, N | NTSR_2X_TIMING_MODE);                     /* Default Speed phases */
  D = SdRd (SMHC_DRV_DL) | DRV_DL_CMD_DRV_PH_SEL;
  SdWr (SMHC_DRV_DL, D & ~DRV_DL_DAT_DRV_PH_SEL);
  SdWr (SMHC_CLKCR, V | CLKCR_CARD_CLOCK_ON | CLKCR_MASK_DATA0);
  Status = SmhcUpdateClk ();
  SdWr (SMHC_CLKCR, SdRd (SMHC_CLKCR) & ~CLKCR_MASK_DATA0);
  return Status;
}

STATIC
EFI_STATUS
WaitRint (
  IN UINT32  Done,
  IN UINT32  Ms
  )
{
  UINT32  Us, R;

  for (Us = 0; ; Us++) {
    R = SdRd (SMHC_RINTR);
    if (R & RINT_ERRORS) {
      mLastRint = R;
      return (R & (RINT_RESP_TIMEOUT | RINT_DATA_TIMEOUT)) ? EFI_TIMEOUT : EFI_DEVICE_ERROR;
    }
    if (R & Done) {
      return EFI_SUCCESS;
    }
    if (Us >= Ms * 1000) {
      mLastRint = R;
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
}

/* Move Bytes through the FIFO (any buffer alignment), XNU smhc_pio_read/write. */
STATIC
EFI_STATUS
Pio (
  IN OUT UINT8    *Buf,
  IN     UINTN    Bytes,
  IN     BOOLEAN  Write,
  IN     UINT32   Ms
  )
{
  UINTN   Off = 0;
  UINT32  Idle = 0, S, R, N, W;

  while (Off < Bytes) {
    S = SdRd (SMHC_STAS);
    if (Write ? (S & STAS_FIFO_FULL) : (S & STAS_FIFO_EMPTY)) {
      R = SdRd (SMHC_RINTR);
      if (R & RINT_ERRORS) {
        mLastRint = R;
        return (R & (RINT_RESP_TIMEOUT | RINT_DATA_TIMEOUT)) ? EFI_TIMEOUT : EFI_DEVICE_ERROR;
      }
      if (Idle++ >= Ms * 1000) {
        mLastRint = R;
        return EFI_TIMEOUT;
      }
      MicroSecondDelay (1);
      continue;
    }
    Idle = 0;
    if (Write) {
      CopyMem (&W, Buf + Off, sizeof (W));
      SdWr (SMHC_FIFO, W);
      Off += 4;
      continue;
    }
    N = STAS_FIFO_LEVEL (S);
    if (N == 0) {
      N = (S & STAS_FIFO_FULL) ? 32 : 1;
    }
    while (N-- && Off < Bytes) {
      W = SdRd (SMHC_FIFO);
      CopyMem (Buf + Off, &W, sizeof (W));
      Off += 4;
    }
  }
  return EFI_SUCCESS;
}

/* One command, optionally with Nblk sectors of data (XNU smhc_xfer). */
STATIC
EFI_STATUS
SmhcXfer (
  IN     UINT32   Idx,
  IN     UINT32   Arg,
  IN     UINT32   Rt,
  OUT    UINT32   *Resp OPTIONAL,
  IN OUT UINT8    *Buf OPTIONAL,
  IN     UINT32   Nblk,
  IN     UINT32   Blksz,
  IN     BOOLEAN  Write
  )
{
  UINT32      Cmd = CMD_START | Idx;
  EFI_STATUS  Status;
  UINTN       I;

  if (EFI_ERROR (WaitClear (SMHC_CMDR, CMD_START, 100))) {
    SmhcReset ();
    SmhcUpdateClk ();
  }
  if (Idx == 0) {
    Cmd |= CMD_SEND_INIT_SEQ;
  }
  if (Idx == 12) {
    Cmd |= CMD_STOP_ABORT;
  }
  if (Rt & RT_PRESENT) {
    Cmd |= CMD_RESP_EXPIRE;
  }
  if (Rt & RT_LONG) {
    Cmd |= CMD_LONG_RESPONSE;
  }
  if (Rt & RT_CRC) {
    Cmd |= CMD_CHECK_RESPONSE_CRC;
  }
  mLastRint = 0;
  SdWr (SMHC_RINTR, 0xffffffffu);
  if (Buf != NULL) {
    Cmd |= CMD_DATA_EXPIRE | CMD_WAIT_PRE_OVER;
    if (Nblk > 1) {
      Cmd |= CMD_SEND_AUTO_STOP;
    }
    if (Write) {
      Cmd |= CMD_WRITE;
      Rt |= RT_BUSY;
    }
    SdWr (SMHC_GCTRL, SdRd (SMHC_GCTRL) | GCTRL_FIFO_RESET);
    WaitClear (SMHC_GCTRL, GCTRL_FIFO_RESET, 10);
    SdWr (SMHC_BLKSZ, Blksz);
    SdWr (SMHC_BCNTR, Nblk * Blksz);
  }
  SdWr (SMHC_CARG, Arg);
  SdWr (SMHC_CMDR, Cmd);

  Status = EFI_SUCCESS;
  if (Buf != NULL) {
    Status = Pio (Buf, (UINTN)Nblk * Blksz, Write, Write ? 1000 + 5 * Nblk : 1000 + 2 * Nblk);
  }
  if (!EFI_ERROR (Status)) {
    Status = WaitRint (RINT_COMMAND_DONE, 1000);
  }
  if (!EFI_ERROR (Status) && Buf != NULL) {
    Status = WaitRint (Nblk > 1 ? RINT_AUTO_CMD_DONE : RINT_DATA_OVER, Write ? 3000 : 500);
  }
  if (!EFI_ERROR (Status) && (Rt & RT_BUSY)) {
    Status = WaitClear (SMHC_STAS, STAS_CARD_DATA_BUSY, 3000);
  }
  if (!EFI_ERROR (Status) && Resp != NULL) {
    Resp[0] = SdRd (SMHC_RESP0);
    if (Rt & RT_LONG) {
      for (I = 1; I < 4; I++) {
        Resp[I] = SdRd (SMHC_RESP0 + 4 * (UINT32)I);
      }
    }
  }
  if (EFI_ERROR (Status)) {
    if (!mLastRint) {
      mLastRint = SdRd (SMHC_RINTR);
    }
    SmhcReset ();
    SmhcUpdateClk ();
  }
  SdWr (SMHC_RINTR, 0xffffffffu);
  return Status;
}

STATIC EFI_STATUS SdCmd (UINT32 Idx, UINT32 Arg, UINT32 Rt, UINT32 *Resp)
{
  return SmhcXfer (Idx, Arg, Rt, Resp, NULL, 0, SD_SECTOR, FALSE);
}

STATIC
EFI_STATUS
SdAcmd (
  IN  UINT32  Idx,
  IN  UINT32  Arg,
  IN  UINT32  Rt,
  OUT UINT32  *Resp
  )
{
  UINT32      R55[4];
  EFI_STATUS  Status = SdCmd (55, mRca << 16, RT_R1, R55);

  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (!(R55[0] & BIT5)) {
    return EFI_DEVICE_ERROR;
  }
  return SdCmd (Idx, Arg, Rt, Resp);
}

/* XNU sd_identify: CMD0 .. ACMD6, then 25 MHz. */
STATIC
EFI_STATUS
SdIdentify (
  VOID
  )
{
  UINT32      R[4];
  EFI_STATUS  Status;
  BOOLEAN     V2;
  UINTN       Ms;

  mRca = 0;
  SdWr (SMHC_WIDTH, 0);
  Status = SmhcSetClock (mIdentDiv);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733 SD: identification clock update timed out\n"));
    return Status;
  }
  MicroSecondDelay (1000);
  SdCmd (0, 0, 0, NULL);
  MicroSecondDelay (2000);
  Status = SdCmd (8, 0x1aa, RT_R1, R);
  if (!EFI_ERROR (Status) && (R[0] & 0xfff) != 0x1aa) {
    DEBUG ((DEBUG_ERROR, "A733 SD: CMD8 echo 0x%x\n", R[0]));
    return EFI_DEVICE_ERROR;
  }
  V2 = !EFI_ERROR (Status);
  if (!V2) {
    SdCmd (0, 0, 0, NULL);
    MicroSecondDelay (2000);
  }
  for (Ms = 0; ; Ms += 10) {
    Status = SdAcmd (41, (V2 ? BIT30 : 0) | 0x00ff8000u, RT_R3, R);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "A733 SD: ACMD41 failed %r (rint 0x%x)\n", Status, mLastRint));
      return Status;
    }
    if (R[0] & BIT31) {
      break;
    }
    if (Ms >= 2000) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (10000);
  }
  mHighCapacity = V2 && (R[0] & BIT30);
  Status = SdCmd (2, 0, RT_R2, R);
  if (!EFI_ERROR (Status)) {
    Status = SdCmd (3, 0, RT_R1, R);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733 SD: CMD2/CMD3 failed %r (rint 0x%x)\n", Status, mLastRint));
    return Status;
  }
  mRca = R[0] >> 16;
  Status = SdCmd (9, mRca << 16, RT_R2, R);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mSectors = CsdSectors (R);
  if (mSectors == 0) {
    DEBUG ((DEBUG_ERROR, "A733 SD: unsupported CSD %08x %08x %08x %08x\n", R[3], R[2], R[1], R[0]));
    return EFI_UNSUPPORTED;
  }
  Status = SdCmd (7, mRca << 16, RT_R1B, R);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (!EFI_ERROR (SdAcmd (6, 2, RT_R1, R))) {
    SdWr (SMHC_WIDTH, 1);
  } else {
    DEBUG ((DEBUG_WARN, "A733 SD: ACMD6 failed, staying on a 1-bit bus\n"));
  }
  if (!mHighCapacity) {
    Status = SdCmd (16, SD_SECTOR, RT_R1, R);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  Status = SmhcSetClock (mXferDiv);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = SdCmd (13, mRca << 16, RT_R1, R);
  if (EFI_ERROR (Status) || R1_STATE (R[0]) != R1_STATE_TRAN || (R[0] & R1_ERRORS)) {
    DEBUG ((DEBUG_ERROR, "A733 SD: card not in transfer state: %r status 0x%08x\n", Status, R[0]));
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }
  DEBUG ((DEBUG_INFO, "A733 SD: card RCA 0x%04x, %lu MiB, %a, %a bus, %lu Hz\n", mRca, RShiftU64 (mSectors, 11),
    mHighCapacity ? "SDHC/SDXC" : "SDSC", SdRd (SMHC_WIDTH) ? "4-bit" : "1-bit", CardHz (mModHz, mXferDiv)));
  return EFI_SUCCESS;
}

/* Read or write Count sectors at Lba with retries and stop recovery (XNU sd_read). */
STATIC
EFI_STATUS
SdRw (
  IN     UINT64   Lba,
  IN     UINT32   Count,
  IN OUT UINT8    *Buf,
  IN     BOOLEAN  Write
  )
{
  UINT32      R[4], St[4], Arg, Idx;
  EFI_STATUS  Status = EFI_DEVICE_ERROR;
  UINTN       Attempt;

  Arg = mHighCapacity ? (UINT32)Lba : (UINT32)(Lba * SD_SECTOR);
  Idx = Write ? (Count > 1 ? 25 : 24) : (Count > 1 ? 18 : 17);
  for (Attempt = 0; Attempt < 3; Attempt++) {
    R[0] = 0;
    Status = SmhcXfer (Idx, Arg, RT_R1, R, Buf, Count, SD_SECTOR, Write);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }
    DEBUG ((DEBUG_ERROR, "A733 SD: CMD%u lba %lu x%u failed %r (rint 0x%x), attempt %u\n",
      Idx, Lba, Count, Status, mLastRint, (UINT32)Attempt + 1));
    St[0] = 0;
    if (EFI_ERROR (SdCmd (13, mRca << 16, RT_R1, St)) ||
        R1_STATE (St[0]) == R1_STATE_DATA || R1_STATE (St[0]) == R1_STATE_RCV) {
      SdCmd (12, 0, RT_R1B, St);
    }
    MicroSecondDelay (1000);
  }
  return Status;
}

/* ------------------------------------------------------ Block I/O */

STATIC
EFI_STATUS
EFIAPI
BlkReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  SmhcReset ();
  return SdIdentify ();
}

STATIC
EFI_STATUS
BlkIo (
  IN     EFI_BLOCK_IO_PROTOCOL  *This,
  IN     UINT32                 MediaId,
  IN     EFI_LBA                Lba,
  IN     UINTN                  BufferSize,
  IN OUT UINT8                  *Buffer,
  IN     BOOLEAN                Write
  )
{
  UINT64      Blocks, Done = 0;
  UINT32      N;
  EFI_STATUS  Status;

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if ((BufferSize % SD_SECTOR) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  Blocks = BufferSize / SD_SECTOR;
  if (Lba > mMedia.LastBlock || Blocks > mMedia.LastBlock - Lba + 1) {
    return EFI_INVALID_PARAMETER;
  }
  while (Done < Blocks) {
    N = (UINT32)MIN (Blocks - Done, (UINT64)SD_MAX_BLOCKS);
    Status = SdRw (Lba + Done, N, Buffer + Done * SD_SECTOR, Write);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }
    Done += N;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
BlkRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  return BlkIo (This, MediaId, Lba, BufferSize, Buffer, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
BlkWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return BlkIo (This, MediaId, Lba, BufferSize, Buffer, TRUE);
}

STATIC
EFI_STATUS
EFIAPI
BlkFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
A733SdMmcDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32      Clk = MmioRead32 (CCU_BASE + CCU_SMHC0_CLK);
  UINT32      Bgr = MmioRead32 (CCU_BASE + CCU_SMHC0_BGR);
  EFI_STATUS  Status;
  UINTN       Attempt;

  DEBUG ((DEBUG_INFO, "A733 SD: CCU SMHC0_CLK 0x%08x BGR 0x%08x (read only)\n", Clk, Bgr));
  if ((Bgr & (BIT0 | BIT16)) != (BIT0 | BIT16)) {
    DEBUG ((DEBUG_ERROR, "A733 SD: SMHC0 bus clock gated or in reset; no microSD in UEFI\n"));
    return EFI_UNSUPPORTED;
  }
  mModHz = CcuModHz (Clk);
  if (mModHz == 0 || mModHz > 408000000ULL) {
    DEBUG ((DEBUG_ERROR, "A733 SD: unusable SMHC0 module clock %lu Hz\n", mModHz));
    return EFI_UNSUPPORTED;
  }
  mIdentDiv = PickDiv (mModHz, 400000);
  mXferDiv = PickDiv (mModHz, 25000000);
  DEBUG ((DEBUG_INFO, "A733 SD: module %lu Hz, ident div %u (%lu Hz), transfer div %u (%lu Hz)\n",
    mModHz, mIdentDiv, CardHz (mModHz, mIdentDiv), mXferDiv, CardHz (mModHz, mXferDiv)));

  Status = SmhcReset ();
  for (Attempt = 0; Attempt < 2; Attempt++) {
    Status = SdIdentify ();
    if (!EFI_ERROR (Status)) {
      break;
    }
    DEBUG ((DEBUG_ERROR, "A733 SD: card initialisation attempt %u failed: %r\n", (UINT32)Attempt + 1, Status));
    SmhcReset ();
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  /* Sector 0 must read back, as XNU checks before using the card. */
  Status = SdRw (0, 1, (UINT8 *)mBounce, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mMedia.MediaId = 1;
  mMedia.RemovableMedia = FALSE;
  mMedia.MediaPresent = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly = FALSE;
  mMedia.WriteCaching = FALSE;
  mMedia.BlockSize = SD_SECTOR;
  mMedia.IoAlign = 0;
  mMedia.LastBlock = mSectors - 1;
  mBlockIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mBlockIo.Media = &mMedia;
  mBlockIo.Reset = BlkReset;
  mBlockIo.ReadBlocks = BlkRead;
  mBlockIo.WriteBlocks = BlkWrite;
  mBlockIo.FlushBlocks = BlkFlush;
  mHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (&mHandle,
                  &gEfiBlockIoProtocolGuid, &mBlockIo,
                  &gEfiDevicePathProtocolGuid, &mDevicePath,
                  NULL);
  DEBUG ((DEBUG_INFO, "A733 SD: Block I/O installed: %r (sector 0 signature %02x%02x)\n", Status,
    ((UINT8 *)mBounce)[510], ((UINT8 *)mBounce)[511]));
  return Status;
}
