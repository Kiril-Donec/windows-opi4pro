# V5 multi-fix

- Enlarges the firmware device from 4 MiB to 8 MiB.
- Adds BootManagerPolicyDxe and disables the default PlatformRecovery loop.
- Restores Level1/2/3, Driver1 and Debug1 built-in shell commands.
- Disables PCI legacy handoff against the synthetic non-discoverable EHCI device.
- Applies the proven sun60iw2 PHY/CCU/pass-by sequence.
- Stops and resets inherited U-Boot EHCI schedules before EDK2 binds.
- Adds register-level diagnostics for CAP/HCSP/HCCP/CMD/STS/CFG/PORTSC.
- Starts RUN and waits for HCHalted to clear before enabling schedules.
- Allows async-only fallback if the periodic schedule is the only failing stage.

The generic EhciDxe patch is included under Patches/ and must be applied after
copying this platform package into the matching EDK2 tree.
