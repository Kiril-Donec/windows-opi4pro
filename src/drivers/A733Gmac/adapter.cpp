#include "precomp.h"

NTSTATUS
GmacCreateAdapter(
    _In_ WDFDEVICE device)
{
    NETADAPTER_INIT *adapterInit = NetAdapterInitAllocate(device);
    if (adapterInit == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NET_ADAPTER_DATAPATH_CALLBACKS callbacks;
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&callbacks, GmacEvtCreateTxQueue, GmacEvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(adapterInit, &callbacks);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, GMAC_ADAPTER);
    NETADAPTER netAdapter = WDF_NO_HANDLE;
    NTSTATUS status = NetAdapterCreate(adapterInit, &attributes, &netAdapter);
    NetAdapterInitFree(adapterInit);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    GMAC_ADAPTER *adapter = GmacGetAdapterContext(netAdapter);
    RtlZeroMemory(adapter, sizeof(*adapter));
    adapter->WdfDevice = device;
    adapter->NetAdapter = netAdapter;
    GmacGetDeviceContext(device)->Adapter = adapter;

    WDF_OBJECT_ATTRIBUTES lockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = netAdapter;
    status = WdfSpinLockCreate(&lockAttributes, &adapter->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // All four DIRQL interrupt objects share one framework interrupt lock.
    // This serializes ISR/Enable/Disable callbacks across main/LPI/TX0/RX0
    // and prevents competing mask writes on multiprocessor systems.
    WDF_OBJECT_ATTRIBUTES interruptLockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&interruptLockAttributes);
    interruptLockAttributes.ParentObject = netAdapter;
    status = WdfSpinLockCreate(&interruptLockAttributes, &adapter->InterruptLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_DMA_ENABLER_CONFIG dmaConfig;
    WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig, WdfDmaProfileScatterGatherDuplex, GMAC_MAX_FRAME_SIZE);
    dmaConfig.Flags |= WDF_DMA_ENABLER_CONFIG_REQUIRE_SINGLE_TRANSFER;
    dmaConfig.WdmDmaVersionOverride = 3;
    WDF_OBJECT_ATTRIBUTES dmaAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&dmaAttributes);
    dmaAttributes.ParentObject = netAdapter;
    status = WdfDmaEnablerCreate(device, &dmaConfig, &dmaAttributes, &adapter->DmaEnabler);
    if (!NT_SUCCESS(status)) {
        return status;
    }

#if GMAC_USE_INTERRUPTS
    // AWIN0040 publishes four interrupt vectors in firmware order:
    // main, LPI, TX0 and RX0. The role is stored in each WDFINTERRUPT context
    // so MAC/LPI events are never mistaken for DMA queue completions.
    for (ULONG interruptIndex = 0; interruptIndex < GMAC_INTERRUPT_COUNT; ++interruptIndex) {
        WDF_INTERRUPT_CONFIG interruptConfig;
        WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, GmacEvtInterruptIsr, GmacEvtInterruptDpc);
        interruptConfig.EvtInterruptEnable = GmacEvtInterruptEnable;
        interruptConfig.EvtInterruptDisable = GmacEvtInterruptDisable;
        interruptConfig.SpinLock = adapter->InterruptLock;

        WDF_OBJECT_ATTRIBUTES interruptAttributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&interruptAttributes, GMAC_INTERRUPT_CONTEXT);
        status = WdfInterruptCreate(
            device,
            &interruptConfig,
            &interruptAttributes,
            &adapter->Interrupts[interruptIndex]);
        if (!NT_SUCCESS(status)) {
            GMAC_LOG_ERROR("interrupt object %lu create failed %08lx", interruptIndex, status);
            return status;
        }

        GMAC_INTERRUPT_CONTEXT *interruptContext =
            GmacGetInterruptContext(adapter->Interrupts[interruptIndex]);
        interruptContext->Role = static_cast<GMAC_INTERRUPT_ROLE>(interruptIndex);
        interruptContext->ResourceIndex = interruptIndex;
    }
