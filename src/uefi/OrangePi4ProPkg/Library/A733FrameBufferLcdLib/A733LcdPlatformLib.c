/** @file
  GOP mode for the framebuffer that vendor U-Boot leaves on HDMI.

  v60: vendor U-Boot's HDMI driver (drm_edid.c, sunxi-hdmi) reads the
  monitor's EDID and programs its mode; EDK2 used to assume 1024x600. The
  active size is now read back from TCON_TV3 (0x05730000, the TCON in front
  of hdmi0): TCON1_BASIC0 (0x94) = (width - 1) << 16 | (height - 1), with
  the TCON (GCTL bit 31) and TCON1 (CTL 0x90 bit 31) enabled. Accepted only
  for 640x480 .. 1920x1200 that fits PcdFrameBufferSize; anything else (TCON
  off, interlaced/odd sizes, too large) falls back to the build-time mode
  PcdFrameBufferWidth x PcdFrameBufferHeight (1024x600). 32 bpp, stride =
  width, framebuffer at PcdFrameBufferBase (U-Boot's allocation).
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/LcdPlatformLib.h>
#include <Library/PcdLib.h>

#define TCON_TV3_BASE     0x05730000UL
#define TCON_GCTL         0x000
#define TCON1_CTL         0x090
#define TCON1_BASIC0      0x094
#define TCON1_BASIC3      0x0a0          /* HT - 1 << 16 | HBP - 1 */
#define TCON1_BASIC4      0x0a4          /* VT * 2 << 16 | VBP - 1 */
#define TCON1_BASIC5      0x0a8          /* HSPW - 1 << 16 | VSPW - 1 */

STATIC SCAN_TIMINGS  mH = { 1024, 104, 144, 40 };
STATIC SCAN_TIMINGS  mV = { 600, 10, 11, 3 };
STATIC UINT32        mWidth, mHeight;
STATIC BOOLEAN       mProbed;

STATIC
VOID
ProbeMode (
  VOID
  )
{
  UINT32  Gctl, Ctl, B0, B3, B4, B5, W, H;

  if (mProbed) {
    return;
  }
  mProbed = TRUE;
  mWidth = FixedPcdGet32 (PcdFrameBufferWidth);
  mHeight = FixedPcdGet32 (PcdFrameBufferHeight);

  Gctl = MmioRead32 (TCON_TV3_BASE + TCON_GCTL);
  Ctl = MmioRead32 (TCON_TV3_BASE + TCON1_CTL);
  B0 = MmioRead32 (TCON_TV3_BASE + TCON1_BASIC0);
  B3 = MmioRead32 (TCON_TV3_BASE + TCON1_BASIC3);
  B4 = MmioRead32 (TCON_TV3_BASE + TCON1_BASIC4);
  B5 = MmioRead32 (TCON_TV3_BASE + TCON1_BASIC5);
  /*
    Board dump 2026-11-15 (Windows KD, U-Boot 1024x600 mode): 0x94 = 0,
    0x98 = 000004f6 (not a size: VT * 2), 0x9C = 03ff0257 = 1024x600,
    0xA0 = HT-1 1343 / HBP-1, 0xA4 = VT*2 1270 / VBP-1. So the active size is
    in 0x9C on this TCON_TV. Take the first of 0x94 / 0x9C / 0x98 that fits
    inside the programmed totals (W <= HT, H <= VT), never a bare non-zero word.
  */
  {
    STATIC CONST UINT32  Offs[] = { TCON1_BASIC0, 0x09C, 0x098 };
    UINT32               Ht = (B3 >> 16) + 1;
    UINT32               Vt = (B4 >> 16) / 2;
    UINTN                I;

    W = 0;
    H = 0;
    for (I = 0; I < ARRAY_SIZE (Offs); I++) {
      UINT32  V = MmioRead32 (TCON_TV3_BASE + Offs[I]);
      UINT32  Cw = (V >> 16) + 1;
      UINT32  Ch = (V & 0xffff) + 1;
      if (V != 0 && Cw >= 320 && Ch >= 200 && Cw <= Ht && Ch <= Vt) {
        B0 = V;
        W = Cw;
        H = Ch;
        break;
      }
    }
  }
  DEBUG ((DEBUG_INFO, "A733 GOP: TCON_TV3 GCTL=%08x CTL=%08x BASIC0=%08x BASIC3=%08x BASIC4=%08x BASIC5=%08x -> %ux%u\n",
    Gctl, Ctl, B0, B3, B4, B5, W, H));

  if (!(Gctl & BIT31) || !(Ctl & BIT31)) {
    DEBUG ((DEBUG_WARN, "A733 GOP: TCON_TV3 not running; using the default %ux%u\n", mWidth, mHeight));
    return;
  }
  if (W < 640 || W > 1920 || H < 480 || H > 1200 || (W % 8) != 0 ||
      (UINT64)W * 9 > (UINT64)H * 21 ||            /* wider than 21:9: interlaced field size */
      (UINT64)W * H * 4 > FixedPcdGet32 (PcdFrameBufferSize)) {
    DEBUG ((DEBUG_WARN, "A733 GOP: HDMI mode %ux%u not usable here; using the default %ux%u\n",
      W, H, mWidth, mHeight));
    return;
  }
  mWidth = W;
  mHeight = H;
  mH.Resolution = W;
  mV.Resolution = H;
  mH.Sync = ((B5 >> 16) & 0x3ff) + 1;
  mV.Sync = (B5 & 0x3ff) + 1;
  mH.BackPorch = (B3 & 0xffff) + 1;
  mV.BackPorch = (B4 & 0xffff) + 1;
  DEBUG ((DEBUG_INFO, "A733 GOP: using the HDMI mode set by U-Boot from EDID: %ux%u\n", W, H));
}

