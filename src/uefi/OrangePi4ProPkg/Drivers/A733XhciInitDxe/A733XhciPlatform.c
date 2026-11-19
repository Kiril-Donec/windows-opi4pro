#include "A733XhciPlatform.h"

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>

#define BIT(N)  (1U << (N))

#define A733_CCU_BASE          0x02002000U
#define A733_PIO_BASE          0x02000000U
#define A733_SYSCFG_BASE       0x03000000U
#define A733_USB2_PHY_BASE     0x06B00000U
#define A733_SERDES_SUBSYS     0x06C00000U
#define A733_COMBO0_TOP        0x06C01000U
#define A733_SERDES_COMBO_TOP  0x06C06000U
#define A733_COMBO0_PHY        0x06C80000U
#define A733_R_CCU_BASE        0x07010000U
#define A733_RTC_CCU_BASE      0x07090000U
#define A733_PCK600_BASE       0x07060000U

#define PCK600_USB2_BASE       (A733_PCK600_BASE + 0x8000U)
#define PCK600_COMMAND_ON      0x8U
#define PCK600_STATUS_ON       0x8U

#define COMBO0_LINK_CTRL0      0x000U
#define COMBO0_LINK_CTRL1      0x004U
#define COMBO0_LINK_CTRL2      0x008U
#define COMBO0_LINK_CTRL3      0x00CU
#define COMBO0_LINK_STATUS0    0x900U

#define PHY0_RESET_N                    BIT (0)
#define PHY0_PIPE_LINK_RESET_N_SOFT     BIT (4)
#define PHY0_DP_LINK_RESET_N_SOFT       BIT (8)
#define PHY0_IDDQ_EN_SOFT               BIT (0)
#define PHY0_IDDQ_EN_SEL                BIT (1)
#define PHY0_PLL0_REFCLK_SEL            BIT (20)
#define PHY0_PLL1_REFCLK_SEL            BIT (24)
#define PHY0_REFCLK_DIG_SEL             BIT (0)
#define PHY0_PLLCLK_EN_LN                BIT (0)
#define PHY0_POWER_STATE_REQ_LN(n)      (((n) & 0x3FU) << 4)
#define PHY0_POWER_STATE_REQ_MASK       (0x3FU << 4)
#define PHY0_PMA_CMN_READY              BIT (0)

#define SERDES_USB3_BGR          0x008U
#define USB3_ACLK_EN             BIT (17)
#define USB3_HCLK_EN             BIT (16)
#define USB3_USB2P0_PHY_RSTN     BIT (4)
#define SERDES_DP_CONVERT        0x004U
#define DP_CFG_DATA_EN           BIT (0)
#define DP_CFG_LANE_EN_MASK      (0xFU << 4)
#define DP_CFG_LANE_RSTN_MASK    (0xFU << 8)
#define USB3_PIPE_CLK_MAP        0x100U
#define USB3_PIPE_RXD_MAP        0x104U
#define USB3_PIPE_MAP_MASK       0xFU
#define SERDES_COMBO0_VERSION    0x1FE0U
#define SERDES_TOP_VERSION       0x1FFCU

// A733 pinctrl HW_TYPE_4/10: bank B begins at PIO + 0x100.
#define PB_CFG0                  0x100U
#define PB_DATA                  0x110U
#define PB7_CFG_SHIFT            28U
#define GPIO_OUTPUT              1U

#define USB2_PHYCTL              0x010U
#define USB2_PHYTUNE             0x018U
#define USB2_PHY_RESET_PULSE     0x028U
#define USB2_PHYCTL_OTGDISABLE   BIT (10)
#define USB2_PHYCTL_VBUSVLDEXT   BIT (5)
#define USB2_PHYCTL_SIDDQ        BIT (3)
#define SYSCFG_RESCAL_CTRL       0x160U
#define SYSCFG_RES1_CTRL         0x168U
#define RESCAL_CAL_EN            BIT (0)
#define RESCAL_PCIE_USB_TRIM_SEL BIT (10)
#define RES1_PCIE_USB_TRIM_MASK  (0xFFU << 8)

#define CCU_USB_REF              0x1340U
#define CCU_USB2_U2_REF          0x1348U
#define CCU_USB2_SUSPEND         0x1350U
#define CCU_USB2_MF              0x1354U
#define CCU_USB2_BGR             0x135CU
#define CCU_USB2_U3_UTMI         0x1360U
#define CCU_USB2_U2_PIPE         0x1364U
#define CCU_SERDES_CFG           0x13C0U
#define CCU_SERDES_BGR           0x13C4U
#define CCU_RES_DCAP_24M         0x1A00U

#define RTC_DCXO_SERDES_GATE     0x16CU
#define RTC_DCXO_SERDES0_EN      BIT (4)

#define ARRAY_SIZE_LOCAL(A)      (sizeof (A) / sizeof ((A)[0]))

/*
 * v60: DWC3 bridge clocks. ccu-sun60iw2.c: 0x1360 usb2-u3-utmi = {sys24M,
 * pll-peri0-300m, dcxo}, 0x1364 usb2-u2-pipe = {sys24M, pll-peri0-480m, dcxo},
 * M [4:0], mux [26:24], gate 31. The vendor DT (serdes node assigned-clock-
 * rates) puts them at 60 MHz and 120 MHz; live Linux reads 0x01000004 /
 * 0x01000003. v59 ran both from the 24 MHz oscillator - a 24 MHz PIPE clock
 * where the SoC expects 120 MHz is a candidate for the dead SuperSpeed link.
 */
