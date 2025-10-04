/*
 * EthernetDriver Component - Tier 4: TCP Echo Server with lwIP
 *
 * This component provides a complete TCP/IP stack using lwIP and implements
 * a TCP echo server listening on port 1234.
 *
 * Architecture:
 * - VirtIO-net device driver for packet RX/TX
 * - lwIP TCP/IP stack for network protocol handling
 * - DHCP client to obtain IP address from QEMU
 * - TCP echo server for testing with telnet
 *
 * Based on:
 * - sDDF echo_server implementation
 * - CAmkES lwIP integration patterns
 */

#include <camkes.h>
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

#define COMPONENT_NAME "EthernetDriver"

/* VirtIO MMIO Register Offsets */
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

/* VirtIO Status Bits */
#define VIRTIO_STATUS_ACKNOWLEDGE       1
#define VIRTIO_STATUS_DRIVER            2
#define VIRTIO_STATUS_DRIVER_OK         4
#define VIRTIO_STATUS_FEATURES_OK       8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED            128

/* VirtIO Feature Bits */
#define VIRTIO_F_VERSION_1              (1ULL << 32)
#define VIRTIO_NET_F_MAC                (1ULL << 5)
#define VIRTIO_NET_F_STATUS             (1ULL << 16)

/* VirtIO Network Queues */
#define VIRTIO_NET_RX_QUEUE             0
#define VIRTIO_NET_TX_QUEUE             1

/* VirtIO IRQ Bits */
#define VIRTIO_MMIO_IRQ_VQUEUE          0x01
#define VIRTIO_MMIO_IRQ_CONFIG          0x02

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT               1
#define VIRTQ_DESC_F_WRITE              2
#define VIRTQ_DESC_F_INDIRECT           4

/* TCP Echo Server Configuration */
#define TCP_ECHO_PORT                   1234
#define MAX_TCP_CONNECTIONS             8

/* Packet buffer configuration */
#define PACKET_BUFFER_SIZE              2048
#define MAX_PACKETS                     32

/*
 * VirtIO MMIO Register Structure
 */
struct virtio_mmio_regs {
    uint32_t MagicValue;
    uint32_t Version;
    uint32_t DeviceID;
    uint32_t VendorID;
    uint32_t DeviceFeatures;
    uint32_t DeviceFeaturesSel;
    uint32_t _reserved1[2];
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;
    uint32_t _reserved2[2];
    uint32_t QueueSel;
    uint32_t QueueNumMax;
    uint32_t QueueNum;
    uint32_t _reserved3[2];
    uint32_t QueueReady;
    uint32_t _reserved4[2];
    uint32_t QueueNotify;
    uint32_t _reserved5[3];
    uint32_t InterruptStatus;
    uint32_t InterruptACK;
    uint32_t _reserved6[2];
    uint32_t Status;
    uint32_t _reserved7[3];
    uint32_t QueueDescLow;
    uint32_t QueueDescHigh;
    uint32_t _reserved8[2];
    uint32_t QueueAvailLow;
    uint32_t QueueAvailHigh;
    uint32_t _reserved9[2];
    uint32_t QueueUsedLow;
    uint32_t QueueUsedHigh;
    uint32_t _reserved10[21];
    uint32_t ConfigGeneration;
    uint32_t Config[0];
} __attribute__((packed));

/*
 * Virtqueue Descriptor
 */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

/*
 * Virtqueue Available Ring
 */
struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

/*
 * Virtqueue Used Element
 */
struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

/*
 * Virtqueue Used Ring
 */
struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

/*
 * Virtqueue Structure
 */
struct virtq {
    unsigned int num;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
};

/* TCP Echo State (per connection) */
struct tcp_echo_state {
    bool in_use;
    struct tcp_pcb *pcb;
};

/* Global state */
static volatile struct virtio_mmio_regs *regs;
static struct virtq rx_virtq;
static struct virtq tx_virtq;
static uint8_t mac_addr[6];

/* lwIP network interface */
static struct netif netif_data;

/* Packet buffers */
static uint8_t packet_buffers[MAX_PACKETS][PACKET_BUFFER_SIZE] __attribute__((aligned(64)));
static bool rx_buffer_used[MAX_PACKETS];

/* TCP Echo State Pool */
static struct tcp_echo_state tcp_state_pool[MAX_TCP_CONNECTIONS];