#else
    GMAC_LOG_INFO("datapath polled every %lu ms (no WDF interrupt objects)", GMAC_POLL_PERIOD_MS);
#endif

    WDF_TIMER_CONFIG timerConfig;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, GmacEvtLinkTimer, 1000U);
    WDF_OBJECT_ATTRIBUTES timerAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = netAdapter;
    status = WdfTimerCreate(&timerConfig, &timerAttributes, &adapter->LinkTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_TIMER_CONFIG pollConfig;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&pollConfig, GmacEvtPollTimer, GMAC_POLL_PERIOD_MS);
    pollConfig.UseHighResolutionTimer = WdfTrue;
    WDF_OBJECT_ATTRIBUTES pollAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&pollAttributes);
    pollAttributes.ParentObject = netAdapter;
    status = WdfTimerCreate(&pollConfig, &pollAttributes, &adapter->PollTimer);
    if (!NT_SUCCESS(status)) {
        GMAC_LOG_ERROR("poll timer create failed %08lx", status);
        return status;
    }

    return GmacStartAdapter(adapter);
}

//
// Board-unique, stable MAC from the SoC chip ID (SID efuse 0x03006000 +
// 0x200, four words), exactly as the XNU driver (a733_eth.c
// eth_mac_from_chipid): FNV-1a over the 16 bytes, locally administered.
// The board gets the same address under iOS and Windows.
//
static
VOID
GmacMacFromChipId(
    _Out_writes_(6) UCHAR mac[6])
{
    static const UCHAR fallback[6] = { 0x02U, 0xA7U, 0x33U, 0x00U, 0x00U, 0x40U };
    UINT32 id[4] = {};
    PHYSICAL_ADDRESS sid = {};
    sid.QuadPart = 0x03006000LL;
    volatile UINT32 *map = static_cast<volatile UINT32 *>(
        MmMapIoSpaceEx(sid, 0x1000U, PAGE_READWRITE | PAGE_NOCACHE));
    if (map != nullptr) {
        for (ULONG i = 0; i < 4U; ++i) {
            id[i] = READ_REGISTER_ULONG(const_cast<PULONG>(
                reinterpret_cast<volatile ULONG *>(map + (0x200U / 4U) + i)));
        }
        MmUnmapIoSpace(const_cast<UINT32 *>(map), 0x1000U);
    }
    if ((id[0] | id[1] | id[2] | id[3]) == 0U) {
        RtlCopyMemory(mac, fallback, sizeof(fallback));
        GMAC_LOG_ERROR("chip ID unreadable, fixed MAC %02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }
    UINT32 h = 2166136261U;
    for (ULONG i = 0; i < 16U; ++i) {
        h = (h ^ ((id[i / 4U] >> (8U * (i % 4U))) & 0xFFU)) * 16777619U;
    }
    const UINT32 h2 = (h ^ id[0] ^ (id[3] << 7)) * 2654435761U;
    mac[0] = 0x02U;
    mac[1] = static_cast<UCHAR>(h >> 24);
    mac[2] = static_cast<UCHAR>(h >> 16);
    mac[3] = static_cast<UCHAR>(h >> 8);
    mac[4] = static_cast<UCHAR>(h2);
    mac[5] = static_cast<UCHAR>(h2 >> 8);
    GMAC_LOG_ERROR("MAC %02x:%02x:%02x:%02x:%02x:%02x from chip ID %08lx %08lx %08lx %08lx",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], id[0], id[1], id[2], id[3]);
}

NTSTATUS
GmacStartAdapter(
    _In_ GMAC_ADAPTER *adapter)
{
    UCHAR initialAddress[6];
    GmacMacFromChipId(initialAddress);
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&adapter->PermanentAddress, sizeof(initialAddress), initialAddress);
    adapter->CurrentAddress = adapter->PermanentAddress;

    NET_ADAPTER_LINK_LAYER_CAPABILITIES linkCapabilities;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&linkCapabilities, 1000000000ULL, 1000000000ULL);
    NetAdapterSetLinkLayerCapabilities(adapter->NetAdapter, &linkCapabilities);
    NetAdapterSetLinkLayerMtuSize(adapter->NetAdapter, 1500U);
    NetAdapterSetPermanentLinkLayerAddress(adapter->NetAdapter, &adapter->PermanentAddress);
    NetAdapterSetCurrentLinkLayerAddress(adapter->NetAdapter, &adapter->CurrentAddress);

    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES filterCapabilities;
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&filterCapabilities, GmacEvtSetPacketFilter);
    filterCapabilities.SupportedPacketFilters =
        NetPacketFilterFlagDirected | NetPacketFilterFlagMulticast |
        NetPacketFilterFlagAllMulticast | NetPacketFilterFlagBroadcast |
        NetPacketFilterFlagPromiscuous;
    // NetAdapterCx rejects a zero multicast-list capacity when the multicast
    // packet filter is advertised. The current hardware path accepts all
    // multicast frames, so retain a small software-facing list capacity.
    filterCapabilities.MaximumMulticastAddresses = 16U;
    NetAdapterSetReceiveFilterCapabilities(adapter->NetAdapter, &filterCapabilities);

    // TX without NetAdapterCx DMA mapping: the driver copies each frame into
    // its own bounce slot below 4 GiB (see queues.h, 0.2.2).
    NET_ADAPTER_TX_CAPABILITIES txCapabilities;
    NET_ADAPTER_TX_CAPABILITIES_INIT(&txCapabilities, 1U);
    txCapabilities.MaximumNumberOfFragments = 8U;
    txCapabilities.FragmentRingNumberOfElementsHint = GMAC_RING_HINT;

    NET_ADAPTER_DMA_CAPABILITIES rxDma;
    NET_ADAPTER_DMA_CAPABILITIES_INIT(&rxDma, adapter->DmaEnabler);
    NET_ADAPTER_RX_CAPABILITIES rxCapabilities;
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED_DMA(
        &rxCapabilities, &rxDma, GMAC_MAX_FRAME_SIZE, 1U);
    rxCapabilities.FragmentBufferAlignment = 64U;
    rxCapabilities.FragmentRingNumberOfElementsHint = GMAC_RING_HINT;
    NetAdapterSetDataPathCapabilities(adapter->NetAdapter, &txCapabilities, &rxCapabilities);

    NET_ADAPTER_LINK_STATE disconnected;
    NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&disconnected);
    NetAdapterSetLinkState(adapter->NetAdapter, &disconnected);
    return NetAdapterStart(adapter->NetAdapter);
}

