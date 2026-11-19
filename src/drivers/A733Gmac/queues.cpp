#include "precomp.h"

static UINT32 GmacNextDescriptor(UINT32 index, UINT32 count)
{
    return (index + 1U) % count;
}

static PHYSICAL_ADDRESS GmacDescriptorAddress(PHYSICAL_ADDRESS base, UINT32 index)
{
    PHYSICAL_ADDRESS address;
    address.QuadPart = base.QuadPart + (static_cast<ULONGLONG>(index) * sizeof(GMAC_DESCRIPTOR));
    return address;
}

static VOID GmacSetTxNotification(GMAC_TX_QUEUE *tx, BOOLEAN enabled)
{
    InterlockedExchange(&tx->Adapter->TxNotifyArmed, enabled);
    if (!enabled) {
        KeFlushQueuedDpcs();
    }
}

static VOID GmacSetRxNotification(GMAC_RX_QUEUE *rx, BOOLEAN enabled)
{
    InterlockedExchange(&rx->Adapter->RxNotifyArmed, enabled);
    if (!enabled) {
        KeFlushQueuedDpcs();
    }
}

static VOID GmacCompleteTx(GMAC_TX_QUEUE *tx)
{
    NET_RING_PACKET_ITERATOR packets = NetRingGetDrainPackets(tx->Rings);
    NET_RING *fragmentRing = NetRingCollectionGetFragmentRing(tx->Rings);

    while (NetPacketIteratorHasAny(&packets)) {
        const UINT32 packetIndex = NetPacketIteratorGetIndex(&packets);
        GMAC_TX_PACKET_CONTEXT *context = &tx->PacketContexts[packetIndex];
        if (context->DescriptorCount != 0U) {
            MemoryBarrier();
            if ((tx->Descriptors[context->DescriptorIndex].Word3 & TX_DESC_OWN) != 0U) {
                break;
            }
            RtlZeroMemory(&tx->Descriptors[context->DescriptorIndex], sizeof(GMAC_DESCRIPTOR));
            context->DescriptorCount = 0U;
        }
        // Frames were copied into the bounce slot at post time, so the
        // fragments go back to NetAdapterCx as soon as the packet drains.
        fragmentRing->BeginIndex = context->FragmentEnd;
        NetPacketIteratorAdvance(&packets);
    }
    NetPacketIteratorSet(&packets);
}
static VOID GmacPostTx(GMAC_TX_QUEUE *tx)
{
    NET_RING_PACKET_ITERATOR packets = NetRingGetPostPackets(tx->Rings);
    NET_RING *fragmentRing = NetRingCollectionGetFragmentRing(tx->Rings);
    BOOLEAN posted = FALSE;

    while (NetPacketIteratorHasAny(&packets)) {
        NET_PACKET *packet = NetPacketIteratorGetPacket(&packets);
        const UINT32 packetIndex = NetPacketIteratorGetIndex(&packets);
        if (packet->Ignore) {
            // Already dropped by the stack: just step over its fragments.
            GMAC_TX_PACKET_CONTEXT *dropped = &tx->PacketContexts[packetIndex];
            UINT32 end = (packet->FragmentCount != 0U) ? packet->FragmentIndex : fragmentRing->NextIndex;
            for (UINT32 count = 0; count < packet->FragmentCount; ++count) {
                end = NetRingIncrementIndex(fragmentRing, end);
            }
            dropped->DescriptorCount = 0U;
            dropped->FragmentEnd = end;
            fragmentRing->NextIndex = end;
        } else {
            GMAC_DESCRIPTOR *descriptor = &tx->Descriptors[tx->Producer];
            if ((descriptor->Word3 & TX_DESC_OWN) != 0U) {
                break;
            }

            // Gather every fragment of the packet into this descriptor's
            // bounce slot (below 4 GiB, uncached for _CCA=0).
            PUCHAR slot = tx->BounceVirtual + (static_cast<SIZE_T>(tx->Producer) * GMAC_TX_SLOT_SIZE);
            UINT32 length = 0U;
            UINT32 fragmentIndex = packet->FragmentIndex;
            BOOLEAN fits = TRUE;
            for (UINT32 count = 0; count < packet->FragmentCount; ++count) {
                const NET_FRAGMENT *fragment = NetRingGetFragmentAtIndex(fragmentRing, fragmentIndex);
                const NET_FRAGMENT_VIRTUAL_ADDRESS *virt =
                    NetExtensionGetFragmentVirtualAddress(&tx->VirtualAddressExtension, fragmentIndex);
                const SIZE_T chunk = static_cast<SIZE_T>(fragment->ValidLength);
                if ((length + chunk) > GMAC_TX_SLOT_SIZE) {
                    fits = FALSE;
                    break;
                }
                RtlCopyMemory(slot + length,
                    static_cast<const UCHAR *>(virt->VirtualAddress) + fragment->Offset, chunk);
                length += static_cast<UINT32>(chunk);
                fragmentIndex = NetRingIncrementIndex(fragmentRing, fragmentIndex);
            }

            GMAC_TX_PACKET_CONTEXT *context = &tx->PacketContexts[packetIndex];
            if (!fits || (length == 0U)) {
                // Oversized or empty: drop it, the completion path returns it.
                UINT32 end = packet->FragmentIndex;
                for (UINT32 count = 0; count < packet->FragmentCount; ++count) {
                    end = NetRingIncrementIndex(fragmentRing, end);
                }
                packet->Ignore = 1U;
                context->DescriptorCount = 0U;
                context->FragmentEnd = end;
                fragmentRing->NextIndex = end;
            } else {
                context->DescriptorIndex = tx->Producer;
                context->DescriptorCount = 1U;
                context->FragmentEnd = fragmentIndex;
                const ULONGLONG address = static_cast<ULONGLONG>(tx->BounceLogical.QuadPart) +
                    (static_cast<ULONGLONG>(tx->Producer) * GMAC_TX_SLOT_SIZE);
                descriptor->Word0 = static_cast<UINT32>(address);
                descriptor->Word1 = static_cast<UINT32>(address >> 32);
                descriptor->Word2 = length;
                MemoryBarrier();
                descriptor->Word3 = length | TX_DESC_FIRST | TX_DESC_LAST | TX_DESC_OWN;
                tx->Producer = GmacNextDescriptor(tx->Producer, tx->DescriptorCount);
                fragmentRing->NextIndex = fragmentIndex;
                posted = TRUE;
            }
        }
        NetPacketIteratorAdvance(&packets);
    }
    NetPacketIteratorSet(&packets);

    if (posted) {
        MemoryBarrier();
        const PHYSICAL_ADDRESS tail = GmacDescriptorAddress(tx->DescriptorLogical, tx->Producer);
        GmacWrite32(tx->Adapter, DMA_CHANNEL0_TX_TAIL_POINTER, tail.LowPart);
    }
}