/* Statistics */
static uint32_t packets_received = 0;
static uint32_t packets_sent = 0;
static uint32_t dhcp_bound = 0;

/* Forward declarations */
static void process_rx_packets(void);
static void refill_rx_queue(void);
static err_t netif_output(struct netif *netif, struct pbuf *p);
static err_t netif_init(struct netif *netif);
static void netif_status_callback(struct netif *netif);
static void setup_tcp_echo_server(void);

/*
 * Get free RX buffer index
 */
static int get_free_rx_buffer(void)
{
    for (int i = 0; i < MAX_PACKETS; i++) {
        if (!rx_buffer_used[i]) {
            rx_buffer_used[i] = true;
            return i;
        }
    }
    return -1;
}

/*
 * Refill RX virtqueue with available buffers
 */
static void refill_rx_queue(void)
{
    struct virtq *vq = &rx_virtq;

    for (int i = 0; i < MAX_PACKETS; i++) {
        if (rx_buffer_used[i]) continue;

        int buf_idx = get_free_rx_buffer();
        if (buf_idx < 0) break;

        uint16_t desc_idx = i;
        if (desc_idx >= vq->num) break;

        vq->desc[desc_idx].addr = (uint64_t)(uintptr_t)&packet_buffers[buf_idx][0];
        vq->desc[desc_idx].len = PACKET_BUFFER_SIZE;
        vq->desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;
        vq->desc[desc_idx].next = 0;

        uint16_t avail_idx = vq->avail->idx % vq->num;
        vq->avail->ring[avail_idx] = desc_idx;
        __sync_synchronize();
        vq->avail->idx++;
    }

    regs->QueueNotify = VIRTIO_NET_RX_QUEUE;
}

/*
 * Network interface output function (lwIP -> hardware)
 */
static err_t netif_output(struct netif *netif, struct pbuf *p)
{
    struct virtq *vq = &tx_virtq;
    static uint16_t next_tx_desc = 0;

    /* Get TX buffer */
    uint16_t desc_idx = next_tx_desc;
    next_tx_desc = (next_tx_desc + 1) % vq->num;

    int tx_buf_idx = (desc_idx + MAX_PACKETS/2) % MAX_PACKETS;

    /* Copy pbuf chain to TX buffer */
    uint16_t copied = pbuf_copy_partial(p, &packet_buffers[tx_buf_idx][0],
                                        p->tot_len, 0);

    if (copied != p->tot_len) {
        printf("%s: Failed to copy pbuf: %u/%u bytes\n",
               COMPONENT_NAME, copied, p->tot_len);
        return ERR_BUF;
    }

    /* Setup descriptor */
    vq->desc[desc_idx].addr = (uint64_t)(uintptr_t)&packet_buffers[tx_buf_idx][0];
    vq->desc[desc_idx].len = p->tot_len;
    vq->desc[desc_idx].flags = 0;
    vq->desc[desc_idx].next = 0;

    /* Add to available ring */
    uint16_t avail_idx = vq->avail->idx % vq->num;
    vq->avail->ring[avail_idx] = desc_idx;
    __sync_synchronize();
    vq->avail->idx++;

    /* Notify device */
    regs->QueueNotify = VIRTIO_NET_TX_QUEUE;

    packets_sent++;
    return ERR_OK;
}

/*
 * Network interface initialization (called by lwIP)
 */
static err_t netif_init(struct netif *netif)
{
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = netif_output;

    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, mac_addr, 6);

    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

    return ERR_OK;
}

/*
 * Network interface status callback (called when DHCP completes)
 */
static void netif_status_callback(struct netif *netif)
{
    if (netif_is_up(netif)) {
        printf("\n");
        printf("╔════════════════════════════════════════════════════════╗\n");
        printf("║  🎉 DHCP SUCCESS! Network Interface Configured         ║\n");
        printf("╚════════════════════════════════════════════════════════╝\n");
        printf("%s: IP Address:  %s\n", COMPONENT_NAME,
               ip4addr_ntoa(netif_ip4_addr(netif)));
        printf("%s: Netmask:     %s\n", COMPONENT_NAME,
               ip4addr_ntoa(netif_ip4_netmask(netif)));
        printf("%s: Gateway:     %s\n", COMPONENT_NAME,
               ip4addr_ntoa(netif_ip4_gw(netif)));
        printf("%s: MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
               COMPONENT_NAME,
               netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
               netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);
        printf("\n");
        printf("%s: TCP Echo Server listening on port %d\n",
               COMPONENT_NAME, TCP_ECHO_PORT);
        printf("%s: Test with: telnet %s %d\n",
               COMPONENT_NAME, ip4addr_ntoa(netif_ip4_addr(netif)), TCP_ECHO_PORT);
        printf("\n");

        dhcp_bound = 1;
    }
}

