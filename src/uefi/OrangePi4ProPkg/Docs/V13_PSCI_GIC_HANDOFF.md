# Orange Pi 4 Pro EDK2 v13 — Windows HAL isolation

v13 leaves the proven GOP/framebuffer and EHCI USB path byte-for-byte unchanged.

Primary changes:
- MADT GICR discovery range rounded to the complete 1 MiB covering all 8 redistributors.
- ACPI 6.2-revision PPTT describing one package, a 6-core Cortex-A55 cluster and a 2-core Cortex-A76 cluster.
- FADT/GTDT/MADT fields aligned with known Windows-on-ARM EDK2 platform practice.
- Expanded GIC, timer, PSCI feature and ExitBootServices diagnostics.

Three test images are produced:
1. MAIN: 8 CPUs, no pre-boot CPU_ON test. Use this first with Windows.
2. PSCI-TEST: exercises CPU_ON/CPU_OFF for CPU1..CPU7. Use without the Windows USB first and capture UART.
3. SINGLE-CORE: exposes only CPU0 in DSDT/MADT/PPTT. This isolates SMP/PSCI from the rest of HAL initialization.

The CPU_ON self-test is intentionally not enabled in MAIN because a broken CPU_OFF implementation could leave a secondary core parked and change the state presented to Windows.

v13 packaging fix:
- restored the physically proven v11 `screen_char`/video-console boot path;
- restored `/dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb`;
- final-image readback now validates uImage, boot.scr and DTB;
- packaging fails if `hdmidet` or the missing root-level v12 DTB path appears.
