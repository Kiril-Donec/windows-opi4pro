/** @file
  A733 CPU frequency for Windows: the boot-time half of the XNU port of the
  vendor Linux cpufreq (ios-a733 bsd/dev/a733_power.c, patch 9999r, proven
  on this board in test #14: A76 1992 MHz, A55 1800 MHz, DSU 1248 MHz).

  Vendor U-Boot leaves A76 / A55 at 936 MHz and the DSU at 720 MHz, and EDK2
  never changed that, so Windows ran at a third of the possible speed.

  Sequence (identical to XNU cf_init / cf_apply):
    - chip speed bin from the efuse (SID + 0x24C), vf_mapping_table column;
    - highest OPP row of that column with mV <= the level's limit;
    - AXP8191 on R_TWI0: raise DCDC3 (A76) / DCDC5 (A55 + DSU) first, wait
      for the ramp, read back;
    - in-place CPU PLL retune (N, update bit 26, wait for lock bit 28);
    - lower the rails if the new rows need less.

  Windows has no cpufreq or thermal driver for this SoC, so the clocks stay
  fixed after ExitBootServices. Level (PcdA733CpuFreqLevel):
    0 off, 1 rows <= 800 mV (no voltage raise), 2 rows <= 950 mV (default:
    on bin vf0500 A76 1896 / A55 1800 / DSU 1248 MHz at 940 / 950 mV),
    3 the bin's top row (exactly XNU's default: A76 1992 MHz at 1000 mV).
  If the hottest THS sensor already reads >= 85 C at boot, level 1 is used.

  Every step is printed on the serial console before it is taken.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define A733_CCU_BASE          0x02002000UL
#define A733_SID_BASE          0x03006000UL
#define A733_THS_BASE          0x02522000UL
#define A733_R_CCU_BASE        0x07010000UL
#define A733_R_TWI0_BASE       0x07083000UL
#define A733_CPUPLL_BASE       0x08870000UL

#define RCCU_APBS1             0x0010
#define RCCU_TWI_BGR           0x019c

#define TWI_DATA               0x08
#define TWI_CNTR               0x0c
#define TWI_STAT               0x10
#define TWI_CCR                0x14
#define TWI_SRST               0x18
#define TWI_EFR                0x1c
#define TWI_DRV_CTRL           0x200
#define CNTR_A_ACK             BIT2
#define CNTR_INT_FLAG          BIT3
#define CNTR_M_STP             BIT4
#define CNTR_M_STA             BIT5
#define CNTR_BUS_EN            BIT6

#define AXP8191_ADDR           0x36
#define AXP8191_DCDC_CTL1      0x10      /* bit 2 DCDC3, bit 4 DCDC5 enabled */
#define AXP8191_DC3_VOL        0x14      /* A76 cluster */
#define AXP8191_DC5_VOL        0x16      /* A55 cluster + DSU */

#define CPUPLL_A55             0x1000
#define CPUPLL_A76             0x2000
#define CPUPLL_DSU             0x3000
#define PLL_UPDATE             BIT26
#define PLL_LOCK               BIT28
#define CF_MAX_MV              1050
/* Level 4 (overclock): the Android v13-OCmax points the user ran stable with a
   fan (A76 2314 MHz @ 1.15 V, A55 2106 MHz @ 1.10 V), rounded DOWN to 24 MHz
   PLL steps. Windows has no thermal throttling here: needs the fan. */
#define CF_OC_MAX_MV           1150
#define CF_FLOOR_MHZ           1008

#define SID_DVFS               0x24c
#define SID_THS_CAL            0x244
#define CCU_GPADC0_24M         0x0fc0
#define CCU_GPADC0_BGR         0x0fc4
#define CCU_THS0_BGR           0x0fe4
#define THS_CTRL0              0x00
#define THS_ENABLE             0x04
#define THS_PC                 0x08
#define THS_DATA_INTS          0x20
#define THS_MFC                0x30
#define THS_CALIB              0xa0
#define THS_DATA               0xc0
#define THS_SENSORS            5
#define CF_BOOT_HOT_MC         85000
#define CF_NO_TEMP             (-2147483647 - 1)

#define CF_BINS                9

typedef struct {
  UINT16    Mhz;
  UINT16    Mv[CF_BINS];
} CF_OPP;