#define USB_BRIDGE_U3_UTMI_DT    0x01000004U   /* pll-peri0-300m / 5 = 60 MHz */
#define USB_BRIDGE_U2_PIPE_DT    0x01000003U   /* pll-peri0-480m / 4 = 120 MHz */

STATIC
VOID
ProgramUsbBridgeClocks (
  VOID
  )
{
  UINT32 Mode = FixedPcdGet32 (PcdA733UsbBridgeClockMode);

  if (Mode == 0U) {
    MmioWrite32 (A733_CCU_BASE + CCU_USB2_U3_UTMI, 0x80000000U);
    MmioWrite32 (A733_CCU_BASE + CCU_USB2_U2_PIPE, 0x80000000U);
  } else {
    /* Rate first with the gate closed, then open it (mode 1). */
    MmioWrite32 (A733_CCU_BASE + CCU_USB2_U3_UTMI, USB_BRIDGE_U3_UTMI_DT);
    MmioWrite32 (A733_CCU_BASE + CCU_USB2_U2_PIPE, USB_BRIDGE_U2_PIPE_DT);
    if (Mode == 1U) {
      MicroSecondDelay (10);
      MmioWrite32 (A733_CCU_BASE + CCU_USB2_U3_UTMI, USB_BRIDGE_U3_UTMI_DT | BIT (31));
      MmioWrite32 (A733_CCU_BASE + CCU_USB2_U2_PIPE, USB_BRIDGE_U2_PIPE_DT | BIT (31));
    }
  }
  DEBUG ((DEBUG_INFO, "A733 XHCI60: bridge clocks mode %u: 1360=%08x 1364=%08x\n", Mode,
          MmioRead32 (A733_CCU_BASE + CCU_USB2_U3_UTMI), MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_PIPE)));
}

STATIC
BOOLEAN
UsbBridgeClocksOk (
  VOID
  )
{
  UINT32 Gate = (FixedPcdGet32 (PcdA733UsbBridgeClockMode) == 2U) ? 0U : BIT (31);

  return ((MmioRead32 (A733_CCU_BASE + CCU_USB2_U3_UTMI) & BIT (31)) == Gate) &&
         ((MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_PIPE) & BIT (31)) == Gate);
}

typedef struct {
  UINT32 WordOffset;
  UINT16 Value;
} PHY16_WRITE;

/*
 * Exact normal-orientation path from Allwinner's A733 vendor driver:
 * combo0_configure_usb_dp(). Orange Pi 4 Pro does not set udp-disable,
 * so Linux uses this USB+DP-capable path.
 */
STATIC CONST PHY16_WRITE mCombo0UsbDpPrelude[] = {
  { 0xE005U, 0x0003U },
  { 0xE006U, 0x1124U },
  { 0x4100U, 0x00FFU },
  { 0x4300U, 0x00FFU },
  { 0x4102U, 0x04AEU },
  { 0x4302U, 0x04AEU },
  { 0x4103U, 0x04AEU },
  { 0x4303U, 0x04AEU },
  { 0x8000U, 0x0000U },
  { 0x8200U, 0x091DU },
  { 0x8002U, 0x0000U },
  { 0x8202U, 0x0900U },
  { 0x8003U, 0x0000U },
  { 0x8203U, 0x0000U },
  { 0x8006U, 0x0000U },
  { 0x8206U, 0x010FU },
  { 0x40EAU, 0x00FFU },
  { 0x42EAU, 0x00FFU },
  { 0x8108U, 0x0000U },
  { 0x8308U, 0x0009U },
  { 0x8110U, 0x0000U },
  { 0x8310U, 0x0009U },
  { 0x8118U, 0x0000U },
  { 0x8318U, 0x000DU },
  { 0x4040U, 0x2A84U },
  { 0x4240U, 0x2A84U },
  { 0x40C6U, 0x00A3U },
  { 0x42C6U, 0x00A3U },
  { 0x4050U, 0x0000U },
  { 0x4250U, 0x0000U },
  { 0x404CU, 0x001CU },
  { 0x424CU, 0x001CU },
};