static VOID GmacIndicateRx(GMAC_RX_QUEUE *rx)
{
    NET_RING_FRAGMENT_ITERATOR fragments = NetRingGetDrainFragments(rx->Rings);
    NET_RING_PACKET_ITERATOR packets = NetRingGetAllPackets(rx->Rings);

    while (NetFragmentIteratorHasAny(&fragments) && NetPacketIteratorHasAny(&packets)) {
        const UINT32 index = NetFragmentIteratorGetIndex(&fragments);
        const GMAC_DESCRIPTOR *descriptor = &rx->Descriptors[index];
        MemoryBarrier();
        if ((descriptor->Word3 & RX_DESC_OWN) != 0U) {
            break;
        }

        NET_FRAGMENT *fragment = NetFragmentIteratorGetFragment(&fragments);
        NET_PACKET *packet = NetPacketIteratorGetPacket(&packets);
        const UINT32 status = descriptor->Word3;
        const UINT32 wireLength = status & RX_DESC_LENGTH_MASK;
        const BOOLEAN valid = ((status & RX_DESC_ERROR) == 0U) &&
            ((status & (RX_DESC_FIRST | RX_DESC_LAST)) == (RX_DESC_FIRST | RX_DESC_LAST)) &&
            (wireLength >= 4U) && ((wireLength - 4U) <= fragment->Capacity);

        fragment->Offset = 0U;
        fragment->ValidLength = valid ? (wireLength - 4U) : 0U;
        packet->FragmentIndex = index;
        packet->FragmentCount = 1U;
        packet->Ignore = valid ? 0U : 1U;
        if (valid) {
            packet->Layout.Layer2Type = NetPacketLayer2TypeEthernet;
        }
        NetFragmentIteratorAdvance(&fragments);
        NetPacketIteratorAdvance(&packets);
    }
    NetFragmentIteratorSet(&fragments);
    NetPacketIteratorSet(&packets);
}

