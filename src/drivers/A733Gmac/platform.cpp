#include "precomp.h"

namespace {

constexpr ULONGLONG A733_CCU_BASE = 0x02002000ULL;
constexpr ULONG A733_CCU_LENGTH = 0x2000U;
constexpr ULONG A733_CCU_GMAC_MBUS = 0x05E4U;
constexpr ULONG A733_CCU_GMAC_PHY_CLK = 0x1410U;
constexpr ULONG A733_CCU_GMAC_BGR = 0x141CU;

constexpr UINT32 A733_CCU_WRITE_KEY = 0x00040302U;
constexpr UINT32 A733_CCU_GMAC_MBUS_GATE = 1U << 11;
constexpr UINT32 A733_CCU_GMAC_CORE_GATE = 1U << 0;
constexpr UINT32 A733_CCU_GMAC_BUS_RESET = 1U << 16;
constexpr UINT32 A733_CCU_GMAC_AXI_RESET = 1U << 17;
constexpr UINT32 A733_CCU_GMAC_PHY_GATE = 1U << 31;
constexpr UINT32 A733_CCU_GMAC_PHY_DIV_MASK = 0x1FU;
constexpr UINT32 A733_CCU_GMAC_PHY_DIV = 5U;
constexpr UINT32 A733_GMAC_WRAPPER_VALUE = A733_GMAC_WRAPPER_RGMII;

UINT32
PlatformRead32(
    _In_ const GMAC_ADAPTER *adapter,
    _In_ ULONG offset)
{
    return READ_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG *>(adapter->Platform.Ccu + offset));
}

UINT32
PlatformWriteReadback(
    _In_ const GMAC_ADAPTER *adapter,
    _In_ ULONG offset,
    _In_ UINT32 value)
{
    WRITE_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG *>(adapter->Platform.Ccu + offset),
        value);
    MemoryBarrier();
    return PlatformRead32(adapter, offset);
}

} // namespace

NTSTATUS
GmacPlatformMap(
    _Inout_ GMAC_ADAPTER *adapter)
{
    if (adapter->Platform.Ccu != nullptr) {
        return STATUS_SUCCESS;
    }

    PHYSICAL_ADDRESS address = {};
    address.QuadPart = A733_CCU_BASE;
    adapter->Platform.Ccu = static_cast<PUCHAR>(MmMapIoSpaceEx(
        address,
        A733_CCU_LENGTH,
        PAGE_READWRITE | PAGE_NOCACHE));
    if (adapter->Platform.Ccu == nullptr) {
        GMAC_LOG_ERROR("CCU map failed physical=%llx length=%lx",
            A733_CCU_BASE, A733_CCU_LENGTH);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    adapter->Platform.CcuLength = A733_CCU_LENGTH;
    GMAC_LOG_INFO("CCU mapped physical=%llx length=%lx",
        A733_CCU_BASE, A733_CCU_LENGTH);
    return STATUS_SUCCESS;
}

VOID
GmacPlatformUnmap(
    _Inout_ GMAC_ADAPTER *adapter)
{
    if (adapter->Platform.Ccu != nullptr) {
        MmUnmapIoSpace(adapter->Platform.Ccu, adapter->Platform.CcuLength);
    }
    adapter->Platform = {};
}

NTSTATUS
GmacPlatformResetAndConfigure(
    _In_ GMAC_ADAPTER *adapter)
{
    if ((adapter->Platform.Ccu == nullptr) ||
        (adapter->Resources.Wrapper == nullptr)) {
        return STATUS_DEVICE_NOT_READY;
    }

    const UINT32 beforeMbus = PlatformRead32(adapter, A733_CCU_GMAC_MBUS);
    const UINT32 beforePhy = PlatformRead32(adapter, A733_CCU_GMAC_PHY_CLK);
    const UINT32 beforeBgr = PlatformRead32(adapter, A733_CCU_GMAC_BGR);
    const UINT32 beforeWrapper = READ_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG *>(
            adapter->Resources.Wrapper + GMAC_WRAPPER_CONFIGURATION));

    // Match the vendor Linux probe ordering: assert the controller resets,
    // prepare its clocks/syscon, deassert the resets, then wait before SWR.
    UINT32 bgr = beforeBgr & ~(A733_CCU_GMAC_BUS_RESET | A733_CCU_GMAC_AXI_RESET);
    PlatformWriteReadback(adapter, A733_CCU_GMAC_BGR, bgr);

    const UINT32 mbus = PlatformWriteReadback(
        adapter,
        A733_CCU_GMAC_MBUS,
        beforeMbus | A733_CCU_WRITE_KEY | A733_CCU_GMAC_MBUS_GATE);

    UINT32 phy = beforePhy & ~A733_CCU_GMAC_PHY_DIV_MASK;
    phy |= A733_CCU_GMAC_PHY_GATE | A733_CCU_GMAC_PHY_DIV;
    phy = PlatformWriteReadback(adapter, A733_CCU_GMAC_PHY_CLK, phy);

    bgr |= A733_CCU_GMAC_CORE_GATE;
    PlatformWriteReadback(adapter, A733_CCU_GMAC_BGR, bgr);
    KeStallExecutionProcessor(10U);

    bgr |= A733_CCU_GMAC_BUS_RESET | A733_CCU_GMAC_AXI_RESET;
    bgr = PlatformWriteReadback(adapter, A733_CCU_GMAC_BGR, bgr);
    KeStallExecutionProcessor(1000U);

    WRITE_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG *>(
            adapter->Resources.Wrapper + GMAC_WRAPPER_CONFIGURATION),
        A733_GMAC_WRAPPER_VALUE);
    MemoryBarrier();
    KeStallExecutionProcessor(10U);

    const UINT32 wrapper = READ_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG *>(
            adapter->Resources.Wrapper + GMAC_WRAPPER_CONFIGURATION));
    GMAC_LOG_INFO(
        "platform reset before=%08lx/%08lx/%08lx/%08lx after=%08lx/%08lx/%08lx/%08lx",
        beforeMbus, beforePhy, beforeBgr, beforeWrapper,
        mbus, phy, bgr, wrapper);

    if (((mbus & A733_CCU_GMAC_MBUS_GATE) == 0U) ||
        ((phy & A733_CCU_GMAC_PHY_GATE) == 0U) ||
        ((phy & A733_CCU_GMAC_PHY_DIV_MASK) != A733_CCU_GMAC_PHY_DIV) ||
        ((bgr & (A733_CCU_GMAC_CORE_GATE |
                 A733_CCU_GMAC_BUS_RESET |
                 A733_CCU_GMAC_AXI_RESET)) !=
            (A733_CCU_GMAC_CORE_GATE |
             A733_CCU_GMAC_BUS_RESET |
             A733_CCU_GMAC_AXI_RESET)) ||
        (wrapper != A733_GMAC_WRAPPER_VALUE)) {
        GMAC_LOG_ERROR("platform reset readback validation failed status=%08lx",
            STATUS_DEVICE_HARDWARE_ERROR);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    return STATUS_SUCCESS;
}