VOID
GmacSetLinkState(
    _In_ GMAC_ADAPTER *adapter,
    _In_ BOOLEAN connected,
    _In_ ULONG speedMbps,
    _In_ BOOLEAN fullDuplex)
{
    if ((adapter->LinkUp == connected) &&
        (!connected || ((adapter->LinkSpeedMbps == speedMbps) && (adapter->FullDuplex == fullDuplex)))) {
        return;
    }

    adapter->LinkUp = connected;
    adapter->LinkSpeedMbps = speedMbps;
    adapter->FullDuplex = fullDuplex;
    if (connected) {
        GmacUpdateLinkMode(adapter, speedMbps, fullDuplex);
    }

    NET_ADAPTER_LINK_STATE state;
    if (connected) {
        NET_ADAPTER_LINK_STATE_INIT(
            &state,
            static_cast<ULONG64>(speedMbps) * 1000000ULL,
            MediaConnectStateConnected,
            fullDuplex ? MediaDuplexStateFull : MediaDuplexStateHalf,
            NetAdapterPauseFunctionTypeUnsupported,
            NetAdapterAutoNegotiationFlagXmitLinkSpeedAutoNegotiated |
            NetAdapterAutoNegotiationFlagRcvLinkSpeedautoNegotiated |
            NetAdapterAutoNegotiationFlagDuplexAutoNegotiated);
    } else {
        NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&state);
    }
    NetAdapterSetLinkState(adapter->NetAdapter, &state);
    GMAC_LOG_INFO("link %s speed=%lu duplex=%s", connected ? "up" : "down", speedMbps, fullDuplex ? "full" : "half");
}

