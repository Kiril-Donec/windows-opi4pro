#include <AsmMacroLib.h>
#include <IndustryStandard/ArmStdSmc.h>

// PSCI CPU_ON enters with x0 = context_id and the MMU disabled. Publish a
// cookie, clean it to the point of coherency, then power this test CPU off.
ASM_FUNC(A733SecondaryEntry)
  mov     x19, x0
  mov     x1, #1
  str     x1, [x19]
  dc      cvac, x19
  dsb     sy
  isb
  movz    x0, #(ARM_SMC_ID_PSCI_CPU_OFF & 0xffff)
  movk    x0, #((ARM_SMC_ID_PSCI_CPU_OFF >> 16) & 0xffff), lsl #16
  smc     #0
  // CPU_OFF only returns on failure. Publish cookie=2 so the primary CPU can
  // distinguish "entered and powered off" from "CPU_OFF returned".
  mov     x1, #2
  str     x1, [x19]
  dc      cvac, x19
  dsb     sy
  sev
1:
  wfe
  b       1b