STATIC CONST PHY16_WRITE mCombo0UsbDpBody[] = {
  { 0x0022U, 0x001AU },
  { 0x002AU, 0x0034U },
  { 0x002CU, 0x00DAU },
  { 0x0032U, 0x0034U },
  { 0x0034U, 0x00DAU },
  { 0x0064U, 0x0082U },
  { 0x0065U, 0x0082U },
  { 0x0074U, 0x001AU },
  { 0x0104U, 0x0020U },
  { 0x0105U, 0x0007U },
  { 0x010CU, 0x0020U },
  { 0x010DU, 0x0007U },
  { 0x0114U, 0x030CU },
  { 0x0115U, 0x0007U },
  { 0x0124U, 0x0007U },
  { 0x0125U, 0x0003U },
  { 0x0126U, 0x000FU },
  { 0x0128U, 0x0132U },
  { 0x8044U, 0x001AU },
  { 0x8244U, 0x001AU },
  { 0x8444U, 0x001AU },
  { 0x8644U, 0x001AU },
  { 0x8045U, 0x0082U },
  { 0x8245U, 0x0082U },
  { 0x8445U, 0x0082U },
  { 0x8645U, 0x0082U },
  { 0x804CU, 0x001AU },
  { 0x824CU, 0x001AU },
  { 0x844CU, 0x001AU },
  { 0x864CU, 0x001AU },
  { 0x804DU, 0x0082U },
  { 0x824DU, 0x0082U },
  { 0x844DU, 0x0082U },
  { 0x864DU, 0x0082U },
  { 0x4123U, 0x0A28U },
  { 0x4323U, 0x0A28U },
  { 0x4523U, 0x0A28U },
  { 0x4723U, 0x0A28U },
  { 0x01A1U, 0x8600U },
  { 0x01C1U, 0x0701U },
  { 0x44E5U, 0x0012U },
  { 0x46E5U, 0x0012U },
  { 0x40E6U, 0x0000U },
  { 0x42E6U, 0x0000U },
  { 0x40E7U, 0x0001U },
  { 0x42E7U, 0x0001U },
  { 0x40E5U, 0x0012U },
  { 0x42E5U, 0x0012U },
  { 0x40E5U, 0x0041U },
  { 0x42E5U, 0x0041U },
  { 0x44E6U, 0x0001U },
  { 0x46E6U, 0x0001U },
  { 0x44E7U, 0x0001U },
  { 0x46E7U, 0x0001U },
  { 0x44E5U, 0x0019U },
  { 0x46E5U, 0x0019U },
  { 0x0094U, 0x0004U },
  { 0x00D4U, 0x0004U },
  { 0x01A4U, 0x0509U },
  { 0x01C4U, 0x0509U },
  { 0x01A5U, 0x0F00U },
  { 0x01C5U, 0x0F00U },
  { 0x01A6U, 0x0F08U },
  { 0x01C6U, 0x0F08U },
  { 0x0090U, 0x0180U },
  { 0x00D0U, 0x019FU },
  { 0x0091U, 0x9D8AU },
  { 0x00D1U, 0x6276U },
  { 0x0092U, 0x0002U },
  { 0x00D2U, 0x0002U },
  { 0x0093U, 0x0102U },
  { 0x00D3U, 0x0116U },
  { 0x01A0U, 0x0002U },
  { 0x01C0U, 0x0002U },
  { 0x0098U, 0x0001U },
  { 0x00D8U, 0x0002U },
  { 0x0099U, 0x045FU },
  { 0x00D9U, 0x04C4U },
  { 0x009AU, 0x006BU },
  { 0x00DAU, 0x006AU },
  { 0x009BU, 0x0004U },
  { 0x00DBU, 0x0004U },
  { 0x0084U, 0x0104U },
  { 0x00C4U, 0x0104U },
  { 0x0085U, 0x0005U },
  { 0x00C5U, 0x0005U },
  { 0x0086U, 0x0337U },
  { 0x00C6U, 0x0337U },
  { 0x0088U, 0x0335U },
  { 0x00C8U, 0x0335U },
  { 0x0082U, 0x0003U },
  { 0x00C2U, 0x0003U },
  { 0x009CU, 0x00CFU },
  { 0x00DCU, 0x00CFU },
  { 0x009EU, 0x00CEU },
  { 0x00DEU, 0x00CEU },
  { 0x009FU, 0x0005U },
  { 0x00DFU, 0x0005U },
  { 0xC010U, 0x5100U },
  { 0xC011U, 0x0100U },
  { 0xC018U, 0x0A0AU },
  { 0xC01AU, 0x1000U },
  { 0xC01BU, 0x0010U },
  { 0x0041U, 0x8200U },
  { 0x0047U, 0x8200U },
  { 0x4700U, 0x00FFU },
  { 0x4701U, 0x04AFU },
  { 0x4702U, 0x04AEU },
  { 0x4703U, 0x04AEU },
  { 0x8400U, 0x091DU },
  { 0x8401U, 0x091DU },
  { 0x8402U, 0x0900U },
  { 0x8403U, 0x0000U },
  { 0x4640U, 0x2A84U },
  { 0x464DU, 0x0011U },
  { 0x8490U, 0x000CU },
  { 0x8508U, 0x0009U },
  { 0x8549U, 0x0C02U },
  { 0x8577U, 0x06F6U },
  { 0x8578U, 0x4606U },
  { 0x44EBU, 0x0006U },
  { 0x8571U, 0x0519U },
  { 0x8572U, 0x0519U },
  { 0x85E8U, 0x1002U },
  { 0x85E5U, 0x0B98U },
  { 0x85E2U, 0x0C01U },
  { 0x85E3U, 0x0000U },
  { 0x85F5U, 0x0000U },
  { 0x85F4U, 0x0311U },
  { 0x85FFU, 0x0000U },
  { 0x87FFU, 0x0000U },
  { 0x8480U, 0x010AU },
  { 0x8482U, 0x0003U },
  { 0x46EAU, 0x00FFU },
  { 0x44EAU, 0x00FFU },
  { 0x4100U, 0x02FFU },
  { 0x4101U, 0x06AFU },
  { 0x4102U, 0x06AEU },
  { 0x4103U, 0x06AEU },
  { 0x8200U, 0x0D1DU },
  { 0x8201U, 0x0D1DU },
  { 0x8202U, 0x0D00U },
  { 0x8203U, 0x0500U },
  { 0x4040U, 0x2A82U },
  { 0x404DU, 0x0014U },
  { 0x8290U, 0x0013U },
  { 0x8308U, 0x0000U },
  { 0x8349U, 0x0C02U },
  { 0x8377U, 0x0330U },
  { 0x8378U, 0x0300U },
  { 0x42EBU, 0x0003U },
  { 0x8371U, 0x0019U },
  { 0x8372U, 0x0019U },
  { 0x83E8U, 0x1004U },
  { 0x83E5U, 0x00F9U },
  { 0x83E2U, 0x0C01U },
  { 0x83E3U, 0x0002U },
  { 0x83F5U, 0x0000U },
  { 0x83F4U, 0x0031U },
  { 0x83FFU, 0x0001U },
  { 0x81FFU, 0x0002U },
  { 0x8280U, 0x018CU },
  { 0x8282U, 0x0003U },
  { 0x40EAU, 0x000FU },
  { 0x42EAU, 0x00F0U },
};

