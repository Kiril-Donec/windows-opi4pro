#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Protocol/NonDiscoverableDevice.h>

#define CCU_BASE                 0x02002000ULL
#define SYSCFG_BASE              0x03000000ULL
#define CCU_MSI_LITE2            (CCU_BASE + 0x05A4)
#define CCU_AHB_GATE_SW_CFG      (CCU_BASE + 0x05C0)
#define CCU_USB1_PHY_CFG         (CCU_BASE + 0x1308)
#define CCU_USB1_HCI_CFG         (CCU_BASE + 0x130C)
#define CCU_USB_REF_CFG          (CCU_BASE + 0x1340)
#define CCU_RES_DCAP_24M         (CCU_BASE + 0x1A00)
#define SYSCFG_RESCAL_CTRL       (SYSCFG_BASE + 0x0160)
#define SYSCFG_RES0_CTRL         (SYSCFG_BASE + 0x0164)
#define BIT(n)                   (1U << (n))
#define AHB_MASTER_KEY_VALUE     0x010000FFU

#define EHCI_HCSPARAMS           0x04
#define EHCI_HCCPARAMS           0x08
#define EHCI_USBCMD              0x00
#define EHCI_USBSTS              0x04
#define EHCI_USBINTR             0x08
#define EHCI_CONFIGFLAG          0x40
#define EHCI_PORTSC0             0x44
#define EHCI_VENDOR_PASSBY       0x800
#define EHCI_VENDOR_PHY_CTRL     0x810
#define EHCI_VENDOR_PHY_TUNE     0x818
#define CMD_RUN                  BIT(0)
#define CMD_RESET                BIT(1)
#define CMD_PSE                  BIT(4)
#define CMD_ASE                  BIT(5)
#define STS_HALT                 BIT(12)
#define PHY_CTRL_SIDDQ           BIT(3)
#define PHY_RANGE_VALUE          0x188U

STATIC VOID
SetBits (UINTN Address, UINT32 Bits)
{
  MmioWrite32 (Address, MmioRead32 (Address) | Bits);
  ArmDataSynchronizationBarrier ();
  (VOID)MmioRead32 (Address);
}

STATIC VOID
ClrBits (UINTN Address, UINT32 Bits)
{
  MmioWrite32 (Address, MmioRead32 (Address) & ~Bits);
  ArmDataSynchronizationBarrier ();
  (VOID)MmioRead32 (Address);
}

STATIC BOOLEAN
WaitMask (UINTN Address, UINT32 Mask, UINT32 Expected, UINTN TimeoutUs)
{
  while (TimeoutUs-- != 0) {
    if ((MmioRead32 (Address) & Mask) == Expected) {
      return TRUE;
    }
    MicroSecondDelay (1);
  }
  return FALSE;
}

STATIC UINT32
ApplyPhyRange (UINT32 Original, UINT32 Data)
{
  UINT32 Value;
  UINT32 Temp;
  UINT32 Dtmp;

  // Exact usb_phyx_write() bit packing from the sun60iw2 Linux BSP.
  Value  = Original;
  Value &= ~((0xFU << 8) | (0x3U << 4) | (0xFU << 0));
  Temp   = Data & ~((0xFU << 4) | (0x3U << 8));
  Value |= Temp << 8;
  Dtmp   = Data & ~((0xFU << 6) | (0xFU << 0));
  Value |= Dtmp;
  Data  &= ~((0x3U << 4) | (0xFU << 0));
  Value |= Data >> 6;
  return Value;
}

