#pragma once

#define GMAC_CONFIGURATION                 0x0000U
#define GMAC_PACKET_FILTER                 0x0008U
#define GMAC_RX_QUEUE_CONTROL0             0x00A0U
#define GMAC_INTERRUPT_STATUS              0x00B0U
#define GMAC_INTERRUPT_ENABLE              0x00B4U
#define GMAC_LPI_CONTROL_STATUS             0x00D0U
#define GMAC_VERSION                       0x0110U
#define GMAC_DEBUG                         0x0114U
#define GMAC_HW_FEATURE0                   0x011CU
#define GMAC_HW_FEATURE1                   0x0120U
#define GMAC_HW_FEATURE2                   0x0124U
#define GMAC_HW_FEATURE3                   0x0128U
#define GMAC_MDIO_ADDRESS                  0x0200U
#define GMAC_MDIO_DATA                     0x0204U
#define GMAC_ADDRESS0_HIGH                 0x0300U
#define GMAC_ADDRESS0_LOW                  0x0304U

#define MTL_TX_QUEUE0_OPERATION_MODE       0x0D00U
#define MTL_RX_QUEUE0_OPERATION_MODE       0x0D30U

#define DMA_MODE                           0x1000U
#define DMA_SYSTEM_BUS_MODE                0x1004U
#define DMA_STATUS                         0x1008U
#define DMA_CHANNEL0_CONTROL               0x1100U
#define DMA_CHANNEL0_TX_CONTROL            0x1104U
#define DMA_CHANNEL0_RX_CONTROL            0x1108U
#define DMA_CHANNEL0_TX_BASE_HIGH          0x1110U
#define DMA_CHANNEL0_TX_BASE_LOW           0x1114U
#define DMA_CHANNEL0_RX_BASE_HIGH          0x1118U
#define DMA_CHANNEL0_RX_BASE_LOW           0x111CU
#define DMA_CHANNEL0_TX_TAIL_POINTER       0x1120U
#define DMA_CHANNEL0_RX_TAIL_POINTER       0x1128U
#define DMA_CHANNEL0_TX_RING_LENGTH        0x112CU
#define DMA_CHANNEL0_RX_RING_LENGTH        0x1130U
#define DMA_CHANNEL0_INTERRUPT_ENABLE      0x1134U
#define DMA_CHANNEL0_STATUS                0x1160U

#define GMAC_WRAPPER_CONFIGURATION         0x0000U
#define GMAC_WRAPPER_STATUS                0x004CU
#define GMAC_WRAPPER_CLOCK                 0x0080U

#define GMAC_CONFIG_RE                     (1U << 0)
#define GMAC_CONFIG_TE                     (1U << 1)
#define GMAC_CONFIG_DM                     (1U << 13)
#define GMAC_CONFIG_FES                    (1U << 14)
#define GMAC_CONFIG_PS                     (1U << 15)
#define GMAC_CONFIG_JE                     (1U << 16)
#define GMAC_CONFIG_JD                     (1U << 17)
#define GMAC_CONFIG_BE                     (1U << 18)
#define GMAC_CONFIG_DCRS                   (1U << 9)

#define GMAC_FILTER_PR                     (1U << 0)
#define GMAC_FILTER_HMC                    (1U << 2)
#define GMAC_FILTER_PM                     (1U << 4)
#define GMAC_FILTER_PCF_ALL                (2U << 6)
#define GMAC_FILTER_RA                     (1U << 31)

#define DMA_MODE_SOFTWARE_RESET            (1U << 0)
#define DMA_MODE_INTM_MASK                 (3U << 16)
#define DMA_MODE_INTM_MODE1                (1U << 16)
#define DMA_SYSTEM_BUS_FIXED_BURST         (1U << 0)
#define DMA_SYSTEM_BUS_ADDRESS_ALIGNED     (1U << 12)
#define DMA_SYSTEM_BUS_MIXED_BURST         (1U << 14)
#define DMA_SYSTEM_BUS_BLEN4               (1U << 1)
#define DMA_SYSTEM_BUS_BLEN8               (1U << 2)
#define DMA_SYSTEM_BUS_BLEN16              (1U << 3)

#define DMA_TX_START                       (1U << 0)
#define DMA_TX_OPERATE_SECOND_PACKET       (1U << 4)
#define DMA_RX_START                       (1U << 0)
#define DMA_RX_BUFFER_SIZE_SHIFT           1U