static VOID GmacPostRx(GMAC_RX_QUEUE *rx)
{
    NET_RING_FRAGMENT_ITERATOR fragments = NetRingGetPostFragments(rx->Rings);
    while (NetFragmentIteratorHasAny(&fragments)) {
        const UINT32 index = NetFragmentIteratorGetIndex(&fragments);
        NET_FRAGMENT *fragment = NetFragmentIteratorGetFragment(&fragments);
        const NET_FRAGMENT_LOGICAL_ADDRESS *logical =
            NetExtensionGetFragmentLogicalAddress(&rx->LogicalAddressExtension, index);
        GMAC_DESCRIPTOR *descriptor = &rx->Descriptors[index];
        const ULONGLONG address = logical->LogicalAddress + fragment->Offset;
        descriptor->Word0 = static_cast<UINT32>(address);
        descriptor->Word1 = static_cast<UINT32>(address >> 32);
        descriptor->Word2 = 0U;
        MemoryBarrier();
        descriptor->Word3 = RX_DESC_OWN | RX_DESC_IOC | RX_DESC_BUFFER1_VALID;
        NetFragmentIteratorAdvance(&fragments);
    }
    NetFragmentIteratorSet(&fragments);
    MemoryBarrier();
    PHYSICAL_ADDRESS tail = rx->DescriptorLogical;
    tail.QuadPart += static_cast<ULONGLONG>(rx->DescriptorCount) * sizeof(GMAC_DESCRIPTOR);
    GmacWrite32(rx->Adapter, DMA_CHANNEL0_RX_TAIL_POINTER, tail.LowPart);
}

