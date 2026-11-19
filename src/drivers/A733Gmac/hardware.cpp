#include "precomp.h"

UINT32
GmacRead32(
    _In_ const GMAC_ADAPTER *adapter,
    _In_ ULONG offset)
{
    return READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(adapter->Resources.Mac + offset));
}

VOID
GmacWrite32(
    _In_ const GMAC_ADAPTER *adapter,
    _In_ ULONG offset,
    _In_ UINT32 value)
{
    WRITE_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(adapter->Resources.Mac + offset), value);
}

VOID
GmacUpdateConfigurationBits(
    _In_ GMAC_ADAPTER *adapter,
    _In_ UINT32 clearMask,
    _In_ UINT32 setMask)
{
    WdfSpinLockAcquire(adapter->Lock);
    UINT32 config = GmacRead32(adapter, GMAC_CONFIGURATION);
    config &= ~clearMask;
    config |= setMask;
    GmacWrite32(adapter, GMAC_CONFIGURATION, config);
    WdfSpinLockRelease(adapter->Lock);
}

NTSTATUS
GmacMapResources(
    _Inout_ GMAC_ADAPTER *adapter,
    _In_ WDFCMRESLIST translated)
{
    ULONG memoryIndex = 0;
    adapter->Resources = {};

    for (ULONG index = 0; index < WdfCmResourceListGetCount(translated); ++index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor = WdfCmResourceListGetDescriptor(translated, index);
        if (descriptor == nullptr) {
            continue;
        }

        if (descriptor->Type == CmResourceTypeMemory) {
            PUCHAR mapping = static_cast<PUCHAR>(MmMapIoSpaceEx(
                descriptor->u.Memory.Start,
                descriptor->u.Memory.Length,
                PAGE_READWRITE | PAGE_NOCACHE));
            if (mapping == nullptr) {
                GMAC_LOG_ERROR("MMIO map failed resource=%lu memoryIndex=%lu", index, memoryIndex);
                GmacUnmapResources(adapter);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            if (memoryIndex == 0) {
                adapter->Resources.Mac = mapping;
                adapter->Resources.MacLength = descriptor->u.Memory.Length;
            } else if (memoryIndex == 1) {
                adapter->Resources.Wrapper = mapping;
                adapter->Resources.WrapperLength = descriptor->u.Memory.Length;
            } else {
                MmUnmapIoSpace(mapping, descriptor->u.Memory.Length);
            }
            ++memoryIndex;
        } else if (descriptor->Type == CmResourceTypeInterrupt) {
            GMAC_LOG_INFO(
                "irq[%lu] vector=%lu level=%lu flags=%04x affinity=%llx",
                adapter->Resources.InterruptCount,
                descriptor->u.Interrupt.Vector,
                descriptor->u.Interrupt.Level,
                descriptor->Flags,
                static_cast<ULONGLONG>(descriptor->u.Interrupt.Affinity));
            ++adapter->Resources.InterruptCount;
        }
    }

    if ((adapter->Resources.Mac == nullptr) ||
        (adapter->Resources.Wrapper == nullptr) ||
        (adapter->Resources.MacLength < 0x2000U) ||
        (adapter->Resources.WrapperLength < 0x100U) ||
        (adapter->Resources.InterruptCount < 1U)) {
        GmacUnmapResources(adapter);
        return STATUS_RESOURCE_TYPE_NOT_FOUND;
    }

    if (adapter->Resources.InterruptCount != GMAC_INTERRUPT_COUNT) {
        GMAC_LOG_ERROR(
            "firmware supplied %lu IRQ resources, expected exactly %lu (main/LPI/TX0/RX0)",
            adapter->Resources.InterruptCount,
            static_cast<ULONG>(GMAC_INTERRUPT_COUNT));
        GmacUnmapResources(adapter);
        return STATUS_RESOURCE_TYPE_NOT_FOUND;
    }

    const UINT32 wrapper = READ_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG *>(adapter->Resources.Wrapper));
    const UINT32 version = GmacRead32(adapter, GMAC_VERSION);
    GMAC_LOG_INFO("resources mac=%p/%lu wrapper=%p/%lu irq=%lu wrap=%08lx version=%08lx hw=%08lx/%08lx/%08lx/%08lx",
        adapter->Resources.Mac, adapter->Resources.MacLength,
        adapter->Resources.Wrapper, adapter->Resources.WrapperLength,
        adapter->Resources.InterruptCount, wrapper, version,
        GmacRead32(adapter, GMAC_HW_FEATURE0), GmacRead32(adapter, GMAC_HW_FEATURE1),
        GmacRead32(adapter, GMAC_HW_FEATURE2), GmacRead32(adapter, GMAC_HW_FEATURE3));

    // UEFI programs the wrapper (EPIT|ETCS, its own delays); the platform
    // reset below rewrites it with the XNU-calibrated value, so only the
    // RGMII + external TX clock bits are required here.
    if (((wrapper & 0x7U) != 0x5U) || (version == 0U) || (version == MAXULONG)) {
        GmacUnmapResources(adapter);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    return STATUS_SUCCESS;
}

VOID
GmacUnmapResources(
    _Inout_ GMAC_ADAPTER *adapter)
{
    if (adapter->Resources.Wrapper != nullptr) {
        MmUnmapIoSpace(adapter->Resources.Wrapper, adapter->Resources.WrapperLength);
    }
    if (adapter->Resources.Mac != nullptr) {
        MmUnmapIoSpace(adapter->Resources.Mac, adapter->Resources.MacLength);
    }
    adapter->Resources = {};
}

NTSTATUS
GmacResetDma(
    _In_ GMAC_ADAPTER *adapter)
{
    GmacWrite32(adapter, DMA_MODE, GmacRead32(adapter, DMA_MODE) | DMA_MODE_SOFTWARE_RESET);
    LARGE_INTEGER delay = {};
    delay.QuadPart = -10LL * 1000LL * 10LL; // Relative 10 ms, in 100 ns units.

    // Match the vendor Linux DWMAC4 reset budget: poll every 10 ms for up to
    // one second. The old 10 ms total budget produced a false timeout while
    // the controller was still completing its software reset.
    for (ULONG attempt = 0; attempt <= 100U; ++attempt) {
        if ((GmacRead32(adapter, DMA_MODE) & DMA_MODE_SOFTWARE_RESET) == 0U) {
            GMAC_LOG_INFO("DMA reset complete mode=%08lx attempts=%lu", GmacRead32(adapter, DMA_MODE), attempt);
            return STATUS_SUCCESS;
        }
        if (attempt != 100U) {
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
    }
    GMAC_LOG_ERROR("DMA reset timeout mode=%08lx dmaStatus=%08lx version=%08lx wrapCfg=%08lx wrapStatus=%08lx wrapClock=%08lx",
                   GmacRead32(adapter, DMA_MODE),
                   GmacRead32(adapter, DMA_STATUS),
                   GmacRead32(adapter, GMAC_VERSION),
                   READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(adapter->Resources.Wrapper + GMAC_WRAPPER_CONFIGURATION)),
                   READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(adapter->Resources.Wrapper + GMAC_WRAPPER_STATUS)),
                   READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(adapter->Resources.Wrapper + GMAC_WRAPPER_CLOCK)));
    return STATUS_IO_TIMEOUT;
}