STATIC VOID
ProgramLocalUsbRegisters (UINTN Ehci)
{
  UINT32 Value;

  // Leave PHY1 out of SIDDQ, apply the board's phy_range=<0x188>, then
  // enable the exact sun60iw2 HCI pass-by bits used by the BSP.
  Value  = MmioRead32 (Ehci + EHCI_VENDOR_PHY_CTRL);
  Value &= ~PHY_CTRL_SIDDQ;
  MmioWrite32 (Ehci + EHCI_VENDOR_PHY_CTRL, Value);

  Value = ApplyPhyRange (MmioRead32 (Ehci + EHCI_VENDOR_PHY_TUNE), PHY_RANGE_VALUE);
  MmioWrite32 (Ehci + EHCI_VENDOR_PHY_TUNE, Value);

  Value  = MmioRead32 (Ehci + EHCI_VENDOR_PASSBY);
  Value |= BIT (15) | BIT (11) | BIT (9) | BIT (8) | BIT (0);
  // Normal operating clock: CCU reference, RC standby clock disabled.
  Value &= ~(BIT (31) | BIT (3) | BIT (2));
  MmioWrite32 (Ehci + EHCI_VENDOR_PASSBY, Value);
  ArmDataSynchronizationBarrier ();
  (VOID)MmioRead32 (Ehci + EHCI_VENDOR_PASSBY);
}

STATIC VOID
EnableSharedUsbClocks (VOID)
{
  UINT32 Value;

  SetBits (CCU_RES_DCAP_24M, BIT (3));
  SetBits (CCU_USB_REF_CFG, BIT (31));
  SetBits (CCU_MSI_LITE2, BIT (0));

  Value  = MmioRead32 (CCU_AHB_GATE_SW_CFG);
  Value |= AHB_MASTER_KEY_VALUE | BIT (9);
  MmioWrite32 (CCU_AHB_GATE_SW_CFG, Value);
  ArmDataSynchronizationBarrier ();
  (VOID)MmioRead32 (CCU_AHB_GATE_SW_CFG);
}

STATIC VOID
ResistanceCalibration (VOID)
{
  UINT32 Value;

  Value  = MmioRead32 (SYSCFG_RESCAL_CTRL);
  Value &= ~BIT (0);
  Value |= BIT (5);
  MmioWrite32 (SYSCFG_RESCAL_CTRL, Value);

  Value  = MmioRead32 (SYSCFG_RES0_CTRL);
  Value &= ~(0xFFU << 8);
  MmioWrite32 (SYSCFG_RES0_CTRL, Value);
  ArmDataSynchronizationBarrier ();
}

STATIC VOID
DumpEhci (CONST CHAR8 *Tag, UINTN CapBase)
{
  UINT32 Cap;
  UINTN  OpBase;

  Cap    = MmioRead32 (CapBase);
  OpBase = CapBase + (Cap & 0xFFU);
  DEBUG ((DEBUG_INFO,
    "A733 USB9 %a CAP=%08x HCSP=%08x HCCP=%08x OP=%08lx CMD=%08x STS=%08x FR=%08x CFG=%08x PORT0=%08x PHY=%08x HCI=%08x PASS=%08x PHYCTRL=%08x PHYTUNE=%08x AHB=%08x REF=%08x MSI=%08x RESCAL=%08x RES0=%08x\n",
    Tag, Cap, MmioRead32 (CapBase + EHCI_HCSPARAMS),
    MmioRead32 (CapBase + EHCI_HCCPARAMS), OpBase,
    MmioRead32 (OpBase + EHCI_USBCMD), MmioRead32 (OpBase + EHCI_USBSTS),
    MmioRead32 (OpBase + 0x0C), MmioRead32 (OpBase + EHCI_CONFIGFLAG),
    MmioRead32 (OpBase + EHCI_PORTSC0), MmioRead32 (CCU_USB1_PHY_CFG),
    MmioRead32 (CCU_USB1_HCI_CFG), MmioRead32 (CapBase + EHCI_VENDOR_PASSBY),
    MmioRead32 (CapBase + EHCI_VENDOR_PHY_CTRL),
    MmioRead32 (CapBase + EHCI_VENDOR_PHY_TUNE),
    MmioRead32 (CCU_AHB_GATE_SW_CFG), MmioRead32 (CCU_USB_REF_CFG),
    MmioRead32 (CCU_MSI_LITE2), MmioRead32 (SYSCFG_RESCAL_CTRL),
    MmioRead32 (SYSCFG_RES0_CTRL)));
}

