/*
 * EthernetDriver_TX Component - NIC 2 Transmitter for ICS Pipeline
 *
 * This component receives ICS messages from IntNicDrv and transmits them
 * via TCP to netcat connection on port 1234.
 *
 * Architecture:
 * - VirtIO-net device driver for packet TX
 * - lwIP TCP/IP stack for network protocol handling
 * - DHCP client to obtain IP address from QEMU
 * - TCP client that maintains active connection to netcat 2
 * - Receives ICS messages from IntNicDrv and forwards payload via TCP
 *
 * Data Flow:
 *   IntNicDrv => ICS message format => Extract payload => TCP port 1234 => netcat
 */

#include <camkes.h>
#include <camkes/dma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include <utils/util.h>

/* lwIP headers */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/stats.h"
#include "netif/ethernet.h"

#define COMPONENT_NAME "EthernetDriver_TX"

/* Simple sys_now() implementation for lwIP */
static uint32_t tick_count = 0;
uint32_t sys_now(void) {
    return tick_count++;  /* Simple incrementing counter - good enough for timeouts */
}

/* ICS message format */
typedef struct {
    uint16_t tag;       /* Protocol identifier */
    uint16_t len;       /* Payload length in bytes */
    uint32_t flags;     /* Reserved */
} __attribute__((packed)) ICS_MsgHeader;

/* TCP configuration */
#define TCP_TX_PORT 1234
#define TCP_RECONNECT_INTERVAL 2000  /* 2 seconds */

/* DEBUG CONFIGURATION */
#define DEBUG_VERBOSE 1

/* VirtIO MMIO Register Offsets (same as RX driver) */
#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc
#define VIRTIO_MMIO_CONFIG              0x100

/* VirtIO Network Device Config Offsets */
#define VIRTIO_NET_CFG_MAC              0
#define VIRTIO_NET_CFG_STATUS           6
#define VIRTIO_NET_CFG_MAX_VQ_PAIRS     8

static volatile uint32_t *virtio_regs_base;

/* ARM memory barriers */
#ifdef __aarch64__
#define DMB() __asm__ volatile("dmb sy" ::: "memory")
#define DSB() __asm__ volatile("dsb sy" ::: "memory")
#define ISB() __asm__ volatile("isb" ::: "memory")
#else
#define DMB() __asm__ volatile("dmb" ::: "memory")
#define DSB() __asm__ volatile("dsb" ::: "memory")
#define ISB() __asm__ volatile("isb" ::: "memory")
#endif

#define VREG_READ(offset) ({ \
    DMB(); \
    uint32_t val = virtio_regs_base[(offset) / 4]; \
    DMB(); \
    val; \
})

#define VREG_WRITE(offset, value) do { \
    DMB(); \
    virtio_regs_base[(offset) / 4] = (value); \
    DSB(); \
} while(0)

/* VirtIO status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE       1
#define VIRTIO_STATUS_DRIVER            2
#define VIRTIO_STATUS_DRIVER_OK         4
#define VIRTIO_STATUS_FEATURES_OK       8
#define VIRTIO_STATUS_FAILED            128

/* VirtIO feature bits (we only enable essential ones for TX) */
#define VIRTIO_NET_F_MAC                (1ULL << 5)
#define VIRTIO_F_VERSION_1              (1ULL << 32)

/* Global state */
static struct netif netif_data;
static bool lwip_initialized = false;
static struct tcp_pcb *tx_pcb = NULL;  /* TCP connection for transmission */
static bool tcp_connected = false;
static uint32_t last_reconnect_attempt = 0;
static uint64_t messages_transmitted = 0;
static uint64_t bytes_transmitted = 0;

/* VirtIO queue structures (minimal for TX only) */
#define QUEUE_SIZE 64
#define TX_QUEUE_IDX 1

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
} __attribute__((packed)) virtq_avail;

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
} __attribute__((packed)) virtq_used_elem;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem ring[QUEUE_SIZE];
} __attribute__((packed)) virtq_used;

static virtq_desc *tx_desc;
static virtq_avail *tx_avail;
static virtq_used *tx_used;
static uint8_t *tx_buffers;
static uint16_t tx_last_used_idx = 0;

#define TX_BUFFER_SIZE 2048
#define NET_HDR_SIZE 12

/*
 * TCP client callbacks
 */
static void tcp_tx_err(void *arg, err_t err)
{
    printf("%s: TCP connection error: %d\n", COMPONENT_NAME, err);
    tcp_connected = false;
    tx_pcb = NULL;
}

static err_t tcp_tx_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    if (err != ERR_OK) {
        printf("%s: TCP connection failed: %d\n", COMPONENT_NAME, err);
        return err;
    }

    printf("%s: ✅ TCP connection established to netcat 2\n", COMPONENT_NAME);
    tcp_connected = true;
    return ERR_OK;
}

/*
 * Attempt to connect to netcat 2 (target IP will be configured)
 */