/* vf_mapping_table (V0.9) and sun60iw2p1-cpu-vf.dtsi 24 MHz rows, from XNU. */
STATIC CONST struct { UINT8 Code; UINT16 Index; } mVfMap[] = {
  { 0x00, 0x0000 }, { 0x01, 0x0100 }, { 0x11, 0x0101 }, { 0x02, 0x0200 }, { 0x12, 0x0201 }, { 0x03, 0x0300 },
  { 0x04, 0x0400 }, { 0x05, 0x0500 }, { 0x06, 0x0600 }, { 0x14, 0x0401 }, { 0x15, 0x0501 },
};
STATIC CONST UINT16 mBins[CF_BINS] = { 0x0000, 0x0100, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0401, 0x0501 };

STATIC CONST CF_OPP mA55[] = {
  {  408, {  800,  800,  800,  800,  800,  800,  800,  800,  800 } },
  {  720, {  800,    0,    0,    0,    0,    0,    0,    0,    0 } },
  {  792, {    0,  800,  800,  800,  800,  800,  800,  800,  800 } },
  { 1008, {  800,  800,  800,  800,  800,  800,  800,  800,  800 } },
  { 1104, {    0,  800,    0,    0,    0,    0,    0,    0,    0 } },
  { 1200, {  800,  850,  800,  800,  800,  800,  800,  800,  800 } },
  { 1296, {    0,  900,  800,    0,    0,    0,    0,    0,    0 } },
  { 1392, {    0,  950,  850,  860,  840,  800,  840,  800,  800 } },
  { 1416, {  800,    0,    0,    0,    0,    0,    0,    0,    0 } },
  { 1512, {    0, 1000,  900,    0,  880,  820,    0,  820,  820 } },
  { 1608, {  900, 1050,  950,    0,  920,  860,    0,  860,  840 } },
  { 1704, {    0,    0, 1000,    0,  960,  900,    0,  900,  860 } },
  { 1800, { 1000,    0, 1050,    0, 1000,  950,    0,  950,  900 } },
  { 2088, { 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1100 } },  /* level 4 only */
};
STATIC CONST CF_OPP mA76[] = {
  {  408, {  800,  800,  800,  800,  800,  800,  800,  800,  800 } },
  {  720, {  800,    0,    0,    0,    0,    0,    0,    0,    0 } },
  {  792, {    0,  800,  800,  800,  800,  800,  800,  800,  800 } },
  { 1008, {  800,  800,  800,  800,  800,  800,  800,  800,  800 } },
  { 1200, {  800,  800,  800,  800,  800,  800,  800,  800,  800 } },
  { 1296, {    0,  800,    0,    0,    0,    0,    0,    0,    0 } },
  { 1392, {  800,  850,  800,  820,  800,  800,  800,  800,  800 } },
  { 1512, {  800,  900,  800,  860,  820,  800,  800,  800,  800 } },
  { 1608, {  850,  950,  850,  900,  840,  820,  850,  820,  820 } },
  { 1704, {  900, 1000,  900,    0,  870,  850,    0,  850,  840 } },
  { 1800, {  950, 1050,  950,    0,  920,  900,    0,  900,  870 } },
  { 1896, {    0,    0, 1000,    0,  970,  940,    0,  940,  900 } },
  { 1920, { 1000,    0,    0,    0,    0,    0,    0,    0,    0 } },
  { 1944, {    0,    0,    0,    0, 1000,  970,    0,  970,  930 } },
  { 1992, { 1050,    0,    0,    0, 1050, 1000,    0, 1000,  960 } },
  { 2304, { 1150, 1150, 1150, 1150, 1150, 1150, 1150, 1150, 1150 } },  /* level 4 only */
};
STATIC CONST CF_OPP mDsu[] = {
  {  288, {  800,    0,    0,    0,    0,    0,    0,    0,    0 } },
  {  312, {    0,  800,  800,  800,  800,  800,  800,  800,  800 } },
  {  528, {  800,    0,    0,    0,    0,    0,    0,    0,    0 } },
  {  600, {    0,  800,  800,  800,  800,  800,  800,  800,  800 } },
  {  744, {  800,    0,    0,    0,    0,    0,    0,    0,    0 } },
  {  792, {    0,  800,  800,  800,  800,  800,  800,  800,  800 } },
  {  840, {    0,  800,    0,    0,    0,    0,    0,    0,    0 } },
  {  888, {  800,    0,    0,    0,    0,    0,    0,    0,    0 } },
  {  912, {    0,  850,  800,  800,  800,  800,  800,  800,  800 } },
  {  984, {    0,  900,  800,    0,    0,    0,    0,    0,    0 } },
  { 1032, {  800,    0,    0,    0,    0,    0,    0,    0,    0 } },
  { 1056, {    0,  950,  850,  860,  840,  800,  840,  800,  800 } },
  { 1152, {  900, 1000,  900,    0,  880,  820,    0,  820,  820 } },
  { 1200, {    0, 1050,  950,    0,  920,  860,    0,  860,  840 } },
  { 1224, {    0,    0, 1000,    0,  960,  900,    0,  900,  860 } },
  { 1248, {    0,    0, 1050,    0, 1000,  950,    0,  950,  900 } },
  { 1296, { 1000,    0,    0,    0,    0,    0,    0,    0,    0 } },
};