STATIC EFI_STATUS EFIAPI
A733EhciInitialize (IN NON_DISCOVERABLE_DEVICE *This)
{
  UINTN  Ehci;
  UINTN  OpBase;
  UINT32 Cap;
  UINT32 Value;

  (VOID)This;
  Ehci = (UINTN)FixedPcdGet64 (PcdEhci1Base);

  // Full hardware reset. Then follow the actual Linux sun60iw2 open_clock()
  // ordering: HCI reset/clock first, PHY clock/reset second.
  ClrBits (CCU_USB1_HCI_CFG, BIT (20) | BIT (4));
  ClrBits (CCU_USB1_PHY_CFG, BIT (30) | BIT (31));
  MicroSecondDelay (2000);

  EnableSharedUsbClocks ();
  ResistanceCalibration ();

  SetBits (CCU_USB1_HCI_CFG, BIT (20));
  MicroSecondDelay (10);
  SetBits (CCU_USB1_HCI_CFG, BIT (4));
  MicroSecondDelay (10);

  SetBits (CCU_USB1_PHY_CFG, BIT (31));
  MicroSecondDelay (10);
  ProgramLocalUsbRegisters (Ehci);
  SetBits (CCU_USB1_PHY_CFG, BIT (30));
  MicroSecondDelay (1000);
  ProgramLocalUsbRegisters (Ehci);
  MicroSecondDelay (20000);

  Cap = MmioRead32 (Ehci);
  if ((Cap == 0) || (Cap == MAX_UINT32) || ((Cap & 0xFFU) < 0x10U) || ((Cap & 0xFFU) > 0x80U)) {
    DEBUG ((DEBUG_ERROR, "A733 USB9 invalid EHCI CAPBASE=%08x\n", Cap));
    return EFI_DEVICE_ERROR;
  }

  OpBase = Ehci + (Cap & 0xFFU);
  DumpEhci ("before-hc-reset", Ehci);

  Value  = MmioRead32 (OpBase + EHCI_USBCMD);
  Value &= ~(CMD_RUN | CMD_PSE | CMD_ASE);
  MmioWrite32 (OpBase + EHCI_USBCMD, Value);
  ArmDataSynchronizationBarrier ();
  if (!WaitMask (OpBase + EHCI_USBSTS, STS_HALT, STS_HALT, 250000)) {
    DEBUG ((DEBUG_WARN, "A733 USB9 inherited HC did not report HCHalted; continuing reset\n"));
  }

  MmioWrite32 (OpBase + EHCI_USBINTR, 0);
  MmioWrite32 (OpBase + EHCI_USBCMD, CMD_RESET);
  ArmDataSynchronizationBarrier ();
  if (!WaitMask (OpBase + EHCI_USBCMD, CMD_RESET, 0, 500000)) {
    DumpEhci ("hc-reset-timeout", Ehci);
    return EFI_TIMEOUT;
  }

  // HCReset may clear registers in the vendor extension window.
  ProgramLocalUsbRegisters (Ehci);
  MmioWrite32 (OpBase + EHCI_USBSTS, 0x3FU);
  ArmDataSynchronizationBarrier ();
  DumpEhci ("platform-ready", Ehci);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
A733UsbInitEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  (VOID)ImageHandle;
  (VOID)SystemTable;
  DEBUG ((DEBUG_INFO, "A733 USB9 registering EHCI1 as non-coherent DMA32 device\n"));
  return RegisterNonDiscoverableMmioDevice (
    NonDiscoverableDeviceTypeEhci,
    NonDiscoverableDeviceDmaTypeNonCoherent,
    A733EhciInitialize,
    NULL,
    1,
    (UINTN)FixedPcdGet64 (PcdEhci1Base),
    (UINTN)0x1000
    );
}
