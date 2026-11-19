#pragma once

#define GMAC_MAX_FRAME_SIZE  1536U
#define GMAC_RING_HINT       64U
#define GMAC_INTERRUPT_COUNT  4U

// 0.2.1: the datapath is polled from a 1 ms high-resolution timer, as the
// working XNU driver polls from a thread. 0.2.0 board test #1 hit a DPC
// watchdog from an interrupt storm in GmacEvtInterruptIsr (a level line the
// ISR did not claim). The IRQ path stays in the source behind this switch
// until its GSIV-to-role mapping is verified on hardware.
#ifndef GMAC_USE_INTERRUPTS
#define GMAC_USE_INTERRUPTS   0
#endif
#define GMAC_POLL_PERIOD_MS   1U

typedef enum _GMAC_INTERRUPT_ROLE : ULONG {
    GmacInterruptMain = 0U,
    GmacInterruptLpi  = 1U,
    GmacInterruptTx0  = 2U,
    GmacInterruptRx0  = 3U,
} GMAC_INTERRUPT_ROLE;

typedef struct _GMAC_INTERRUPT_CONTEXT {
    GMAC_INTERRUPT_ROLE Role;
    ULONG ResourceIndex;
} GMAC_INTERRUPT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GMAC_INTERRUPT_CONTEXT, GmacGetInterruptContext);

typedef struct _GMAC_ADAPTER {
    WDFDEVICE WdfDevice;
    NETADAPTER NetAdapter;
    GMAC_RESOURCES Resources;
    GMAC_PLATFORM_RESOURCES Platform;
    WDFDMAENABLER DmaEnabler;
    WDFINTERRUPT Interrupts[GMAC_INTERRUPT_COUNT];
    WDFTIMER LinkTimer;
    WDFTIMER PollTimer;
    volatile LONG PollRunning;
    WDFSPINLOCK Lock;
    WDFSPINLOCK InterruptLock;
    NETPACKETQUEUE TxQueue;
    NETPACKETQUEUE RxQueue;
    NET_ADAPTER_LINK_LAYER_ADDRESS PermanentAddress;
    NET_ADAPTER_LINK_LAYER_ADDRESS CurrentAddress;
    NET_PACKET_FILTER_FLAGS PacketFilter;
    volatile LONG TxNotifyArmed;
    volatile LONG RxNotifyArmed;
    volatile LONG PendingInterrupts;
    volatile LONG EnabledInterruptRoles;
    BOOLEAN HardwarePrepared;
    BOOLEAN LinkUp;
    ULONG LinkSpeedMbps;
    BOOLEAN FullDuplex;
} GMAC_ADAPTER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GMAC_ADAPTER, GmacGetAdapterContext);

NTSTATUS GmacCreateAdapter(_In_ WDFDEVICE device);
NTSTATUS GmacStartAdapter(_In_ GMAC_ADAPTER *adapter);
VOID GmacSetLinkState(_In_ GMAC_ADAPTER *adapter, _In_ BOOLEAN connected, _In_ ULONG speedMbps, _In_ BOOLEAN fullDuplex);

EVT_NET_ADAPTER_CREATE_TXQUEUE GmacEvtCreateTxQueue;
EVT_NET_ADAPTER_CREATE_RXQUEUE GmacEvtCreateRxQueue;
EVT_NET_ADAPTER_SET_RECEIVE_FILTER GmacEvtSetPacketFilter;
EVT_WDF_TIMER GmacEvtLinkTimer;
EVT_WDF_TIMER GmacEvtPollTimer;
VOID GmacStartPolling(_In_ GMAC_ADAPTER *adapter);
VOID GmacStopPolling(_In_ GMAC_ADAPTER *adapter);
EVT_WDF_INTERRUPT_ISR GmacEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC GmacEvtInterruptDpc;
EVT_WDF_INTERRUPT_ENABLE GmacEvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE GmacEvtInterruptDisable;