_Use_decl_annotations_
VOID
GmacEvtSetPacketFilter(
    NETADAPTER netAdapter,
    NETRECEIVEFILTER receiveFilter)
{
    GMAC_ADAPTER *adapter = GmacGetAdapterContext(netAdapter);
    const NET_PACKET_FILTER_FLAGS packetFilter = NetReceiveFilterGetPacketFilter(receiveFilter);
    adapter->PacketFilter = packetFilter;
    if (adapter->Resources.Mac == nullptr) {
        return;
    }
    UINT32 filter = 0U;
    if ((packetFilter & NetPacketFilterFlagPromiscuous) != 0U) filter |= GMAC_FILTER_PR;
    if ((packetFilter & (NetPacketFilterFlagAllMulticast | NetPacketFilterFlagMulticast)) != 0U) {
        // Hash-table programming is intentionally deferred; accept multicast
        // in hardware so protocol discovery still works correctly.
        filter |= GMAC_FILTER_PM;
    }
    if ((packetFilter & NetPacketFilterFlagBroadcast) == 0U) filter |= (1U << 5);
    GmacWrite32(adapter, GMAC_PACKET_FILTER, filter);
}

_Use_decl_annotations_
VOID
GmacEvtLinkTimer(WDFTIMER timer)
{
    NETADAPTER netAdapter = static_cast<NETADAPTER>(WdfTimerGetParentObject(timer));
    GMAC_ADAPTER *adapter = GmacGetAdapterContext(netAdapter);
    if (adapter->HardwarePrepared) {
        GmacPhyPollLink(adapter);
    }
}

//
// Polled datapath (XNU a733_eth.c polls from a thread). While NetAdapterCx has
// a queue's notification armed it waits for "more work"; tell it every tick
// and let Advance read the descriptor rings. Disarming flushes queued DPCs
// (GmacSetTx/RxNotification -> KeFlushQueuedDpcs), which also covers this
// timer's DPC, so no notify can follow a disarm.
//
_Use_decl_annotations_
VOID
GmacEvtPollTimer(WDFTIMER timer)
{
    NETADAPTER netAdapter = static_cast<NETADAPTER>(WdfTimerGetParentObject(timer));
    GMAC_ADAPTER *adapter = GmacGetAdapterContext(netAdapter);
    if (!adapter->HardwarePrepared) {
        return;
    }
    if ((adapter->TxQueue != WDF_NO_HANDLE) &&
        (InterlockedExchange(&adapter->TxNotifyArmed, FALSE) != FALSE)) {
        NetTxQueueNotifyMoreCompletedPacketsAvailable(adapter->TxQueue);
    }
    if ((adapter->RxQueue != WDF_NO_HANDLE) &&
        (InterlockedExchange(&adapter->RxNotifyArmed, FALSE) != FALSE)) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(adapter->RxQueue);
    }
}

VOID
GmacStartPolling(
    _In_ GMAC_ADAPTER *adapter)
{
    if ((adapter->PollTimer != WDF_NO_HANDLE) &&
        (InterlockedExchange(&adapter->PollRunning, TRUE) == FALSE)) {
        WdfTimerStart(adapter->PollTimer, WDF_REL_TIMEOUT_IN_MS(GMAC_POLL_PERIOD_MS));
        GMAC_LOG_INFO("poll timer started");
    }
}

VOID
GmacStopPolling(
    _In_ GMAC_ADAPTER *adapter)
{
    if ((adapter->PollTimer != WDF_NO_HANDLE) &&
        (InterlockedExchange(&adapter->PollRunning, FALSE) != FALSE)) {
        WdfTimerStop(adapter->PollTimer, TRUE);
    }
}

