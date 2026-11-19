# Orange Pi 4 Pro EDK2 v3 Shell fix

The v2 hardware log reached BDS, GOP, GraphicsConsole and loaded the embedded
Shell image, but failed in `ShellLibConstructor()` before `UefiMain()` with
`EFI_NOT_FOUND`. The generated `AutoGen.c` asserted at the Shell library
constructor.

For an embedded Shell image, the Shell application creates the Shell protocols
itself. Therefore the Shell library must not try to locate those protocols in
its constructor. v3 applies the standard EDK2 component override:

```ini
ShellPkg/Application/Shell/Shell.inf {
  <PcdsFixedAtBuild>
    gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
}
```

Expected result without any USB drive attached: the built-in Shell reaches its
interactive prompt instead of returning EFI_NOT_FOUND. The existing A733 EHCI
initialization failure is intentionally left visible for the next hardware
bring-up step; it must not be required for the embedded Shell to start.