/*
 * Process received packets and feed to lwIP
 */
static void process_rx_packets(void)
{
    struct virtq *vq = &rx_virtq;
    static uint16_t last_used_idx = 0;

    if (vq->used->idx == last_used_idx) {
        return;
    }

    while (last_used_idx != vq->used->idx) {
        uint16_t used_ring_idx = last_used_idx % vq->num;
        struct virtq_used_elem *used_elem = &vq->used->ring[used_ring_idx];

        uint16_t desc_idx = used_elem->id;
        uint32_t len = used_elem->len;

        uint8_t *packet_data = (uint8_t *)(uintptr_t)vq->desc[desc_idx].addr;

        packets_received++;

        /* Allocate pbuf and copy packet data */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        if (p != NULL) {
            pbuf_take(p, packet_data, len);

            /* Feed packet to lwIP */
            if (netif_data.input(p, &netif_data) != ERR_OK) {
                pbuf_free(p);
            }
        }

        /* Mark buffer as free */
        int buf_idx = (desc_idx < MAX_PACKETS) ? desc_idx : 0;
        rx_buffer_used[buf_idx] = false;

        last_used_idx++;
    }

    refill_rx_queue();
}

/*
 * TCP Echo callbacks
 */
static err_t tcp_echo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (p == NULL) {
        /* Connection closed */
        printf("%s: TCP connection closed\n", COMPONENT_NAME);
        tcp_close(pcb);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    /* Echo data back */
    tcp_write(pcb, p->payload, p->len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);

    /* Tell TCP we've processed the data */
    tcp_recved(pcb, p->len);

    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_echo_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    printf("%s: TCP connection accepted\n", COMPONENT_NAME);

    tcp_setprio(newpcb, TCP_PRIO_MIN);
    tcp_recv(newpcb, tcp_echo_recv);

    return ERR_OK;
}

/*
 * Setup TCP echo server
 */
