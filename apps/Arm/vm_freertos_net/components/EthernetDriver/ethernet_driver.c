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

/* Register accessor macros - use pointer arithmetic instead of struct */
/* Forward declaration of regs pointer (defined in global state section) */
static volatile uint32_t *virtio_regs_base;

#define VREG_READ(offset)       (virtio_regs_base[(offset) / 4])
#define VREG_WRITE(offset, val) (virtio_regs_base[(offset) / 4] = (val))

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

/* VirtIO Net Header (required before each packet) */
#define VIRTIO_NET_HDR_SIZE             10  /* Legacy header size (without num_buffers) */
#define VIRTIO_NET_HDR_GSO_NONE         0

typedef struct virtio_net_hdr {
    uint8_t flags;          /* Offload flags */
    uint8_t gso_type;       /* GSO type (VIRTIO_NET_HDR_GSO_NONE for us) */
    uint16_t hdr_len;       /* Ethernet + IP + TCP/UDP headers (not used without GSO) */
    uint16_t gso_size;      /* Bytes to append to hdr_len per frame (not used without GSO) */
    uint16_t csum_start;    /* Position to start checksumming from */
    uint16_t csum_offset;   /* Offset after that to place checksum */
} __attribute__((packed)) virtio_net_hdr_t;
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
/* Note: virtio_regs_base forward-declared above near macros */
static struct virtq rx_virtq;
static struct virtq tx_virtq;
static uint8_t mac_addr[6];

/* lwIP network interface */
static struct netif netif_data;

/* Packet buffers - allocated from DMA memory for VirtIO device access */
static uint8_t *packet_buffers[MAX_PACKETS];  /* Virtual addresses */
static uintptr_t packet_buffers_paddr[MAX_PACKETS];  /* Physical addresses for VirtIO DMA */
static bool rx_buffer_used[MAX_PACKETS];

/* VirtIO net headers - one per TX descriptor (DMA-accessible) */
static virtio_net_hdr_t *tx_headers;  /* Virtual address base */
static uintptr_t tx_headers_paddr;    /* Physical address base */

/* TCP Echo State Pool */
static struct tcp_echo_state tcp_state_pool[MAX_TCP_CONNECTIONS];

/* Statistics */
static uint32_t packets_received = 0;
static uint32_t packets_sent = 0;
static uint32_t dhcp_bound = 0;

/* lwIP time tracking */
static volatile uint32_t lwip_time_ms = 0;

/*
 * lwIP system time function (required by lwIP NO_SYS mode)
 */
uint32_t sys_now(void)
{
    lwip_time_ms++;
    return lwip_time_ms;
}

/* Forward declarations */
static void process_rx_packets(void);
static void refill_rx_queue(void);
static err_t netif_output(struct netif *netif, struct pbuf *p);
static err_t custom_netif_init(struct netif *netif);
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
    static bool first_call = true;
    int buffers_added = 0;

    /* Debug: count how many buffers are marked as used */
    if (first_call) {
        int used_count = 0;
        for (int i = 0; i < MAX_PACKETS; i++) {
            if (rx_buffer_used[i]) used_count++;
        }
        printf("%s: refill_rx_queue: %d/%d buffers marked as used\n",
               COMPONENT_NAME, used_count, MAX_PACKETS);
        printf("%s: refill_rx_queue: vq->num=%u (virtqueue size)\n",
               COMPONENT_NAME, vq->num);
        first_call = false;
    }

    /* Add available buffers to RX virtqueue */
    for (int i = 0; i < MAX_PACKETS; i++) {
        /* Only add buffers that aren't already in use */
        if (rx_buffer_used[i]) continue;

        /* Use this buffer index as the descriptor index */
        uint16_t desc_idx = i;
        if (desc_idx >= vq->num) break;

        /* Mark buffer as in use */
        rx_buffer_used[i] = true;

        /* Setup descriptor pointing to packet buffer - MUST use physical address for DMA! */
        /* Buffer includes space for virtio_net_hdr at the start */
        vq->desc[desc_idx].addr = (uint64_t)packet_buffers_paddr[i];
        vq->desc[desc_idx].len = VIRTIO_NET_HDR_SIZE + PACKET_BUFFER_SIZE;  /* Header + data */
        vq->desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;
        vq->desc[desc_idx].next = 0;

        /* Add to available ring */
        uint16_t avail_idx = vq->avail->idx % vq->num;
        vq->avail->ring[avail_idx] = desc_idx;
        __sync_synchronize();
        vq->avail->idx++;
        buffers_added++;
    }

    printf("%s: Refilled RX queue with %d buffers (avail_idx now=%u)\n",
           COMPONENT_NAME, buffers_added, vq->avail->idx);

    /* Notify device of new buffers */
    if (buffers_added > 0) {
        VREG_WRITE(VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_NET_RX_QUEUE);
    }
}

