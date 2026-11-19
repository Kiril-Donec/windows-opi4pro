# v6 EHCI DMA fix

Hardware log from v5 reached the A733 EHCI capability/operational registers but
failed in `EhcInitSched()` with `EFI_OUT_OF_RESOURCES`.

The cause was the A733 resource HOB advertising DRAM as write-back capable only.
`NonDiscoverablePciDeviceDxe` requires UC or WC capability before it can create
non-coherent common buffers for the EHCI periodic frame list, QHs, and qTDs.

v6 adds `A733MemoryInitPeiLib`, advertising UC/WC/WT/WB capabilities while
keeping normal DRAM mapped write-back by default. Only pages allocated by the
non-coherent PCI I/O layer are remapped for DMA.

v6 also adds exact host/DMA address diagnostics to `EhcInitSched()` and
`UsbHcMem.c`, and corrects the error-path order to Unmap before FreeBuffer.