_Use_decl_annotations_
BOOLEAN
GmacEvtInterruptIsr(WDFINTERRUPT interrupt, ULONG messageId)
{
    UNREFERENCED_PARAMETER(messageId);
    WDFDEVICE device = static_cast<WDFDEVICE>(WdfInterruptGetDevice(interrupt));
    GMAC_ADAPTER *adapter = GmacGetDeviceContext(device)->Adapter;
    if ((adapter == nullptr) || (adapter->Resources.Mac == nullptr)) {
        return FALSE;
    }

    const GMAC_INTERRUPT_CONTEXT *interruptContext = GmacGetInterruptContext(interrupt);
    switch (interruptContext->Role) {
    case GmacInterruptMain: {
        // Main is a MAC/core diagnostic line. It must never complete TX/RX
        // NetAdapterCx queues. Only claim sources that this driver actually
        // enabled. LPI has its own interrupt object and is excluded here.
        const UINT32 macStatus = GmacRead32(adapter, GMAC_INTERRUPT_STATUS);
        const UINT32 macEnable = GmacRead32(adapter, GMAC_INTERRUPT_ENABLE);
        const UINT32 active = macStatus & macEnable & ~GMAC_INT_LPI;
        if (active == 0U) {
            return FALSE;
        }

        // No non-LPI MAC source is serviced yet. Mask any unexpected enabled
        // source after reporting it so a sticky status cannot interrupt-storm.
        GmacWrite32(adapter, GMAC_INTERRUPT_ENABLE, macEnable & ~active);
        GMAC_LOG_ERROR(
            "main IRQ active=%08lx macStatus=%08lx macEnable=%08lx; source masked (diagnostic only)",
            active,
            macStatus,
            macEnable);
        return TRUE;
    }

    case GmacInterruptLpi: {
        const UINT32 macStatus = GmacRead32(adapter, GMAC_INTERRUPT_STATUS);
        if ((macStatus & GMAC_INT_LPI) == 0U) {
            return FALSE;
        }

        // DWMAC4/5 clears the LPI interrupt by reading MAC_LPI_Control_Status.
        const UINT32 lpiStatus = GmacRead32(adapter, GMAC_LPI_CONTROL_STATUS);
        GMAC_LOG_INFO("LPI IRQ macStatus=%08lx lpiStatus=%08lx", macStatus, lpiStatus);
        return TRUE;
    }

    case GmacInterruptTx0:
    case GmacInterruptRx0: {
        const UINT32 dmaStatus = GmacRead32(adapter, DMA_CHANNEL0_STATUS);
        const UINT32 causeMask = (interruptContext->Role == GmacInterruptTx0) ?
            DMA_INT_TX_CAUSE : DMA_INT_RX_CAUSE;
        const UINT32 causes = dmaStatus & causeMask;
        if (causes == 0U) {
            return FALSE;
        }

        // DMA_CH0_STATUS is W1C. Acknowledge only this IRQ direction plus the
        // summary bits observed with it. Never write back the full register:
        // doing so could erase an RX completion from TX0 (or vice versa).
        const UINT32 acknowledge = causes | (dmaStatus & DMA_INT_SUMMARY);
        GmacWrite32(adapter, DMA_CHANNEL0_STATUS, acknowledge);
        InterlockedOr(&adapter->PendingInterrupts, static_cast<LONG>(causes));
        WdfInterruptQueueDpcForIsr(interrupt);
        return TRUE;
    }

    default:
        return FALSE;
    }
}