STATIC UINT32  mTwiErrors;
STATIC INT32   mCol = -1;
STATIC INT32   mRowA76 = -1, mRowA55 = -1, mRowDsu = -1;   /* -1 = U-Boot's clock */
STATIC UINT16  mV3, mV5;

/* ------------------------------------------------------------- tables */

STATIC
INT32
CfColumn (
  IN  UINT32  Sid24c,
  OUT UINT16  *Index
  )
{
  UINT32  Ori = (Sid24c >> 8) & 0xff;
  UINT32  Bak = (Sid24c >> 16) & 0xff;
  UINT32  Dvfs = Bak ? Bak : Ori;
  UINT16  Idx = 0x0100;
  UINT16  ColIdx;
  UINTN   I;

  for (I = 0; I < ARRAY_SIZE (mVfMap); I++) {
    if (mVfMap[I].Code == Dvfs) {
      Idx = mVfMap[I].Index;
      break;
    }
  }
  *Index = Idx;
  ColIdx = (Idx == 0x0101 || Idx == 0x0201) ? (UINT16)(Idx & 0xff00) : Idx;
  for (I = 0; I < CF_BINS; I++) {
    if (mBins[I] == ColIdx) {
      return (INT32)I;
    }
  }
  return -1;
}

STATIC
INT32
CfPick (
  IN CONST CF_OPP  *T,
  IN UINTN         N,
  IN INT32         Col,
  IN UINT16        MaxMv,
  IN UINT16        MinMhz
  )
{
  INT32  Best = -1;
  UINTN  I;

  for (I = 0; I < N; I++) {
    if (T[I].Mv[Col] && T[I].Mv[Col] <= MaxMv && T[I].Mhz >= MinMhz) {
      Best = (INT32)I;
    }
  }
  return Best;
}

/* AXP8191 DCDC2-5: 0.5 V + 10 mV/step up to 1.2 V (code 0x46), then 20 mV/step. */
STATIC
UINT8
CfMvCode (
  IN UINT16  Mv
  )
{
  if (Mv < 500) {
    Mv = 500;
  }
  if (Mv <= 1200) {
    return (UINT8)((Mv - 500) / 10);
  }
  return (UINT8)(0x47 + (Mv - 1220 + 19) / 20);
}

STATIC
UINT16
CfCodeMv (
  IN UINT8  Code
  )
{
  Code &= 0x7f;
  return Code <= 0x46 ? (UINT16)(500 + 10 * Code) : (UINT16)(1220 + 20 * (Code - 0x47));
}

/* ---------------------------------------------------------------- TWI */

STATIC
UINT32
RApbs1Hz (
  IN UINT32  Reg
  )
{
  STATIC CONST UINT32  Parent[8] = { 24000000, 32768, 16000000, 200000000, 24000000, 24000000, 24000000, 24000000 };

  return Parent[(Reg >> 24) & 7] / ((Reg & 0x1f) + 1);
}

STATIC
UINT32
TwiCcrFor (
  IN  UINT32  InHz,
  IN  UINT32  MaxHz,
  OUT UINT32  *OutHz
  )
{
  UINT32  Best = 0x7u | (0xfu << 3);
  UINT32  BestHz = InHz / (10u * 16u * 128u);
  UINT32  N, M, Hz;

  for (N = 0; N < 8; N++) {
    for (M = 0; M < 16; M++) {
      Hz = InHz / ((10u * (M + 1)) << N);
      if (Hz <= MaxHz && Hz > BestHz) {
        BestHz = Hz;
        Best = N | (M << 3);
      }
    }
  }
  *OutHz = BestHz;
  return Best;
}

STATIC VOID TwiClear (VOID)
{
  UINT32  V = MmioRead32 (A733_R_TWI0_BASE + TWI_CNTR);
  MmioWrite32 (A733_R_TWI0_BASE + TWI_CNTR, (V | CNTR_INT_FLAG) & ~(CNTR_M_STA | CNTR_M_STP));
}