static void setup_tcp_echo_server(void)
{
    struct tcp_pcb *pcb;

    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        printf("%s: Failed to create TCP PCB\n", COMPONENT_NAME);
        return;
    }

    err_t err = tcp_bind(pcb, IP_ANY_TYPE, TCP_ECHO_PORT);
    if (err != ERR_OK) {
        printf("%s: Failed to bind TCP port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
        return;
    }

    pcb = tcp_listen_with_backlog(pcb, MAX_TCP_CONNECTIONS);
    if (pcb == NULL) {
        printf("%s: Failed to listen on TCP port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
        return;
    }

    tcp_accept(pcb, tcp_echo_accept);

    printf("%s: TCP echo server created (will bind after DHCP)\n", COMPONENT_NAME);
}

/*
 * VirtIO IRQ Handler
 */
void virtio_irq_handle(void)
{
    uint32_t irq_status = regs->InterruptStatus;

    if (irq_status & VIRTIO_MMIO_IRQ_VQUEUE) {
        process_rx_packets();
        regs->InterruptACK = VIRTIO_MMIO_IRQ_VQUEUE;
    }

    if (irq_status & VIRTIO_MMIO_IRQ_CONFIG) {
        regs->InterruptACK = VIRTIO_MMIO_IRQ_CONFIG;
    }

    virtio_irq_acknowledge();
}

/*
 * Initialize VirtIO device (same as Tier 2)
 */
static int virtio_net_init(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║         EthernetDriver Component - Tier 4                ║\n");
    printf("║      TCP Echo Server with lwIP TCP/IP Stack               ║\n");
    printf("║              (CAmkES Port of sDDF Driver)                 ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\n");

    /* Map hardware resources */
    regs = (volatile struct virtio_mmio_regs *)((uintptr_t)hw_ring_buffer + 0xe00);

    /* Reset device */
    regs->Status = 0;

    /* Acknowledge device */
    regs->Status = VIRTIO_STATUS_ACKNOWLEDGE;

    /* Set driver bit */
    regs->Status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    /* Negotiate features */
    uint64_t features = VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    regs->DriverFeaturesSel = 0;
    regs->DriverFeatures = (uint32_t)features;
    regs->DriverFeaturesSel = 1;
    regs->DriverFeatures = (uint32_t)(features >> 32);

    regs->Status |= VIRTIO_STATUS_FEATURES_OK;

    /* Read MAC address */
    uint8_t *mac_base = (uint8_t *)&regs->Config[VIRTIO_NET_CFG_MAC / 4];
    for (int i = 0; i < 6; i++) {
        mac_addr[i] = mac_base[i];
    }

    printf("%s: MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", COMPONENT_NAME,
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5]);

    /* Setup virtqueues */
    uint8_t *ring_base = (uint8_t *)hw_ring_buffer;

    /* RX queue */
    regs->QueueSel = VIRTIO_NET_RX_QUEUE;
    rx_virtq.num = regs->QueueNumMax;
    rx_virtq.desc = (struct virtq_desc *)ring_base;
    rx_virtq.avail = (struct virtq_avail *)(ring_base + 0x2000);
    rx_virtq.used = (struct virtq_used *)(ring_base + 0x2408);

    regs->QueueNum = rx_virtq.num;
    regs->QueueDescLow = (uintptr_t)rx_virtq.desc;
    regs->QueueDescHigh = 0;
    regs->QueueAvailLow = (uintptr_t)rx_virtq.avail;
    regs->QueueAvailHigh = 0;
    regs->QueueUsedLow = (uintptr_t)rx_virtq.used;
    regs->QueueUsedHigh = 0;
    regs->QueueReady = 1;

    /* TX queue */
    regs->QueueSel = VIRTIO_NET_TX_QUEUE;
    tx_virtq.num = regs->QueueNumMax;
    tx_virtq.desc = (struct virtq_desc *)(ring_base + 0x3410);
    tx_virtq.avail = (struct virtq_avail *)(ring_base + 0x5410);
    tx_virtq.used = (struct virtq_used *)(ring_base + 0x5818);

    regs->QueueNum = tx_virtq.num;
    regs->QueueDescLow = (uintptr_t)tx_virtq.desc;
    regs->QueueDescHigh = 0;
    regs->QueueAvailLow = (uintptr_t)tx_virtq.avail;
    regs->QueueAvailHigh = 0;
    regs->QueueUsedLow = (uintptr_t)tx_virtq.used;
    regs->QueueUsedHigh = 0;
    regs->QueueReady = 1;

    /* Device ready */
    regs->Status |= VIRTIO_STATUS_DRIVER_OK;

    printf("%s: ✓ VirtIO device initialized\n", COMPONENT_NAME);

    return 0;
}

/*
 * Component initialization
 */
void post_init(void)
{
    printf("%s: Component started\n\n", COMPONENT_NAME);

    /* Initialize VirtIO device */
    if (virtio_net_init() != 0) {
        printf("%s: Failed to initialize VirtIO device\n", COMPONENT_NAME);
        return;
    }

    /* Initialize packet buffers */
    memset(rx_buffer_used, 0, sizeof(rx_buffer_used));
    refill_rx_queue();

    /* Initialize lwIP */
    printf("%s: Initializing lwIP TCP/IP stack...\n", COMPONENT_NAME);
    lwip_init();

    /* Add network interface */
    struct ip4_addr ipaddr, netmask, gw;
    IP4_ADDR(&gw, 0, 0, 0, 0);
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);

    netif_add(&netif_data, &ipaddr, &netmask, &gw, NULL, netif_init, ethernet_input);
    netif_set_default(&netif_data);
    netif_set_status_callback(&netif_data, netif_status_callback);
    netif_set_up(&netif_data);

    /* Start DHCP */
    printf("%s: Starting DHCP client...\n", COMPONENT_NAME);
    dhcp_start(&netif_data);

    /* Setup TCP echo server */
    setup_tcp_echo_server();

    printf("%s: ✓ Initialization complete\n", COMPONENT_NAME);
    printf("%s: Waiting for DHCP...\n\n", COMPONENT_NAME);
}

/*
 * Component main loop (handle lwIP timers)
 */
int run(void)
{
    /* Process lwIP timers */
    sys_check_timeouts();

    /* Small delay to avoid spinning */
    seL4_Yield();

    return 0;
}
