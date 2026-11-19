#include "precomp.h"

static
NTSTATUS
GmacWaitMdioIdle(
    _In_ GMAC_ADAPTER *adapter)
{
    for (ULONG attempt = 0; attempt < 1000U; ++attempt) {
        if ((GmacRead32(adapter, GMAC_MDIO_ADDRESS) & MDIO_BUSY) == 0U) {
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(10U);
    }
    return STATUS_IO_TIMEOUT;
}

NTSTATUS
GmacMdioRead(
    _In_ GMAC_ADAPTER *adapter,
    _In_ UINT8 reg,
    _Out_ UINT16 *value)
{
    NTSTATUS status = GmacWaitMdioIdle(adapter);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    const UINT32 command = MDIO_BUSY | MDIO_READ |
        MDIO_CLOCK_RANGE |
        (static_cast<UINT32>(reg) << MDIO_REGISTER_SHIFT) |
        (PHY_ADDRESS << MDIO_PHY_SHIFT);
    GmacWrite32(adapter, GMAC_MDIO_ADDRESS, command);
    status = GmacWaitMdioIdle(adapter);
    if (NT_SUCCESS(status)) {
        *value = static_cast<UINT16>(GmacRead32(adapter, GMAC_MDIO_DATA));
    }
    return status;
}

NTSTATUS
GmacMdioWrite(
    _In_ GMAC_ADAPTER *adapter,
    _In_ UINT8 reg,
    _In_ UINT16 value)
{
    NTSTATUS status = GmacWaitMdioIdle(adapter);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    GmacWrite32(adapter, GMAC_MDIO_DATA, value);
    const UINT32 command = MDIO_BUSY | MDIO_WRITE |
        MDIO_CLOCK_RANGE |
        (static_cast<UINT32>(reg) << MDIO_REGISTER_SHIFT) |
        (PHY_ADDRESS << MDIO_PHY_SHIFT);
    GmacWrite32(adapter, GMAC_MDIO_ADDRESS, command);
    return GmacWaitMdioIdle(adapter);
}

static
NTSTATUS
GmacPhyReadExtended(
    _In_ GMAC_ADAPTER *adapter,
    _In_ UINT16 reg,
    _Out_ UINT16 *value)
{
    NTSTATUS status = GmacMdioWrite(adapter, PHY_EXT_ADDRESS, reg);
    return NT_SUCCESS(status) ? GmacMdioRead(adapter, PHY_EXT_DATA, value) : status;
}

static
NTSTATUS
GmacPhyWriteExtended(
    _In_ GMAC_ADAPTER *adapter,
    _In_ UINT16 reg,
    _In_ UINT16 value)
{
    NTSTATUS status = GmacMdioWrite(adapter, PHY_EXT_ADDRESS, reg);
    return NT_SUCCESS(status) ? GmacMdioWrite(adapter, PHY_EXT_DATA, value) : status;
}

static
VOID
GmacDelayMs(
    _In_ ULONG milliseconds)
{
    LARGE_INTEGER delay;
    delay.QuadPart = -10LL * 1000LL * static_cast<LONGLONG>(milliseconds);
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

//
// Runs BEFORE the DMA software reset (XNU a733_eth.c eth_phy_init order).
// The wrapper takes the RGMII TX clock from the PHY CLK_OUT pin (ETCS=1), so
// the DWMAC reset cannot complete until the YT8531 drives 125 MHz there.
// MDIO only needs the CSR clock, so it works before the reset.
//
NTSTATUS
GmacPhyPrepareClocks(
    _In_ GMAC_ADAPTER *adapter)
{
    UINT16 id1 = 0;
    UINT16 id2 = 0;
    NTSTATUS status = GmacMdioRead(adapter, PHY_ID1, &id1);
    if (NT_SUCCESS(status)) {
        status = GmacMdioRead(adapter, PHY_ID2, &id2);
    }
    const UINT32 phyId = (static_cast<UINT32>(id1) << 16) | id2;
    GMAC_LOG_INFO("PHY address=%u id=%08lx status=%08lx mdioAddr=%08lx",
        PHY_ADDRESS, phyId, status, GmacRead32(adapter, GMAC_MDIO_ADDRESS));
    if (!NT_SUCCESS(status) || (phyId != 0x4F51E91BU)) {
        GMAC_LOG_ERROR("no YT8531 on MDIO (PHY power DC1SW1 off? UEFI A733 ETHPWR line)");
        return NT_SUCCESS(status) ? STATUS_DEVICE_HARDWARE_ERROR : status;
    }

    // BMCR soft reset, as XNU, then wait for the bit to clear.
    UINT16 bmcr = 0;
    status = GmacMdioWrite(adapter, PHY_BMCR, PHY_BMCR_RESET);
    for (ULONG attempt = 0; NT_SUCCESS(status) && attempt < 100U; ++attempt) {
        GmacDelayMs(5U);
        if (NT_SUCCESS(GmacMdioRead(adapter, PHY_BMCR, &bmcr)) && ((bmcr & PHY_BMCR_RESET) == 0U)) {
            break;
        }
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // CLK_OUT: enabled, source 0 (PLL), 125 MHz (XNU yt8531_clkout()).
    UINT16 old = 0;
    UINT16 now = 0;
    status = GmacPhyReadExtended(adapter, YT8531_SYNCE_CFG, &old);
    if (NT_SUCCESS(status)) {
        const UINT16 want = static_cast<UINT16>(
            (old & ~(YT8531_SCR_ENABLE | YT8531_SCR_125M | YT8531_SCR_SRC_MASK)) |
            YT8531_SCR_ENABLE | YT8531_SCR_125M);
        status = GmacPhyWriteExtended(adapter, YT8531_SYNCE_CFG, want);
        if (NT_SUCCESS(status)) {
            status = GmacPhyReadExtended(adapter, YT8531_SYNCE_CFG, &now);
        }
        if (NT_SUCCESS(status) && (now != want)) {
            status = STATUS_DEVICE_HARDWARE_ERROR;
        }
    }
    GMAC_LOG_INFO("PHY reset bmcr=%04x, CLK_OUT 125 MHz SyncE %04x -> %04x status=%08lx",
        bmcr, old, now, status);
    if (NT_SUCCESS(status)) {
        GmacDelayMs(10U);               // let the MAC see a stable TX clock
    }
    return status;
}

NTSTATUS
GmacPhyInitialize(
    _In_ GMAC_ADAPTER *adapter)
{
    // Vendor setup as in XNU (BSP motorcomm.c): auto sleep off, LEDs. The
    // PHY's own RGMII delays stay as strapped (test #18: turning them off
    // broke RX); CLK_OUT was set by GmacPhyPrepareClocks.
    UINT16 value = 0;
    NTSTATUS status = GmacPhyReadExtended(adapter, YT8531_SLEEP_CTRL1, &value);
    if (NT_SUCCESS(status)) {
        status = GmacPhyWriteExtended(adapter, YT8531_SLEEP_CTRL1,
            static_cast<UINT16>(value & ~YT8531_SLEEP_SW));
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    static const struct { UINT16 Reg; UINT16 Value; } led[] = {
        { 0xA00DU, 0x2600U }, { 0xA00EU, 0x2070U }, { 0xA00FU, 0x000AU }
    };
    for (ULONG index = 0; index < ARRAYSIZE(led); ++index) {
        status = GmacPhyWriteExtended(adapter, led[index].Reg, led[index].Value);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    // Advertise 10/100 half/full and 1000 half/full, then restart
    // autonegotiation (XNU eth_autoneg).
    status = GmacMdioWrite(adapter, PHY_ANAR, 0x01E1U);
    if (NT_SUCCESS(status)) {
        status = GmacMdioWrite(adapter, PHY_GBCR, 0x0300U);
    }
    value = static_cast<UINT16>(PHY_BMCR_AUTONEG_ENABLE | PHY_BMCR_RESTART_AUTONEG);
    if (NT_SUCCESS(status)) {
        status = GmacMdioWrite(adapter, PHY_BMCR, value);
    }
    GMAC_LOG_INFO("PHY vendor init complete status=%08lx bmcr=%04x", status, value);
    return status;
}

VOID
GmacPhyPollLink(
    _In_ GMAC_ADAPTER *adapter)
{
    UINT16 bmsr = 0;
    UINT16 vendorStatus = 0;
    if (!NT_SUCCESS(GmacMdioRead(adapter, PHY_BMSR, &bmsr)) ||
        !NT_SUCCESS(GmacMdioRead(adapter, PHY_BMSR, &bmsr)) ||
        !NT_SUCCESS(GmacMdioRead(adapter, 0x11U, &vendorStatus))) {
        GmacSetLinkState(adapter, FALSE, 0U, FALSE);
        return;
    }

    const BOOLEAN connected = ((bmsr & PHY_BMSR_LINK) != 0U) &&
        ((vendorStatus & (1U << 10)) != 0U);
    ULONG speed = 0U;
    switch ((vendorStatus >> 14) & 3U) {
    case 0U: speed = 10U; break;
    case 1U: speed = 100U; break;
    case 2U: speed = 1000U; break;
    default: speed = 0U; break;
    }
    const BOOLEAN fullDuplex = (vendorStatus & (1U << 13)) != 0U;
    GmacSetLinkState(adapter, connected, speed, fullDuplex);
}
