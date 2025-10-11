/*
 * VirtIO_Net0_Driver - External Network (Bidirectional)
 *
 * This component manages the external network interface with bidirectional
 * data flow and protocol break architecture.
 *
 * Architecture:
 * - VirtIO-net device driver for packet RX/TX
 * - lwIP TCP/IP stack for network protocol handling
 * - DHCP client to obtain IP address from QEMU
 * - TCP server on port 6000 for INBOUND connections
 * - TCP client for OUTBOUND connections
 * - Frame header parsing to extract FrameMetadata
 *
 * Data Flow:
 *   INBOUND:  External TCP:6000 => lwIP => extract metadata+payload => ICS_Inbound
 *   OUTBOUND: ICS_Outbound => create TCP packet => lwIP => External
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

/* ICS common definitions */
#include "common.h"

#define COMPONENT_NAME "VirtIO_Net0_Driver"
#define TCP_SERVER_PORT 502  /* INBOUND: Modbus port - pretends to be PLC */

/*
 * VLAN-BASED DEPLOYMENT CONFIGURATION
 *
 * Net0 uses PRIVATE network (10.2.0.0/24) connected to tap0
 * - Listens on 10.2.0.2:502
 * - Receives traffic from eth0 (192.168.95.2) via iptables DNAT
 * - Forwards validated traffic to Net1 (10.3.0.2)
 *
 * Host iptables translates:
 *   eth0 (192.168.95.2:502) ←→ tap0 (10.2.0.2:502)
 */
#define OUTBOUND_FORWARD_IP "10.3.0.2"        /* Forward to Net1 (private network) */
#define OUTBOUND_FORWARD_PORT 502              /* Modbus TCP port */
#define INBOUND_FORWARD_PORT 502               /* Unused - Net1 handles inbound */

/*
 * DEBUG CONFIGURATION
 * Set to 1 to enable, 0 to disable
 */
#define DEBUG_VERBOSE 0           /* Enable verbose debug output */
#define DEBUG_PACKET_LOG 0        /* Enable detailed packet logging (VERY VERBOSE - hex dumps) */
#define DEBUG_PACKET_SUMMARY 1    /* Enable lightweight packet summary (source→dest, protocol) */
#define DEBUG_MESSAGE_FLOW 1      /* Track message flow through RX→ICS→TX pipeline */
#define ENABLE_GDB_WAIT 0         /* Enable 60-second GDB wait during init */
#define ENABLE_PAINT_TEST 0       /* Enable virtqueue memory paint test */

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

/* ARM memory barriers for MMIO access */
#ifdef __aarch64__
/* AArch64 requires explicit barrier scope */
#define DMB() __asm__ volatile("dmb sy" ::: "memory")  /* Data Memory Barrier - System */
#define DSB() __asm__ volatile("dsb sy" ::: "memory")  /* Data Synchronization Barrier - System */
#define ISB() __asm__ volatile("isb" ::: "memory")     /* Instruction Synchronization Barrier */
#else
/* ARM32 uses implicit full system scope */
#define DMB() __asm__ volatile("dmb" ::: "memory")
#define DSB() __asm__ volatile("dsb" ::: "memory")
#define ISB() __asm__ volatile("isb" ::: "memory")
#endif

/* MMIO register access with memory barriers */
#define VREG_READ(offset) ({ \
    DMB(); \
    uint32_t val = virtio_regs_base[(offset) / 4]; \
    DMB(); \
    val; \
})

#define VREG_WRITE(offset, val) do { \
    DMB(); \
    virtio_regs_base[(offset) / 4] = (val); \
    DSB(); \
} while (0)

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
#define VIRTIO_NET_HDR_SIZE             12  /* Modern VirtIO header size (with num_buffers field) */
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

/* TCP Server Configuration */
#define TCP_ECHO_PORT                   TCP_SERVER_PORT  /* Use port 6000 */
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

/* Message flow tracking */
static uint32_t message_id_counter = 0;

/* lwIP network interface */
static struct netif netif_data;

/* TCP server deferred initialization flag */
static bool tcp_server_initialized = false;

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

/* Initialization status flag (shared with other components for validation) */
static volatile bool initialization_successful = false;

/*
 * lwIP system time function (required by lwIP NO_SYS mode)
 */
uint32_t sys_now(void)
{
    lwip_time_ms++;
    return lwip_time_ms;
}

/* Forward declarations */

/* ═══════════════════════════════════════════════════════════
 * Network Traffic Logging Helpers
 * ═══════════════════════════════════════════════════════════ */

/* Ethernet header structure */
struct ethhdr {
    uint8_t h_dest[6];
    uint8_t h_source[6];
    uint16_t h_proto;
} __attribute__((packed));

/* IP header structure (simplified) */
struct iphdr {
    uint8_t ihl:4;
    uint8_t version:4;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
} __attribute__((packed));

/* TCP header structure (simplified) */
struct tcphdr {
    uint16_t source;
    uint16_t dest;
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t res1:4;
    uint16_t doff:4;
    uint16_t fin:1;
    uint16_t syn:1;
    uint16_t rst:1;
    uint16_t psh:1;
    uint16_t ack:1;
    uint16_t urg:1;
    uint16_t res2:2;
    uint16_t window;
    uint16_t check;
    uint16_t urg_ptr;
} __attribute__((packed));

/* Network byte order conversions - provided by lwIP */
/* ntohs() and ntohl() are already defined in lwip/def.h */

static void hex_dump_packet(const char *prefix, const uint8_t *data, size_t len, size_t max_display)
{
    printf("%s: ", prefix);

    size_t display_len = (len < max_display) ? len : max_display;

    for (size_t i = 0; i < display_len; i++) {
        printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0 && (i + 1) < display_len) {
            printf("\n%*s", (int)strlen(prefix) + 2, "");
        }
    }

    if (len > max_display) {
        printf("... (%zu more bytes)", len - max_display);
    }
    printf("\n");
}

