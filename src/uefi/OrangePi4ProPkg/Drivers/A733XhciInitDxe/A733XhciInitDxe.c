#include "A733XhciPlatform.h"

#include <Guid/EventGroup.h>
#include <Protocol/A733XhciReady.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/SimplePointer.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#define BIT(N)                         (1U << (N))
#define DWC3_GSBUSCFG0                 0xC100U
#define DWC3_GCTL                      0xC110U
#define DWC3_GSTS                      0xC118U
#define DWC3_GSNPSID                   0xC120U
#define DWC3_LLUCTL                    0xD024U
#define DWC3_GUSB2PHYCFG0              0xC200U
#define DWC3_GUSB3PIPECTL0             0xC2C0U
#define DWC3_GCTL_CORESOFTRESET        BIT (11)
#define DWC3_GCTL_PRTCAPDIR_MASK       (BIT (12) | BIT (13))
#define DWC3_GCTL_PRTCAP_HOST          BIT (12)
#define DWC3_GUSB2PHYCFG_PHYSOFTRST    BIT (31)
#define DWC3_GUSB2PHYCFG_ENBLSLPM      BIT (8)
#define DWC3_GUSB2PHYCFG_SUSPHY        BIT (6)
#define DWC3_GUSB3PIPECTL_PHYSOFTRST   BIT (31)
#define DWC3_GUSB3PIPECTL_SUSPHY       BIT (17)
#define DWC3_LLUCTL_INV_SYNC_HDR       BIT (30)
#define DWC3_GCTL_SOFITPSYNC           BIT (10)
#define DWC3_GCTL_DSBLCLKGTNG          BIT (0)
#define SUNXI_APP                      0x10000U
#define SUNXI_PIPE_CLOCK_CONTROL       0x10014U
#define SUNXI_PHY_TUNE_LOW             0x10018U
#define SUNXI_PHY_TUNE_HIGH            0x1001CU
#define SUNXI_PHY_EXTERNAL_CONTROL     0x10020U
#define SUNXI_APP_FORCE_VBUS           (3U << 12)
#define SUNXI_PIPE_CLK_OPEN            BIT (6)
#define SUNXI_PEC_EXTERN_VBUS          (3U << 1)
#define SUNXI_PEC_SSC_EN               BIT (24)
#define SUNXI_PEC_REF_SSP_EN           BIT (26)
#define XHCI_USBSTS_CNR                BIT (11)
#define XHCI_USBSTS_HCH                BIT (0)
#define XHCI_USBCMD_RS                 BIT (0)
#define XHCI_USBCMD_INTE               BIT (2)

STATIC EFI_EVENT mReadyToBootEvent;
STATIC EFI_EVENT mExitBootServicesEvent;
STATIC EFI_HANDLE mReadyHandle;
STATIC EFI_HANDLE mXhciHandle;
STATIC EFI_USB2_HC_PROTOCOL *mUsb2Hc;
STATIC VOID DumpSuperSpeedPort (IN CONST CHAR8 *Stage);
STATIC A733_XHCI_READY_PROTOCOL mReadyProtocol = { 5, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE };

STATIC UINTN XhciOperational (VOID)
{
  return A733_XHCI_BASE + (MmioRead32 (A733_XHCI_BASE) & 0xFFU);
}

STATIC BOOLEAN CapabilityValid (VOID)
{
  UINT32 Cap = MmioRead32 (A733_XHCI_BASE);
  UINT32 Len = Cap & 0xFFU;
  UINT32 Ver = (Cap >> 16) & 0xFFFFU;
  return Len >= 0x20U && Len <= 0x80U && Ver >= 0x0090U && Ver <= 0x0200U;
}