VOID
GmacProgramAddress(
    _In_ GMAC_ADAPTER *adapter)
{
    const UCHAR *address = adapter->CurrentAddress.Address;
    const UINT32 low = static_cast<UINT32>(address[0]) |
        (static_cast<UINT32>(address[1]) << 8) |
        (static_cast<UINT32>(address[2]) << 16) |
        (static_cast<UINT32>(address[3]) << 24);
    const UINT32 high = static_cast<UINT32>(address[4]) |
        (static_cast<UINT32>(address[5]) << 8) | (1U << 31);
    GmacWrite32(adapter, GMAC_ADDRESS0_LOW, low);
    GmacWrite32(adapter, GMAC_ADDRESS0_HIGH, high);
}

VOID
GmacConfigureCore(
    _In_ GMAC_ADAPTER *adapter)
{
    // AWIN0040 exposes per-direction TX0/RX0 interrupt lines. DWMAC4/5
    // routes those lines separately only in INTM mode 1 (DMA_MODE[17:16]=1).
    UINT32 dmaMode = GmacRead32(adapter, DMA_MODE);
    dmaMode &= ~DMA_MODE_INTM_MASK;
    dmaMode |= DMA_MODE_INTM_MODE1;
    GmacWrite32(adapter, DMA_MODE, dmaMode);
    GMAC_LOG_INFO("DMA multi-IRQ INTM mode1 configured mode=%08lx", dmaMode);

    GmacWrite32(adapter, DMA_SYSTEM_BUS_MODE,
        DMA_SYSTEM_BUS_FIXED_BURST | DMA_SYSTEM_BUS_ADDRESS_ALIGNED |
        DMA_SYSTEM_BUS_MIXED_BURST | DMA_SYSTEM_BUS_BLEN4 |
        DMA_SYSTEM_BUS_BLEN8 | DMA_SYSTEM_BUS_BLEN16);
    GmacWrite32(adapter, MTL_TX_QUEUE0_OPERATION_MODE,
        MTL_TX_QUEUE_ENABLE | MTL_TX_STORE_AND_FORWARD);
    GmacWrite32(adapter, MTL_RX_QUEUE0_OPERATION_MODE, MTL_RX_STORE_AND_FORWARD);
    GmacWrite32(adapter, GMAC_RX_QUEUE_CONTROL0, 2U);
    GmacWrite32(adapter, GMAC_PACKET_FILTER, GMAC_FILTER_RA);
    // Start from driver-owned interrupt masks. Individual KMDF interrupt
    // callbacks enable only the sources associated with their IRQ role.
    GmacWrite32(adapter, GMAC_INTERRUPT_ENABLE, 0U);
    GmacWrite32(adapter, DMA_CHANNEL0_INTERRUPT_ENABLE, 0U);
    adapter->EnabledInterruptRoles = 0;
    GmacProgramAddress(adapter);
    GmacWrite32(adapter, GMAC_CONFIGURATION,
        GMAC_CONFIG_JD | GMAC_CONFIG_BE | GMAC_CONFIG_DCRS |
        GMAC_CONFIG_JE | GMAC_CONFIG_DM);
}

