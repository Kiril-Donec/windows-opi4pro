# v7 EHCI Normal Non-Cacheable DMA fix

## Failure identified from the complete v6 UART log

The v6 DMA32 allocation succeeded, but the first `ZeroMem()` of an EHCI qTD
raised an AArch64 alignment fault at `FFFEF038`. Symbolization mapped the stack to:

- `InternalMemSetMem`
- `UsbHcAllocateMem`
- `EhcCreateQtd`
- `EhcCreateHelpQ`
- `EhcInitSched`

## Root cause in upstream EDK2 sources

`NonCoherentPciIoAllocateBuffer()` chooses `EFI_MEMORY_UC` when the caller does
not request write combining. On AArch64, `ArmCpuDxe` translates `EFI_MEMORY_UC`
to Device memory. Device memory does not permit the wide/unaligned accesses used
by the optimized BaseMemoryLib `ZeroMem()` implementation.

The same EDK2 PCI I/O implementation explicitly allocates bounce buffers with
`EFI_PCI_IO_ATTRIBUTE_MEMORY_WRITE_COMBINE`. On AArch64 this becomes
`EFI_MEMORY_WC`, which maps to Normal Non-Cacheable memory and is appropriate for
CPU-accessed non-coherent DMA descriptors.

## Fix

Both EHCI common-buffer allocations now request
`EFI_PCI_IO_ATTRIBUTE_MEMORY_WRITE_COMBINE`:

- periodic frame list (`EhciSched.c`)
- qTD/QH descriptor pool (`UsbHcMem.c`)

Additional changes:

- initialize mapping outputs before diagnostics
- unmap successful short mappings before freeing buffers
- assert 64-byte descriptor alignment
- memory fences before publishing schedules to the controller
- retain the v6 DMA32 capability HOB and all prior display/Shell fixes

## Source references reviewed

- EDK2 `NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceIo.c`
- EDK2 `ArmPkg/Drivers/CpuDxe/AArch64/Mmu.c`
- EDK2 `MdeModulePkg/Bus/Pci/EhciDxe/*`
- MiniUEFI v86 `src/usb/ehci.c` and `src/platform/a733_usb.c`
- vendor U-Boot / sun60iw2 USB clock, reset, pass-by implementation

No physical USB success is claimed until the image is tested on hardware.