STATIC EFI_STATUS WaitCnrClear (UINTN TimeoutUs)
{
  UINTN Op = XhciOperational ();
  UINTN I;
  for (I = 0; I < TimeoutUs; I++) {
    if ((MmioRead32 (Op + 4U) & XHCI_USBSTS_CNR) == 0U) return EFI_SUCCESS;
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS WaitHalted (UINTN TimeoutUs)
{
  UINTN Op = XhciOperational ();
  UINTN I;
  for (I = 0; I < TimeoutUs; I++) {
    if ((MmioRead32 (Op + 4U) & XHCI_USBSTS_HCH) != 0U) return EFI_SUCCESS;
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

STATIC VOID
ProgramSunxiDwc3Wrapper (
  VOID
  )
{
  UINT32 Value;

  // Exact sunxi_core_open_phy() wrapper setup from Allwinner's A733 BSP.
  Value = MmioRead32 (A733_XHCI_BASE + SUNXI_PHY_EXTERNAL_CONTROL);
  Value |= SUNXI_PEC_EXTERN_VBUS | SUNXI_PEC_SSC_EN | SUNXI_PEC_REF_SSP_EN;
  MmioWrite32 (A733_XHCI_BASE + SUNXI_PHY_EXTERNAL_CONTROL, Value);

  Value = MmioRead32 (A733_XHCI_BASE + SUNXI_PIPE_CLOCK_CONTROL);
  Value |= SUNXI_PIPE_CLK_OPEN;
  MmioWrite32 (A733_XHCI_BASE + SUNXI_PIPE_CLOCK_CONTROL, Value);

  Value = MmioRead32 (A733_XHCI_BASE + SUNXI_APP);
  Value |= SUNXI_APP_FORCE_VBUS;
  MmioWrite32 (A733_XHCI_BASE + SUNXI_APP, Value);

  MmioWrite32 (A733_XHCI_BASE + SUNXI_PHY_TUNE_LOW, 0x0047FC57U);

  Value = MmioRead32 (A733_XHCI_BASE + SUNXI_PHY_TUNE_HIGH);
  Value |= (0x7U << 0) | (0x7U << 3);
  Value &= ~(0x7FU << 6);
  Value |= (0x55U << 6);
  Value &= ~(0x3FU << 13);
  Value |= (0x20U << 13);
  Value &= ~(0x3FU << 19);
  Value |= (0x15U << 19);
  MmioWrite32 (A733_XHCI_BASE + SUNXI_PHY_TUNE_HIGH, Value);

  // v60: GCTL as the live Linux dwc3 (0x30C11005): clock gating disabled,
  // SOFITPSYNC clear. SOFITPSYNC came from the legacy xhci_sunxi.c
  // sunxi_core_open_phy(), which the A733 Linux (dwc3-sunxi-plat) never runs.
  // PcdA733UsbBridgeClockMode 0 = exactly the v59 xHCI setup (SOFITPSYNC set).
  Value = MmioRead32 (A733_XHCI_BASE + DWC3_GCTL);
  if (FixedPcdGet32 (PcdA733UsbBridgeClockMode) == 0U) {
    Value |= DWC3_GCTL_SOFITPSYNC;
  } else {
    Value |= DWC3_GCTL_DSBLCLKGTNG;
    Value &= ~DWC3_GCTL_SOFITPSYNC;
  }
  MmioWrite32 (A733_XHCI_BASE + DWC3_GCTL, Value);

  DEBUG ((DEBUG_INFO,
          "A733 XHCI55 wrapper: APP=%08x PIPE=%08x TUNE=%08x/%08x PEC=%08x GCTL=%08x\n",
          MmioRead32 (A733_XHCI_BASE + SUNXI_APP),
          MmioRead32 (A733_XHCI_BASE + SUNXI_PIPE_CLOCK_CONTROL),
          MmioRead32 (A733_XHCI_BASE + SUNXI_PHY_TUNE_LOW),
          MmioRead32 (A733_XHCI_BASE + SUNXI_PHY_TUNE_HIGH),
          MmioRead32 (A733_XHCI_BASE + SUNXI_PHY_EXTERNAL_CONTROL),
          MmioRead32 (A733_XHCI_BASE + DWC3_GCTL)));
}



STATIC VOID
DumpSupportedProtocols (
  VOID
  )
{
  UINT32 Hcc;
  UINT32 Offset;
  UINT32 Guard;

  Hcc = MmioRead32 (A733_XHCI_BASE + 0x10U);
  Offset = ((Hcc >> 16) & 0xFFFFU) * 4U;
  for (Guard = 0; (Offset != 0U) && (Guard < 64U); Guard++) {
    UINT32 Cap0;
    UINT32 Cap1;
    UINT32 Cap2;
    UINT32 Cap3;
    UINT32 Next;
    UINT32 Id;

    if (Offset >= A733_XHCI_SIZE - 16U) {
      DEBUG ((DEBUG_ERROR, "A733 XHCI49 XECP invalid offset=%x\n", Offset));
      break;
    }

    Cap0 = MmioRead32 (A733_XHCI_BASE + Offset);
    Cap1 = MmioRead32 (A733_XHCI_BASE + Offset + 4U);
    Cap2 = MmioRead32 (A733_XHCI_BASE + Offset + 8U);
    Cap3 = MmioRead32 (A733_XHCI_BASE + Offset + 12U);
    Id   = Cap0 & 0xFFU;
    Next = (Cap0 >> 8) & 0xFFU;

    if (Id == 2U) {
      DEBUG ((DEBUG_INFO,
              "A733 XHCI49 PROTOCOL: off=%x rev=%u.%u name=%c%c%c%c ports=%u+%u slot=%u psi=%u raw=%08x/%08x/%08x/%08x\n",
              Offset, (Cap0 >> 24) & 0xFFU, (Cap0 >> 16) & 0xFFU,
              (CHAR8)(Cap1 & 0xFFU), (CHAR8)((Cap1 >> 8) & 0xFFU),
              (CHAR8)((Cap1 >> 16) & 0xFFU), (CHAR8)((Cap1 >> 24) & 0xFFU),
              Cap2 & 0xFFU, (Cap2 >> 8) & 0xFFU,
              Cap3 & 0x1FU, (Cap2 >> 28) & 0xFU,
              Cap0, Cap1, Cap2, Cap3));
    } else {
      DEBUG ((DEBUG_INFO,
              "A733 XHCI49 XECP: off=%x id=%u next=%u raw=%08x\n",
              Offset, Id, Next, Cap0));
    }

    if (Next == 0U) {
      break;
    }
    Offset += Next * 4U;
  }
}

STATIC EFI_STATUS EFIAPI
A733XhciNonDiscoverableInitialize (
  IN NON_DISCOVERABLE_DEVICE *This
  )
{
  EFI_STATUS Status;

  (VOID)This;
  if (!CapabilityValid () || !A733XhciPlatformPmaReady ()) {
    DEBUG ((DEBUG_ERROR,
            "A733 XHCI49 UEFI init rejected: cap=%u pma=%u\n",
            CapabilityValid () ? 1U : 0U,
            A733XhciPlatformPmaReady () ? 1U : 0U));
    return EFI_DEVICE_ERROR;
  }

  Status = WaitCnrClear (250000U);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
          "A733 XHCI49 UEFI non-discoverable initialize: CNR=%r\n",
          Status));
  return Status;
}

STATIC EFI_STATUS
ConnectFirmwareXhci (
  OUT BOOLEAN *PortConnected
  )
{
  EFI_STATUS          RegisterStatus;
  EFI_STATUS          ConnectStatus;
  EFI_STATUS          ProtocolStatus;
  EFI_STATUS          CapabilityStatus;
  EFI_STATUS          PointerStatus;
  UINT8               MaxSpeed;
  UINT8               PortCount;
  UINT8               Is64Bit;
  UINT8               Port;
  UINTN               Pass;
  EFI_SIMPLE_POINTER_PROTOCOL *Pointer;

  *PortConnected = FALSE;
  mXhciHandle = NULL;
  mUsb2Hc = NULL;

  RegisterStatus = RegisterNonDiscoverableMmioDevice (
                     NonDiscoverableDeviceTypeXhci,
                     NonDiscoverableDeviceDmaTypeNonCoherent,
                     A733XhciNonDiscoverableInitialize,
                     &mXhciHandle,
                     1,
                     (UINTN)A733_XHCI_BASE,
                     (UINTN)A733_XHCI_SIZE
                     );
  if (EFI_ERROR (RegisterStatus)) {
    DEBUG ((DEBUG_ERROR,
            "A733 XHCI49 UEFI registration failed: %r\n",
            RegisterStatus));
    return RegisterStatus;
  }

  ConnectStatus = gBS->ConnectController (mXhciHandle, NULL, NULL, TRUE);
  if ((ConnectStatus == EFI_ALREADY_STARTED) || (ConnectStatus == EFI_SUCCESS)) {
    ConnectStatus = EFI_SUCCESS;
  }

  ProtocolStatus = gBS->HandleProtocol (
                          mXhciHandle,
                          &gEfiUsb2HcProtocolGuid,
                          (VOID **)&mUsb2Hc
                          );
  DEBUG (((EFI_ERROR (ConnectStatus) || EFI_ERROR (ProtocolStatus)) ? DEBUG_ERROR : DEBUG_INFO,
          "A733 XHCI49 UEFI bind: register=%r connect=%r usb2hc=%r handle=%p protocol=%p\n",
          RegisterStatus, ConnectStatus, ProtocolStatus, mXhciHandle, mUsb2Hc));
  // Recursive ConnectController may report a child-connect error even after
  // the xHCI driver itself has successfully installed EFI_USB2_HC_PROTOCOL.
  // The protocol on this exact controller handle is the authoritative proof.
  if (EFI_ERROR (ProtocolStatus) || (mUsb2Hc == NULL)) {
    return EFI_ERROR (ConnectStatus) ? ConnectStatus : EFI_NOT_FOUND;
  }

  MaxSpeed = 0;
  PortCount = 0;
  Is64Bit = 0;
  CapabilityStatus = mUsb2Hc->GetCapability (
                                  mUsb2Hc,
                                  &MaxSpeed,
                                  &PortCount,
                                  &Is64Bit
                                  );
  DEBUG ((EFI_ERROR (CapabilityStatus) ? DEBUG_ERROR : DEBUG_INFO,
          "A733 XHCI49 UEFI capability: status=%r speed=%u ports=%u dma64=%u revision=%u.%u\n",
          CapabilityStatus, MaxSpeed, PortCount, Is64Bit,
          mUsb2Hc->MajorRevision, mUsb2Hc->MinorRevision));
  if (EFI_ERROR (CapabilityStatus)) {
    return CapabilityStatus;
  }

  // Give the root hub and USB bus driver enough time for debounce/enumeration.
  for (Pass = 0; Pass < 20U; Pass++) {
    for (Port = 0; Port < PortCount; Port++) {
      EFI_USB_PORT_STATUS PortStatus;
      EFI_STATUS          Status;

      PortStatus.PortStatus = 0;
      PortStatus.PortChangeStatus = 0;
      Status = mUsb2Hc->GetRootHubPortStatus (mUsb2Hc, Port, &PortStatus);
      if ((Pass == 0U) || (Pass == 19U) ||
          (!EFI_ERROR (Status) && ((PortStatus.PortStatus & USB_PORT_STAT_CONNECTION) != 0U))) {
        DEBUG ((EFI_ERROR (Status) ? DEBUG_WARN : DEBUG_INFO,
                "A733 XHCI49 UEFI root-port[%u] pass=%u status=%r current=%04x change=%04x\n",
                Port, (UINT32)Pass, Status,
                PortStatus.PortStatus, PortStatus.PortChangeStatus));
      }
      if (!EFI_ERROR (Status) &&
          ((PortStatus.PortStatus & USB_PORT_STAT_CONNECTION) != 0U)) {
        *PortConnected = TRUE;
      }
    }
    if (*PortConnected) {
      break;
    }
    MicroSecondDelay (100000U);
  }

  DumpSuperSpeedPort ("after-root-port-poll");

  Pointer = NULL;
  PointerStatus = gBS->LocateProtocol (
                          &gEfiSimplePointerProtocolGuid,
                          NULL,
                          (VOID **)&Pointer
                          );
  DEBUG ((EFI_ERROR (PointerStatus) ? DEBUG_WARN : DEBUG_INFO,
          "A733 XHCI49 UEFI HID pointer: status=%r protocol=%p root-connected=%u\n",
          PointerStatus, Pointer, *PortConnected ? 1U : 0U));

  return EFI_SUCCESS;
}

/*
 * v60: decode the SuperSpeed root port (xHCI port 2, protocol USB 3.1) so a
 * UART log taken with a USB 3 device in the blue port tells where the link
 * stops: RxDetect = no receiver seen on the SS lanes, Polling / Inactive /
 * Compliance = training fails, U0 + CCS = SuperSpeed link up.
 */
STATIC VOID
DumpSuperSpeedPort (
  IN CONST CHAR8 *Stage
  )
{
  STATIC CONST CHAR8 *CONST Pls[16] = {
    "U0", "U1", "U2", "U3", "Disabled", "RxDetect", "Inactive", "Polling",
    "Recovery", "HotReset", "Compliance", "TestMode", "12", "13", "14", "Resume"
  };
  UINTN  Op = XhciOperational ();
  UINT32 P1 = MmioRead32 (Op + 0x400U);
  UINT32 P2 = MmioRead32 (Op + 0x410U);
  UINT32 Li = MmioRead32 (Op + 0x418U);

  DEBUG ((DEBUG_INFO,
    "A733 XHCI60 %a: USB3 port PORTSC=%08x CCS=%u PED=%u PLS=%u(%a) speed=%u PLC=%u CEC=%u WRC=%u link-errors=%u | USB2 port PORTSC=%08x CCS=%u speed=%u\n",
    Stage, P2, P2 & 1U, (P2 >> 1) & 1U, (P2 >> 5) & 0xFU, Pls[(P2 >> 5) & 0xFU],
    (P2 >> 10) & 0xFU, (P2 >> 22) & 1U, (P2 >> 23) & 1U, (P2 >> 19) & 1U, Li & 0xFFFFU,
    P1, P1 & 1U, (P1 >> 10) & 0xFU));
}

STATIC VOID DumpController (IN CONST CHAR8 *Stage)
{
  UINTN Op = XhciOperational ();
  DEBUG ((DEBUG_INFO,
    "A733 XHCI49 %a: CAP=%08x HCSP1=%08x HCC1=%08x GSNPSID=%08x GCTL=%08x GSTS=%08x U2=%08x U3=%08x LLU=%08x CMD=%08x STS=%08x PAGE=%08x CFG=%08x P1=%08x P2=%08x PMA=%u\n",
    Stage, MmioRead32 (A733_XHCI_BASE), MmioRead32 (A733_XHCI_BASE + 4U),
    MmioRead32 (A733_XHCI_BASE + 0x10U), MmioRead32 (A733_XHCI_BASE + DWC3_GSNPSID),
    MmioRead32 (A733_XHCI_BASE + DWC3_GCTL), MmioRead32 (A733_XHCI_BASE + DWC3_GSTS),
    MmioRead32 (A733_XHCI_BASE + DWC3_GUSB2PHYCFG0), MmioRead32 (A733_XHCI_BASE + DWC3_GUSB3PIPECTL0),
    MmioRead32 (A733_XHCI_BASE + DWC3_LLUCTL), MmioRead32 (Op), MmioRead32 (Op + 4U),
    MmioRead32 (Op + 8U), MmioRead32 (Op + 0x38U), MmioRead32 (Op + 0x400U),
    MmioRead32 (Op + 0x410U), A733XhciPlatformPmaReady () ? 1U : 0U));
}

STATIC EFI_STATUS
InitializeDwc3HostOnce (
  IN UINT32 Attempt
  )
{
  UINT32 V;
  UINT32 CapBefore;
  EFI_STATUS Status;

  /*
   * The xHCI capability window is produced by the DWC3 host core.  On A733 it
   * can legitimately read CAPLENGTH=0 while the DWC3 core is still held in
   * reset.  Linux initializes/resets DWC3 first and creates the xHCI child
   * only afterwards.  Do the same here; validating before this sequence made
   * v51 return early and guaranteed that the controller could never recover.
   */
  CapBefore = MmioRead32 (A733_XHCI_BASE);
  DEBUG ((DEBUG_INFO,
          "A733 XHCI52: DWC3 attempt %u core-first CAP-before=%08x\n",
          Attempt,
          CapBefore));

  // Hold DWC3 core and both PHY interfaces in soft reset.
  V = MmioRead32 (A733_XHCI_BASE + DWC3_GCTL) | DWC3_GCTL_CORESOFTRESET;
  MmioWrite32 (A733_XHCI_BASE + DWC3_GCTL, V);
  MmioWrite32 (
    A733_XHCI_BASE + DWC3_GUSB2PHYCFG0,
    MmioRead32 (A733_XHCI_BASE + DWC3_GUSB2PHYCFG0) |
    DWC3_GUSB2PHYCFG_PHYSOFTRST
    );
  MmioWrite32 (
    A733_XHCI_BASE + DWC3_GUSB3PIPECTL0,
    MmioRead32 (A733_XHCI_BASE + DWC3_GUSB3PIPECTL0) |
    DWC3_GUSB3PIPECTL_PHYSOFTRST
    );
  MicroSecondDelay (1000);

  // Apply firmware-applicable DTS quirks before releasing reset.
  V = MmioRead32 (A733_XHCI_BASE + DWC3_GUSB2PHYCFG0);
  V &= ~(DWC3_GUSB2PHYCFG_PHYSOFTRST |
         DWC3_GUSB2PHYCFG_ENBLSLPM |
         DWC3_GUSB2PHYCFG_SUSPHY);
  MmioWrite32 (A733_XHCI_BASE + DWC3_GUSB2PHYCFG0, V);

  V = MmioRead32 (A733_XHCI_BASE + DWC3_GUSB3PIPECTL0);
  V &= ~(DWC3_GUSB3PIPECTL_PHYSOFTRST |
         DWC3_GUSB3PIPECTL_SUSPHY);
  MmioWrite32 (A733_XHCI_BASE + DWC3_GUSB3PIPECTL0, V);

  MmioWrite32 (
    A733_XHCI_BASE + DWC3_LLUCTL,
    MmioRead32 (A733_XHCI_BASE + DWC3_LLUCTL) |
    DWC3_LLUCTL_INV_SYNC_HDR
    );

  MmioWrite32 (
    A733_XHCI_BASE + DWC3_GSBUSCFG0,
    MmioRead32 (A733_XHCI_BASE + DWC3_GSBUSCFG0) | 0xFFU
    );

  V = MmioRead32 (A733_XHCI_BASE + DWC3_GCTL);
  V &= ~DWC3_GCTL_PRTCAPDIR_MASK;
  V |= DWC3_GCTL_PRTCAP_HOST;
  V &= ~DWC3_GCTL_CORESOFTRESET;
  MmioWrite32 (A733_XHCI_BASE + DWC3_GCTL, V);
  ProgramSunxiDwc3Wrapper ();
  MicroSecondDelay (1000);

  if (!CapabilityValid ()) {
    DEBUG ((DEBUG_WARN,
            "A733 XHCI52: capability window still invalid after DWC3 release CAP=%08x\n",
            MmioRead32 (A733_XHCI_BASE)));
    DumpController ("post-core-invalid-cap");
    return EFI_DEVICE_ERROR;
  }

  Status = WaitCnrClear (250000U);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_WARN : DEBUG_INFO,
          "A733 XHCI49: DWC3 attempt %u CNR=%r\n",
          Attempt,
          Status));
  DumpController (EFI_ERROR (Status) ? "CNR-timeout" : "initialized");
  return Status;
}