static void print_ascii_payload(const uint8_t *data, size_t len)
{
    printf("  ASCII: \"");
    for (size_t i = 0; i < len && i < 80; i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            printf("%c", data[i]);
        } else {
            printf(".");
        }
    }
    if (len > 80) printf("...");
    printf("\"\n");
}

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

    /* CRITICAL DEBUG: Confirm this function is being called */
    if (tx_count <= 20) {
        printf("%s: ⚡ netif_output() CALLED - tx_count=%u, pbuf len=%u\n",
               COMPONENT_NAME, tx_count, p->tot_len);
    }

    /* Detailed TX logging for first 10 packets */
    if (tx_count <= 10) {
        uint32_t timestamp_ms = sys_now();
        printf("\n╔══════════════════════════════════════════════════════════╗\n");
        printf("║  📤 OUTGOING PACKET #%u [T=%u.%03us]                      ║\n",
               tx_count, timestamp_ms / 1000, timestamp_ms % 1000);
        printf("╚══════════════════════════════════════════════════════════╝\n");
        printf("  Size: %u bytes\n", p->tot_len);
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

    /* Detailed TX packet inspection for first 10 packets */
    if (tx_count <= 10) {
        uint8_t *tx_data = packet_buffers[tx_buf_idx];

        /* Hex dump */
        hex_dump_packet("  Raw packet", tx_data, p->tot_len, 128);

        /* Parse Ethernet header */
        if (p->tot_len >= sizeof(struct ethhdr)) {
            struct ethhdr *eth = (struct ethhdr *)tx_data;
            printf("  Ethernet: %02x:%02x:%02x:%02x:%02x:%02x → %02x:%02x:%02x:%02x:%02x:%02x\n",
                   eth->h_source[0], eth->h_source[1], eth->h_source[2],
                   eth->h_source[3], eth->h_source[4], eth->h_source[5],
                   eth->h_dest[0], eth->h_dest[1], eth->h_dest[2],
                   eth->h_dest[3], eth->h_dest[4], eth->h_dest[5]);
            printf("  EtherType: 0x%04x", ntohs(eth->h_proto));

            /* Parse IP if present */
            if (ntohs(eth->h_proto) == 0x0800 && p->tot_len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
                printf(" (IPv4)\n");
                struct iphdr *ip = (struct iphdr *)(tx_data + sizeof(struct ethhdr));
                uint32_t saddr = ntohl(ip->saddr);
                uint32_t daddr = ntohl(ip->daddr);
                printf("  IP: %u.%u.%u.%u → %u.%u.%u.%u (protocol=%u)\n",
                       (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF,
                       (saddr >> 8) & 0xFF, saddr & 0xFF,
                       (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF,
                       (daddr >> 8) & 0xFF, daddr & 0xFF,
                       ip->protocol);

                /* Parse TCP if present */
                if (ip->protocol == 6) {
                    size_t ip_hdr_len = (ip->ihl) * 4;
                    if (p->tot_len >= sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct tcphdr)) {
                        struct tcphdr *tcp = (struct tcphdr *)(tx_data + sizeof(struct ethhdr) + ip_hdr_len);
                        printf("  TCP: port %u → %u (flags: ", ntohs(tcp->source), ntohs(tcp->dest));
                        if (tcp->syn) printf("SYN ");
                        if (tcp->ack) printf("ACK ");
                        if (tcp->fin) printf("FIN ");
                        if (tcp->rst) printf("RST ");
                        if (tcp->psh) printf("PSH ");
                        printf(")\n");

                        size_t tcp_hdr_len = (tcp->doff) * 4;
                        size_t payload_offset = sizeof(struct ethhdr) + ip_hdr_len + tcp_hdr_len;
                        if (p->tot_len > payload_offset) {
                            size_t payload_len = p->tot_len - payload_offset;
                            printf("  TCP Payload (%zu bytes):\n", payload_len);
                            print_ascii_payload(tx_data + payload_offset, payload_len);
                        }
                    }
                }
            } else if (ntohs(eth->h_proto) == 0x0806) {
                printf(" (ARP)\n");
            } else {
                printf("\n");
            }
        }
        printf("══════════════════════════════════════════════════════════\n\n");
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

    #if DEBUG_VERBOSE
    /* DEBUG: Log descriptor setup for first TX */
    if (tx_count == 1) {
        printf("%s: DEBUG TX descriptor chain:\n", COMPONENT_NAME);
        printf("  Desc[%u] (header): addr=0x%lx, len=%u, flags=0x%x, next=%u\n",
               hdr_desc_idx, hdr_paddr, VIRTIO_NET_HDR_SIZE, VIRTQ_DESC_F_NEXT, pkt_desc_idx);
        printf("  Desc[%u] (packet): addr=0x%lx, len=%u, flags=0x%x, next=%u\n",
               pkt_desc_idx, packet_buffers_paddr[tx_buf_idx], p->tot_len, 0, 0);
        printf("  avail->ring[%u] = %u (head of chain)\n", avail_idx, hdr_desc_idx);
    }
    #endif
    __sync_synchronize();
    vq->avail->idx++;

    /* Notify device */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_NET_TX_QUEUE);

    #if DEBUG_VERBOSE
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
    #endif

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

    /* Process packets using correct wraparound arithmetic
     * CRITICAL: Re-read used->idx on EVERY iteration to avoid reading stale data
     * when we skip corrupted packets
     */
    uint32_t loop_count = 0;
    while (1) {
        /* VirtIO Spec 2.4.5: Read used->idx with ACQUIRE semantics
         * This ensures we see all ring entry writes BEFORE we read the ring data.
         * Must re-read on every iteration to avoid advancing past valid entries.
         */
        uint16_t current_used_idx = __atomic_load_n(&vq->used->idx, __ATOMIC_ACQUIRE);

        /* CRITICAL: Use wraparound-safe comparison for uint16_t indices
         * When current_used_idx wraps (65535 -> 0), simple == comparison fails!
         * Example: last_used_idx=4776, current_used_idx=89 after wraparound
         * Correct check: (uint16_t)(current_used_idx - last_used_idx) == 0
         *
         * IMPORTANT: VirtIO used->idx is EXCLUSIVE upper bound
         * If used->idx=2, valid entries are indices 0 and 1 only
         * Reading at index 2 when used->idx=2 accesses uninitialized/stale data
         */
        uint16_t pending_packets = (uint16_t)(current_used_idx - last_used_idx);

        if (pending_packets == 0) {
            return;  /* No more packets to process */
        }

        /* SAFETY: Detect impossible wraparound scenarios and desynchronization
         *
         * Check 1: VirtIO ring can hold at most vq->num entries.
         *          If pending_packets > vq->num, it's IMPOSSIBLE - indicates desync!
         *
         * Example desync: last_used_idx=2034, current_used_idx=15, vq->num=256
         *   -> pending = (uint16_t)(15 - 2034) = 63517
         *   -> 63517 > 256 = DESYNC DETECTED!
         *
         * Check 2: VirtIO spec max queue size is 1024 (validated at init).
         *          If last_used_idx is absurdly large, it's corrupted state.
         *
         * Why vq->num and not arbitrary 1000?
         *   - vq->num is read from VIRTIO_MMIO_QUEUE_NUM_MAX register
         *   - It's the actual PHYSICAL LIMIT of the device
         *   - Theoretically sound: pending > ring_size is mathematically impossible
         */
        /* CRITICAL FIX: This is NOT desync - it's normal when last_used_idx advances
         * ahead of device's used_idx update. The device might not have written new
         * packets yet, so last_used_idx (our consumption counter) > current_used_idx
         * (device production counter) causes wraparound: (uint16_t)(current - last)
         * becomes 65535, 65534, etc.
         *
         * Real desync: pending > vq->num (ring physically can't hold that many)
         * False alarm: last_used_idx caught up to device, no new packets available
         */
        if (pending_packets > vq->num) {
            /* Check if this is a false alarm (last_used caught up to device) */
            if (current_used_idx < last_used_idx) {
                /* This is expected: we consumed faster than device produced
                 * Just resync and exit - no packets available right now */
                last_used_idx = current_used_idx;
                return;
            }

            /* True desync - should never happen with proper memory barriers */
            printf("%s: ⚠️ TRUE DESYNC: pending=%u exceeds ring_size=%u\n",
                   COMPONENT_NAME, pending_packets, vq->num);
            printf("%s:   last_used_idx=%u, current_used_idx=%u\n",
                   COMPONENT_NAME, last_used_idx, current_used_idx);
            last_used_idx = current_used_idx;
            return;
        }

        uint16_t used_ring_idx = last_used_idx % vq->num;
        struct virtq_used_elem *used_elem = &vq->used->ring[used_ring_idx];

        uint16_t desc_idx = used_elem->id;
        uint32_t len = used_elem->len;

        /* Safety check: prevent infinite loops (should never happen with memory barrier) */
        loop_count++;
        if (loop_count > 1000) {
            printf("%s: ERROR - Processed 1000 packets in single call, breaking to prevent freeze\n",
                   COMPONENT_NAME);
            printf("%s:   last_used_idx=%u, current_used_idx=%u, pending=%u\n",
                   COMPONENT_NAME, last_used_idx, current_used_idx,
                   (uint16_t)(current_used_idx - last_used_idx));
            break;
        }

        /* CRITICAL: Validate VirtIO reported length before processing
         * Valid Ethernet frames: 60-1514 bytes + 12 byte VirtIO header = 72-1526 bytes
         * With memory barrier fix, invalid lengths should be VERY rare (real hardware corruption)
         */
        if (len < VIRTIO_NET_HDR_SIZE || len > (1514 + VIRTIO_NET_HDR_SIZE)) {
            printf("%s: ⚠️  INVALID packet length: %u bytes (expected %u-%u)\n",
                   COMPONENT_NAME, len, VIRTIO_NET_HDR_SIZE, 1514 + VIRTIO_NET_HDR_SIZE);
            printf("%s:     desc_idx=%u, used_ring_idx=%u, last_used_idx=%u, current_used_idx=%u\n",
                   COMPONENT_NAME, desc_idx, used_ring_idx, last_used_idx, current_used_idx);

            /* Mark buffer as free and continue */
            if (desc_idx < MAX_PACKETS) {
                rx_buffer_used[desc_idx] = false;
            }

            last_used_idx++;
            continue;  /* Skip this corrupted entry */
        }

        /* Validate descriptor index is in range */
        if (desc_idx >= MAX_PACKETS) {
            printf("%s: ⚠️  INVALID descriptor index: %u (max %u)\n",
                   COMPONENT_NAME, desc_idx, MAX_PACKETS);
            printf("%s:     Skipping corrupted descriptor\n", COMPONENT_NAME);
            last_used_idx++;
            continue;
        }

        /* Get packet buffer (use buffer index, not physical address from descriptor) */
        int buf_idx = desc_idx;
        uint8_t *buffer = packet_buffers[buf_idx];

        /* Skip virtio_net_hdr at start of buffer */
        uint8_t *packet_data = buffer + VIRTIO_NET_HDR_SIZE;
        uint16_t packet_len = len - VIRTIO_NET_HDR_SIZE;

        packets_received++;

        /* NOTE: TCP server initialization moved to post_init()
         * The tcp_server_initialized flag is set there.
         * This deferred initialization code is no longer needed.
         */

        #if DEBUG_PACKET_LOG
        /* Log packet arrival with detailed inspection (VERY VERBOSE - only for debugging) */
        uint32_t timestamp_ms = sys_now();
        printf("\n╔══════════════════════════════════════════════════════════╗\n");
        printf("║  📥 [Net0] INCOMING PACKET #%u [T=%u.%03us]              ║\n",
               packets_received, timestamp_ms / 1000, timestamp_ms % 1000);
        printf("╚══════════════════════════════════════════════════════════╝\n");
        printf("  Size: %u bytes (total %u with VirtIO header)\n", packet_len, len);

        /* Hex dump of raw packet */
        hex_dump_packet("  Raw packet", packet_data, packet_len, 128);

        /* Parse Ethernet header */
        if (packet_len >= sizeof(struct ethhdr)) {
            struct ethhdr *eth = (struct ethhdr *)packet_data;
            printf("  Ethernet: %02x:%02x:%02x:%02x:%02x:%02x → %02x:%02x:%02x:%02x:%02x:%02x\n",
                   eth->h_source[0], eth->h_source[1], eth->h_source[2],
                   eth->h_source[3], eth->h_source[4], eth->h_source[5],
                   eth->h_dest[0], eth->h_dest[1], eth->h_dest[2],
                   eth->h_dest[3], eth->h_dest[4], eth->h_dest[5]);
            printf("  EtherType: 0x%04x", ntohs(eth->h_proto));

            /* Parse IP if present */
            if (ntohs(eth->h_proto) == 0x0800 && packet_len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
                printf(" (IPv4)\n");
                struct iphdr *ip = (struct iphdr *)(packet_data + sizeof(struct ethhdr));
                uint32_t saddr = ntohl(ip->saddr);
                uint32_t daddr = ntohl(ip->daddr);
                printf("  IP: %u.%u.%u.%u → %u.%u.%u.%u (protocol=%u)\n",
                       (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF,
                       (saddr >> 8) & 0xFF, saddr & 0xFF,
                       (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF,
                       (daddr >> 8) & 0xFF, daddr & 0xFF,
                       ip->protocol);

                /* Parse TCP if present */
                if (ip->protocol == 6) {
                    size_t ip_hdr_len = (ip->ihl) * 4;
                    if (packet_len >= sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct tcphdr)) {
                        struct tcphdr *tcp = (struct tcphdr *)(packet_data + sizeof(struct ethhdr) + ip_hdr_len);
                        printf("  TCP: port %u → %u (flags: ", ntohs(tcp->source), ntohs(tcp->dest));
                        if (tcp->syn) printf("SYN ");
                        if (tcp->ack) printf("ACK ");
                        if (tcp->fin) printf("FIN ");
                        if (tcp->rst) printf("RST ");
                        if (tcp->psh) printf("PSH ");
                        printf(")\n");

                        size_t tcp_hdr_len = (tcp->doff) * 4;
                        size_t payload_offset = sizeof(struct ethhdr) + ip_hdr_len + tcp_hdr_len;
                        if (packet_len > payload_offset) {
                            size_t payload_len = packet_len - payload_offset;
                            printf("  TCP Payload (%zu bytes):\n", payload_len);
                            print_ascii_payload(packet_data + payload_offset, payload_len);
                        }
                    }
                }
            } else if (ntohs(eth->h_proto) == 0x0806) {
                printf(" (ARP)\n");
            } else {
                printf("\n");
            }
        }
        printf("══════════════════════════════════════════════════════════\n\n");
        #endif /* DEBUG_PACKET_LOG */

        #if DEBUG_PACKET_SUMMARY
        /* Lightweight packet summary - just protocol and ports */
        if (packet_len >= sizeof(struct ethhdr)) {
            struct ethhdr *eth = (struct ethhdr *)packet_data;
            uint16_t eth_proto = ntohs(eth->h_proto);

            if (eth_proto == 0x0800 && packet_len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
                struct iphdr *ip = (struct iphdr *)(packet_data + sizeof(struct ethhdr));
                uint32_t saddr = ntohl(ip->saddr);
                uint32_t daddr = ntohl(ip->daddr);

                if (ip->protocol == 6) {  /* TCP */
                    size_t ip_hdr_len = (ip->ihl) * 4;
                    if (packet_len >= sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct tcphdr)) {
                        struct tcphdr *tcp = (struct tcphdr *)(packet_data + sizeof(struct ethhdr) + ip_hdr_len);
                        printf("📥 Net0 RX #%u: %u.%u.%u.%u:%u → %u.%u.%u.%u:%u TCP %s%s%s%s\n",
                               packets_received,
                               (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF, (saddr >> 8) & 0xFF, saddr & 0xFF, ntohs(tcp->source),
                               (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF, ntohs(tcp->dest),
                               tcp->syn ? "[SYN]" : "", tcp->ack ? "[ACK]" : "", tcp->fin ? "[FIN]" : "", tcp->rst ? "[RST]" : "");
                    }
                } else {
                    printf("📥 Net0 RX #%u: %u.%u.%u.%u → %u.%u.%u.%u proto=%u (%ub)\n",
                           packets_received,
                           (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF, (saddr >> 8) & 0xFF, saddr & 0xFF,
                           (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF,
                           ip->protocol, packet_len);
                }
            } else if (eth_proto == 0x0806) {
                printf("📥 Net0 RX #%u: ARP (%u bytes)\n", packets_received, packet_len);
            } else {
                printf("📥 Net0 RX #%u: EtherType=0x%04x (%u bytes)\n", packets_received, eth_proto, packet_len);
            }
        }
        #endif /* DEBUG_PACKET_SUMMARY */

        #if DEBUG_MESSAGE_FLOW
        uint32_t msg_id = ++message_id_counter;
        printf("\n🔵 [MSG #%u] ═══ RX: Packet received from VirtIO device ═══\n", msg_id);
        printf("   Size: %u bytes, Buffer index: %d\n", packet_len, buf_idx);
        printf("   Action: Feeding to lwIP stack for processing\n");
        #endif

        /* Allocate pbuf and copy packet data (skipping header) */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, packet_len, PBUF_POOL);
        if (p != NULL) {
            pbuf_take(p, packet_data, packet_len);

            #if DEBUG_MESSAGE_FLOW
            printf("   ✓ pbuf allocated, passing to lwIP input handler\n");
            #endif

            /* Feed packet to lwIP */
            err_t lwip_result = netif_data.input(p, &netif_data);

            #if DEBUG_MESSAGE_FLOW
            if (lwip_result == ERR_OK) {
                printf("   ✓ lwIP accepted packet (will route to TCP/UDP/etc.)\n");
            } else {
                printf("   ✗ lwIP rejected packet (err=%d)\n", lwip_result);
            }
            #endif

            if (lwip_result != ERR_OK) {
                pbuf_free(p);
            }
        } else {
            /* CRITICAL: pbuf allocation failed - this means lwIP is out of memory */
            printf("%s: ⚠️  WARNING: Failed to allocate pbuf for packet #%u - dropping (lwIP out of memory)\n",
                   COMPONENT_NAME, packets_received);
        }

        /* Mark buffer as free (buf_idx already defined above) */
        rx_buffer_used[buf_idx] = false;

        /* Move to next packet */
        last_used_idx++;
    }

    /* Print pbuf pool statistics every 10 packets to monitor allocation/deallocation */
    if (packets_received % 10 == 0 && packets_received > 0) {
        printf("%s: 📊 PBUF Pool Stats - Used: %u/%u, Avail: %u, Peak: %u\n",
               COMPONENT_NAME,
               lwip_stats.memp[MEMP_PBUF_POOL]->used,
               PBUF_POOL_SIZE,
               lwip_stats.memp[MEMP_PBUF_POOL]->avail,
               lwip_stats.memp[MEMP_PBUF_POOL]->max);
    }

    refill_rx_queue();
}

/*
 * Connection tracking
 */
static uint32_t active_connections = 0;
static uint32_t total_connections_created = 0;
static uint32_t total_connections_closed = 0;

/*
 * TCP Error callback - handles connection errors and cleanup
 */
static void tcp_echo_err(void *arg, err_t err)
{
    const char *err_name;
    switch (err) {
        case ERR_ABRT:     err_name = "ERR_ABRT (Connection aborted)"; break;
        case ERR_RST:      err_name = "ERR_RST (Connection reset)"; break;
        case ERR_CLSD:     err_name = "ERR_CLSD (Connection closed)"; break;
        case ERR_CONN:     err_name = "ERR_CONN (Not connected)"; break;
        case ERR_TIMEOUT:  err_name = "ERR_TIMEOUT (Timeout)"; break;
        default:           err_name = "UNKNOWN"; break;
    }

    active_connections--;
    total_connections_closed++;

    printf("%s: ⚠️  TCP connection error - err=%d (%s)\n", COMPONENT_NAME, err, err_name);
    printf("%s:    → Active connections: %u | Total created: %u | Total closed: %u\n",
           COMPONENT_NAME, active_connections, total_connections_created, total_connections_closed);

    /* PCB is already freed by lwIP when err callback is called - don't access it! */
}

/*
 * TCP Echo callbacks
 */
static err_t tcp_echo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (p == NULL) {
        /* Connection closed gracefully by remote peer */
        active_connections--;
        total_connections_closed++;

        printf("%s: 🔌 TCP connection closed gracefully\n", COMPONENT_NAME);
        printf("%s:    Remote: %u.%u.%u.%u:%u\n", COMPONENT_NAME,
               ip4_addr1(&pcb->remote_ip), ip4_addr2(&pcb->remote_ip),
               ip4_addr3(&pcb->remote_ip), ip4_addr4(&pcb->remote_ip), pcb->remote_port);
        printf("%s:    → Active connections: %u | Total created: %u | Total closed: %u\n",
               COMPONENT_NAME, active_connections, total_connections_created, total_connections_closed);

        tcp_close(pcb);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    /* ═══ Forward TCP data to ICS_Inbound (INBOUND path) ═══ */

    #if DEBUG_MESSAGE_FLOW
    uint32_t msg_id = ++message_id_counter;
    printf("\n🟢 [MSG #%u] ═══ TCP: Data received from TCP connection ═══\n", msg_id);
    printf("   Connection: %u.%u.%u.%u:%u → %u.%u.%u.%u:%u\n",
           ip4_addr1(&pcb->remote_ip), ip4_addr2(&pcb->remote_ip),
           ip4_addr3(&pcb->remote_ip), ip4_addr4(&pcb->remote_ip), pcb->remote_port,
           ip4_addr1(&pcb->local_ip), ip4_addr2(&pcb->local_ip),
           ip4_addr3(&pcb->local_ip), ip4_addr4(&pcb->local_ip), pcb->local_port);
    printf("   Payload size: %u bytes\n", p->len);

    /* Print ASCII payload preview */
    printf("   Payload preview: \"");
    for (uint16_t i = 0; i < (p->len < 60 ? p->len : 60); i++) {
        char c = ((char*)p->payload)[i];
        if (c >= 32 && c <= 126) printf("%c", c);
        else if (c == '\n') printf("\\n");
        else if (c == '\r') printf("\\r");
        else printf(".");
    }
    if (p->len > 60) printf("...");
    printf("\"\n");
    #endif

    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (inbound_dp == NULL) {
        printf("%s: ❌ FATAL: inbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        printf("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        pbuf_free(p);
        return ERR_MEM;
    }

    printf("%s: ✓ Dataport check: inbound_dp=%p (valid)\n", COMPONENT_NAME, (void*)inbound_dp);

    /* Step 1: Create ICS message with metadata */
    ICS_Message *ics_msg = (ICS_Message *)inbound_dp;

    /* Step 2: Populate FrameMetadata (Phase 1: basic info, Phase 2: full header parsing) */
    printf("%s: About to memset ics_msg->metadata at %p (size=%zu)\n",
           COMPONENT_NAME, (void*)&ics_msg->metadata, sizeof(FrameMetadata));
    memset(&ics_msg->metadata, 0, sizeof(FrameMetadata));

    /* Basic metadata - will be enhanced with full frame parsing */
    ics_msg->metadata.ethertype = 0x0800;  /* IPv4 */
    ics_msg->metadata.ip_protocol = 6;     /* TCP */
    ics_msg->metadata.is_ip = 1;
    ics_msg->metadata.is_tcp = 1;

    /* Extract IP addresses from lwIP pcb (network byte order -> host byte order) */
    ics_msg->metadata.src_ip = ntohl(ip4_addr_get_u32(&pcb->remote_ip));
    ics_msg->metadata.dst_ip = ntohl(ip4_addr_get_u32(&pcb->local_ip));

    ics_msg->metadata.src_port = pcb->remote_port;
    ics_msg->metadata.dst_port = pcb->local_port;
    ics_msg->metadata.payload_offset = 0;  /* TCP payload directly */
    ics_msg->metadata.payload_length = (p->len < MAX_PAYLOAD_SIZE) ? p->len : MAX_PAYLOAD_SIZE;

    /* Step 3: Copy TCP payload */
    ics_msg->payload_length = ics_msg->metadata.payload_length;
    memcpy(ics_msg->payload, p->payload, ics_msg->payload_length);

    printf("%s: INBOUND: Forwarding %u bytes to ICS_Inbound (proto=TCP, src_port=%u, dst_port=%u)\n",
           COMPONENT_NAME, ics_msg->payload_length,
           ics_msg->metadata.src_port, ics_msg->metadata.dst_port);

    /* Always show RAW payload for debugging */
    printf("%s: RAW PAYLOAD (%u bytes): \"", COMPONENT_NAME, ics_msg->payload_length);
    for (uint16_t i = 0; i < ics_msg->payload_length && i < 200; i++) {
        char c = ics_msg->payload[i];
        if (c >= 32 && c <= 126) printf("%c", c);
        else if (c == '\n') printf("\\n");
        else if (c == '\r') printf("\\r");
        else if (c == '\t') printf("\\t");
        else printf("[0x%02x]", (unsigned char)c);
    }
    if (ics_msg->payload_length > 200) printf("... (%u more bytes)", ics_msg->payload_length - 200);
    printf("\"\n");

    #if DEBUG_MESSAGE_FLOW
    printf("   ✓ ICS message prepared in shared memory (inbound_dp)\n");
    printf("   Action: Signaling ICS_Inbound component via inbound_ready_emit()\n");
    #endif

    /* Step 4: Signal ICS_Inbound that message is ready */
    inbound_ready_emit();

    #if DEBUG_MESSAGE_FLOW
    printf("   ✓ Signal sent to ICS_Inbound - message handoff complete\n");
    printf("   [MSG #%u now in ICS pipeline - waiting for processing]\n\n", msg_id);
    #endif

    /* Tell TCP we've processed the data */
    tcp_recved(pcb, p->len);

    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_echo_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == NULL) {
        printf("%s: ❌ TCP accept FAILED - err=%d (%s), newpcb=%p\n",
               COMPONENT_NAME, err,
               err == -1 ? "OUT OF MEMORY (ERR_MEM)" :
               err == -13 ? "CONNECTION ABORTED (ERR_ABRT)" : "UNKNOWN",
               newpcb);
        if (err == -1) {
            printf("%s:    → lwIP ran out of TCP PCBs! Check MEMP_NUM_TCP_PCB in lwipopts.h\n",
                   COMPONENT_NAME);
            printf("%s:    → Current active connections: %u\n", COMPONENT_NAME, active_connections);
        }
        return err != ERR_OK ? err : ERR_VAL;
    }

    active_connections++;
    total_connections_created++;

    printf("%s: ✓ TCP connection accepted from %u.%u.%u.%u:%u (pcb=%p)\n",
           COMPONENT_NAME,
           ip4_addr1(&newpcb->remote_ip), ip4_addr2(&newpcb->remote_ip),
           ip4_addr3(&newpcb->remote_ip), ip4_addr4(&newpcb->remote_ip), newpcb->remote_port,
           newpcb);
    printf("%s:    → Active connections: %u | Total created: %u | Total closed: %u\n",
           COMPONENT_NAME, active_connections, total_connections_created, total_connections_closed);

    tcp_setprio(newpcb, TCP_PRIO_MIN);
    tcp_recv(newpcb, tcp_echo_recv);
    tcp_err(newpcb, tcp_echo_err);  /* Register error callback for connection cleanup */

    return ERR_OK;
}

/*
 * Setup TCP echo server
 */
static void setup_tcp_echo_server(void)
{
    struct tcp_pcb *pcb;

#if DEBUG_VERBOSE
    printf("\n%s: ╔════════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
    printf("%s: ║  [DEBUG] Entering setup_tcp_echo_server()                 ║\n", COMPONENT_NAME);
    printf("%s: ╚════════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
    printf("%s: [DEBUG] About to call tcp_new_ip_type(IPADDR_TYPE_V4)...\n", COMPONENT_NAME);
    fflush(stdout);
#endif

    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);

#if DEBUG_VERBOSE
    printf("%s: [DEBUG] tcp_new_ip_type() returned: pcb=%p\n", COMPONENT_NAME, (void*)pcb);
    fflush(stdout);
#endif

    if (pcb == NULL) {
        printf("%s: ❌ Failed to create TCP PCB\n", COMPONENT_NAME);
#if DEBUG_VERBOSE
        printf("%s: [DEBUG] TCP PCB creation returned NULL - malloc likely failed\n", COMPONENT_NAME);
        printf("%s: [DEBUG] This suggests lwIP memory allocator is not ready\n", COMPONENT_NAME);
        fflush(stdout);
#endif
        return;
    }

#if DEBUG_VERBOSE
    printf("%s: [DEBUG] ✓ TCP PCB created successfully at %p\n", COMPONENT_NAME, (void*)pcb);
    printf("%s: [DEBUG] About to call tcp_bind(pcb, IP_ANY_TYPE, %d)...\n", COMPONENT_NAME, TCP_ECHO_PORT);
    fflush(stdout);
#endif

    err_t err = tcp_bind(pcb, IP_ANY_TYPE, TCP_ECHO_PORT);

#if DEBUG_VERBOSE
    printf("%s: [DEBUG] tcp_bind() returned: err=%d (%s)\n", COMPONENT_NAME,
           err, err == ERR_OK ? "ERR_OK" : "ERROR");
    fflush(stdout);
#endif

    if (err != ERR_OK) {
        printf("%s: ❌ Failed to bind TCP port %d (err=%d)\n", COMPONENT_NAME, TCP_ECHO_PORT, err);
        return;
    }

#if DEBUG_VERBOSE
    printf("%s: [DEBUG] ✓ Successfully bound to port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
    printf("%s: [DEBUG] About to call tcp_listen_with_backlog(pcb, %d)...\n", COMPONENT_NAME, MAX_TCP_CONNECTIONS);
    fflush(stdout);
#endif

    pcb = tcp_listen_with_backlog(pcb, MAX_TCP_CONNECTIONS);

#if DEBUG_VERBOSE
    printf("%s: [DEBUG] tcp_listen_with_backlog() returned: pcb=%p\n", COMPONENT_NAME, (void*)pcb);
    fflush(stdout);
#endif

    if (pcb == NULL) {
        printf("%s: ❌ Failed to listen on TCP port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
        return;
    }

#if DEBUG_VERBOSE
    printf("%s: [DEBUG] ✓ Now listening on port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
    printf("%s: [DEBUG] About to call tcp_accept(pcb, tcp_echo_accept)...\n", COMPONENT_NAME);
    fflush(stdout);
#endif

    tcp_accept(pcb, tcp_echo_accept);

#if DEBUG_VERBOSE
    printf("%s: [DEBUG] ✓ Accept callback registered\n", COMPONENT_NAME);
    printf("%s: [DEBUG] Exiting setup_tcp_echo_server() - SUCCESS\n", COMPONENT_NAME);
    fflush(stdout);
#endif

    printf("%s: TCP echo server created (will bind after DHCP)\n", COMPONENT_NAME);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * OUTBOUND PATH: ICS_Outbound → External Network (TCP Client)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* TCP client connection state for OUTBOUND forwarding */
struct tcp_outbound_client_state {
    struct tcp_pcb *pcb;
    uint8_t *payload_data;
    uint16_t payload_len;
    uint16_t bytes_sent;
    bool active;
};

static struct tcp_outbound_client_state outbound_tcp_client = {0};

/*
 * TCP client callbacks for OUTBOUND path
 */
static err_t outbound_tcp_sent_callback(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    struct tcp_outbound_client_state *state = (struct tcp_outbound_client_state *)arg;

    printf("%s: OUTBOUND: Sent %u bytes to external network\n", COMPONENT_NAME, len);

    state->bytes_sent += len;

    /* Check if all data sent */
    if (state->bytes_sent >= state->payload_len) {
        printf("%s: OUTBOUND: Complete - sent %u/%u bytes\n",
               COMPONENT_NAME, state->bytes_sent, state->payload_len);

        /* Close connection after successful transmission */
        tcp_close(pcb);
        state->active = false;
        state->pcb = NULL;

        return ERR_OK;
    }

    /* Send remaining data if needed */
    uint16_t remaining = state->payload_len - state->bytes_sent;
    uint16_t to_send = (remaining > tcp_sndbuf(pcb)) ? tcp_sndbuf(pcb) : remaining;

    if (to_send > 0) {
        err_t err = tcp_write(pcb, state->payload_data + state->bytes_sent, to_send, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK) {
            tcp_output(pcb);
        } else {
            printf("%s: OUTBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        }
    }

    return ERR_OK;
}

static err_t outbound_tcp_connected_callback(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct tcp_outbound_client_state *state = (struct tcp_outbound_client_state *)arg;

    if (err != ERR_OK) {
        printf("%s: OUTBOUND: Connection failed: %d\n", COMPONENT_NAME, err);
        state->active = false;
        state->pcb = NULL;
        return err;
    }

    printf("%s: OUTBOUND: Connected to external network\n", COMPONENT_NAME);

    /* Set sent callback */
    tcp_sent(pcb, outbound_tcp_sent_callback);

    /* Send the payload */
    uint16_t to_send = (state->payload_len > tcp_sndbuf(pcb)) ? tcp_sndbuf(pcb) : state->payload_len;

    err = tcp_write(pcb, state->payload_data, to_send, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("%s: OUTBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        tcp_close(pcb);
        state->active = false;
        state->pcb = NULL;
        return err;
    }

    state->bytes_sent = to_send;

    /* Trigger transmission */
    tcp_output(pcb);

    printf("%s: OUTBOUND: Sent initial %u bytes\n", COMPONENT_NAME, to_send);

    return ERR_OK;
}

/*
 * OUTBOUND notification handler - called when ICS_Outbound has validated data
 * Creates TCP client connection to forward data to external network
 */
void outbound_ready_handle(void)
{
    #if DEBUG_MESSAGE_FLOW
    uint32_t msg_id = ++message_id_counter;
    printf("\n🟡 [MSG #%u] ═══ ICS→NET: Received from ICS_Outbound ═══\n", msg_id);
    printf("   Source: ICS pipeline validation complete\n");
    printf("   Action: Creating TCP client to forward to external network\n");
    #endif

    printf("%s: ╔═══════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
    printf("%s: ║  OUTBOUND: Received message from ICS_Outbound            ║\n", COMPONENT_NAME);

    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (outbound_dp == NULL) {
        printf("%s: ❌ FATAL: outbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        printf("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        printf("%s: ╚═══════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
        return;
    }

    printf("%s: ✓ Dataport check: outbound_dp=%p (valid)\n", COMPONENT_NAME, (void*)outbound_dp);

    ICS_Message *ics_msg = (ICS_Message *)outbound_dp;
    printf("%s: ╚═══════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);

    /* Validate message */
    if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
        printf("%s: OUTBOUND: Invalid payload length %u (max %u)\n",
               COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
        #if DEBUG_MESSAGE_FLOW
        printf("   ✗ [MSG #%u] DROPPED - invalid payload size\n\n", msg_id);
        #endif
        return;
    }

    /* Check if we already have an active connection - if so, close it and create new one */
    if (outbound_tcp_client.active) {
        printf("%s: OUTBOUND: Previous connection still active, closing it to handle new message\n", COMPONENT_NAME);
        if (outbound_tcp_client.pcb != NULL) {
            tcp_close(outbound_tcp_client.pcb);
        }
        outbound_tcp_client.active = false;
        outbound_tcp_client.pcb = NULL;
    }

    #if DEBUG_MESSAGE_FLOW
    printf("   Payload size: %u bytes\n", ics_msg->payload_length);
    printf("   Destination: %u.%u.%u.%u:%u\n",
           (ics_msg->metadata.dst_ip >> 24) & 0xFF,
           (ics_msg->metadata.dst_ip >> 16) & 0xFF,
           (ics_msg->metadata.dst_ip >> 8) & 0xFF,
           ics_msg->metadata.dst_ip & 0xFF,
           ics_msg->metadata.dst_port);

    /* Print ASCII payload preview */
    printf("   Payload preview: \"");
    for (uint16_t i = 0; i < (ics_msg->payload_length < 60 ? ics_msg->payload_length : 60); i++) {
        char c = ics_msg->payload[i];
        if (c >= 32 && c <= 126) printf("%c", c);
        else if (c == '\n') printf("\\n");
        else if (c == '\r') printf("\\r");
        else printf(".");
    }
    if (ics_msg->payload_length > 60) printf("...");
    printf("\"\n");
    #endif

    /* Print metadata */
    printf("%s: OUTBOUND: Protocol=%s, Src=%u.%u.%u.%u:%u, Dst=%u.%u.%u.%u:%u, Payload=%u bytes\n",
           COMPONENT_NAME,
           ics_msg->metadata.is_tcp ? "TCP" : (ics_msg->metadata.is_udp ? "UDP" : "Other"),
           (ics_msg->metadata.src_ip >> 24) & 0xFF, (ics_msg->metadata.src_ip >> 16) & 0xFF,
           (ics_msg->metadata.src_ip >> 8) & 0xFF, ics_msg->metadata.src_ip & 0xFF,
           ics_msg->metadata.src_port,
           (ics_msg->metadata.dst_ip >> 24) & 0xFF, (ics_msg->metadata.dst_ip >> 16) & 0xFF,
           (ics_msg->metadata.dst_ip >> 8) & 0xFF, ics_msg->metadata.dst_ip & 0xFF,
           ics_msg->metadata.dst_port,
           ics_msg->payload_length);

    /* Create TCP client connection */
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        printf("%s: OUTBOUND: Failed to create TCP PCB\n", COMPONENT_NAME);
        return;
    }

    /* Set up client state */
    outbound_tcp_client.pcb = pcb;
    outbound_tcp_client.payload_data = ics_msg->payload;
    outbound_tcp_client.payload_len = ics_msg->payload_length;
    outbound_tcp_client.bytes_sent = 0;
    outbound_tcp_client.active = true;

    /* Set up destination IP address - use QEMU gateway to reach host */
    ip_addr_t dest_ip;
    ipaddr_aton(OUTBOUND_FORWARD_IP, &dest_ip);  /* 10.0.2.2 - QEMU gateway */

    /* CRITICAL: Set callback argument BEFORE tcp_connect()
     * This prevents null pointer dereference in outbound_tcp_sent_callback
     */
    tcp_arg(pcb, &outbound_tcp_client);

    /* CROSS-DOMAIN PORT MAPPING:
     * Internal port 7000 (Net1) → maps to → Host port 19000 (via QEMU gateway)
     * This creates the protocol break diode architecture
     */
    uint16_t mapped_port = OUTBOUND_FORWARD_PORT;  /* Configurable destination port */

    printf("%s: OUTBOUND: Port mapping: internal:%u → host:%s:%u (via QEMU gateway)\n",
           COMPONENT_NAME, ics_msg->metadata.dst_port, OUTBOUND_FORWARD_IP, mapped_port);

    /* Connect to host via QEMU gateway */
    printf("%s: OUTBOUND: Connecting to host via %s:%u...\n",
           COMPONENT_NAME, OUTBOUND_FORWARD_IP, mapped_port);

    err_t err = tcp_connect(pcb, &dest_ip, mapped_port, outbound_tcp_connected_callback);
    if (err != ERR_OK) {
        printf("%s: OUTBOUND: tcp_connect failed: %d\n", COMPONENT_NAME, err);
        tcp_close(pcb);
        outbound_tcp_client.active = false;
        outbound_tcp_client.pcb = NULL;
        return;
    }

    printf("%s: OUTBOUND: Connection initiated\n", COMPONENT_NAME);
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

    /* ═══════════════════════════════════════════════════════════════ */
    /* COMPREHENSIVE VIRTIO SLOT SCANNER                                */
    /* Scan all 32 VirtIO MMIO slots to find which ones have devices   */
    /* ═══════════════════════════════════════════════════════════════ */
    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  [DISABLED] SCANNING MAPPED VIRTIO MMIO SLOTS FOR ACTIVE DEVICES         ║\n");
    printf("║  Base: 0x0a000000, Each slot: 0x200 bytes apart               ║\n");
    printf("║  Scanning slots 24-31 only (one 4KB page mapped)                ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");

    /* Only scan slots within the mapped 4KB page (8 slots of 0x200 bytes each) */
//     for (int slot = 24; slot < 32; slot++) {
//         /* Calculate offset for this slot */
//         uint32_t offset = slot * 0x200;
//         volatile uint32_t *slot_base = (volatile uint32_t *)((uintptr_t)virtio_mmio_regs + offset);
// 
//         /* Read device identification registers with memory barriers (like VREG_READ) */
//         DMB();  /* Ensure all prior memory operations complete before reading */
//         /* Read device identification registers */
//         uint32_t slot_magic = slot_base[VIRTIO_MMIO_MAGIC_VALUE / 4];
//         uint32_t slot_version = slot_base[VIRTIO_MMIO_VERSION / 4];
//         uint32_t slot_device_id = slot_base[VIRTIO_MMIO_DEVICE_ID / 4];
//         DMB();  /* Ensure reads complete before using values */
//         uint32_t slot_vendor_id = slot_base[VIRTIO_MMIO_VENDOR_ID / 4];
// 
//         /* Only print slots with valid VirtIO magic */
//         if (slot_magic == 0x74726976) {
//             printf("Slot %2d @ 0x%08lx (offset +0x%03x): Magic=0x%08x Version=%u DeviceID=%u Vendor=0x%08x",
//                    slot, 0x0a000000 + offset, offset,
//                    slot_magic, slot_version, slot_device_id, slot_vendor_id);
// 
//             /* Identify device type */
//             if (slot_device_id == 1) {
//                 printf(" [NETWORK]\n");
//             } else if (slot_device_id == 0) {
//                 printf(" [NO DEVICE]\n");
//             } else {
//                 printf(" [UNKNOWN TYPE]\n");
//             }
//         }
//     }

    printf("\n");

    /* CRITICAL: Check if CAmkES dataport is properly mapped */
    if (virtio_mmio_regs == NULL) {
        printf("%s: ❌ FATAL: virtio_mmio_regs dataport is NULL!\n", COMPONENT_NAME);
        printf("%s:    CAmkES failed to map hardware component net0_hw\n", COMPONENT_NAME);
        printf("%s:    Check ics_dual_nic.camkes configuration\n", COMPONENT_NAME);
        return -1;
    }

    printf("%s: virtio_mmio_regs dataport mapped at %p\n", COMPONENT_NAME, (void *)virtio_mmio_regs);

    /* Access VirtIO device at SLOT 31 (offset 0xe00 from page base 0xa003000) */
    /* QEMU assigns FIRST -device virtio-net-device to slot 31 - matches vm_ethernet_echo */
    virtio_regs_base = (volatile uint32_t *)((uintptr_t)virtio_mmio_regs + 0xe00);

    printf("%s: virtio_regs_base (slot 31) = %p (base + 0xe00)\n",
           COMPONENT_NAME, (void *)virtio_regs_base);

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

    /* CRITICAL CHECK: Enforce modern VirtIO protocol */
    if (version != 2) {
        printf("\n");
        printf("╔════════════════════════════════════════════════════════════════╗\n");
        printf("║  ❌ FATAL ERROR: Legacy VirtIO Protocol Detected              ║\n");
        printf("╚════════════════════════════════════════════════════════════════╝\n");
        printf("\n");
        printf("%s: VirtIO Version=%u (expected 2 for modern protocol)\n", COMPONENT_NAME, version);
        printf("\n");
        printf("VirtIO legacy mode (Version 1) DOES NOT WORK on seL4 ARM32 hypervisor!\n");
        printf("\n");
        printf("ROOT CAUSE:\n");
        printf("  - Legacy VirtIO has MMIO write failures in Stage 2 page tables\n");
        printf("  - QueueReady/QueueSel registers become unresponsive\n");
        printf("  - Device initialization will FAIL\n");
        printf("\n");
        printf("REQUIRED FIX:\n");
        printf("  Add this QEMU flag to enable modern VirtIO protocol:\n");
        printf("\n");
        printf("  ./simulate --extra-qemu-args=\"-global virtio-mmio.force-legacy=false \\\n");
        printf("    -netdev user,id=net0,hostfwd=tcp::6000-:6000 \\\n");
        printf("    -device virtio-net-device,netdev=net0 \\\n");
        printf("    -netdev user,id=net1,hostfwd=tcp::7000-:7000 \\\n");
        printf("    -device virtio-net-device,netdev=net1\"\n");
        printf("\n");
        printf("WHAT THIS DOES:\n");
        printf("  ✓ Enables VirtIO 1.0+ modern protocol (Version 2)\n");
        printf("  ✓ Fixes MMIO write issues\n");
        printf("  ✓ Allocates devices to slots 6-7 (not 30-31)\n");
        printf("  ✓ Makes QueueReady registers writable\n");
        printf("\n");
        printf("DOCUMENTATION:\n");
        printf("  See: research-docs/VIRTIO-FORCE-LEGACY-REQUIREMENT.md\n");
        printf("\n");
        printf("╔════════════════════════════════════════════════════════════════╗\n");
        printf("║  System halted - cannot continue with legacy VirtIO           ║\n");
        printf("╚════════════════════════════════════════════════════════════════╝\n");
        printf("\n");
        return -1;
    }

    if (device_id != 1) {
        printf("%s: ERROR: DeviceID=%u (expected 1 for network)\n", COMPONENT_NAME, device_id);
        printf("%s: QEMU may have allocated the device to a different slot.\n", COMPONENT_NAME);
        printf("%s: With force-legacy=false, devices should be at slots 6-7.\n", COMPONENT_NAME);
        return -1;
    }

    printf("%s: ✓ Found VirtIO network device (modern protocol, Version 2)\n", COMPONENT_NAME);

    /* Reset device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, 0);

    /* Acknowledge device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* ═══════════════════════════════════════════════════════════
     * VirtIO Device Initialization Summary
     * ═══════════════════════════════════════════════════════════ */

    uint32_t device_features = VREG_READ(VIRTIO_MMIO_DEVICE_FEATURES);

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  VirtIO Network Device Initialization                   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("%s: Device ID: 0x%x (VirtIO-Net)\n", COMPONENT_NAME, device_id);
    printf("%s: DeviceFeatures: 0x%08x (CTRL_VQ %s)\n", COMPONENT_NAME,
           device_features, (device_features & (1<<18)) ? "enabled" : "disabled");
    printf("\n");


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

    #if DEBUG_VERBOSE
    printf("%s: DEBUG: ring_base virtual  = 0x%lx\n", COMPONENT_NAME, (uintptr_t)ring_base);
    printf("%s: DEBUG: ring_base physical = 0x%lx (via camkes_dma_get_paddr)\n", COMPONENT_NAME, ring_base_paddr);
    #endif

    /* RX queue */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_RX_QUEUE);
    #if DEBUG_VERBOSE
    printf("%s: DEBUG: QueueSel set to %u\n", COMPONENT_NAME, VIRTIO_NET_RX_QUEUE);
    printf("%s: DEBUG: QueueSel readback = %u\n", COMPONENT_NAME, VREG_READ(VIRTIO_MMIO_QUEUE_SEL));
    printf("%s: DEBUG: QueueNumMax = %u\n", COMPONENT_NAME, VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX));
    #endif

    /* Read and validate QueueNumMax from device register
     * VirtIO spec: Max queue size is typically 1024 for network devices
     */
    uint32_t queue_num_max = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);

    if (queue_num_max == 0) {
        printf("%s: WARNING: QueueNumMax is 0, using default 256\n", COMPONENT_NAME);
        rx_virtq.num = 256;
    } else if (queue_num_max > 1024) {
        printf("%s: WARNING: QueueNumMax=%u exceeds VirtIO max (1024), capping to 1024\n",
               COMPONENT_NAME, queue_num_max);
        rx_virtq.num = 1024;
    } else {
        rx_virtq.num = queue_num_max;
        printf("%s: RX Queue: QueueNumMax=%u (from device register)\n",
               COMPONENT_NAME, queue_num_max);
    }

    #if DEBUG_VERBOSE
    printf("%s: DEBUG: rx_virtq.num = %u\n", COMPONENT_NAME, rx_virtq.num);
    #endif

    /* Virtual addresses for driver access */
    rx_virtq.desc = (struct virtq_desc *)ring_base;
    rx_virtq.avail = (struct virtq_avail *)(ring_base + 0x2000);
    rx_virtq.used = (struct virtq_used *)(ring_base + 0x2408);

    /* Physical addresses for device DMA access */
    uintptr_t desc_paddr = ring_base_paddr;
    uintptr_t avail_paddr = ring_base_paddr + 0x2000;
    uintptr_t used_paddr = ring_base_paddr + 0x2408;

    #if DEBUG_VERBOSE
    printf("%s: DEBUG: RX desc paddr  = 0x%lx\n", COMPONENT_NAME, desc_paddr);
    printf("%s: DEBUG: RX avail paddr = 0x%lx\n", COMPONENT_NAME, avail_paddr);
    printf("%s: DEBUG: RX used paddr  = 0x%lx\n", COMPONENT_NAME, used_paddr);
    #endif

    VREG_WRITE(VIRTIO_MMIO_QUEUE_NUM, rx_virtq.num);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)used_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used_paddr >> 32));

    printf("\n%s: RX Queue Configuration BEFORE setting ready:\n", COMPONENT_NAME);
    printf("  QueueNum written: %u\n", rx_virtq.num);
    printf("  Desc  paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", desc_paddr, (uint32_t)desc_paddr, (uint32_t)(desc_paddr >> 32));
    printf("  Avail paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", avail_paddr, (uint32_t)avail_paddr, (uint32_t)(avail_paddr >> 32));
    printf("  Used  paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", used_paddr, (uint32_t)used_paddr, (uint32_t)(used_paddr >> 32));

    printf("\n%s: Reading back RX queue registers BEFORE ready:\n", COMPONENT_NAME);
    printf("  QueueNum:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_NUM), rx_virtq.num);
    printf("  DescLow:      0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_DESC_LOW), (uint32_t)desc_paddr);
    printf("  DescHigh:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_DESC_HIGH), (uint32_t)(desc_paddr >> 32));
    printf("  AvailLow:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_AVAIL_LOW), (uint32_t)avail_paddr);
    printf("  AvailHigh:    0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_AVAIL_HIGH), (uint32_t)(avail_paddr >> 32));
    printf("  UsedLow:      0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_USED_LOW), (uint32_t)used_paddr);
    printf("  UsedHigh:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_USED_HIGH), (uint32_t)(used_paddr >> 32));
    printf("  QueueReady:   0x%08x (expect 0 before write)\n", VREG_READ(VIRTIO_MMIO_QUEUE_READY));

    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);
    DMB();

    uint32_t rx_ready_after = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    printf("\n%s: After writing QUEUE_READY=1:\n", COMPONENT_NAME);
    printf("  QueueReady readback: 0x%08x (expect 1 if QEMU accepted config)\n", rx_ready_after);
    if (rx_ready_after == 0) {
        printf("  ❌ QEMU REJECTED RX queue - configuration invalid!\n");
    } else {
        printf("  ✅ QEMU ACCEPTED RX queue\n");
    }

    /* TX queue */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_TX_QUEUE);

    /* Read and validate QueueNumMax from device register */
    queue_num_max = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);

    if (queue_num_max == 0) {
        printf("%s: WARNING: TX QueueNumMax is 0, using default 256\n", COMPONENT_NAME);
        tx_virtq.num = 256;
    } else if (queue_num_max > 1024) {
        printf("%s: WARNING: TX QueueNumMax=%u exceeds VirtIO max (1024), capping to 1024\n",
               COMPONENT_NAME, queue_num_max);
        tx_virtq.num = 1024;
    } else {
        tx_virtq.num = queue_num_max;
        printf("%s: TX Queue: QueueNumMax=%u (from device register)\n",
               COMPONENT_NAME, queue_num_max);
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

    printf("\n%s: TX Queue Configuration BEFORE setting ready:\n", COMPONENT_NAME);
    printf("  QueueNum written: %u\n", tx_virtq.num);
    printf("  Desc  paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", tx_desc_paddr, (uint32_t)tx_desc_paddr, (uint32_t)(tx_desc_paddr >> 32));
    printf("  Avail paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", tx_avail_paddr, (uint32_t)tx_avail_paddr, (uint32_t)(tx_avail_paddr >> 32));
    printf("  Used  paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", tx_used_paddr, (uint32_t)tx_used_paddr, (uint32_t)(tx_used_paddr >> 32));

    printf("\n%s: Reading back TX queue registers BEFORE ready:\n", COMPONENT_NAME);
    printf("  QueueNum:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_NUM), tx_virtq.num);
    printf("  DescLow:      0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_DESC_LOW), (uint32_t)tx_desc_paddr);
    printf("  DescHigh:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_DESC_HIGH), (uint32_t)(tx_desc_paddr >> 32));
    printf("  AvailLow:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_AVAIL_LOW), (uint32_t)tx_avail_paddr);
    printf("  AvailHigh:    0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_AVAIL_HIGH), (uint32_t)(tx_avail_paddr >> 32));
    printf("  UsedLow:      0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_USED_LOW), (uint32_t)tx_used_paddr);
    printf("  UsedHigh:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_USED_HIGH), (uint32_t)(tx_used_paddr >> 32));
    printf("  QueueReady:   0x%08x (expect 0 before write)\n", VREG_READ(VIRTIO_MMIO_QUEUE_READY));

    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);
    DMB();

    uint32_t tx_ready_after = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    printf("\n%s: After writing QUEUE_READY=1:\n", COMPONENT_NAME);
    printf("  QueueReady readback: 0x%08x (expect 1 if QEMU accepted config)\n", tx_ready_after);
    if (tx_ready_after == 0) {
        printf("  ❌ QEMU REJECTED TX queue - configuration invalid!\n");
    } else {
        printf("  ✅ QEMU ACCEPTED TX queue\n");
    }

    /* Device ready - activate the device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VREG_READ(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);
    printf("%s: ✓ VirtIO device initialized and activated\n", COMPONENT_NAME);
    /* ═══ CRITICAL: Test if MMIO writes work ═══ */
    printf("\n%s: Testing MMIO write capability...\n", COMPONENT_NAME);

    /* Select queue 0 (RX queue) */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);

    /* Read current QueueReady state */
    uint32_t original_ready = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    printf("%s:   Queue 0 original ready state = 0x%x\n", COMPONENT_NAME, original_ready);

    /* Test write by toggling QueueReady */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 0x0);
    uint32_t read_back_0 = VREG_READ(VIRTIO_MMIO_QUEUE_READY);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 0x1);
    uint32_t read_back_1 = VREG_READ(VIRTIO_MMIO_QUEUE_READY);

    printf("%s:   Write 0x0, read back: 0x%x (expect 0x0)\n", COMPONENT_NAME, read_back_0);
    printf("%s:   Write 0x1, read back: 0x%x (expect 0x1)\n", COMPONENT_NAME, read_back_1);

    /* Restore original state */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, original_ready);

    if (read_back_0 != 0x0 || read_back_1 != 0x1) {
        printf("\n%s: ❌❌❌ FATAL ERROR: MMIO WRITES DO NOT WORK! ❌❌❌\n", COMPONENT_NAME);
        printf("%s: Device memory attributes are incorrect.\n", COMPONENT_NAME);
        printf("%s: This will cause infinite IRQ loops and duplicate packets.\n", COMPONENT_NAME);
        printf("%s: Cannot continue - terminating initialization.\n\n", COMPONENT_NAME);
        return -1;
    }

    printf("%s:   ✅ MMIO writes work correctly!\n\n", COMPONENT_NAME);

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

    /* Allocate packet buffers from DMA memory */
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

    /* Allocate TX headers array */
    size_t tx_headers_size = MAX_PACKETS * sizeof(virtio_net_hdr_t);
    tx_headers = camkes_dma_alloc(tx_headers_size, 16, false);
    if (!tx_headers) {
        printf("%s: ERROR: Failed to allocate TX headers DMA memory\n", COMPONENT_NAME);
        return;
    }
    tx_headers_paddr = camkes_dma_get_paddr(tx_headers);
    memset(tx_headers, 0, tx_headers_size);
    printf("%s: ✓ Allocated TX headers array (vaddr=%p, paddr=0x%lx)\n",
           COMPONENT_NAME, tx_headers, tx_headers_paddr);

    /* Initialize packet buffers */
    memset(rx_buffer_used, 0, sizeof(rx_buffer_used));
    refill_rx_queue();

    /* Initialize lwIP */
    printf("%s: Initializing lwIP TCP/IP stack...\n", COMPONENT_NAME);
    lwip_init();


    /* Add network interface with STATIC IP */
    struct ip4_addr ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 10, 2, 0, 2);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 2, 0, 1);

    printf("%s: Using STATIC IP configuration:\n", COMPONENT_NAME);
    printf("%s:   IP:      10.2.0.2\n", COMPONENT_NAME);
    printf("%s:   Netmask: 255.255.255.0\n", COMPONENT_NAME);
    printf("%s:   Gateway: 10.2.0.1\n", COMPONENT_NAME);

    netif_add(&netif_data, &ipaddr, &netmask, &gw, NULL, custom_netif_init, ethernet_input);
    netif_set_default(&netif_data);
    netif_set_status_callback(&netif_data, netif_status_callback);
    netif_set_up(&netif_data);

    printf("%s: Network interface UP with static IP\n", COMPONENT_NAME);
    setup_tcp_echo_server();
    tcp_server_initialized = true;
    printf("%s: ✓ Initialization complete\n", COMPONENT_NAME);
    printf("%s: Network ready\n\n", COMPONENT_NAME);

    /* Mark initialization as successful */
    initialization_successful = true;

    printf("%s: post_init() complete - returning to allow pipeline to start\n", COMPONENT_NAME);
}

int run(void)
{
    /* Validate initialization completed successfully */
    if (!initialization_successful) {
        printf("\n");
        printf("╔══════════════════════════════════════════════════════════╗\n");
        printf("║  ❌ FATAL: VirtIO_Net0_Driver initialization FAILED     ║\n");
        printf("╚══════════════════════════════════════════════════════════╝\n");
        printf("\n");
        printf("%s: Initialization did not complete successfully\n", COMPONENT_NAME);
        printf("%s: Common causes:\n", COMPONENT_NAME);
        printf("%s:   - DMA memory pool exhausted (check MAX_PACKETS setting)\n", COMPONENT_NAME);
        printf("%s:   - VirtIO device not found or misconfigured\n", COMPONENT_NAME);
        printf("%s:   - Network interface setup failed\n", COMPONENT_NAME);
        printf("\n");
        printf("%s: SYSTEM HALTED - cannot continue without working network driver\n", COMPONENT_NAME);
        printf("\n");
        while (1) {
            seL4_Yield();  /* Halt forever */
        }
    }

    printf("%s: ✅ Initialization validation passed - starting main loop\n", COMPONENT_NAME);

    /* Main event loop - process lwIP timers, RX packets, and ICS notifications */
    /* Note: TCP server is now initialized in RX path after first packet */
    while (1) {
        /* Check for OUTBOUND notifications from ICS_Outbound (non-blocking) */
        if (outbound_ready_poll()) {
            outbound_ready_handle();
        }

        /* Process lwIP timers and RX packets */
        sys_check_timeouts();
        process_rx_packets();

        seL4_Yield();
    }

    return 0;
}