EFI_STATUS
LcdPlatformInitializeDisplay (
  IN EFI_HANDLE  Handle
  )
{
  (VOID)Handle;
  ProbeMode ();
  return EFI_SUCCESS;
}

EFI_STATUS
LcdPlatformGetVram (
  OUT EFI_PHYSICAL_ADDRESS  *VramBaseAddress,
  OUT UINTN                 *VramSize
  )
{
  if ((VramBaseAddress == NULL) || (VramSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *VramBaseAddress = FixedPcdGet64 (PcdFrameBufferBase);
  *VramSize = FixedPcdGet32 (PcdFrameBufferSize);
  return EFI_SUCCESS;
}

UINT32
LcdPlatformGetMaxMode (
  VOID
  )
{
  return 1;
}

EFI_STATUS
LcdPlatformSetMode (
  IN UINT32  ModeNumber
  )
{
  return (ModeNumber == 0) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

EFI_STATUS
LcdPlatformQueryMode (
  IN  UINT32                                ModeNumber,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info
  )
{
  if ((ModeNumber != 0) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  ProbeMode ();
  ZeroMem (Info, sizeof (*Info));
  Info->HorizontalResolution = mWidth;
  Info->VerticalResolution = mHeight;
  Info->PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
  Info->PixelsPerScanLine = mWidth;
  return EFI_SUCCESS;
}

EFI_STATUS
LcdPlatformGetTimings (
  IN  UINT32        ModeNumber,
  OUT SCAN_TIMINGS  **Horizontal,
  OUT SCAN_TIMINGS  **Vertical
  )
{
  if ((ModeNumber != 0) || (Horizontal == NULL) || (Vertical == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  ProbeMode ();
  *Horizontal = &mH;
  *Vertical = &mV;
  return EFI_SUCCESS;
}

EFI_STATUS
LcdPlatformGetBpp (
  IN  UINT32   ModeNumber,
  OUT LCD_BPP  *Bpp
  )
{
  if ((ModeNumber != 0) || (Bpp == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *Bpp = LcdBitsPerPixel_24;
  return EFI_SUCCESS;
}