STATIC EFI_STATUS
ReleaseDwc3AfterExternalReset (
  VOID
  )
{
  UINT32     V;
  EFI_STATUS Status;

  /*
   * Do not assert the internal DWC3 resets again here. The external A733
   * RST_USB_2 pulse is the step that cleared CNR in every successful v34/v35
   * capture; reasserting CORESOFTRESET immediately afterwards would recreate
   * the state we are recovering from.
   */
  V = MmioRead32 (A733_XHCI_BASE + DWC3_GUSB2PHYCFG0);
  V &= ~(DWC3_GUSB2PHYCFG_PHYSOFTRST |
         DWC3_GUSB2PHYCFG_ENBLSLPM |
         DWC3_GUSB2PHYCFG_SUSPHY);
  MmioWrite32 (A733_XHCI_BASE + DWC3_GUSB2PHYCFG0, V);

  V = MmioRead32 (A733_XHCI_BASE + DWC3_GUSB3PIPECTL0);
  V &= ~(DWC3_GUSB3PIPECTL_PHYSOFTRST |
         DWC3_GUSB3PIPECTL_SUSPHY);
  MmioWrite32 (A733_XHCI_BASE + DWC3_GUSB3PIPECTL0, V);

  MmioWrite32 (
    A733_XHCI_BASE + DWC3_LLUCTL,
    MmioRead32 (A733_XHCI_BASE + DWC3_LLUCTL) |
    DWC3_LLUCTL_INV_SYNC_HDR
    );
  MmioWrite32 (
    A733_XHCI_BASE + DWC3_GSBUSCFG0,
    MmioRead32 (A733_XHCI_BASE + DWC3_GSBUSCFG0) | 0xFFU
    );

  V = MmioRead32 (A733_XHCI_BASE + DWC3_GCTL);
  V &= ~DWC3_GCTL_PRTCAPDIR_MASK;
  V |= DWC3_GCTL_PRTCAP_HOST;
  V &= ~DWC3_GCTL_CORESOFTRESET;
  MmioWrite32 (A733_XHCI_BASE + DWC3_GCTL, V);
  ProgramSunxiDwc3Wrapper ();
  MicroSecondDelay (1000);

  if (!CapabilityValid ()) {
    DEBUG ((DEBUG_ERROR,
            "A733 XHCI52: capability window invalid after external/core reset CAP=%08x\n",
            MmioRead32 (A733_XHCI_BASE)));
    DumpController ("external-reset-invalid-cap");
    return EFI_DEVICE_ERROR;
  }

  Status = WaitCnrClear (250000U);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
          "A733 XHCI49: external-reset CNR=%r\n",
          Status));
  DumpController (EFI_ERROR (Status) ? "external-reset-timeout" :
                                      "external-reset-recovered");
  return Status;
}