STATIC CONST PHY16_WRITE mCombo0UsbDpFinalize[] = {
  { 0xE005U, 0x000BU },
  { 0xE006U, 0x2224U },
  { 0x4500U, 0x00FBU },
  { 0x4700U, 0x00FBU },
  { 0x4502U, 0x04AAU },
  { 0x4702U, 0x04AAU },
  { 0x4503U, 0x04AAU },
  { 0x4703U, 0x04AAU },
  { 0x8400U, 0x0000U },
  { 0x8600U, 0x0000U },
  { 0x8402U, 0x0000U },
  { 0x8602U, 0x0000U },
  { 0x8403U, 0x0000U },
  { 0x8603U, 0x0000U },
  { 0x8406U, 0x0000U },
  { 0x8606U, 0x0000U },
  { 0x44EAU, 0x000FU },
  { 0x46EAU, 0x000FU },
  { 0x8508U, 0x0000U },
  { 0x8708U, 0x0000U },
  { 0x8510U, 0x0000U },
  { 0x8710U, 0x0000U },
  { 0x8518U, 0x0000U },
  { 0x8718U, 0x0000U },
  { 0x4440U, 0x08A4U },
  { 0x4640U, 0x08A4U },
  { 0x44C6U, 0x0003U },
  { 0x46C6U, 0x0003U },
  { 0x4450U, 0x002AU },
  { 0x4650U, 0x002AU },
  { 0x444CU, 0x0000U },
  { 0x464CU, 0x0000U },
  { 0x0103U, 0x007FU },
  { 0x010BU, 0x007FU },
};

STATIC VOID
SetBits32 (
  IN UINTN  Address,
  IN UINT32 Bits
  )
{
  MmioWrite32 (Address, MmioRead32 (Address) | Bits);
}

STATIC VOID
ClearBits32 (
  IN UINTN  Address,
  IN UINT32 Bits
  )
{
  MmioWrite32 (Address, MmioRead32 (Address) & ~Bits);
}

STATIC VOID
WritePhyTable (
  IN CONST PHY16_WRITE *Table,
  IN UINTN             Count
  )
{
  UINTN Index;

  for (Index = 0; Index < Count; Index++) {
    MmioWrite16 (
      A733_COMBO0_PHY + (Table[Index].WordOffset << 1),
      Table[Index].Value
      );
  }
}