STATIC VOID TwiSet (UINT32 Bits)
{
  UINT32  V = MmioRead32 (A733_R_TWI0_BASE + TWI_CNTR);
  MmioWrite32 (A733_R_TWI0_BASE + TWI_CNTR, (V | Bits) & ~CNTR_INT_FLAG);
}

STATIC INT32 TwiWait (VOID)
{
  UINTN  I;

  for (I = 0; I < 2000; I++) {
    if (MmioRead32 (A733_R_TWI0_BASE + TWI_CNTR) & CNTR_INT_FLAG) {
      return (INT32)(MmioRead32 (A733_R_TWI0_BASE + TWI_STAT) & 0xff);
    }
    MicroSecondDelay (5);
  }
  return -1;
}

STATIC VOID TwiStop (VOID)
{
  UINTN  I;

  TwiSet (CNTR_M_STP);
  TwiClear ();
  for (I = 0; I < 200 && (MmioRead32 (A733_R_TWI0_BASE + TWI_CNTR) & CNTR_M_STP); I++) {
    MicroSecondDelay (5);
  }
}

STATIC BOOLEAN TwiExpect (INT32 Want, CONST CHAR8 *What)
{
  INT32  St = TwiWait ();

  if (St == Want) {
    return TRUE;
  }
  if (mTwiErrors++ < 8) {
    DEBUG ((DEBUG_ERROR, "A733 CPUFREQ: TWI %a: status 0x%x (expected 0x%x)\n", What, St, Want));
  }
  TwiStop ();
  return FALSE;
}