STATIC EFI_STATUS
InitializeDwc3Host (
  VOID
  )
{
  EFI_STATUS Status;

  Status = InitializeDwc3HostOnce (1);
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_WARN,
          "A733 XHCI52: first DWC3 core-first cycle failed; applying external RST_USB_2 recovery\n"));

  Status = A733XhciPlatformRecoverController ();
  A733XhciPlatformDump ("external-reset-recovery");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return ReleaseDwc3AfterExternalReset ();
}

STATIC VOID
ObserveWindowsHandoff (
  VOID
  )
{
  // XhciDxe owns the host controller after registration and performs its
  // standard ExitBootServices quiesce at TPL_NOTIFY. This lower-priority
  // callback only records the resulting state; it never resets the DWC3 core
  // or destroys the platform/PHY configuration that Windows will inherit.
  DumpController ("ExitBootServices-observed");
  A733XhciPlatformDump ("ExitBootServices");
}

STATIC VOID EFIAPI ReadyToBootCallback (IN EFI_EVENT Event, IN VOID *Context)
{
  UINT8 PortCount;
  UINT8 MaxSpeed;
  UINT8 Is64Bit;
  UINT8 Port;

  (VOID)Event;
  (VOID)Context;
  DumpController ("ReadyToBoot");
  DumpSuperSpeedPort ("ReadyToBoot");
  A733XhciPlatformDump ("ReadyToBoot");

  if ((mUsb2Hc != NULL) &&
      !EFI_ERROR (mUsb2Hc->GetCapability (mUsb2Hc, &MaxSpeed, &PortCount, &Is64Bit))) {
    for (Port = 0; Port < PortCount; Port++) {
      EFI_USB_PORT_STATUS PortStatus;
      EFI_STATUS Status;

      PortStatus.PortStatus = 0;
      PortStatus.PortChangeStatus = 0;
      Status = mUsb2Hc->GetRootHubPortStatus (mUsb2Hc, Port, &PortStatus);
      DEBUG ((EFI_ERROR (Status) ? DEBUG_WARN : DEBUG_INFO,
              "A733 XHCI49 ReadyToBoot root-port[%u]: status=%r current=%04x change=%04x\n",
              Port, Status, PortStatus.PortStatus, PortStatus.PortChangeStatus));
    }
  }
}