/*
 * Network interface output function (lwIP -> hardware)
 */
static err_t netif_output(struct netif *netif, struct pbuf *p)
{
    struct virtq *vq = &tx_virtq;
    static uint16_t next_tx_desc = 0;
    static uint32_t tx_count = 0;

    tx_count++;
    if (tx_count <= 3) {
        printf("%s: TX #%u: sending %u bytes\n", COMPONENT_NAME, tx_count, p->tot_len);
    }

    /* Get TX descriptor pair (header + packet) - need 2 consecutive descriptors */
    uint16_t hdr_desc_idx = next_tx_desc;
    uint16_t pkt_desc_idx = (next_tx_desc + 1) % vq->num;
    next_tx_desc = (next_tx_desc + 2) % vq->num;  /* Advance by 2 for chaining */

    int tx_buf_idx = (hdr_desc_idx + MAX_PACKETS/2) % MAX_PACKETS;

    /* Copy pbuf chain to TX buffer */
    uint16_t copied = pbuf_copy_partial(p, packet_buffers[tx_buf_idx],
                                        p->tot_len, 0);

    if (copied != p->tot_len) {
        printf("%s: Failed to copy pbuf: %u/%u bytes\n",
               COMPONENT_NAME, copied, p->tot_len);
        return ERR_BUF;
    }

    /* Setup virtio_net_hdr (already zero-initialized, no offloads needed) */
    uintptr_t hdr_paddr = tx_headers_paddr + (hdr_desc_idx * sizeof(virtio_net_hdr_t));

    /* Descriptor 0: VirtIO net header */
    vq->desc[hdr_desc_idx].addr = (uint64_t)hdr_paddr;
    vq->desc[hdr_desc_idx].len = VIRTIO_NET_HDR_SIZE;
    vq->desc[hdr_desc_idx].flags = VIRTQ_DESC_F_NEXT;  /* Chain to next descriptor */
    vq->desc[hdr_desc_idx].next = pkt_desc_idx;

    /* Descriptor 1: Packet data */
    vq->desc[pkt_desc_idx].addr = (uint64_t)packet_buffers_paddr[tx_buf_idx];
    vq->desc[pkt_desc_idx].len = p->tot_len;
    vq->desc[pkt_desc_idx].flags = 0;  /* Last descriptor in chain */
    vq->desc[pkt_desc_idx].next = 0;

    /* Add to available ring (only add the FIRST descriptor of the chain) */
    uint16_t avail_idx = vq->avail->idx % vq->num;
    vq->avail->ring[avail_idx] = hdr_desc_idx;

    /* DEBUG: Log descriptor setup for first TX */
    if (tx_count == 1) {
        printf("%s: DEBUG TX descriptor chain:\n", COMPONENT_NAME);
        printf("  Desc[%u] (header): addr=0x%lx, len=%u, flags=0x%x, next=%u\n",
               hdr_desc_idx, hdr_paddr, VIRTIO_NET_HDR_SIZE, VIRTQ_DESC_F_NEXT, pkt_desc_idx);
        printf("  Desc[%u] (packet): addr=0x%lx, len=%u, flags=0x%x, next=%u\n",
               pkt_desc_idx, packet_buffers_paddr[tx_buf_idx], p->tot_len, 0, 0);
        printf("  avail->ring[%u] = %u (head of chain)\n", avail_idx, hdr_desc_idx);
    }
    __sync_synchronize();
    vq->avail->idx++;

    /* Notify device */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_NET_TX_QUEUE);

    /* BOUNDARY CHECK: Verify notification was written and device state */
    if (tx_count == 1) {
        uint32_t dev_status = VREG_READ(VIRTIO_MMIO_STATUS);
        uint32_t irq_status = VREG_READ(VIRTIO_MMIO_INTERRUPT_STATUS);
        printf("%s: ═══ QEMU BOUNDARY CHECK after TX notify ═══\n", COMPONENT_NAME);
        printf("  DEVICE_STATUS = 0x%x (should be 0xF = DRIVER_OK)\n", dev_status);
        printf("  IRQ_STATUS = 0x%x\n", irq_status);
        printf("  TX queue used_idx = %u (should increment if QEMU processed)\n", tx_virtq.used->idx);

        /* Wait a bit and check again */
        for (volatile int i = 0; i < 100000; i++);
        printf("  TX queue used_idx after delay = %u\n", tx_virtq.used->idx);
    }

    packets_sent++;
    return ERR_OK;
}