STATIC EFI_STATUS
PowerOnUsb2Domain (
  VOID
  )
{
  UINTN Retry;

  SetBits32 (A733_R_CCU_BASE + 0x01ACU, BIT (0));
  MmioWrite32 (PCK600_USB2_BASE, PCK600_COMMAND_ON);
  for (Retry = 0; Retry < 2000; Retry++) {
    if ((MmioRead32 (PCK600_USB2_BASE + 8U) & 0xFU) == PCK600_STATUS_ON) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (10);
  }

  DEBUG ((DEBUG_ERROR,
          "A733 XHCI49: USB2 power domain timeout cmd=%08x status=%08x\n",
          MmioRead32 (PCK600_USB2_BASE),
          MmioRead32 (PCK600_USB2_BASE + 8U)));
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
EnableDcxoSerdes0 (
  VOID
  )
{
  UINT32 Value;

  SetBits32 (A733_RTC_CCU_BASE + RTC_DCXO_SERDES_GATE, RTC_DCXO_SERDES0_EN);
  MicroSecondDelay (100);
  Value = MmioRead32 (A733_RTC_CCU_BASE + RTC_DCXO_SERDES_GATE);
  DEBUG ((DEBUG_INFO,
          "A733 XHCI49: RTC dcxo-serdes gate=%08x serdes0=%u\n",
          Value,
          (Value & RTC_DCXO_SERDES0_EN) != 0U ? 1U : 0U));

  return (Value & RTC_DCXO_SERDES0_EN) != 0U ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
EnableUsb3VbusPb7 (
  VOID
  )
{
  UINT32 Value;
  UINT32 Mux;

  Value = MmioRead32 (A733_PIO_BASE + PB_CFG0);
  Value &= ~(0xFU << PB7_CFG_SHIFT);
  Value |= GPIO_OUTPUT << PB7_CFG_SHIFT;
  MmioWrite32 (A733_PIO_BASE + PB_CFG0, Value);

  // Board DTS: PB7 active-low fixed regulator. Drive low to enable 5 V VBUS.
  Value = MmioRead32 (A733_PIO_BASE + PB_DATA);
  Value &= ~BIT (7);
  MmioWrite32 (A733_PIO_BASE + PB_DATA, Value);
  MicroSecondDelay (5000);

  Value = MmioRead32 (A733_PIO_BASE + PB_CFG0);
  Mux = (Value >> PB7_CFG_SHIFT) & 0xFU;
  DEBUG ((DEBUG_INFO,
          "A733 XHCI49: PB7 VBUS cfg=%08x data=%08x mux=%u active-low=%u\n",
          Value,
          MmioRead32 (A733_PIO_BASE + PB_DATA),
          Mux,
          (MmioRead32 (A733_PIO_BASE + PB_DATA) & BIT (7)) == 0U ? 1U : 0U));

  if ((Mux != GPIO_OUTPUT) ||
      ((MmioRead32 (A733_PIO_BASE + PB_DATA) & BIT (7)) != 0U)) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EnableFabricAndClocks (
  VOID
  )
{
  EFI_STATUS Status;

  SetBits32 (A733_CCU_BASE + 0x05A4U, BIT (0) | BIT (16) | BIT (17));
  MmioWrite32 (A733_CCU_BASE + 0x05C0U, 0xB10103F8U);
  MmioWrite32 (A733_CCU_BASE + 0x05E0U, 0xF0050803U);
  MmioWrite32 (A733_CCU_BASE + 0x05E4U, 0x00000805U);
  SetBits32 (A733_CCU_BASE + CCU_RES_DCAP_24M, BIT (3));

  // Vendor BSP and the board-proven v34/v35 recovery both enable the
  // common USB reference gate in addition to the three xHCI node clocks.
  MmioWrite32 (A733_CCU_BASE + CCU_USB_REF,      0x80000000U);
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_U2_REF,  0x80000000U);
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_SUSPEND, 0x81000000U);
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_MF,      0x80000000U);
  // The A733 CCU exposes two additional DWC3 bridge clocks.  Leaving
  // either gate disabled keeps both xHCI root ports in RxDetect (CCS=0),
  // even though the DWC3 core, VBUS and the external PHY resets look ready.
  ProgramUsbBridgeClocks ();

  Status = EnableDcxoSerdes0 ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ClearBits32 (A733_CCU_BASE + CCU_USB2_BGR, BIT (16));
  ClearBits32 (A733_CCU_BASE + CCU_SERDES_BGR, BIT (16));
  MicroSecondDelay (100);
  MmioWrite32 (A733_CCU_BASE + CCU_SERDES_CFG, 0x81000005U);
  SetBits32 (A733_CCU_BASE + CCU_SERDES_BGR, BIT (16));
  SetBits32 (A733_CCU_BASE + CCU_USB2_BGR, BIT (16));
  MicroSecondDelay (1000);

  if (((MmioRead32 (A733_CCU_BASE + CCU_USB_REF) & BIT (31)) == 0U) ||
      ((MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_REF) & BIT (31)) == 0U) ||
      ((MmioRead32 (A733_CCU_BASE + CCU_USB2_SUSPEND) & BIT (31)) == 0U) ||
      ((MmioRead32 (A733_CCU_BASE + CCU_USB2_MF) & BIT (31)) == 0U) ||
      !UsbBridgeClocksOk () ||
      ((MmioRead32 (A733_CCU_BASE + CCU_SERDES_CFG) & BIT (31)) == 0U) ||
      ((MmioRead32 (A733_CCU_BASE + CCU_SERDES_BGR) & BIT (16)) == 0U) ||
      ((MmioRead32 (A733_CCU_BASE + CCU_USB2_BGR) & BIT (16)) == 0U)) {
    DEBUG ((DEBUG_ERROR,
            "A733 XHCI54: clock/reset readback failed 1340=%08x 1348=%08x 1350=%08x 1354=%08x 1360=%08x 1364=%08x 135c=%08x 13c0=%08x 13c4=%08x\n",
            MmioRead32 (A733_CCU_BASE + CCU_USB_REF),
            MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_REF),
            MmioRead32 (A733_CCU_BASE + CCU_USB2_SUSPEND),
            MmioRead32 (A733_CCU_BASE + CCU_USB2_MF),
            MmioRead32 (A733_CCU_BASE + CCU_USB2_U3_UTMI),
            MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_PIPE),
            MmioRead32 (A733_CCU_BASE + CCU_USB2_BGR),
            MmioRead32 (A733_CCU_BASE + CCU_SERDES_CFG),
            MmioRead32 (A733_CCU_BASE + CCU_SERDES_BGR)));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC VOID