STATIC VOID EFIAPI ExitBootServicesCallback (IN EFI_EVENT Event, IN VOID *Context)
{
  ObserveWindowsHandoff ();
}

EFI_STATUS EFIAPI A733XhciInitEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS PlatformStatus;
  EFI_STATUS CoreStatus;
  EFI_STATUS HaltStatus;
  EFI_STATUS FirmwareStatus;
  EFI_STATUS Status;
  BOOLEAN    FirmwarePortConnected;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  PlatformStatus = A733XhciPlatformInit ();
  A733XhciPlatformDump ("platform-init");
  if (!EFI_ERROR (PlatformStatus)) {
    DumpSupportedProtocols ();
  }

  CoreStatus = EFI_NOT_READY;
  if (!EFI_ERROR (PlatformStatus)) {
    CoreStatus = InitializeDwc3Host ();
  }

  HaltStatus = EFI_NOT_READY;
  if (!EFI_ERROR (CoreStatus)) {
    HaltStatus = WaitHalted (100000U);
  }

  FirmwareStatus = EFI_NOT_READY;
  FirmwarePortConnected = FALSE;
  if (!EFI_ERROR (PlatformStatus) &&
      !EFI_ERROR (CoreStatus) &&
      !EFI_ERROR (HaltStatus) &&
      A733XhciPlatformPmaReady ()) {
    FirmwareStatus = ConnectFirmwareXhci (&FirmwarePortConnected);
  }

  mReadyProtocol.PmaReady = A733XhciPlatformPmaReady ();
  mReadyProtocol.CnrClear = !EFI_ERROR (CoreStatus);
  mReadyProtocol.Halted = !EFI_ERROR (HaltStatus);
  mReadyProtocol.PortConnected = FirmwarePortConnected;
  mReadyProtocol.UefiDriverReady = !EFI_ERROR (FirmwareStatus);
  mReadyProtocol.PlatformReady = !EFI_ERROR (PlatformStatus) &&
                                 mReadyProtocol.PmaReady &&
                                 mReadyProtocol.CnrClear &&
                                 mReadyProtocol.Halted &&
                                 !EFI_ERROR (FirmwareStatus);
  DEBUG ((mReadyProtocol.PlatformReady ? DEBUG_INFO : DEBUG_ERROR,
          "A733 XHCI49 READY: platform=%r pma=%u cnr=%u prehalted=%u uefi=%r port=%u expose=%u\n",
          PlatformStatus, mReadyProtocol.PmaReady ? 1U : 0U,
          mReadyProtocol.CnrClear ? 1U : 0U,
          mReadyProtocol.Halted ? 1U : 0U,
          FirmwareStatus,
          mReadyProtocol.PortConnected ? 1U : 0U,
          mReadyProtocol.PlatformReady ? 1U : 0U));

  mReadyHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (&mReadyHandle,
                                                   &gA733XhciReadyProtocolGuid,
                                                   &mReadyProtocol,
                                                   NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // ACPI waits on this protocol and hides XHC2 if either the hardware bring-up
  // or the real EDK2 xHCI driver failed. That prevents Windows from receiving
  // an untested DMA master while still making the failure visible in UART.
  if (!mReadyProtocol.PlatformReady) {
    return EFI_SUCCESS;
  }

  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ReadyToBootCallback, NULL,
                               &gEfiEventReadyToBootGuid, &mReadyToBootEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ExitBootServicesCallback, NULL,
                             &gEfiEventExitBootServicesGuid, &mExitBootServicesEvent);
}
