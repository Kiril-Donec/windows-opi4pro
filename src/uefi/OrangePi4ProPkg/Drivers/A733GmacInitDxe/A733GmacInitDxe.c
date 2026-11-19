/** @file
  Orange Pi 4 Pro / Allwinner A733 GMAC0 board initialization.

  This driver owns only board-global pinmux, clock/reset, and wrapper setup.
  The Windows AWIN0040 function driver owns the DWMAC MAC, DMA, MDIO and PHY.
**/

#include <Uefi.h>

#include <Protocol/A733GmacReady.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#define A733_PIO_BASE            0x02000000U
#define A733_PIO_PH_CFG0         (A733_PIO_BASE + 0x0400U)
#define A733_PIO_PH_CFG1         (A733_PIO_BASE + 0x0404U)
#define A733_PIO_PH_DRV0         (A733_PIO_BASE + 0x0420U)
#define A733_PIO_PH_DRV1         (A733_PIO_BASE + 0x0424U)
#define A733_PIO_PH_PULL0        (A733_PIO_BASE + 0x0430U)

#define A733_CCU_BASE            0x02002000U
#define A733_CCU_GMAC_MBUS       (A733_CCU_BASE + 0x05E4U)
#define A733_CCU_GMAC_PHY_CLK    (A733_CCU_BASE + 0x1410U)
#define A733_CCU_GMAC_BGR        (A733_CCU_BASE + 0x141CU)
#define A733_CCU_WRITE_KEY       0x00040302U
#define A733_CCU_GMAC_MBUS_GATE  BIT11
#define A733_CCU_GMAC_CORE_GATE  BIT0
#define A733_CCU_GMAC_BUS_RST    BIT16
#define A733_CCU_GMAC_AXI_RST    BIT17
#define A733_CCU_GMAC_PHY_GATE   BIT31
#define A733_CCU_GMAC_PHY_DIV    5U

#define A733_GMAC_BASE           0x04500000U
#define A733_GMAC_VERSION        (A733_GMAC_BASE + 0x0110U)
#define A733_GMAC_HW_FEATURE0    (A733_GMAC_BASE + 0x011CU)
#define A733_GMAC_WRAPPER_BASE   0x04508000U
#define A733_GMAC_WRAPPER_CFG    (A733_GMAC_WRAPPER_BASE + 0x0000U)
#define A733_GMAC_WRAPPER_STATUS (A733_GMAC_WRAPPER_BASE + 0x004CU)
#define A733_GMAC_WRAPPER_CLOCK  (A733_GMAC_WRAPPER_BASE + 0x0080U)
#define A733_GMAC_WRAPPER_VALUE  0x00011145U

STATIC EFI_HANDLE                 mGmacReadyHandle;
STATIC A733_GMAC_READY_PROTOCOL   mGmacReady = { 1, FALSE, 0, 0, { 0, 0, 0, 0 }, 0, 0, 0 };

STATIC
VOID
WriteReadback (
  IN UINTN        Address,
  IN UINT32       Value,
  IN CONST CHAR8  *Name
  )
{
  UINT32 Before;
  UINT32 After;

  Before = MmioRead32 (Address);
  MmioWrite32 (Address, Value);
  MemoryFence ();
  After = MmioRead32 (Address);
  DEBUG ((DEBUG_INFO, "A733 GMAC59 %a: %08x -> %08x (wanted %08x)\n",
          Name, Before, After, Value));
}

STATIC
VOID
ConfigureRgmiiPins (
  VOID
  )
{
  // PH0-PH15 exclusively occupy these fields. PH16-PH31 are not touched.
  WriteReadback (A733_PIO_PH_CFG0,  0x55555555U, "PH_CFG0");
  WriteReadback (A733_PIO_PH_CFG1,  0x55555555U, "PH_CFG1");
  WriteReadback (A733_PIO_PH_DRV0,  0x11111111U, "PH_DRV0");
  WriteReadback (A733_PIO_PH_DRV1,  0x11111111U, "PH_DRV1");
  WriteReadback (A733_PIO_PH_PULL0, 0x55555555U, "PH_PULL0");
}

