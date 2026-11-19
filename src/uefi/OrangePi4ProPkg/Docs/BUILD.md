# Build instructions

## Host packages

A Linux host needs Python 3, GNU make, a C compiler for EDK2 BaseTools,
Clang/LLVM (`clang`, `ld.lld`, `llvm-ar`, `llvm-objcopy`) and standard EDK2
Python dependencies.

The source tree is expected at:

```text
edk2/
  Platform/OrangePi/OrangePi4ProPkg/
```

Initialize the normal EDK2 submodules when using a fresh official checkout.
The retained development ZIP used while creating v2 did not include every
submodule, so the build environment used empty include directories only for
unused MipiSysT/FDT/Brotli paths. A normal recursive EDK2 checkout is preferred.

## Build

```bash
cd edk2/Platform/OrangePi/OrangePi4ProPkg
EDK2_TARGET=DEBUG JOBS=8 ./build.sh
EDK2_TARGET=RELEASE JOBS=8 ./build.sh
```

Raw firmware output:

```text
Build/OrangePi4Pro-AARCH64/DEBUG_CLANGDWARF/FV/ORANGEPI4PRO_EFI.fd
Build/OrangePi4Pro-AARCH64/RELEASE_CLANGDWARF/FV/ORANGEPI4PRO_EFI.fd
```

## Wrap for vendor bootm

```bash
python3 Tools/mk_legacy_uimage.py \
  Build/OrangePi4Pro-AARCH64/DEBUG_CLANGDWARF/FV/ORANGEPI4PRO_EFI.fd \
  orangepi4pro_edk2_v2_DEBUG.uimg
```

The wrapper deliberately uses legacy architecture byte `2`, matching the
verified Stage0 v13 vendor U-Boot header. Do not change it to ARM64 byte 22
until that path has been tested against this 2018 vendor U-Boot.

## Build a complete microSD image

This requires `libext2fs-dev` and `libcom-err` on the host:

```bash
Tools/make_flashable_image.sh \
  orangepi4pro_uefi_stage0_v13.img \
  Build/OrangePi4Pro-AARCH64/DEBUG_CLANGDWARF/FV/ORANGEPI4PRO_EFI.fd \
  orangepi4pro_edk2_v2_dxe_fix_debug_flashable.img DEBUG
```

The tool preserves the known-good early vendor boot material and DTB, replaces
`/uefi_stage0.uimg` and `/boot.scr` in partition 1, then reads both files back
and compares them exactly.
