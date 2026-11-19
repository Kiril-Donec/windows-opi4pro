#pragma once

typedef struct _GMAC_TX_PACKET_CONTEXT {
    UINT32 DescriptorIndex;
    UINT32 DescriptorCount;
    UINT32 FragmentEnd;         // fragment ring index after this packet
} GMAC_TX_PACKET_CONTEXT;

typedef struct _GMAC_TX_QUEUE {
    GMAC_ADAPTER *Adapter;
    NET_RING_COLLECTION const *Rings;
    WDFCOMMONBUFFER DescriptorBuffer;
    GMAC_DESCRIPTOR *Descriptors;
    PHYSICAL_ADDRESS DescriptorLogical;
    WDFMEMORY PacketContextMemory;
    GMAC_TX_PACKET_CONTEXT *PacketContexts;
    UINT32 DescriptorCount;
    UINT32 Producer;
    // 0.2.2: TX frames are copied into this common buffer (below 4 GiB, one
    // GMAC_TX_SLOT_SIZE slot per descriptor), as XNU copies into its DMA
    // window. 0.2.1 board test: HAL scatter/gather mapping of NBLs above 4 GiB
    // for the 32-bit DMA profile spun in HalpDmaStartWcb (DPC watchdog).
    WDFCOMMONBUFFER BounceBuffer;
    PUCHAR BounceVirtual;
    PHYSICAL_ADDRESS BounceLogical;
    NET_EXTENSION VirtualAddressExtension;
} GMAC_TX_QUEUE;

#define GMAC_TX_SLOT_SIZE 2048U

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GMAC_TX_QUEUE, GmacGetTxQueueContext);

typedef struct _GMAC_RX_QUEUE {
    GMAC_ADAPTER *Adapter;
    NET_RING_COLLECTION const *Rings;
    WDFCOMMONBUFFER DescriptorBuffer;
    GMAC_DESCRIPTOR *Descriptors;
    PHYSICAL_ADDRESS DescriptorLogical;
    UINT32 DescriptorCount;
    NET_EXTENSION LogicalAddressExtension;
} GMAC_RX_QUEUE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GMAC_RX_QUEUE, GmacGetRxQueueContext);

NTSTATUS GmacInitializeTxQueue(_In_ NETPACKETQUEUE queue, _In_ GMAC_ADAPTER *adapter);
NTSTATUS GmacInitializeRxQueue(_In_ NETPACKETQUEUE queue, _In_ GMAC_ADAPTER *adapter);

EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED GmacEvtTxSetNotificationEnabled;
EVT_PACKET_QUEUE_ADVANCE GmacEvtTxAdvance;
EVT_PACKET_QUEUE_CANCEL GmacEvtTxCancel;
EVT_PACKET_QUEUE_START GmacEvtTxStart;
EVT_PACKET_QUEUE_STOP GmacEvtTxStop;
EVT_WDF_OBJECT_CONTEXT_DESTROY GmacEvtTxDestroy;

EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED GmacEvtRxSetNotificationEnabled;
EVT_PACKET_QUEUE_ADVANCE GmacEvtRxAdvance;
EVT_PACKET_QUEUE_CANCEL GmacEvtRxCancel;
EVT_PACKET_QUEUE_START GmacEvtRxStart;
EVT_PACKET_QUEUE_STOP GmacEvtRxStop;
EVT_WDF_OBJECT_CONTEXT_DESTROY GmacEvtRxDestroy;