#define DMA_INT_TX                         (1U << 0)
#define DMA_INT_TX_STOP                    (1U << 1)
#define DMA_INT_TX_BUFFER_UNAVAILABLE      (1U << 2)
#define DMA_INT_RX                         (1U << 6)
#define DMA_INT_RX_BUFFER_UNAVAILABLE      (1U << 7)
#define DMA_INT_FATAL_BUS                  (1U << 12)
#define DMA_INT_ABNORMAL_SUMMARY           (1U << 14)
#define DMA_INT_NORMAL_SUMMARY             (1U << 15)
#define DMA_INT_SUMMARY                    (DMA_INT_ABNORMAL_SUMMARY | DMA_INT_NORMAL_SUMMARY)
#define DMA_INT_TX_CAUSE                   (DMA_INT_TX | DMA_INT_TX_STOP | DMA_INT_TX_BUFFER_UNAVAILABLE | DMA_INT_FATAL_BUS)
#define DMA_INT_RX_CAUSE                   (DMA_INT_RX | DMA_INT_RX_BUFFER_UNAVAILABLE | DMA_INT_FATAL_BUS)
#define DMA_INT_TX_ENABLE                  (DMA_INT_TX | DMA_INT_FATAL_BUS)
#define DMA_INT_RX_ENABLE                  (DMA_INT_RX | DMA_INT_RX_BUFFER_UNAVAILABLE | DMA_INT_FATAL_BUS)
#define DMA_INT_DEFAULT                    (DMA_INT_TX_ENABLE | DMA_INT_RX_ENABLE | DMA_INT_SUMMARY)

#define GMAC_INT_LPI                       (1U << 5)

#define MTL_TX_QUEUE_ENABLE                (2U << 2)
#define MTL_TX_STORE_AND_FORWARD           (1U << 1)
#define MTL_RX_STORE_AND_FORWARD           (1U << 5)

#define MDIO_BUSY                          (1U << 0)
#define MDIO_WRITE                         (1U << 2)
#define MDIO_READ                          (3U << 2)
#define MDIO_CLOCK_RANGE_100_150_MHZ       (1U << 8)
// CSR clock / 124: MDC stays under 2.5 MHz up to a 300 MHz CSR clock. This is
// the value of the working XNU driver (a733_eth.c MDIO_CR).
#define MDIO_CLOCK_RANGE_CSR_DIV124        (5U << 8)
#define MDIO_CLOCK_RANGE                   MDIO_CLOCK_RANGE_CSR_DIV124

// sunxi-gmac-210 wrapper (syscon): EPIT (RGMII), ETCS=1 (TX clock from the PHY
// CLK_OUT pin), ERXDC=13, ETXDC=12. Calibrated by PHY loopback in the XNU port
// (board test #19: 1000 Mbit/s passes at rx 3..13, 100 Mbit/s everywhere).
#define A733_GMAC_WRAPPER_RGMII            0x000111A5U

// Motorcomm YT8531 (PHY id 4f51:e91b) extended registers
#define YT8531_SYNCE_CFG                   0xA012U
#define YT8531_SCR_ENABLE                  (1U << 6)
#define YT8531_SCR_125M                    (1U << 4)
#define YT8531_SCR_SRC_MASK                (7U << 1)
#define YT8531_SLEEP_CTRL1                 0x0027U
#define YT8531_SLEEP_SW                    (1U << 15)
#define PHY_ANAR                           4U
#define PHY_GBCR                           9U
#define MDIO_REGISTER_SHIFT                16U
#define MDIO_PHY_SHIFT                     21U

#define TX_DESC_IOC                        (1U << 31)
#define TX_DESC_LAST                       (1U << 28)
#define TX_DESC_FIRST                      (1U << 29)
#define TX_DESC_OWN                        (1U << 31)
#define RX_DESC_BUFFER1_VALID              (1U << 24)
#define RX_DESC_IOC                        (1U << 30)
#define RX_DESC_OWN                        (1U << 31)
#define RX_DESC_FIRST                      (1U << 29)
#define RX_DESC_LAST                       (1U << 28)
#define RX_DESC_ERROR                      (1U << 15)
#define RX_DESC_LENGTH_MASK                0x7FFFU

#define PHY_ADDRESS                        1U
#define PHY_BMCR                           0U
#define PHY_BMSR                           1U
#define PHY_ID1                            2U
#define PHY_ID2                            3U
#define PHY_EXT_ADDRESS                    0x1EU
#define PHY_EXT_DATA                       0x1FU
#define PHY_BMCR_RESET                     (1U << 15)
#define PHY_BMCR_AUTONEG_ENABLE            (1U << 12)
#define PHY_BMCR_RESTART_AUTONEG           (1U << 9)
#define PHY_BMSR_LINK                      (1U << 2)
#define PHY_BMSR_AUTONEG_COMPLETE          (1U << 5)

typedef struct _GMAC_DESCRIPTOR {
    UINT32 Word0;
    UINT32 Word1;
    UINT32 Word2;
    UINT32 Word3;
} GMAC_DESCRIPTOR;

static_assert(sizeof(GMAC_DESCRIPTOR) == 16, "DWMAC descriptor size");
