# v9 EHCI clock/reset recovery

Changes from v8:

- follows the actual sun60iw2 Linux BSP clock order: HCI reset/clock before PHY clock/reset;
- clears PHY SIDDQ and applies board `phy_range = <0x188>` at vendor registers;
- restores pass-by, PHY control and PHY tune after every EHCI HCReset;
- uses standard ITC=8 command baseline;
- samples FRINDEX after RUN;
- performs one full CCU/PHY recovery if FRINDEX remains stuck;
- accepts missing PSS/ASS mirror bits only when the frame engine is demonstrably advancing and no halt/system error is present;
- retains v7 Normal-NC DMA, v8 low-DMA allocation and terminal-failure suppression.