/*
 * Network interface initialization (called by lwIP)
 */
static err_t custom_netif_init(struct netif *netif)
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
    static uint32_t check_count = 0;

    check_count++;

    /* FUNDAMENTAL CHECK: Poll VirtIO device InterruptStatus register */
    if (check_count <= 5) {
        uint32_t irq_status = VREG_READ(VIRTIO_MMIO_INTERRUPT_STATUS);
        uint32_t dev_status = VREG_READ(VIRTIO_MMIO_STATUS);
        printf("%s: RX check #%u: used_idx=%u, last_used=%u, IRQ_STATUS=0x%x, DEV_STATUS=0x%x, regs=%p\n",
               COMPONENT_NAME, check_count, vq->used->idx, last_used_idx, irq_status, dev_status, (void*)virtio_regs_base);
    } else if (vq->used->idx != last_used_idx) {
        printf("%s: RX queue check #%u: used_idx=%u, last_used=%u\n",
               COMPONENT_NAME, check_count, vq->used->idx, last_used_idx);
    }

    if (vq->used->idx == last_used_idx) {
        return;
    }

    while (last_used_idx != vq->used->idx) {
        uint16_t used_ring_idx = last_used_idx % vq->num;
        struct virtq_used_elem *used_elem = &vq->used->ring[used_ring_idx];

        uint16_t desc_idx = used_elem->id;
        uint32_t len = used_elem->len;

        /* Get packet buffer (use buffer index, not physical address from descriptor) */
        int buf_idx = (desc_idx < MAX_PACKETS) ? desc_idx : 0;
        uint8_t *buffer = packet_buffers[buf_idx];

        /* Skip virtio_net_hdr at start of buffer */
        uint8_t *packet_data = buffer + VIRTIO_NET_HDR_SIZE;
        uint16_t packet_len = len - VIRTIO_NET_HDR_SIZE;

        packets_received++;

        /* Log packet arrival */
        printf("%s: 📥 RX packet #%u, %u bytes (total %u with header)\n",
               COMPONENT_NAME, packets_received, packet_len, len);

        /* Allocate pbuf and copy packet data (skipping header) */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, packet_len, PBUF_POOL);
        if (p != NULL) {
            pbuf_take(p, packet_data, packet_len);

            /* Feed packet to lwIP */
            if (netif_data.input(p, &netif_data) != ERR_OK) {
                pbuf_free(p);
            }
        }

        /* Mark buffer as free (buf_idx already defined above) */
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
    static uint32_t irq_count = 0;
    uint32_t irq_status = VREG_READ(VIRTIO_MMIO_INTERRUPT_STATUS);

    irq_count++;
    printf("%s: ⚡ IRQ #%u: status=0x%x\n", COMPONENT_NAME, irq_count, irq_status);

    if (irq_status & VIRTIO_MMIO_IRQ_VQUEUE) {
        printf("%s:   → VQUEUE interrupt - processing RX\n", COMPONENT_NAME);
        process_rx_packets();
        VREG_WRITE(VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_MMIO_IRQ_VQUEUE);
    }

    if (irq_status & VIRTIO_MMIO_IRQ_CONFIG) {
        printf("%s:   → CONFIG interrupt\n", COMPONENT_NAME);
        VREG_WRITE(VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_MMIO_IRQ_CONFIG);
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

    /* Access VirtIO device at slot 31 (offset 0xe00 from page base 0xa003000) */
    /* sDDF confirmed QEMU allocates virtio-net-device to this slot */
    virtio_regs_base = (volatile uint32_t *)((uintptr_t)virtio_mmio_regs + 0xe00);

    /* Verify we have the network device using pointer arithmetic */
    uint32_t magic = VREG_READ(VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version = VREG_READ(VIRTIO_MMIO_VERSION);
    uint32_t device_id = VREG_READ(VIRTIO_MMIO_DEVICE_ID);

    printf("%s: VirtIO @ slot 31 (+0xe00): Magic=0x%x, Version=%u, DeviceID=%u\n",
           COMPONENT_NAME, magic, version, device_id);

    if (magic != 0x74726976) {
        printf("%s: ERROR: Invalid VirtIO magic! Device not accessible.\n", COMPONENT_NAME);
        return -1;
    }

    if (device_id != 1) {
        printf("%s: ERROR: DeviceID=%u (expected 1 for network)\n", COMPONENT_NAME, device_id);
        printf("%s: QEMU may have allocated the device to a different slot.\n", COMPONENT_NAME);
        return -1;
    }

    printf("%s: ✓ Found VirtIO network device at slot 31\n", COMPONENT_NAME);

    /* Reset device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, 0);

    /* Acknowledge device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* Set driver bit */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate features */
    uint64_t features = VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)features);
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)(features >> 32));

    VREG_WRITE(VIRTIO_MMIO_STATUS, VREG_READ(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FEATURES_OK);

    /* Read MAC address */
    uint8_t *mac_base = (uint8_t*)(virtio_regs_base + (VIRTIO_MMIO_CONFIG/4) + (VIRTIO_NET_CFG_MAC/4));
    for (int i = 0; i < 6; i++) {
        mac_addr[i] = mac_base[i];
    }

    printf("%s: MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", COMPONENT_NAME,
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5]);

    /* Setup virtqueues using CAmkES DMA allocation (sDDF equivalent) */
    /* Allocate 64KB DMA buffer for virtqueue rings, 4K-aligned, uncached for device DMA */
    uint8_t *ring_base = camkes_dma_alloc(0x10000, 4096, false);
    if (!ring_base) {
        printf("%s: ERROR: Failed to allocate DMA buffer for virtqueues\n", COMPONENT_NAME);
        return -1;
    }
    memset(ring_base, 0, 0x10000);

    /* Get physical address for VirtIO device DMA access (sDDF: device_resources.regions[1].io_addr) */
    uintptr_t ring_base_paddr = camkes_dma_get_paddr(ring_base);

    printf("%s: DEBUG: ring_base virtual  = 0x%lx\n", COMPONENT_NAME, (uintptr_t)ring_base);
    printf("%s: DEBUG: ring_base physical = 0x%lx (via camkes_dma_get_paddr)\n", COMPONENT_NAME, ring_base_paddr);

    /* RX queue */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_RX_QUEUE);
    printf("%s: DEBUG: QueueSel set to %u\n", COMPONENT_NAME, VIRTIO_NET_RX_QUEUE);
    printf("%s: DEBUG: QueueSel readback = %u\n", COMPONENT_NAME, VREG_READ(VIRTIO_MMIO_QUEUE_SEL));
    printf("%s: DEBUG: QueueNumMax = %u\n", COMPONENT_NAME, VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX));

    /* If QueueNumMax is 0, use a default value */
    if (VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX) == 0) {
        printf("%s: WARNING: QueueNumMax is 0, using default 256\n", COMPONENT_NAME);
        rx_virtq.num = 256;
    } else {
        rx_virtq.num = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    }

    printf("%s: DEBUG: rx_virtq.num = %u\n", COMPONENT_NAME, rx_virtq.num);

    /* Virtual addresses for driver access */
    rx_virtq.desc = (struct virtq_desc *)ring_base;
    rx_virtq.avail = (struct virtq_avail *)(ring_base + 0x2000);
    rx_virtq.used = (struct virtq_used *)(ring_base + 0x2408);

    /* Physical addresses for device DMA access */
    uintptr_t desc_paddr = ring_base_paddr;
    uintptr_t avail_paddr = ring_base_paddr + 0x2000;
    uintptr_t used_paddr = ring_base_paddr + 0x2408;

    printf("%s: DEBUG: RX desc paddr  = 0x%lx\n", COMPONENT_NAME, desc_paddr);
    printf("%s: DEBUG: RX avail paddr = 0x%lx\n", COMPONENT_NAME, avail_paddr);
    printf("%s: DEBUG: RX used paddr  = 0x%lx\n", COMPONENT_NAME, used_paddr);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_NUM, rx_virtq.num);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)used_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);

    /* TX queue */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_TX_QUEUE);

    /* If QueueNumMax is 0, use default value */
    if (VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX) == 0) {
        tx_virtq.num = 256;
    } else {
        tx_virtq.num = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    }

    /* Virtual addresses for driver access */
    tx_virtq.desc = (struct virtq_desc *)(ring_base + 0x3410);
    tx_virtq.avail = (struct virtq_avail *)(ring_base + 0x5410);
    tx_virtq.used = (struct virtq_used *)(ring_base + 0x5818);

    /* Physical addresses for device DMA access */
    uintptr_t tx_desc_paddr = ring_base_paddr + 0x3410;
    uintptr_t tx_avail_paddr = ring_base_paddr + 0x5410;
    uintptr_t tx_used_paddr = ring_base_paddr + 0x5818;

    VREG_WRITE(VIRTIO_MMIO_QUEUE_NUM, tx_virtq.num);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)tx_desc_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(tx_desc_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)tx_avail_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(tx_avail_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)tx_used_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(tx_used_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);

    /* Device ready */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VREG_READ(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);

    /* FUNDAMENTAL CHECK: Read back Status to verify write succeeded */
    uint32_t final_status = VREG_READ(VIRTIO_MMIO_STATUS);
    printf("%s: DEBUG: After DRIVER_OK write, Status readback = 0x%x (expected 0xF)\n",
           COMPONENT_NAME, final_status);

    /* BOUNDARY CHECK: Verify MMIO register access is working */
    printf("\n%s: ═══ MMIO REGISTER ACCESS VERIFICATION ═══\n", COMPONENT_NAME);
    printf("  virtio_regs_base = %p\n", virtio_regs_base);
    printf("  QUEUE_NOTIFY register address = %p\n",
           (void*)(virtio_regs_base + (VIRTIO_MMIO_QUEUE_NOTIFY/4)));

    /* Test write/read to a safe register (QueueSel) */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qsel = VREG_READ(VIRTIO_MMIO_QUEUE_SEL);
    printf("  Test: Wrote QueueSel=0, readback=%u (should match)\n", qsel);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 1);
    qsel = VREG_READ(VIRTIO_MMIO_QUEUE_SEL);
    printf("  Test: Wrote QueueSel=1, readback=%u (should match)\n", qsel);
    printf("  ✓ MMIO register writes are %s\n", (qsel == 1) ? "WORKING" : "BROKEN!");

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

    /* Allocate packet buffers from DMA memory (matching sDDF approach) */
    printf("%s: Allocating %d DMA packet buffers (%d bytes each)...\n",
           COMPONENT_NAME, MAX_PACKETS, PACKET_BUFFER_SIZE);
    for (int i = 0; i < MAX_PACKETS; i++) {
        packet_buffers[i] = camkes_dma_alloc(PACKET_BUFFER_SIZE, 64, false);
        if (!packet_buffers[i]) {
            printf("%s: ERROR: Failed to allocate DMA buffer %d\n", COMPONENT_NAME, i);
            return;
        }
        packet_buffers_paddr[i] = camkes_dma_get_paddr(packet_buffers[i]);
    }
    printf("%s: ✓ Allocated DMA packet buffers (vaddr=%p, paddr=0x%lx)\n",
           COMPONENT_NAME, packet_buffers[0], packet_buffers_paddr[0]);

    /* Allocate TX headers array (one header per possible TX descriptor) */
    size_t tx_headers_size = MAX_PACKETS * sizeof(virtio_net_hdr_t);
    tx_headers = camkes_dma_alloc(tx_headers_size, 16, false);  /* 16-byte aligned */
    if (!tx_headers) {
        printf("%s: ERROR: Failed to allocate TX headers DMA memory\n", COMPONENT_NAME);
        return;
    }
    tx_headers_paddr = camkes_dma_get_paddr(tx_headers);

    /* Initialize all TX headers (zero-fill = no offloads) */
    memset(tx_headers, 0, tx_headers_size);
    printf("%s: ✓ Allocated TX headers array (vaddr=%p, paddr=0x%lx)\n",
           COMPONENT_NAME, tx_headers, tx_headers_paddr);

    /* Initialize packet buffers */
    memset(rx_buffer_used, 0, sizeof(rx_buffer_used));
    refill_rx_queue();

    /* Initialize lwIP */
    printf("%s: Initializing lwIP TCP/IP stack...\n", COMPONENT_NAME);
    lwip_init();

    /* Add network interface with STATIC IP (like sDDF) */
    struct ip4_addr ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 10, 0, 2, 15);      /* Static IP: 10.0.2.15 */
    IP4_ADDR(&netmask, 255, 255, 255, 0);  /* Netmask: 255.255.255.0 */
    IP4_ADDR(&gw, 10, 0, 2, 2);            /* Gateway: 10.0.2.2 (QEMU default) */

    printf("%s: Using STATIC IP configuration:\n", COMPONENT_NAME);
    printf("%s:   IP:      10.0.2.15\n", COMPONENT_NAME);
    printf("%s:   Netmask: 255.255.255.0\n", COMPONENT_NAME);
    printf("%s:   Gateway: 10.0.2.2\n", COMPONENT_NAME);

    netif_add(&netif_data, &ipaddr, &netmask, &gw, NULL, custom_netif_init, ethernet_input);
    netif_set_default(&netif_data);
    netif_set_status_callback(&netif_data, netif_status_callback);
    netif_set_up(&netif_data);

    /* Skip DHCP - use static IP */
    printf("%s: Network interface UP with static IP\n", COMPONENT_NAME);

    /* Setup TCP echo server */
    setup_tcp_echo_server();

    printf("%s: ✓ Initialization complete\n", COMPONENT_NAME);
    printf("%s: TCP Echo Server ready on 10.0.2.15:1234\n", COMPONENT_NAME);
    printf("%s: Test with: telnet localhost 8888 (or nc localhost 8888)\n\n", COMPONENT_NAME);

    /* Main event loop - process lwIP timers and RX packets */
    while (1) {
        sys_check_timeouts();
        process_rx_packets();
        seL4_Yield();
    }
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