EnableUsb2PhyV2 (
  VOID
  )
{
  UINT32 Value;

  Value = MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL);
  Value &= ~RESCAL_CAL_EN;
  Value |= RESCAL_PCIE_USB_TRIM_SEL;
  MmioWrite32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL, Value);

  Value = MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RES1_CTRL);
  Value &= ~RES1_PCIE_USB_TRIM_MASK;
  MmioWrite32 (A733_SYSCFG_BASE + SYSCFG_RES1_CTRL, Value);

  // Exact sunxi_plat_phy_init() RMW: set bits 10 and 5, clear bit 3.
  Value = MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYCTL);
  Value |= USB2_PHYCTL_OTGDISABLE | USB2_PHYCTL_VBUSVLDEXT;
  Value &= ~USB2_PHYCTL_SIDDQ;
  MmioWrite32 (A733_USB2_PHY_BASE + USB2_PHYCTL, Value);
  MmioWrite32 (A733_USB2_PHY_BASE + USB2_PHYTUNE, 0x143338D6U);

  // This write-one pulse is present in the board-proven v34/v35 sequence.
  // It self-clears; do it while USB2P0 external PHY reset is asserted.
  MmioWrite32 (A733_USB2_PHY_BASE + USB2_PHY_RESET_PULSE, BIT (3));
  MicroSecondDelay (1000);

  DEBUG ((DEBUG_INFO,
          "A733 XHCI49: USB2 PHY ctl=%08x tune=%08x status=%08x res=%08x/%08x\n",
          MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYCTL),
          MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYTUNE),
          MmioRead32 (A733_USB2_PHY_BASE + USB2_PHY_RESET_PULSE),
          MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL),
          MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RES1_CTRL)));
}

STATIC VOID
ProgramCombo0UsbDpParameters (
  VOID
  )
{
  WritePhyTable (mCombo0UsbDpPrelude, ARRAY_SIZE_LOCAL (mCombo0UsbDpPrelude));
  MmioWrite32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL1, 0x01100001U);
  WritePhyTable (mCombo0UsbDpBody, ARRAY_SIZE_LOCAL (mCombo0UsbDpBody));
  WritePhyTable (mCombo0UsbDpFinalize, ARRAY_SIZE_LOCAL (mCombo0UsbDpFinalize));
}

STATIC EFI_STATUS
WaitCombo0PmaReady (
  VOID
  )
{
  UINT32 Value;
  UINTN Retry;

  for (Retry = 0; Retry < 1000; Retry++) {
    Value = MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_STATUS0);
    if ((Value & PHY0_PMA_CMN_READY) != 0U) {
      DEBUG ((DEBUG_INFO,
              "A733 XHCI49: Combo0 PMA ready after %u us status=%08x\n",
              (UINT32)(Retry * 5U),
              Value));
      return EFI_SUCCESS;
    }
    MicroSecondDelay (5);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
EnableCombo0Usb3 (
  VOID
  )
{
  EFI_STATUS Status;
  UINT32 Value;

  Value = MmioRead32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR);
  Value |= USB3_ACLK_EN | USB3_HCLK_EN | USB3_USB2P0_PHY_RSTN;
  MmioWrite32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR, Value);

  ClearBits32 (
    A733_COMBO0_TOP + COMBO0_LINK_CTRL2,
    PHY0_IDDQ_EN_SEL | PHY0_IDDQ_EN_SOFT
    );

  SetBits32 (
    A733_COMBO0_TOP + COMBO0_LINK_CTRL1,
    PHY0_PLL1_REFCLK_SEL | PHY0_PLL0_REFCLK_SEL | PHY0_REFCLK_DIG_SEL
    );

  Value = MmioRead32 (A733_SERDES_COMBO_TOP + USB3_PIPE_CLK_MAP);
  Value &= ~USB3_PIPE_MAP_MASK;
  MmioWrite32 (A733_SERDES_COMBO_TOP + USB3_PIPE_CLK_MAP, Value);

  Value = MmioRead32 (A733_SERDES_COMBO_TOP + USB3_PIPE_RXD_MAP);
  Value &= ~USB3_PIPE_MAP_MASK;
  MmioWrite32 (A733_SERDES_COMBO_TOP + USB3_PIPE_RXD_MAP, Value);

  ProgramCombo0UsbDpParameters ();

  ClearBits32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL0, PHY0_RESET_N);
  MicroSecondDelay (1000);
  SetBits32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL0, PHY0_RESET_N);

  SetBits32 (
    A733_SERDES_COMBO_TOP + SERDES_DP_CONVERT,
    DP_CFG_DATA_EN | DP_CFG_LANE_EN_MASK | DP_CFG_LANE_RSTN_MASK
    );

  MicroSecondDelay (1000);
  SetBits32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL0, PHY0_PIPE_LINK_RESET_N_SOFT);
  SetBits32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL3, PHY0_PLLCLK_EN_LN);

  Value = MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL3);
  Value &= ~PHY0_POWER_STATE_REQ_MASK;
  Value |= PHY0_POWER_STATE_REQ_LN (1);
  MmioWrite32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL3, Value);

  SetBits32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL0, PHY0_DP_LINK_RESET_N_SOFT);

  if ((MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_STATUS0) &
       PHY0_PMA_CMN_READY) == 0U) {
    MicroSecondDelay (10000);
    Value = MmioRead16 (A733_COMBO0_PHY + (0xC008U << 1));
    Value &= ~(0xFU << 8);
    MmioWrite16 (A733_COMBO0_PHY + (0xC008U << 1), (UINT16)Value);
  }

  Status = WaitCombo0PmaReady ();
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR,
          "A733 XHCI49: Combo0 PMA timeout status=%08x ctrl0=%08x ctrl1=%08x ctrl2=%08x ctrl3=%08x E003=%04x E005=%04x E006=%04x C008=%04x\n",
          MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_STATUS0),
          MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL0),
          MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL1),
          MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL2),
          MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL3),
          MmioRead16 (A733_COMBO0_PHY + (0xE003U << 1)),
          MmioRead16 (A733_COMBO0_PHY + (0xE005U << 1)),
          MmioRead16 (A733_COMBO0_PHY + (0xE006U << 1)),
          MmioRead16 (A733_COMBO0_PHY + (0xC008U << 1))));
  return Status;
}


