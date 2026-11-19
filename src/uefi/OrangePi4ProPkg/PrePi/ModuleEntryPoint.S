// Orange Pi 4 Pro A733 fixed-load PrePi entry.
// Loaded at 0x41000000 using the arm64 Linux Image header in the FDF.
#include <AsmMacroLib.h>

ASM_FUNC(_ModuleEntryPoint)
  // Preserve loader-provided DTB pointer for future platform code.
  mov   x28, x0

  // Earliest possible board marker, before MMU/HOB/DXE setup.
  adr   x0, A733EarlyMessage
  bl    A733EarlyUartWrite

  // EDK2 C code may use FP/SIMD. BL31 normally enables it, but make it explicit.
  bl    ArmEnableVFP

  // Primary core only for v2 bring-up. Secondary cores remain parked by PSCI/BL31.
  bl    ASM_PFX(ArmReadMpidr)
  mov   x20, x0

  // Permanent UEFI memory: 0x50000000..0x53ffffff (64 MiB).
  ldr   x21, =0x50000000
  ldr   x1,  =0x54000000
  mov   sp, x1
  movz  x2, #1, lsl #16
  sub   x22, x1, x2

  mov   x0, x20
  mov   x1, x21
  mov   x2, x22
  mov   x29, xzr
  bl    ASM_PFX(CEntryPoint)

1: b 1b

// Write a zero-terminated ASCII string through the inherited A733 UART0.
// UART0 is DesignWare/16550 compatible, 32-bit registers, stride 4.
A733EarlyUartWrite:
  ldr   x9, =0x02500000
2:
  ldrb  w10, [x0], #1
  cbz   w10, 4f
3:
  ldr   w11, [x9, #0x14]
  tbz   w11, #5, 3b
  str   w10, [x9]
  b     2b
4:
  ret

  .section .rodata
  .balign 4
A733EarlyMessage:
  .asciz "\r\n[A733 EDK2 v55 XHCI BSP WRAPPER] SEC entered at 0x41000000\r\n"