VOID
GmacStopCore(
    _In_ GMAC_ADAPTER *adapter)
{
    if (adapter->Resources.Mac == nullptr) {
        return;
    }
    GmacWrite32(adapter, DMA_CHANNEL0_INTERRUPT_ENABLE, 0U);
    GmacWrite32(adapter, GMAC_INTERRUPT_ENABLE, 0U);
    adapter->EnabledInterruptRoles = 0;
    GmacWrite32(adapter, DMA_CHANNEL0_TX_CONTROL,
        GmacRead32(adapter, DMA_CHANNEL0_TX_CONTROL) & ~DMA_TX_START);
    GmacWrite32(adapter, DMA_CHANNEL0_RX_CONTROL,
        GmacRead32(adapter, DMA_CHANNEL0_RX_CONTROL) & ~DMA_RX_START);
    GmacUpdateConfigurationBits(adapter, GMAC_CONFIG_TE | GMAC_CONFIG_RE, 0U);
}

VOID
GmacUpdateLinkMode(
    _In_ GMAC_ADAPTER *adapter,
    _In_ ULONG speedMbps,
    _In_ BOOLEAN fullDuplex)
{
    UINT32 setMask = fullDuplex ? GMAC_CONFIG_DM : 0U;
    if (speedMbps == 100U) {
        setMask |= GMAC_CONFIG_PS | GMAC_CONFIG_FES;
    } else if (speedMbps == 10U) {
        setMask |= GMAC_CONFIG_PS;
    }
    GmacUpdateConfigurationBits(
        adapter,
        GMAC_CONFIG_PS | GMAC_CONFIG_FES | GMAC_CONFIG_DM,
        setMask);
}