EFI_STATUS
A733XhciPlatformRecoverController (
  VOID
  )
{
  EFI_STATUS Status;
  UINT32     BeforeCcu;
  UINT32     BeforeSub;
  UINT32     AfterCcu;
  UINT32     AfterSub;
  UINT32     PhyCtl;
  UINT32     PhyTune;

  /*
   * Exact merge of the two states already proven on this board:
   *   - v42: Combo0 PMA, dcxo-serdes0 and VBUS are alive;
   *   - v34/v35: a full external RST_USB_2 + USB2P0 reset replay clears CNR.
   * Do not reset or reprogram the ready Combo0 analog PMA here.
   */
  Status = PowerOnUsb2Domain ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  BeforeCcu = MmioRead32 (A733_CCU_BASE + CCU_USB2_BGR);
  BeforeSub = MmioRead32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR);
  DEBUG ((DEBUG_WARN,
          "A733 XHCI49: exact recovery begin 1340=%08x 1348=%08x 1350=%08x 1354=%08x 135c=%08x SUB8=%08x PMA=%u\n",
          MmioRead32 (A733_CCU_BASE + CCU_USB_REF),
          MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_REF),
          MmioRead32 (A733_CCU_BASE + CCU_USB2_SUSPEND),
          MmioRead32 (A733_CCU_BASE + CCU_USB2_MF),
          BeforeCcu,
          BeforeSub,
          A733XhciPlatformPmaReady () ? 1U : 0U));

  // 1. Assert the external DWC3 controller reset and USB2P0 PHY reset.
  ClearBits32 (A733_CCU_BASE + CCU_USB2_BGR, BIT (16));
  ClearBits32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR, USB3_USB2P0_PHY_RSTN);
  MicroSecondDelay (100);

  // 2. Replay every fabric/clock register used by the successful v34/v35
  // sequence. 0x1340 was missing from v41-v43.
  SetBits32 (A733_CCU_BASE + 0x05A4U, BIT (0) | BIT (16) | BIT (17));
  MmioWrite32 (A733_CCU_BASE + 0x05C0U, 0xB10103F8U);
  MmioWrite32 (A733_CCU_BASE + 0x05E0U, 0xF0050803U);
  MmioWrite32 (A733_CCU_BASE + 0x05E4U, 0x00000805U);
  SetBits32 (A733_CCU_BASE + CCU_RES_DCAP_24M, BIT (3));

  MmioWrite32 (A733_CCU_BASE + CCU_USB_REF,      0x80000000U);
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_U2_REF,  0x80000000U);
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_SUSPEND, 0x81000000U);
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_MF,      0x80000000U);
  ProgramUsbBridgeClocks ();

  Status = EnableDcxoSerdes0 ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Keep the Combo0 register clock and SERDES bus out of reset. Do not pulse
  // the SERDES bus reset because PMA is already confirmed ready.
  MmioWrite32 (A733_CCU_BASE + CCU_SERDES_CFG, 0x81000005U);
  SetBits32 (A733_CCU_BASE + CCU_SERDES_BGR, BIT (16));
  SetBits32 (
    A733_SERDES_SUBSYS + SERDES_USB3_BGR,
    USB3_ACLK_EN | USB3_HCLK_EN
    );

  // 3. Release only RST_USB_2. Keep USB2P0 PHY reset asserted while the
  // vendor USB2 PHY V2 RMW/tune sequence is written, exactly as in v34/v35.
  SetBits32 (A733_CCU_BASE + CCU_USB2_BGR, BIT (16));
  MicroSecondDelay (20);
  EnableUsb2PhyV2 ();

  PhyCtl  = MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYCTL);
  PhyTune = MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYTUNE);
  DEBUG ((DEBUG_INFO,
          "A733 XHCI49: exact recovery pre-PHY-release U2=%08x TUNE=%08x RESET=%08x SUB8=%08x\n",
          PhyCtl,
          PhyTune,
          MmioRead32 (A733_USB2_PHY_BASE + USB2_PHY_RESET_PULSE),
          MmioRead32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR)));

  // The tune value must latch before the external PHY reset is released.
  if (PhyTune != 0x143338D6U) {
    DEBUG ((DEBUG_ERROR,
            "A733 XHCI49: USB2 PHY tune did not latch before reset release\n"));
    return EFI_DEVICE_ERROR;
  }

  // 4. Release USB2P0 PHY reset and allow clocks/PLL to synchronize.
  SetBits32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR, USB3_USB2P0_PHY_RSTN);
  MicroSecondDelay (1000);

  AfterCcu = MmioRead32 (A733_CCU_BASE + CCU_USB2_BGR);
  AfterSub = MmioRead32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR);
  DEBUG ((DEBUG_INFO,
          "A733 XHCI49: exact recovery end 1340=%08x 1348=%08x 1350=%08x 1354=%08x 135c=%08x SUB8=%08x U2=%08x TUNE=%08x PMA=%u\n",
          MmioRead32 (A733_CCU_BASE + CCU_USB_REF),
          MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_REF),
          MmioRead32 (A733_CCU_BASE + CCU_USB2_SUSPEND),
          MmioRead32 (A733_CCU_BASE + CCU_USB2_MF),
          AfterCcu,
          AfterSub,
          MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYCTL),
          MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYTUNE),
          A733XhciPlatformPmaReady () ? 1U : 0U));

  if (((MmioRead32 (A733_CCU_BASE + CCU_USB_REF) & BIT (31)) == 0U) ||
      ((MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_REF) & BIT (31)) == 0U) ||
      ((MmioRead32 (A733_CCU_BASE + CCU_USB2_SUSPEND) & BIT (31)) == 0U) ||
      ((MmioRead32 (A733_CCU_BASE + CCU_USB2_MF) & BIT (31)) == 0U) ||
      !UsbBridgeClocksOk () ||
      ((AfterCcu & BIT (16)) == 0U) ||
      ((AfterSub & (USB3_ACLK_EN | USB3_HCLK_EN | USB3_USB2P0_PHY_RSTN)) !=
       (USB3_ACLK_EN | USB3_HCLK_EN | USB3_USB2P0_PHY_RSTN)) ||
      (MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYTUNE) != 0x143338D6U) ||
      !A733XhciPlatformPmaReady ()) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