_Use_decl_annotations_
NTSTATUS GmacInitializeTxQueue(NETPACKETQUEUE queue, GMAC_ADAPTER *adapter)
{
    GMAC_TX_QUEUE *tx = GmacGetTxQueueContext(queue);
    tx->Adapter = adapter;
    tx->Rings = NetTxQueueGetRingCollection(queue);
    NET_RING *packetRing = NetRingCollectionGetPacketRing(tx->Rings);
    NET_RING *fragmentRing = NetRingCollectionGetFragmentRing(tx->Rings);
    if ((packetRing == nullptr) || (packetRing->NumberOfElements == 0U)) {
        GMAC_LOG_ERROR("%s", "TX packet ring is empty");
        return STATUS_INVALID_BUFFER_SIZE;
    }
    if (fragmentRing == nullptr) {
        GMAC_LOG_ERROR("%s", "TX fragment ring is missing");
        return STATUS_INVALID_BUFFER_SIZE;
    }
    tx->DescriptorCount = fragmentRing->NumberOfElements;
    if ((tx->DescriptorCount == 0U) || (tx->DescriptorCount > 1024U)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    ULONG descriptorBytes = 0U;
    NTSTATUS status = RtlULongMult(tx->DescriptorCount, sizeof(GMAC_DESCRIPTOR), &descriptorBytes);
    if (!NT_SUCCESS(status)) return status;
    status = WdfCommonBufferCreate(adapter->DmaEnabler, descriptorBytes,
        WDF_NO_OBJECT_ATTRIBUTES, &tx->DescriptorBuffer);
    if (!NT_SUCCESS(status)) return status;
    tx->Descriptors = static_cast<GMAC_DESCRIPTOR *>(
        WdfCommonBufferGetAlignedVirtualAddress(tx->DescriptorBuffer));
    tx->DescriptorLogical = WdfCommonBufferGetAlignedLogicalAddress(tx->DescriptorBuffer);

    ULONG bounceBytes = 0U;
    status = RtlULongMult(tx->DescriptorCount, GMAC_TX_SLOT_SIZE, &bounceBytes);
    if (!NT_SUCCESS(status)) return status;
    status = WdfCommonBufferCreate(adapter->DmaEnabler, bounceBytes,
        WDF_NO_OBJECT_ATTRIBUTES, &tx->BounceBuffer);
    if (!NT_SUCCESS(status)) {
        GMAC_LOG_ERROR("TX bounce buffer %lu bytes failed %08lx", bounceBytes, status);
        return status;
    }
    tx->BounceVirtual = static_cast<PUCHAR>(WdfCommonBufferGetAlignedVirtualAddress(tx->BounceBuffer));
    tx->BounceLogical = WdfCommonBufferGetAlignedLogicalAddress(tx->BounceBuffer);
    GMAC_LOG_ERROR("TX ring %lu descriptors at %llx, bounce %lu bytes at %llx (copy TX, polled)",
        tx->DescriptorCount, tx->DescriptorLogical.QuadPart, bounceBytes, tx->BounceLogical.QuadPart);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = queue;
    SIZE_T packetContextBytes = 0U;
    status = RtlSizeTMult(
        sizeof(GMAC_TX_PACKET_CONTEXT),
        static_cast<SIZE_T>(packetRing->NumberOfElements),
        &packetContextBytes);
    if (!NT_SUCCESS(status) || (packetContextBytes == 0U)) {
        return NT_SUCCESS(status) ? STATUS_INVALID_BUFFER_SIZE : status;
    }
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, GMAC_POOL_TAG,
        packetContextBytes,
        &tx->PacketContextMemory, reinterpret_cast<PVOID *>(&tx->PacketContexts));
    if (!NT_SUCCESS(status)) return status;
    RtlZeroMemory(tx->PacketContexts, packetContextBytes);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS GmacInitializeRxQueue(NETPACKETQUEUE queue, GMAC_ADAPTER *adapter)
{
    GMAC_RX_QUEUE *rx = GmacGetRxQueueContext(queue);
    rx->Adapter = adapter;
    rx->Rings = NetRxQueueGetRingCollection(queue);
    NET_RING *fragmentRing = NetRingCollectionGetFragmentRing(rx->Rings);
    if (fragmentRing == nullptr) {
        GMAC_LOG_ERROR("%s", "RX fragment ring is missing");
        return STATUS_INVALID_BUFFER_SIZE;
    }
    rx->DescriptorCount = fragmentRing->NumberOfElements;
    if ((rx->DescriptorCount == 0U) || (rx->DescriptorCount > 1024U)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    ULONG descriptorBytes = 0U;
    NTSTATUS status = RtlULongMult(rx->DescriptorCount, sizeof(GMAC_DESCRIPTOR), &descriptorBytes);
    if (!NT_SUCCESS(status)) return status;
    status = WdfCommonBufferCreate(adapter->DmaEnabler, descriptorBytes,
        WDF_NO_OBJECT_ATTRIBUTES, &rx->DescriptorBuffer);
    if (!NT_SUCCESS(status)) return status;
    rx->Descriptors = static_cast<GMAC_DESCRIPTOR *>(
        WdfCommonBufferGetAlignedVirtualAddress(rx->DescriptorBuffer));
    rx->DescriptorLogical = WdfCommonBufferGetAlignedLogicalAddress(rx->DescriptorBuffer);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS GmacEvtCreateTxQueue(NETADAPTER netAdapter, NETTXQUEUE_INIT *queueInit)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, GMAC_TX_QUEUE);
    attributes.EvtDestroyCallback = GmacEvtTxDestroy;
    NET_PACKET_QUEUE_CONFIG config;
    NET_PACKET_QUEUE_CONFIG_INIT(&config, GmacEvtTxAdvance, GmacEvtTxSetNotificationEnabled, GmacEvtTxCancel);
    config.EvtStart = GmacEvtTxStart;
    config.EvtStop = GmacEvtTxStop;
    NETPACKETQUEUE queue = WDF_NO_HANDLE;
    NTSTATUS status = NetTxQueueCreate(queueInit, &attributes, &config, &queue);
    if (!NT_SUCCESS(status)) return status;
    GMAC_TX_QUEUE *tx = GmacGetTxQueueContext(queue);
    NET_EXTENSION_QUERY extension;
    NET_EXTENSION_QUERY_INIT(&extension, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
        NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetTxQueueGetExtension(queue, &extension, &tx->VirtualAddressExtension);
    return GmacInitializeTxQueue(queue, GmacGetAdapterContext(netAdapter));
}

_Use_decl_annotations_ NTSTATUS GmacEvtCreateRxQueue(NETADAPTER netAdapter, NETRXQUEUE_INIT *queueInit)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, GMAC_RX_QUEUE);
    attributes.EvtDestroyCallback = GmacEvtRxDestroy;
    NET_PACKET_QUEUE_CONFIG config;
    NET_PACKET_QUEUE_CONFIG_INIT(&config, GmacEvtRxAdvance, GmacEvtRxSetNotificationEnabled, GmacEvtRxCancel);
    config.EvtStart = GmacEvtRxStart;
    config.EvtStop = GmacEvtRxStop;
    NETPACKETQUEUE queue = WDF_NO_HANDLE;
    NTSTATUS status = NetRxQueueCreate(queueInit, &attributes, &config, &queue);
    if (!NT_SUCCESS(status)) return status;
    GMAC_RX_QUEUE *rx = GmacGetRxQueueContext(queue);
    NET_EXTENSION_QUERY extension;
    NET_EXTENSION_QUERY_INIT(&extension, NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
        NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetRxQueueGetExtension(queue, &extension, &rx->LogicalAddressExtension);
    return GmacInitializeRxQueue(queue, GmacGetAdapterContext(netAdapter));
}

_Use_decl_annotations_ VOID GmacEvtTxAdvance(NETPACKETQUEUE queue)
{
    GMAC_TX_QUEUE *tx = GmacGetTxQueueContext(queue);
    GmacCompleteTx(tx);
    GmacPostTx(tx);
}

_Use_decl_annotations_ VOID GmacEvtTxSetNotificationEnabled(NETPACKETQUEUE queue, BOOLEAN enabled)
{
    GmacSetTxNotification(GmacGetTxQueueContext(queue), enabled);
}

_Use_decl_annotations_ VOID GmacEvtTxCancel(NETPACKETQUEUE queue)
{
    // Frames live in our bounce slots, so every packet can go back at once.
    GMAC_TX_QUEUE *tx = GmacGetTxQueueContext(queue);
    GmacWrite32(tx->Adapter, DMA_CHANNEL0_TX_CONTROL,
        GmacRead32(tx->Adapter, DMA_CHANNEL0_TX_CONTROL) & ~DMA_TX_START);
    NET_RING_PACKET_ITERATOR packets = NetRingGetAllPackets(tx->Rings);
    NetPacketIteratorAdvanceToTheEnd(&packets);
    NetPacketIteratorSet(&packets);
    NET_RING_FRAGMENT_ITERATOR fragments = NetRingGetAllFragments(tx->Rings);
    NetFragmentIteratorAdvanceToTheEnd(&fragments);
    NetFragmentIteratorSet(&fragments);
}

_Use_decl_annotations_ VOID GmacEvtTxStart(NETPACKETQUEUE queue)
{
    GMAC_TX_QUEUE *tx = GmacGetTxQueueContext(queue);
    RtlZeroMemory(tx->Descriptors, tx->DescriptorCount * sizeof(GMAC_DESCRIPTOR));
    tx->Producer = 0U;
    GmacWrite32(tx->Adapter, DMA_CHANNEL0_TX_BASE_HIGH, tx->DescriptorLogical.HighPart);
    GmacWrite32(tx->Adapter, DMA_CHANNEL0_TX_BASE_LOW, tx->DescriptorLogical.LowPart);
    GmacWrite32(tx->Adapter, DMA_CHANNEL0_TX_RING_LENGTH, tx->DescriptorCount - 1U);
    tx->Adapter->TxQueue = queue;
    GmacWrite32(tx->Adapter, DMA_CHANNEL0_TX_CONTROL,
        GmacRead32(tx->Adapter, DMA_CHANNEL0_TX_CONTROL) | DMA_TX_START | DMA_TX_OPERATE_SECOND_PACKET);
    GmacUpdateConfigurationBits(tx->Adapter, 0U, GMAC_CONFIG_TE);
}

_Use_decl_annotations_ VOID GmacEvtTxStop(NETPACKETQUEUE queue)
{
    GMAC_TX_QUEUE *tx = GmacGetTxQueueContext(queue);
    GmacSetTxNotification(tx, FALSE);
    GmacWrite32(tx->Adapter, DMA_CHANNEL0_TX_CONTROL,
        GmacRead32(tx->Adapter, DMA_CHANNEL0_TX_CONTROL) & ~DMA_TX_START);
    GmacUpdateConfigurationBits(tx->Adapter, GMAC_CONFIG_TE, 0U);
    tx->Adapter->TxQueue = WDF_NO_HANDLE;
}

_Use_decl_annotations_ VOID GmacEvtTxDestroy(WDFOBJECT object)
{
    GMAC_TX_QUEUE *tx = GmacGetTxQueueContext(object);
    tx->DescriptorBuffer = WDF_NO_HANDLE;
    tx->BounceBuffer = WDF_NO_HANDLE;
    tx->PacketContextMemory = WDF_NO_HANDLE;
}

_Use_decl_annotations_ VOID GmacEvtRxAdvance(NETPACKETQUEUE queue)
{
    GMAC_RX_QUEUE *rx = GmacGetRxQueueContext(queue);
    GmacIndicateRx(rx);
    GmacPostRx(rx);
}

_Use_decl_annotations_ VOID GmacEvtRxSetNotificationEnabled(NETPACKETQUEUE queue, BOOLEAN enabled)
{
    GmacSetRxNotification(GmacGetRxQueueContext(queue), enabled);
}

_Use_decl_annotations_ VOID GmacEvtRxCancel(NETPACKETQUEUE queue)
{
    GMAC_RX_QUEUE *rx = GmacGetRxQueueContext(queue);
    GmacWrite32(rx->Adapter, DMA_CHANNEL0_RX_CONTROL,
        GmacRead32(rx->Adapter, DMA_CHANNEL0_RX_CONTROL) & ~DMA_RX_START);
    GmacIndicateRx(rx);
    NET_RING_PACKET_ITERATOR packets = NetRingGetAllPackets(rx->Rings);
    while (NetPacketIteratorHasAny(&packets)) {
        NetPacketIteratorGetPacket(&packets)->Ignore = 1U;
        NetPacketIteratorAdvance(&packets);
    }
    NetPacketIteratorSet(&packets);
    NET_RING_FRAGMENT_ITERATOR fragments = NetRingGetAllFragments(rx->Rings);
    NetFragmentIteratorAdvanceToTheEnd(&fragments);
    NetFragmentIteratorSet(&fragments);
}

_Use_decl_annotations_ VOID GmacEvtRxStart(NETPACKETQUEUE queue)
{
    GMAC_RX_QUEUE *rx = GmacGetRxQueueContext(queue);
    RtlZeroMemory(rx->Descriptors, rx->DescriptorCount * sizeof(GMAC_DESCRIPTOR));
    GmacWrite32(rx->Adapter, DMA_CHANNEL0_RX_BASE_HIGH, rx->DescriptorLogical.HighPart);
    GmacWrite32(rx->Adapter, DMA_CHANNEL0_RX_BASE_LOW, rx->DescriptorLogical.LowPart);
    GmacWrite32(rx->Adapter, DMA_CHANNEL0_RX_RING_LENGTH, rx->DescriptorCount - 1U);
    rx->Adapter->RxQueue = queue;
    GmacWrite32(rx->Adapter, DMA_CHANNEL0_RX_CONTROL,
        GmacRead32(rx->Adapter, DMA_CHANNEL0_RX_CONTROL) |
        DMA_RX_START | (GMAC_MAX_FRAME_SIZE << DMA_RX_BUFFER_SIZE_SHIFT));
    GmacUpdateConfigurationBits(rx->Adapter, 0U, GMAC_CONFIG_RE);
}

_Use_decl_annotations_ VOID GmacEvtRxStop(NETPACKETQUEUE queue)
{
    GMAC_RX_QUEUE *rx = GmacGetRxQueueContext(queue);
    GmacSetRxNotification(rx, FALSE);
    GmacWrite32(rx->Adapter, DMA_CHANNEL0_RX_CONTROL,
        GmacRead32(rx->Adapter, DMA_CHANNEL0_RX_CONTROL) & ~DMA_RX_START);
    GmacUpdateConfigurationBits(rx->Adapter, GMAC_CONFIG_RE, 0U);
    rx->Adapter->RxQueue = WDF_NO_HANDLE;
}

_Use_decl_annotations_ VOID GmacEvtRxDestroy(WDFOBJECT object)
{
    GMAC_RX_QUEUE *rx = GmacGetRxQueueContext(object);
    rx->DescriptorBuffer = WDF_NO_HANDLE;
}
