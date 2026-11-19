/** @file
  Second console UART for Windows on the 40-pin header: UART7 = R_UART0
  (0x07080000), PL6 = TX (header pin 8), PL7 = RX (header pin 10), R-PIO
  function 3 (Orange Pi 4 Pro manual 5.12.2; vendor pinctrl-sun60iw2-r.c,
  ccu-sun60iw2-r.c).

  With PcdA733SpcrUart = 7 the ACPI SPCR points Windows' headless console
  (EMS / SAC, a command prompt over serial) at this UART, while DBG2 and the
  kernel debugger stay on the debug UART0: KD and cmd at the same time.

  This driver only clocks the UART, muxes PL6/PL7, programs 115200 8N1 and
  prints a banner (also at ReadyToBoot) so the wiring can be checked.
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define A733_R_CCU_BASE      0x07010000UL
#define A733_R_PIO_BASE      0x07025000UL
#define RCCU_APBS1           0x0010          /* r-apbs1: mux 26:24, M 4:0 */
#define RCCU_UART_BGR        0x018C          /* bit 0 R_UART0 gate, bit 16 reset */
#define PL_CFG0              0x00            /* R-PIO HW type 1: bank L at 0 */
#define PL_PULL0             0x24
#define UART7_BASE           0x07080000UL

#define UART_THR             0x00
#define UART_DLL             0x00
#define UART_DLH             0x04
#define UART_FCR             0x08
#define UART_LCR             0x0C
#define UART_MCR             0x10
#define UART_LSR             0x14
#define LSR_THRE             BIT5

STATIC EFI_EVENT  mReadyToBoot;

/* r-apbs1 parents (ccu-sun60iw2-r.c): dcxo, rtc32k, iosc, pll-peri0-200m, pll-ref */
STATIC
UINT32
RApbs1Hz (
  IN UINT32  Reg
  )
{
  STATIC CONST UINT32  Parent[8] = { 24000000, 32768, 16000000, 200000000, 24000000, 24000000, 24000000, 24000000 };

  return Parent[(Reg >> 24) & 7] / ((Reg & 0x1F) + 1);
}

STATIC
VOID
UartPuts (
  IN CONST CHAR8  *S
  )
{
  UINTN  Guard;

  for ( ; *S != '\0'; S++) {
    if (*S == '\n') {
      for (Guard = 0; Guard < 100000 && !(MmioRead32 (UART7_BASE + UART_LSR) & LSR_THRE); Guard++) {
      }
      MmioWrite32 (UART7_BASE + UART_THR, '\r');
    }
    for (Guard = 0; Guard < 100000 && !(MmioRead32 (UART7_BASE + UART_LSR) & LSR_THRE); Guard++) {
    }
    MmioWrite32 (UART7_BASE + UART_THR, (UINT8)*S);
  }
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UartPuts ("\nA733 UART7: UEFI ReadyToBoot - the Windows headless console (SAC) follows on this port\n");
}

EFI_STATUS
EFIAPI
A733SerialBDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  InHz = RApbs1Hz (MmioRead32 (A733_R_CCU_BASE + RCCU_APBS1));
  UINT32  Baud = FixedPcdGet32 (PcdA733SerialBBaudRate);
  UINT32  Div = (InHz + 8U * Baud) / (16U * Baud);
  UINT32  V;

  MmioOr32 (A733_R_CCU_BASE + RCCU_UART_BGR, BIT0 | BIT16);
  MicroSecondDelay (10);

  V = MmioRead32 (A733_R_PIO_BASE + PL_CFG0);
  V = (V & ~(0xFFU << 24)) | (0x3U << 24) | (0x3U << 28);
  MmioWrite32 (A733_R_PIO_BASE + PL_CFG0, V);
  V = MmioRead32 (A733_R_PIO_BASE + PL_PULL0);
  V = (V & ~(0xFU << 12)) | (0x1U << 14);          /* pull-up on PL7 (RX) */
  MmioWrite32 (A733_R_PIO_BASE + PL_PULL0, V);

  if (Div == 0) {
    Div = 1;
  }
  MmioWrite32 (UART7_BASE + UART_LCR, 0x83);       /* DLAB, 8N1 */
  MmioWrite32 (UART7_BASE + UART_DLL, Div & 0xFF);
  MmioWrite32 (UART7_BASE + UART_DLH, (Div >> 8) & 0xFF);
  MmioWrite32 (UART7_BASE + UART_LCR, 0x03);
  MmioWrite32 (UART7_BASE + UART_FCR, 0x07);
  MmioWrite32 (UART7_BASE + UART_MCR, 0x03);

  DEBUG ((DEBUG_INFO, "A733 UART7: r-apbs1 %u Hz, divisor %u (%u baud), R_CCU 18c=%08x PL_CFG0=%08x PL_PULL0=%08x\n",
    InHz, Div, InHz / (16U * Div), MmioRead32 (A733_R_CCU_BASE + RCCU_UART_BGR),
    MmioRead32 (A733_R_PIO_BASE + PL_CFG0), MmioRead32 (A733_R_PIO_BASE + PL_PULL0)));
  UartPuts ("\nA733 UART7 alive: header pin 8 (PL6) = TX, pin 10 (PL7) = RX, 115200 8N1\n");

  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL,
         &gEfiEventReadyToBootGuid, &mReadyToBoot);
  return EFI_SUCCESS;
}