STATIC
EFI_STATUS
TwiRead (
  IN  UINT8  Addr,
  IN  UINT8  Reg,
  OUT UINT8  *Val
  )
{
  MmioWrite32 (A733_R_TWI0_BASE + TWI_CNTR,
    (MmioRead32 (A733_R_TWI0_BASE + TWI_CNTR) | CNTR_BUS_EN) & ~(CNTR_A_ACK | CNTR_INT_FLAG));
  TwiSet (CNTR_M_STA);
  if (!TwiExpect (0x08, "start")) {
    return EFI_DEVICE_ERROR;
  }
  MmioWrite32 (A733_R_TWI0_BASE + TWI_DATA, (UINT32)Addr << 1);
  TwiClear ();
  if (!TwiExpect (0x18, "address+W")) {
    return EFI_NOT_FOUND;
  }
  MmioWrite32 (A733_R_TWI0_BASE + TWI_DATA, Reg);
  TwiClear ();
  if (!TwiExpect (0x28, "register")) {
    return EFI_DEVICE_ERROR;
  }
  TwiSet (CNTR_M_STA);
  TwiClear ();
  if (!TwiExpect (0x10, "restart")) {
    return EFI_DEVICE_ERROR;
  }
  MmioWrite32 (A733_R_TWI0_BASE + TWI_DATA, ((UINT32)Addr << 1) | 1);
  TwiClear ();
  if (!TwiExpect (0x40, "address+R")) {
    return EFI_DEVICE_ERROR;
  }
  TwiClear ();                          /* one byte: A_ACK stays off -> NACK */
  if (!TwiExpect (0x58, "data")) {
    return EFI_DEVICE_ERROR;
  }
  *Val = (UINT8)MmioRead32 (A733_R_TWI0_BASE + TWI_DATA);
  TwiStop ();
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
TwiWrite (
  IN UINT8  Addr,
  IN UINT8  Reg,
  IN UINT8  Val
  )
{
  MmioWrite32 (A733_R_TWI0_BASE + TWI_CNTR,
    (MmioRead32 (A733_R_TWI0_BASE + TWI_CNTR) | CNTR_BUS_EN) & ~(CNTR_A_ACK | CNTR_INT_FLAG));
  TwiSet (CNTR_M_STA);
  if (!TwiExpect (0x08, "start")) {
    return EFI_DEVICE_ERROR;
  }
  MmioWrite32 (A733_R_TWI0_BASE + TWI_DATA, (UINT32)Addr << 1);
  TwiClear ();
  if (!TwiExpect (0x18, "address+W")) {
    return EFI_NOT_FOUND;
  }
  MmioWrite32 (A733_R_TWI0_BASE + TWI_DATA, Reg);
  TwiClear ();
  if (!TwiExpect (0x28, "register")) {
    return EFI_DEVICE_ERROR;
  }
  MmioWrite32 (A733_R_TWI0_BASE + TWI_DATA, Val);
  TwiClear ();
  if (!TwiExpect (0x28, "value")) {
    return EFI_DEVICE_ERROR;
  }
  TwiStop ();
  return EFI_SUCCESS;
}

STATIC
VOID
TwiInit (
  VOID
  )
{
  UINTN   I;
  UINT32  InHz, Scl, Ccr;

  MmioOr32 (A733_R_CCU_BASE + RCCU_TWI_BGR, BIT16 | BIT0);
  MicroSecondDelay (10);
  MmioAnd32 (A733_R_TWI0_BASE + TWI_DRV_CTRL, ~(UINT32)BIT0);      /* engine mode */
  MmioWrite32 (A733_R_TWI0_BASE + TWI_SRST, 1);
  for (I = 0; I < 100 && (MmioRead32 (A733_R_TWI0_BASE + TWI_SRST) & 1); I++) {
    MicroSecondDelay (10);
  }
  MmioWrite32 (A733_R_TWI0_BASE + TWI_EFR, 0);
  InHz = RApbs1Hz (MmioRead32 (A733_R_CCU_BASE + RCCU_APBS1));
  Ccr = TwiCcrFor (InHz, 100000, &Scl);
  MmioWrite32 (A733_R_TWI0_BASE + TWI_CCR, Ccr);
  MmioWrite32 (A733_R_TWI0_BASE + TWI_CNTR, CNTR_BUS_EN);
  DEBUG ((DEBUG_INFO, "A733 CPUFREQ: R_TWI0 in %u Hz, CCR 0x%x -> %u Hz\n", InHz, Ccr, Scl));
}

/* ---------------------------------------------------------------- THS */

STATIC INT32 ThsMc (INT32 Reg)
{
  return Reg > 1769 ? (Reg - 2822) * -62 : (Reg - 2835) * -59;
}

STATIC
BOOLEAN
ThsCalib (
  IN  CONST UINT32  Cal[3],
  IN  UINTN         I,
  OUT UINT32        *Cdata
  )
{
  INT32  Ft = (INT32)(Cal[0] & 0xfff);
  INT32  Reg, Delta, C;

  if (!Ft) {
    return FALSE;
  }
  switch (I) {
    case 0: Reg = (INT32)((Cal[0] >> 12) & 0xfff); break;
    case 1: Reg = (INT32)(((Cal[0] >> 24) | (Cal[1] << 8)) & 0xfff); break;
    case 2: Reg = (INT32)((Cal[1] >> 4) & 0xfff); break;
    case 3: Reg = (INT32)((Cal[1] >> 16) & 0xfff); break;
    case 4: Reg = (INT32)(((Cal[1] >> 28) | (Cal[2] << 4)) & 0xfff); break;
    default: return FALSE;
  }
  Delta = (Ft * 100 - ThsMc (Reg)) / -62;
  C = 0x800 - Delta;
  if (C & ~0xfff) {
    return FALSE;
  }
  *Cdata = (UINT32)C;
  return TRUE;
}

/* sun60iw2_thermal_init() as in XNU ths_init(); returns the hottest sensor. */
STATIC
INT32
ThsHottestMc (
  VOID
  )
{
  UINT32  Cal[3], Cdata, O, Sh, Ints, V;
  INT32   Best = CF_NO_TEMP, Mc;
  UINTN   I, Try;

  MmioWrite32 (A733_CCU_BASE + CCU_GPADC0_24M,
    (MmioRead32 (A733_CCU_BASE + CCU_GPADC0_24M) & ~((7u << 24) | 0x1fu)) | BIT31);
  MmioOr32 (A733_CCU_BASE + CCU_GPADC0_BGR, BIT16 | BIT0);
  MmioOr32 (A733_CCU_BASE + CCU_THS0_BGR, BIT16 | BIT0);
  MicroSecondDelay (50);
  for (I = 0; I < 3; I++) {
    Cal[I] = MmioRead32 (A733_SID_BASE + SID_THS_CAL + 4 * I);
  }
  for (I = 0; I < THS_SENSORS; I++) {
    if (ThsCalib (Cal, I, &Cdata)) {
      O = THS_CALIB + (UINT32)(I / 2) * 4;
      Sh = (UINT32)(I % 2) * 16;
      MmioWrite32 (A733_THS_BASE + O, (MmioRead32 (A733_THS_BASE + O) & ~(0xfffu << Sh)) | (Cdata << Sh));
    }
  }
  MmioWrite32 (A733_THS_BASE + THS_CTRL0, 47u | (479u << 16));
  MmioWrite32 (A733_THS_BASE + THS_MFC, BIT2 | BIT0);
  MmioWrite32 (A733_THS_BASE + THS_PC, 28u << 12);
  MmioWrite32 (A733_THS_BASE + THS_DATA_INTS, 0x1f);
  MmioWrite32 (A733_THS_BASE + THS_ENABLE, 0x1f);
  for (Try = 0; Try < 50 && Best == CF_NO_TEMP; Try++) {
    MicroSecondDelay (2000);
    Ints = MmioRead32 (A733_THS_BASE + THS_DATA_INTS);
    for (I = 0; I < THS_SENSORS; I++) {
      V = MmioRead32 (A733_THS_BASE + THS_DATA + 4 * I) & 0xfff;
      if (V && (Ints & (1u << I))) {
        Mc = ThsMc ((INT32)V);
        Best = Mc > Best ? Mc : Best;
      }
    }
  }
  return Best;
}

/* -------------------------------------------------------- rails / PLL */

STATIC
EFI_STATUS
CfSetMv (
  IN     UINT8   Reg,
  IN     UINT16  Mv,
  IN OUT UINT16  *Cur
  )
{
  UINT8   Code = CfMvCode (Mv), Back = 0;
  UINT32  Delta;

  if (Mv > CF_OC_MAX_MV || Mv < 700) {
    return EFI_INVALID_PARAMETER;
  }
  DEBUG ((DEBUG_INFO, "A733 CPUFREQ: rail 0x%02x %u -> %u mV\n", Reg, *Cur, Mv));
  if (EFI_ERROR (TwiWrite (AXP8191_ADDR, Reg, Code))) {
    return EFI_DEVICE_ERROR;
  }
  Delta = Mv > *Cur ? (UINT32)(Mv - *Cur) : (UINT32)(*Cur - Mv);
  MicroSecondDelay (200 + Delta * 8);    /* 250 uV/us ramp, twice over */
  if (EFI_ERROR (TwiRead (AXP8191_ADDR, Reg, &Back)) || (Back & 0x7f) != Code) {
    DEBUG ((DEBUG_ERROR, "A733 CPUFREQ: rail 0x%02x read back 0x%02x, wanted 0x%02x\n", Reg, Back, Code));
    return EFI_DEVICE_ERROR;
  }
  *Cur = Mv;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CfSetPll (
  IN UINT32       Off,
  IN UINT16       Mhz,
  IN CONST CHAR8  *Name
  )
{
  UINT32      N, Clk, Reg;
  UINTN       I, Locked = 0;
  EFI_TPL     OldTpl;
  EFI_STATUS  Status = EFI_SUCCESS;

  N = (Mhz % 24) ? 0 : Mhz / 24u;
  if (N < 28 || N > 96) {
    N = 0;
  }
  Clk = MmioRead32 (A733_CPUPLL_BASE + Off + 0x1c);
  if (!N || ((Clk >> 24) & 7) != 3 || ((Clk >> 16) & 3) != 0) {
    DEBUG ((DEBUG_ERROR, "A733 CPUFREQ: %a not on its own PLL with P=1 (clk 0x%08x); left alone\n", Name, Clk));
    return EFI_UNSUPPORTED;
  }
  DEBUG ((DEBUG_INFO, "A733 CPUFREQ: %a PLL -> %u MHz (N %u)\n", Name, Mhz, N));
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  Reg = (MmioRead32 (A733_CPUPLL_BASE + Off) & ~(0xffu << 8)) | (N << 8);
  MmioWrite32 (A733_CPUPLL_BASE + Off, Reg);
  MmioWrite32 (A733_CPUPLL_BASE + Off, Reg | PLL_UPDATE);
  for (I = 0; I < 1000 && (MmioRead32 (A733_CPUPLL_BASE + Off) & PLL_UPDATE); I++) {
    MicroSecondDelay (10);
  }
  if (I == 1000) {
    Status = EFI_TIMEOUT;
  }
  for (I = 0; !EFI_ERROR (Status) && I < 7000 && Locked < 3; I++) {
    Locked = (MmioRead32 (A733_CPUPLL_BASE + Off) & PLL_LOCK) ? Locked + 1 : 0;
    MicroSecondDelay (3);
  }
  if (!EFI_ERROR (Status) && Locked < 3) {
    Status = EFI_TIMEOUT;
  }
  gBS->RestoreTPL (OldTpl);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733 CPUFREQ: %a PLL did not lock (0x%08x)\n", Name, MmioRead32 (A733_CPUPLL_BASE + Off)));
  }
  return Status;
}

/* Raise rails first, retune, then lower rails (XNU cf_apply). */
STATIC
EFI_STATUS
CfApply (
  IN INT32  A76,
  IN INT32  A55,
  IN INT32  Dsu
  )
{
  UINT16      W3 = mA76[A76].Mv[mCol];
  UINT16      W5 = mA55[A55].Mv[mCol] > mDsu[Dsu].Mv[mCol] ? mA55[A55].Mv[mCol] : mDsu[Dsu].Mv[mCol];
  EFI_STATUS  Status = EFI_SUCCESS;

  if (W3 > mV3) {
    Status = CfSetMv (AXP8191_DC3_VOL, W3, &mV3);
  }
  if (!EFI_ERROR (Status) && W5 > mV5) {
    Status = CfSetMv (AXP8191_DC5_VOL, W5, &mV5);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (A76 != mRowA76) {
    Status = CfSetPll (CPUPLL_A76, mA76[A76].Mhz, "A76");
    if (!EFI_ERROR (Status)) {
      mRowA76 = A76;
    }
  }
  if (!EFI_ERROR (Status) && A55 != mRowA55) {
    Status = CfSetPll (CPUPLL_A55, mA55[A55].Mhz, "A55");
    if (!EFI_ERROR (Status)) {
      mRowA55 = A55;
    }
  }
  if (!EFI_ERROR (Status) && Dsu != mRowDsu) {
    Status = CfSetPll (CPUPLL_DSU, mDsu[Dsu].Mhz, "DSU");
    if (!EFI_ERROR (Status)) {
      mRowDsu = Dsu;
    }
  }
  if (EFI_ERROR (Status)) {
    return Status;                      /* keep the higher voltage */
  }
  if (W3 < mV3) {
    Status = CfSetMv (AXP8191_DC3_VOL, W3, &mV3);
  }
  if (!EFI_ERROR (Status) && W5 < mV5) {
    Status = CfSetMv (AXP8191_DC5_VOL, W5, &mV5);
  }
  return Status;
}

STATIC
VOID
CfReport (
  IN CONST CHAR8  *Tag
  )
{
  STATIC CONST struct { UINT32 Pll; CONST CHAR8 *Name; } Cl[] = {
    { CPUPLL_A55, "A55" }, { CPUPLL_A76, "A76" }, { CPUPLL_DSU, "DSU" },
  };
  UINTN   I;
  UINT32  Pll, Clk;

  for (I = 0; I < ARRAY_SIZE (Cl); I++) {
    Pll = MmioRead32 (A733_CPUPLL_BASE + Cl[I].Pll);
    Clk = MmioRead32 (A733_CPUPLL_BASE + Cl[I].Pll + 0x1c);
    DEBUG ((DEBUG_INFO, "A733 CPUFREQ: %a %a PLL 0x%08x clk 0x%08x = %u MHz\n", Tag, Cl[I].Name, Pll, Clk,
      (24u * ((Pll >> 8) & 0xff)) >> ((Clk >> 16) & 3)));
  }
}

/*
  Ethernet rails, as the working XNU driver (a733_power.c): AXP8191 DCDC_CTL2
  bit 3 DC1SW1 powers the RGMII PHY, bit 4 DC1SW2 the PH pin bank. Without
  them the PHY gives no RGMII clocks and the DWMAC DMA software reset never
  completes (Windows A733Gmac 0.1.0.4: "DMA reset timeout").
*/
#define AXP8191_DCDC_CTL2      0x11
#define AXP8191_DC1SW1         BIT3
#define AXP8191_DC1SW2         BIT4

STATIC
VOID
EthRailsOn (
  VOID
  )
{
  UINT8       Ctl = 0;
  UINT8       Want;
  UINT8       Back = 0;
  EFI_STATUS  Status;

  Status = TwiRead (AXP8191_ADDR, AXP8191_DCDC_CTL2, &Ctl);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733 ETHPWR: DCDC_CTL2 unreadable (%r)\n", Status));
    return;
  }
  Want = Ctl | AXP8191_DC1SW1 | AXP8191_DC1SW2;
  if (Want != Ctl) {
    Status = TwiWrite (AXP8191_ADDR, AXP8191_DCDC_CTL2, Want);
    MicroSecondDelay (20000);           /* PHY power-up before anything is clocked from it */
  }
  (VOID)TwiRead (AXP8191_ADDR, AXP8191_DCDC_CTL2, &Back);
  DEBUG ((EFI_ERROR (Status) || (Back & Want) != Want ? DEBUG_ERROR : DEBUG_INFO,
    "A733 ETHPWR: Ethernet rails DC1SW1/DC1SW2: 0x%02x -> 0x%02x (read back 0x%02x) %r\n",
    Ctl, Want, Back, Status));
}

EFI_STATUS
EFIAPI
A733CpuFreqDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32      Level = FixedPcdGet32 (PcdA733CpuFreqLevel);
  UINT32      Word;
  UINT16      Index, MaxMv;
  UINT8       Ctl = 0, Code3 = 0, Code5 = 0;
  INT32       A76, A55, Dsu, Mc;
  EFI_STATUS  Status;

  CfReport ("before");
  TwiInit ();
  EthRailsOn ();
  if (Level == 0) {
    DEBUG ((DEBUG_INFO, "A733 CPUFREQ: off (level 0)\n"));
    return EFI_SUCCESS;
  }
  Mc = ThsHottestMc ();
  if (Mc != CF_NO_TEMP) {
    DEBUG ((DEBUG_INFO, "A733 CPUFREQ: THS hottest sensor %d.%d C\n", Mc / 1000, (Mc % 1000) / 100));
    if (Mc >= CF_BOOT_HOT_MC && Level > 1) {
      DEBUG ((DEBUG_WARN, "A733 CPUFREQ: already hot at boot, using level 1 (no voltage raise)\n"));
      Level = 1;
    }
  }
  Word = MmioRead32 (A733_SID_BASE + SID_DVFS);
  mCol = CfColumn (Word, &Index);
  MaxMv = (Level == 1) ? 800 : (Level == 2) ? 950 : (Level >= 4) ? CF_OC_MAX_MV : CF_MAX_MV;
  Status = (mCol < 0) ? EFI_UNSUPPORTED : TwiRead (AXP8191_ADDR, AXP8191_DC3_VOL, &Code3);
  if (!EFI_ERROR (Status)) {
    Status = TwiRead (AXP8191_ADDR, AXP8191_DC5_VOL, &Code5);
  }
  if (!EFI_ERROR (Status)) {
    Status = TwiRead (AXP8191_ADDR, AXP8191_DCDC_CTL1, &Ctl);
  }
  mV3 = CfCodeMv (Code3);
  mV5 = CfCodeMv (Code5);
  if (EFI_ERROR (Status) || (Ctl & 0x14) != 0x14) {
    DEBUG ((DEBUG_ERROR, "A733 CPUFREQ: efuse 0x%08x column %d, PMIC rails unreadable or off (ctl 0x%02x); off\n",
      Word, mCol, Ctl));
    return EFI_SUCCESS;
  }
  A76 = CfPick (mA76, ARRAY_SIZE (mA76), mCol, MaxMv, CF_FLOOR_MHZ);
  A55 = CfPick (mA55, ARRAY_SIZE (mA55), mCol, MaxMv, CF_FLOOR_MHZ);
  Dsu = CfPick (mDsu, ARRAY_SIZE (mDsu), mCol, MaxMv, 672);
  if (A76 < 0 || A55 < 0 || Dsu < 0) {
    DEBUG ((DEBUG_ERROR, "A733 CPUFREQ: no OPP for column %d at %u mV; off\n", mCol, MaxMv));
    return EFI_SUCCESS;
  }
  DEBUG ((DEBUG_INFO,
    "A733 CPUFREQ: efuse 0x%08x -> vf%04x (column %d), level %u (<= %u mV): A76 %u MHz %u mV, "
    "A55 %u MHz %u mV, DSU %u MHz; rails now DCDC3 %u mV, DCDC5 %u mV\n",
    Word, Index, mCol, Level, MaxMv, mA76[A76].Mhz, mA76[A76].Mv[mCol], mA55[A55].Mhz, mA55[A55].Mv[mCol],
    mDsu[Dsu].Mhz, mV3, mV5));
  Status = CfApply (A76, A55, Dsu);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO, "A733 CPUFREQ: %a (%r)\n",
    EFI_ERROR (Status) ? "stopped" : "done", Status));
  CfReport ("after");
  return EFI_SUCCESS;
}