BOOLEAN
A733XhciPlatformPmaReady (
  VOID
  )
{
  return (MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_STATUS0) &
          PHY0_PMA_CMN_READY) != 0U;
}

EFI_STATUS
A733XhciPlatformInit (
  VOID
  )
{
  EFI_STATUS Status;
  UINT32     Value;

  Status = PowerOnUsb2Domain ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EnableFabricAndClocks ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EnableUsb3VbusPb7 ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Program USB2 PHY with its external reset asserted. The v42 UART showed
  // that doing this before ACLK/HCLK were enabled produced zero readback.
  Value = MmioRead32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR);
  Value |= USB3_ACLK_EN | USB3_HCLK_EN;
  Value &= ~USB3_USB2P0_PHY_RSTN;
  MmioWrite32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR, Value);
  MicroSecondDelay (100);
  EnableUsb2PhyV2 ();
  SetBits32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR, USB3_USB2P0_PHY_RSTN);
  MicroSecondDelay (1000);

  return EnableCombo0Usb3 ();
}

VOID
A733XhciPlatformDump (
  IN CONST CHAR8 *Stage
  )
{
  DEBUG ((DEBUG_INFO,
    "A733 XHCI54 %a: PD=%08x/%08x PB_CFG=%08x DATA=%08x RTC16c=%08x 1340=%08x 1348=%08x 1350=%08x 1354=%08x 1360=%08x 1364=%08x CCU135c=%08x 13c0=%08x 13c4=%08x U2=%08x TUNE=%08x RES160=%08x RES168=%08x SUB8=%08x C0=%08x/%08x/%08x/%08x ST=%08x MAP=%08x/%08x VER=%08x/%08x E005=%04x E006=%04x C008=%04x\n",
    Stage,
    MmioRead32 (PCK600_USB2_BASE),
    MmioRead32 (PCK600_USB2_BASE + 8U),
    MmioRead32 (A733_PIO_BASE + PB_CFG0),
    MmioRead32 (A733_PIO_BASE + PB_DATA),
    MmioRead32 (A733_RTC_CCU_BASE + RTC_DCXO_SERDES_GATE),
    MmioRead32 (A733_CCU_BASE + CCU_USB_REF),
    MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_REF),
    MmioRead32 (A733_CCU_BASE + CCU_USB2_SUSPEND),
    MmioRead32 (A733_CCU_BASE + CCU_USB2_MF),
    MmioRead32 (A733_CCU_BASE + CCU_USB2_U3_UTMI),
    MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_PIPE),
    MmioRead32 (A733_CCU_BASE + CCU_USB2_BGR),
    MmioRead32 (A733_CCU_BASE + CCU_SERDES_CFG),
    MmioRead32 (A733_CCU_BASE + CCU_SERDES_BGR),
    MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYCTL),
    MmioRead32 (A733_USB2_PHY_BASE + USB2_PHYTUNE),
    MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL),
    MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RES1_CTRL),
    MmioRead32 (A733_SERDES_SUBSYS + SERDES_USB3_BGR),
    MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL0),
    MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL1),
    MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL2),
    MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_CTRL3),
    MmioRead32 (A733_COMBO0_TOP + COMBO0_LINK_STATUS0),
    MmioRead32 (A733_SERDES_COMBO_TOP + USB3_PIPE_CLK_MAP),
    MmioRead32 (A733_SERDES_COMBO_TOP + USB3_PIPE_RXD_MAP),
    MmioRead32 (A733_SERDES_COMBO_TOP + SERDES_COMBO0_VERSION),
    MmioRead32 (A733_SERDES_COMBO_TOP + SERDES_TOP_VERSION),
    MmioRead16 (A733_COMBO0_PHY + (0xE005U << 1)),
    MmioRead16 (A733_COMBO0_PHY + (0xE006U << 1)),
    MmioRead16 (A733_COMBO0_PHY + (0xC008U << 1))));
}