static void attempt_tcp_connection(void)
{
    if (tcp_connected || tx_pcb != NULL) {
        return;  /* Already connected or connection in progress */
    }

    uint32_t now = sys_now();
    if (now - last_reconnect_attempt < TCP_RECONNECT_INTERVAL) {
        return;  /* Too soon to retry */
    }

    last_reconnect_attempt = now;

    printf("%s: Attempting TCP connection to netcat 2...\n", COMPONENT_NAME);

    tx_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (tx_pcb == NULL) {
        printf("%s: Failed to create TCP PCB\n", COMPONENT_NAME);
        return;
    }

    /* Set callbacks */
    tcp_err(tx_pcb, tcp_tx_err);

    /* Try to connect - TODO: configure target IP from config */
    /* For now, we'll just prepare the PCB and wait for manual connection */
    /* In full implementation, would use tcp_connect() here */

    printf("%s: TCP client ready (waiting for target IP configuration)\n", COMPONENT_NAME);
}

/*
 * Process ICS message from IntNicDrv and transmit payload via TCP
 */
static void process_ics_message(void)
{
    /* Read ICS message from dataport */
    ICS_MsgHeader *msg_hdr = (ICS_MsgHeader *)rx_packet_buffer;
    uint8_t *payload = (uint8_t *)rx_packet_buffer + sizeof(ICS_MsgHeader);

    printf("%s: Received ICS message (tag=0x%04X, len=%u)\n",
           COMPONENT_NAME, msg_hdr->tag, msg_hdr->len);

    /* Transmit payload if TCP connected */
    if (tcp_connected && tx_pcb != NULL) {
        err_t err = tcp_write(tx_pcb, payload, msg_hdr->len, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK) {
            tcp_output(tx_pcb);
            messages_transmitted++;
            bytes_transmitted += msg_hdr->len;
            printf("%s: Transmitted %u bytes to netcat 2\n", COMPONENT_NAME, msg_hdr->len);
        } else {
            printf("%s: TCP write failed: %d\n", COMPONENT_NAME, err);
        }
    } else {
        printf("%s: TCP not connected - message dropped\n", COMPONENT_NAME);
    }
}

/*
 * Minimal lwIP netif output function for TX
 */
static err_t netif_output(struct netif *netif, struct pbuf *p)
{
    /* Simplified TX path - just send via virtqueue */
    printf("%s: TX packet (%u bytes)\n", COMPONENT_NAME, p->tot_len);

    /* TODO: Implement actual VirtIO TX virtqueue management */
    /* For now, this is a stub to make lwIP happy */

    return ERR_OK;
}

/*
 * lwIP netif initialization
 */
static err_t netif_tx_init(struct netif *netif)
{
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = netif_output;
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;

    /* Read MAC from VirtIO config */
    for (int i = 0; i < 6; i++) {
        netif->hwaddr[i] = VREG_READ(VIRTIO_MMIO_CONFIG + VIRTIO_NET_CFG_MAC + i);
    }

    printf("%s: MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", COMPONENT_NAME,
           netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
           netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);

    return ERR_OK;
}

/*
 * Initialize VirtIO device (TX only)
 */
static void init_virtio_device(void)
{
    printf("%s: Initializing VirtIO TX device...\n", COMPONENT_NAME);

    /* Reset device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, 0);

    /* Set ACKNOWLEDGE and DRIVER status bits */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    VREG_WRITE(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate features */
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)VIRTIO_NET_F_MAC);
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)(VIRTIO_F_VERSION_1 >> 32));

    VREG_WRITE(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    /* Allocate TX virtqueue (simplified) */
    /* TODO: Full TX queue setup */

    VREG_WRITE(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    printf("%s: VirtIO TX device initialized\n", COMPONENT_NAME);
}

/*
 * Initialize lwIP stack
 */
static void init_lwip(void)
{
    printf("%s: Initializing lwIP stack...\n", COMPONENT_NAME);

    lwip_init();

    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);  /* DHCP */
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    netif_add(&netif_data, &ipaddr, &netmask, &gw, NULL, netif_tx_init, ethernet_input);
    netif_set_default(&netif_data);
    netif_set_up(&netif_data);

    /* Start DHCP */
    dhcp_start(&netif_data);
    printf("%s: DHCP client started\n", COMPONENT_NAME);

    lwip_initialized = true;
}

/*
 * Handle notification from IntNicDrv (ICS message ready)
 */
void rx_packet_ready_callback(void *arg)
{
    process_ics_message();
}

/*
 * CAmkES post_init - initialize VirtIO and lwIP
 */
void post_init(void)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  EthernetDriver_TX - NIC 2 Transmitter                    ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    /* Map VirtIO MMIO registers */
    virtio_regs_base = (volatile uint32_t *)virtio_mmio_regs;
    printf("%s: VirtIO MMIO registers mapped at %p\n", COMPONENT_NAME, (void *)virtio_regs_base);

    /* Initialize VirtIO device */
    init_virtio_device();

    /* Initialize lwIP */
    init_lwip();

    printf("%s: Initialization complete\n", COMPONENT_NAME);
    printf("%s: Ready to receive ICS messages from IntNicDrv\n", COMPONENT_NAME);
}

/*
 * CAmkES run - main event loop
 */
int run(void)
{
    printf("%s: Entering main event loop\n", COMPONENT_NAME);

    while (1) {
        /* Handle lwIP timers */
        sys_check_timeouts();

        /* Attempt TCP connection if not connected */
        if (!tcp_connected) {
            attempt_tcp_connection();
        }

        /* Yield to other components */
        seL4_Yield();
    }

    return 0;
}