STATIC
VOID
ConfigureGmacClocks (
  VOID
  )
{
  UINT32 Value;

  Value = MmioRead32 (A733_CCU_GMAC_BGR);
  Value &= ~(A733_CCU_GMAC_BUS_RST | A733_CCU_GMAC_AXI_RST);
  WriteReadback (A733_CCU_GMAC_BGR, Value, "BGR_ASSERT");

  Value = MmioRead32 (A733_CCU_GMAC_MBUS);
  Value |= A733_CCU_WRITE_KEY | A733_CCU_GMAC_MBUS_GATE;
  WriteReadback (A733_CCU_GMAC_MBUS, Value, "MBUS_GATE");

  Value = MmioRead32 (A733_CCU_GMAC_PHY_CLK);
  Value &= ~0x1FU;
  Value |= A733_CCU_GMAC_PHY_DIV | A733_CCU_GMAC_PHY_GATE;
  WriteReadback (A733_CCU_GMAC_PHY_CLK, Value, "PHY_CLK");

  Value = MmioRead32 (A733_CCU_GMAC_BGR);
  Value |= A733_CCU_GMAC_CORE_GATE;
  WriteReadback (A733_CCU_GMAC_BGR, Value, "CORE_GATE");
  MicroSecondDelay (10U);

  Value |= A733_CCU_GMAC_BUS_RST | A733_CCU_GMAC_AXI_RST;
  WriteReadback (A733_CCU_GMAC_BGR, Value, "BGR_DEASSERT");
  MicroSecondDelay (1000U);
}

STATIC
BOOLEAN
ProbeGmac (
  VOID
  )
{
  UINT32 Index;

  mGmacReady.WrapperConfig = MmioRead32 (A733_GMAC_WRAPPER_CFG);
  mGmacReady.CoreVersion = MmioRead32 (A733_GMAC_VERSION);
  for (Index = 0; Index < ARRAY_SIZE (mGmacReady.HardwareFeature); Index++) {
    mGmacReady.HardwareFeature[Index] =
      MmioRead32 (A733_GMAC_HW_FEATURE0 + (Index * sizeof (UINT32)));
  }

  mGmacReady.CcuMbus = MmioRead32 (A733_CCU_GMAC_MBUS);
  mGmacReady.CcuPhyClock = MmioRead32 (A733_CCU_GMAC_PHY_CLK);
  mGmacReady.CcuBusReset = MmioRead32 (A733_CCU_GMAC_BGR);

  DEBUG ((DEBUG_INFO,
          "A733 GMAC59 probe: wrap=%08x status=%08x clock=%08x ver=%08x features=%08x/%08x/%08x/%08x ccu=%08x/%08x/%08x\n",
          mGmacReady.WrapperConfig,
          MmioRead32 (A733_GMAC_WRAPPER_STATUS),
          MmioRead32 (A733_GMAC_WRAPPER_CLOCK),
          mGmacReady.CoreVersion,
          mGmacReady.HardwareFeature[0], mGmacReady.HardwareFeature[1],
          mGmacReady.HardwareFeature[2], mGmacReady.HardwareFeature[3],
          mGmacReady.CcuMbus, mGmacReady.CcuPhyClock, mGmacReady.CcuBusReset));

  if (mGmacReady.WrapperConfig != A733_GMAC_WRAPPER_VALUE) {
    return FALSE;
  }

  if ((mGmacReady.CoreVersion == 0U) ||
      (mGmacReady.CoreVersion == MAX_UINT32)) {
    return FALSE;
  }

  return TRUE;
}

EFI_STATUS
EFIAPI
A733GmacInitEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  DEBUG ((DEBUG_INFO, "A733 GMAC59: board bring-up begin (PA14 untouched)\n"));
  ConfigureRgmiiPins ();
  ConfigureGmacClocks ();
  WriteReadback (A733_GMAC_WRAPPER_CFG, A733_GMAC_WRAPPER_VALUE, "WRAPPER_CFG");
  MicroSecondDelay (1000U);

  mGmacReady.PlatformReady = ProbeGmac ();
  mGmacReadyHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mGmacReadyHandle,
                  &gA733GmacReadyProtocolGuid,
                  &mGmacReady,
                  NULL
                  );
  DEBUG ((mGmacReady.PlatformReady ? DEBUG_INFO : DEBUG_ERROR,
          "A733 GMAC59: ready=%u protocol=%r\n",
          mGmacReady.PlatformReady ? 1U : 0U, Status));
  return Status;
}