_Use_decl_annotations_
VOID
GmacEvtInterruptDpc(WDFINTERRUPT interrupt, WDFOBJECT associatedObject)
{
    UNREFERENCED_PARAMETER(associatedObject);
    WDFDEVICE device = static_cast<WDFDEVICE>(WdfInterruptGetDevice(interrupt));
    GMAC_ADAPTER *adapter = GmacGetDeviceContext(device)->Adapter;
    const LONG status = InterlockedExchange(&adapter->PendingInterrupts, 0);
    if (((status & DMA_INT_TX) != 0) &&
        (InterlockedExchange(&adapter->TxNotifyArmed, FALSE) != FALSE) &&
        (adapter->TxQueue != WDF_NO_HANDLE)) {
        NetTxQueueNotifyMoreCompletedPacketsAvailable(adapter->TxQueue);
    }
    if (((status & (DMA_INT_RX | DMA_INT_RX_BUFFER_UNAVAILABLE)) != 0) &&
        (InterlockedExchange(&adapter->RxNotifyArmed, FALSE) != FALSE) &&
        (adapter->RxQueue != WDF_NO_HANDLE)) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(adapter->RxQueue);
    }
    if ((status & DMA_INT_TX_STOP) != 0) {
        GMAC_LOG_ERROR("TX DMA stopped status=%08lx", status);
    }
    if ((status & DMA_INT_TX_BUFFER_UNAVAILABLE) != 0) {
        GMAC_LOG_INFO("TX DMA buffer unavailable status=%08lx", status);
    }
    if ((status & DMA_INT_FATAL_BUS) != 0) {
        GMAC_LOG_ERROR("fatal DMA interrupt status=%08lx debug=%08lx", status, GmacRead32(adapter, GMAC_DEBUG));
    }
}

_Use_decl_annotations_
NTSTATUS
GmacEvtInterruptEnable(WDFINTERRUPT interrupt, WDFDEVICE device)
{
    GMAC_ADAPTER *adapter = GmacGetDeviceContext(device)->Adapter;
    if ((adapter == nullptr) || (adapter->Resources.Mac == nullptr)) {
        return STATUS_SUCCESS;
    }

    const GMAC_INTERRUPT_CONTEXT *interruptContext = GmacGetInterruptContext(interrupt);
    const LONG roleBit = 1L << static_cast<LONG>(interruptContext->Role);
    const LONG enabledRoles = InterlockedOr(&adapter->EnabledInterruptRoles, roleBit) | roleBit;

    UINT32 dmaEnable = 0U;
    if ((enabledRoles & (1L << GmacInterruptTx0)) != 0) {
        dmaEnable |= DMA_INT_TX_ENABLE;
    }
    if ((enabledRoles & (1L << GmacInterruptRx0)) != 0) {
        dmaEnable |= DMA_INT_RX_ENABLE;
    }
    if (dmaEnable != 0U) {
        dmaEnable |= DMA_INT_SUMMARY;
    }
    GmacWrite32(adapter, DMA_CHANNEL0_INTERRUPT_ENABLE, dmaEnable);

    // Only LPI is currently enabled at MAC/core level. Main remains a
    // diagnostic line for firmware/unexpected core events.
    GmacWrite32(
        adapter,
        GMAC_INTERRUPT_ENABLE,
        ((enabledRoles & (1L << GmacInterruptLpi)) != 0) ? GMAC_INT_LPI : 0U);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
GmacEvtInterruptDisable(WDFINTERRUPT interrupt, WDFDEVICE device)
{
    GMAC_ADAPTER *adapter = GmacGetDeviceContext(device)->Adapter;
    if ((adapter == nullptr) || (adapter->Resources.Mac == nullptr)) {
        return STATUS_SUCCESS;
    }

    const GMAC_INTERRUPT_CONTEXT *interruptContext = GmacGetInterruptContext(interrupt);
    const LONG roleBit = 1L << static_cast<LONG>(interruptContext->Role);
    const LONG enabledRoles = InterlockedAnd(&adapter->EnabledInterruptRoles, ~roleBit) & ~roleBit;

    UINT32 dmaEnable = 0U;
    if ((enabledRoles & (1L << GmacInterruptTx0)) != 0) {
        dmaEnable |= DMA_INT_TX_ENABLE;
    }
    if ((enabledRoles & (1L << GmacInterruptRx0)) != 0) {
        dmaEnable |= DMA_INT_RX_ENABLE;
    }
    if (dmaEnable != 0U) {
        dmaEnable |= DMA_INT_SUMMARY;
    }
    GmacWrite32(adapter, DMA_CHANNEL0_INTERRUPT_ENABLE, dmaEnable);
    GmacWrite32(
        adapter,
        GMAC_INTERRUPT_ENABLE,
        ((enabledRoles & (1L << GmacInterruptLpi)) != 0) ? GMAC_INT_LPI : 0U);
    return STATUS_SUCCESS;
}
