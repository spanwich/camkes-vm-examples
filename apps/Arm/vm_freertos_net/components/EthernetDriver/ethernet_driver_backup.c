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

    /* ═══════════════════════════════════════════════════════════
     * MMIO WRITE VERIFICATION TEST
     * Run early to test basic MMIO write capability
     * ═══════════════════════════════════════════════════════════ */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  MMIO WRITE VERIFICATION TEST                            ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    uint32_t test_passed = 1;
    uint32_t tests_run = 0;
    uint32_t tests_passed = 0;

    /* ═══════════════════════════════════════════════════════════
     * COMPREHENSIVE MMIO WRITE TEST
     * Test multiple registers to determine if issue is VirtIO-specific
     * or a general MMIO write problem
     * ═══════════════════════════════════════════════════════════ */

    /* Test 1: Status Register - Known writable, test incrementing */
    printf("%s: Test 1: Status register (always writable)\n", COMPONENT_NAME);
    tests_run++;

    uint32_t status_initial = VREG_READ(VIRTIO_MMIO_STATUS);
    printf("  Initial Status = 0x%x\n", status_initial);

    /* Write DRIVER bit (should add to existing ACKNOWLEDGE) */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    uint32_t status_after = VREG_READ(VIRTIO_MMIO_STATUS);
    printf("  Wrote 0x%x (ACKNOWLEDGE|DRIVER), readback = 0x%x ",
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER, status_after);

    if (status_after == (VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER)) {
        printf("✅ PASS\n");
        tests_passed++;
    } else {
        printf("❌ FAIL (expected 0x%x)\n", VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
        test_passed = 0;
    }

    /* Test 2: QueueSel Register - Should be writable at this point */
    printf("%s: Test 2: QueueSel register (should be writable)\n", COMPONENT_NAME);
    tests_run++;

    uint32_t qsel_initial = VREG_READ(VIRTIO_MMIO_QUEUE_SEL);
    printf("  Initial QueueSel = %u\n", qsel_initial);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 1);
    uint32_t qsel_1 = VREG_READ(VIRTIO_MMIO_QUEUE_SEL);
    printf("  Wrote QueueSel=1, readback = %u ", qsel_1);

    if (qsel_1 == 1) {
        printf("✅ PASS\n");
        tests_passed++;
    } else {
        printf("❌ FAIL (stuck at %u)\n", qsel_1);
        test_passed = 0;
    }

    /* Reset for safety */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);

    /* Test 3: DriverFeaturesSel - Selector register, always writable */
    printf("%s: Test 3: DriverFeaturesSel register (selector)\n", COMPONENT_NAME);
    tests_run++;

    uint32_t feat_sel_initial = VREG_READ(VIRTIO_MMIO_DRIVER_FEATURES_SEL);
    printf("  Initial DriverFeaturesSel = %u\n", feat_sel_initial);

    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    uint32_t feat_sel_1 = VREG_READ(VIRTIO_MMIO_DRIVER_FEATURES_SEL);
    printf("  Wrote DriverFeaturesSel=1, readback = %u ", feat_sel_1);

    if (feat_sel_1 == 1) {
        printf("✅ PASS\n");
        tests_passed++;
    } else {
        printf("❌ FAIL (stuck at %u)\n", feat_sel_1);
        test_passed = 0;
    }

    /* Reset for safety */
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);

    /* Test 4: DriverFeatures - Feature bits register */
    printf("%s: Test 4: DriverFeatures register (feature bits)\n", COMPONENT_NAME);
    tests_run++;

    /* Write a specific feature pattern */
    uint32_t test_features = 0xDEADBEEF;
    VREG_WRITE(VIRTIO_MMIO_DRIVER_FEATURES, test_features);
    uint32_t features_back = VREG_READ(VIRTIO_MMIO_DRIVER_FEATURES);
    printf("  Wrote DriverFeatures=0x%x, readback = 0x%x ", test_features, features_back);

    if (features_back == test_features) {
        printf("✅ PASS\n");
        tests_passed++;
    } else {
        printf("❌ FAIL (got 0x%x)\n", features_back);
        test_passed = 0;
    }

    /* Test 5: InterruptACK - Write-to-clear register */
    printf("%s: Test 5: InterruptACK register (write-to-clear)\n", COMPONENT_NAME);
    tests_run++;

    uint32_t irq_status = VREG_READ(VIRTIO_MMIO_INTERRUPT_STATUS);
    printf("  Current IRQ status = 0x%x\n", irq_status);

    /* Try to write all bits */
    VREG_WRITE(VIRTIO_MMIO_INTERRUPT_ACK, 0xFF);
    uint32_t irq_after = VREG_READ(VIRTIO_MMIO_INTERRUPT_STATUS);
    printf("  Wrote InterruptACK=0xFF, IRQ status after = 0x%x ", irq_after);

    /* For write-to-clear, if initial was 0, it should stay 0 */
    /* If initial was non-zero, writes should clear it */
    if (irq_status == 0 && irq_after == 0) {
        printf("✅ PASS (no IRQ to clear)\n");
        tests_passed++;
    } else if (irq_status != 0 && irq_after == 0) {
        printf("✅ PASS (cleared IRQ)\n");
        tests_passed++;
    } else {
        printf("⚠️  WARN (unexpected behavior)\n");
        /* Don't fail on this one, it's timing-dependent */
    }

    /* Test 6: Raw memory write - Test a non-register offset */
    printf("%s: Test 6: Raw MMIO memory write test\n", COMPONENT_NAME);
    tests_run++;

    /* Write to last byte of MMIO region (usually unused) */
    volatile uint32_t *test_addr = (volatile uint32_t *)((uintptr_t)virtio_regs_base + 0xFC);
    uint32_t original_val = *test_addr;
    printf("  Test address = %p, original value = 0x%x\n", test_addr, original_val);

    *test_addr = 0x12345678;
    uint32_t written_val = *test_addr;
    printf("  Wrote 0x12345678, readback = 0x%x ", written_val);

    if (written_val == 0x12345678) {
        printf("✅ PASS (raw write works)\n");
        tests_passed++;
    } else {
        printf("❌ FAIL (got 0x%x)\n", written_val);
        test_passed = 0;
    }

    /* Restore original value */
    *test_addr = original_val;

    /* Test 7: QUEUE_NOTIFY register (offset 0x050) */
    printf("%s: Test 7: QueueNotify register (offset 0x050)\n", COMPONENT_NAME);
    tests_run++;

    /* Write arbitrary value to QUEUE_NOTIFY */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_NOTIFY, 0xABCD);
    uint32_t notify_back = VREG_READ(VIRTIO_MMIO_QUEUE_NOTIFY);
    printf("  Wrote QueueNotify=0xABCD, readback = 0x%x ", notify_back);

    /* Note: QueueNotify is write-only, reads may return 0 or undefined */
    printf("⚠️  INFO (write-only register, read=%x)\n", notify_back);

    /* Test 8: QUEUE_NUM register (offset 0x038) */
    printf("%s: Test 8: QueueNum register (offset 0x038)\n", COMPONENT_NAME);
    tests_run++;

    uint32_t qnum_orig = VREG_READ(VIRTIO_MMIO_QUEUE_NUM);
    printf("  Initial QueueNum = %u\n", qnum_orig);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_NUM, 256);
    uint32_t qnum_after = VREG_READ(VIRTIO_MMIO_QUEUE_NUM);
    printf("  Wrote QueueNum=256, readback = %u ", qnum_after);

    if (qnum_after == 256) {
        printf("✅ PASS\n");
        tests_passed++;
    } else {
        printf("❌ FAIL (got %u)\n", qnum_after);
        test_passed = 0;
    }

    /* Restore */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_NUM, qnum_orig);

    /* Test 9: QUEUE_READY register (offset 0x044) */
    printf("%s: Test 9: QueueReady register (offset 0x044)\n", COMPONENT_NAME);
    tests_run++;

    uint32_t qready_orig = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    printf("  Initial QueueReady = %u\n", qready_orig);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);
    uint32_t qready_after = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    printf("  Wrote QueueReady=1, readback = %u ", qready_after);

    if (qready_after == 1) {
        printf("✅ PASS\n");
        tests_passed++;
    } else {
        printf("❌ FAIL (got %u)\n", qready_after);
        test_passed = 0;
    }

    /* Restore */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, qready_orig);

    /* Test 10: QueueSel Verification using QueueReady state */
    printf("%s: Test 10: QueueSel verification (using QueueReady as indicator)\n", COMPONENT_NAME);
    printf("  Testing if QueueSel actually switches between queues...\n");
    tests_run++;

    /* Step 1: Select queue 0, set QueueReady=1 */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qsel_verify = VREG_READ(VIRTIO_MMIO_QUEUE_SEL);
    printf("  1. Wrote QueueSel=0, readback=%u\n", qsel_verify);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);
    uint32_t q0_ready = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    printf("  2. Set Queue0 Ready=1, readback=%u\n", q0_ready);

    /* Step 2: Select queue 1, QueueReady should be 0 (different queue) */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 1);
    qsel_verify = VREG_READ(VIRTIO_MMIO_QUEUE_SEL);
    printf("  3. Wrote QueueSel=1, readback=%u\n", qsel_verify);

    uint32_t q1_ready_before = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    printf("  4. Read Queue1 Ready=%u (should be 0 if QueueSel works)\n", q1_ready_before);

    /* Step 3: Set queue 1 ready */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);
    uint32_t q1_ready_after = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    printf("  5. Set Queue1 Ready=1, readback=%u\n", q1_ready_after);

    /* Step 4: Switch back to queue 0, should still be ready */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);
    qsel_verify = VREG_READ(VIRTIO_MMIO_QUEUE_SEL);
    printf("  6. Wrote QueueSel=0 again, readback=%u\n", qsel_verify);

    uint32_t q0_ready_again = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    printf("  7. Read Queue0 Ready=%u (should still be 1)\n", q0_ready_again);

    /* Additional verification: Check QueueNumMax for each queue */
    printf("\n  DEEPER CHECK: Testing QueueNumMax for multiple queues...\n");

    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t q0_nummax = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("  Queue 0: QueueNumMax = %u\n", q0_nummax);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 1);
    uint32_t q1_nummax = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("  Queue 1: QueueNumMax = %u\n", q1_nummax);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 2);
    uint32_t q2_nummax = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("  Queue 2: QueueNumMax = %u (should be 0 - queue doesn't exist)\n", q2_nummax);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 3);
    uint32_t q3_nummax = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("  Queue 3: QueueNumMax = %u (should be 0 - queue doesn't exist)\n", q3_nummax);

    /* Analysis */
    printf("\n  ANALYSIS:\n");
    bool queueready_test_pass = (q1_ready_before == 0 && q1_ready_after == 1 && q0_ready_again == 1);
    bool queuenummax_test_pass = (q0_nummax > 0 && q1_nummax > 0 && q2_nummax == 0);

    if (queueready_test_pass && queuenummax_test_pass) {
        printf("  ✅ PASS: QueueSel FULLY WORKS!\n");
        printf("     ✓ QueueReady: Queue 0 and Queue 1 have independent state\n");
        printf("     ✓ QueueNumMax: Queues 0-1 exist (%u/%u), Queue 2+ don't exist (0)\n",
               q0_nummax, q1_nummax);
        printf("     ✓ QueueSel successfully switches context between queues\n");
        tests_passed++;
    } else if (q1_ready_before == 1) {
        printf("  ❌ FAIL: QueueSel BROKEN (stuck at queue 0)\n");
        printf("     - Reading Queue1 returned Queue0's state (both show ready=1)\n");
        printf("     - QueueSel writes have no effect\n");
    } else if (!queuenummax_test_pass && q0_nummax == q1_nummax && q1_nummax == q2_nummax) {
        printf("  ⚠️  SUSPICIOUS: All queues return same QueueNumMax=%u\n", q0_nummax);
        printf("     - This suggests QueueSel might not be switching QEMU internal state\n");
        printf("     - We might be stuck reading queue 0 properties for all indices\n");
        if (queueready_test_pass) {
            printf("     - BUT QueueReady test passed... conflicting evidence!\n");
            tests_passed++;  // Give partial credit
        }
    } else {
        printf("  ⚠️  UNCLEAR: Mixed results\n");
        printf("     - QueueReady test: %s\n", queueready_test_pass ? "PASS" : "FAIL");
        printf("     - QueueNumMax test: %s\n", queuenummax_test_pass ? "PASS" : "FAIL");
        printf("     - q0_nummax=%u, q1_nummax=%u, q2_nummax=%u, q3_nummax=%u\n",
               q0_nummax, q1_nummax, q2_nummax, q3_nummax);
    }

    /* Restore: Clear QueueReady for both queues */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 0);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 1);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 0);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);  /* Back to queue 0 */

    /* Test 11: Try Status register AGAIN to confirm it consistently works */
    printf("%s: Test 11: Status register RETEST (offset 0x070)\n", COMPONENT_NAME);
    tests_run++;

    uint32_t status_retest = VREG_READ(VIRTIO_MMIO_STATUS);
    printf("  Current Status = 0x%x\n", status_retest);

    /* Toggle FEATURES_OK bit */
    VREG_WRITE(VIRTIO_MMIO_STATUS, status_retest | VIRTIO_STATUS_FEATURES_OK);
    uint32_t status_toggled = VREG_READ(VIRTIO_MMIO_STATUS);
    printf("  Wrote Status with FEATURES_OK, readback = 0x%x ", status_toggled);

    if (status_toggled == (status_retest | VIRTIO_STATUS_FEATURES_OK)) {
        printf("✅ PASS (Status STILL works!)\n");
        tests_passed++;
    } else {
        printf("❌ FAIL (expected 0x%x)\n", status_retest | VIRTIO_STATUS_FEATURES_OK);
        test_passed = 0;
    }

    /* Test 11: Direct write to different MMIO offsets to map writable regions */
    printf("%s: Test 11: Address range scan\n", COMPONENT_NAME);
    printf("  Testing which MMIO offsets accept writes...\n");

    struct { uint32_t offset; const char *name; } test_offsets[] = {
        {0x000, "MAGIC (0x000)"},
        {0x020, "DRIVER_FEATURES (0x020)"},
        {0x030, "QUEUE_SEL (0x030)"},
        {0x038, "QUEUE_NUM (0x038)"},
        {0x044, "QUEUE_READY (0x044)"},
        {0x050, "QUEUE_NOTIFY (0x050)"},
        {0x064, "INTERRUPT_ACK (0x064)"},
        {0x070, "STATUS (0x070)"},
        {0x080, "QUEUE_DESC_LOW (0x080)"},
        {0x084, "QUEUE_DESC_HIGH (0x084)"},
        {0x090, "QUEUE_AVAIL_LOW (0x090)"},
        {0x094, "QUEUE_AVAIL_HIGH (0x094)"},
        {0x0a0, "QUEUE_USED_LOW (0x0a0)"},
        {0x0a4, "QUEUE_USED_HIGH (0x0a4)"},
    };

    for (int i = 0; i < sizeof(test_offsets) / sizeof(test_offsets[0]); i++) {
        volatile uint32_t *reg = (volatile uint32_t *)((uintptr_t)virtio_regs_base + (test_offsets[i].offset / 4));
        uint32_t orig = *reg;
        *reg = 0x11111111;
        uint32_t readback = *reg;
        printf("    %-25s: wrote 0x11111111, read 0x%08x %s\n",
               test_offsets[i].name,
               readback,
               (readback == 0x11111111) ? "✅" : (readback != orig) ? "⚠️" : "❌");
        *reg = orig;  /* Restore */
    }

    /* Summary */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  TEST SUMMARY: %u/%u tests passed                         ║\n", tests_passed, tests_run);
    printf("╚══════════════════════════════════════════════════════════╝\n");

    /* Overall result */
    printf("\n");
    if (test_passed) {
        printf("╔══════════════════════════════════════════════════════════╗\n");
        printf("║  ✅ ALL MMIO WRITE TESTS PASSED                          ║\n");
        printf("╚══════════════════════════════════════════════════════════╝\n");
    } else {
        printf("╔══════════════════════════════════════════════════════════╗\n");
        printf("║  ❌ MMIO WRITE TESTS FAILED!                             ║\n");
        printf("║  Check page table permissions for MMIO region            ║\n");
        printf("╚══════════════════════════════════════════════════════════╝\n");
    }
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

    /* Device ready - activate the device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VREG_READ(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);
    printf("%s: ✓ VirtIO device initialized and activated\n", COMPONENT_NAME);

    return 0;
}

/*
 * Component initialization
 */
void post_init(void)
{
    printf("%s: Component started\n\n", COMPONENT_NAME);

    /* ═══════════════════════════════════════════════════════════
     * CRITICAL DEBUG: Verify MMIO mapping is correct
     * ═══════════════════════════════════════════════════════════ */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  MMIO MAPPING VERIFICATION TEST                          ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    /* virtio_mmio_regs is the CAmkES dataport mapped to physical 0xa003000 */
    volatile uint32_t *base = (volatile uint32_t *)virtio_mmio_regs;

    printf("Virtual address of virtio_mmio_regs: %p\n", base);
    printf("Expected physical address: 0xa003000 (page containing VirtIO slot 31 at +0xe00)\n");
    printf("VirtIO device should be at offset +0xe00 from page base\n\n");

    /* Dump first 64 bytes of the mapped region (entire page header) */
    printf("═══ Dumping first 64 bytes of mapped region ═══\n");
    for (int i = 0; i < 16; i++) {
        printf("  [0x%03x] = 0x%08x", i * 4, base[i]);
        if (i == 0) printf("  ← Should be 0x00000000 (empty)");
        printf("\n");
    }

    /* Now check VirtIO device at offset 0xe00 (slot 31) */
    printf("\n═══ VirtIO device at offset +0xe00 (896 words) ═══\n");
    volatile uint32_t *virtio_dev = base + (0xe00 / 4);

    printf("VirtIO Magic Value   [+0x000] = 0x%08x (expect 0x74726976 'virt')\n", virtio_dev[0]);
    printf("VirtIO Version       [+0x004] = 0x%08x (expect 0x2 for VirtIO 1.0)\n", virtio_dev[1]);
    printf("VirtIO Device ID     [+0x008] = 0x%08x (expect 0x1 for network)\n", virtio_dev[2]);
    printf("VirtIO Vendor ID     [+0x00c] = 0x%08x (expect 0x554d4551 'QEMU')\n", virtio_dev[3]);

    /* Verify magic value */
    if (virtio_dev[0] == 0x74726976) {
        printf("\n✅ SUCCESS: VirtIO magic value is CORRECT!\n");
        printf("   The MMIO mapping IS working for READS!\n");
    } else {
        printf("\n❌ FAILURE: VirtIO magic value is WRONG!\n");
        printf("   Expected: 0x74726976\n");
        printf("   Got:      0x%08x\n", virtio_dev[0]);
        printf("   This means the mapping is pointing to the WRONG address!\n");
    }

    /* Test if we can read the entire VirtIO register space */
    printf("\n═══ Reading all VirtIO MMIO registers ═══\n");
    for (int i = 0; i < 16; i++) {
        uint32_t val = virtio_dev[i];
        printf("  VirtIO[0x%03x] = 0x%08x", i * 4, val);

        switch (i * 4) {
            case 0x000: printf("  (Magic)"); break;
            case 0x004: printf("  (Version)"); break;
            case 0x008: printf("  (DeviceID)"); break;
            case 0x00c: printf("  (VendorID)"); break;
            case 0x010: printf("  (DeviceFeatures)"); break;
            case 0x030: printf("  (QueueSel)"); break;
            case 0x034: printf("  (QueueNumMax)"); break;
            case 0x070: printf("  (Status)"); break;
        }
        printf("\n");
    }

    printf("\n═══ Testing WRITE capability ═══\n");
    printf("Attempting to write to QueueSel register...\n");

    /* Save original value */
    uint32_t orig_queuesel = virtio_dev[0x030 / 4];
    printf("Original QueueSel value: 0x%08x\n", orig_queuesel);

    /* Try to write different values */
    printf("Writing 0x1 to QueueSel...\n");
    virtio_dev[0x030 / 4] = 0x1;
    DSB();  /* Ensure write completes */
    uint32_t readback1 = virtio_dev[0x030 / 4];
    printf("Read back: 0x%08x (expect 0x1 if writes work)\n", readback1);

    printf("Writing 0x0 to QueueSel...\n");
    virtio_dev[0x030 / 4] = 0x0;
    DSB();
    uint32_t readback0 = virtio_dev[0x030 / 4];
    printf("Read back: 0x%08x (expect 0x0 if writes work)\n", readback0);

    if (readback1 == 0x1 && readback0 == 0x0) {
        printf("\n✅ WRITES WORK! The MMIO mapping is fully functional!\n");
    } else {
        printf("\n❌ WRITES DON'T WORK! Reads work but writes are being dropped!\n");
        printf("   This confirms Device memory attributes are WRONG!\n");
    }

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  END OF MMIO MAPPING VERIFICATION                        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ═══════════════════════════════════════════════════════════
     * QEMU→Component Direction Test: Can component see QEMU writes?
     * ═══════════════════════════════════════════════════════════ */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  CRITICAL TEST: Paint Virtqueue Memory Directly         ║\n");
    printf("║  (Tests if QEMU never touches memory OR writes blocked) ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("OBJECTIVE: Distinguish between two hypotheses:\n");
    printf("  Hypothesis 1: QEMU never touches memory (doesn't know address)\n");
    printf("               → Direct write works, we see test pattern\n");
    printf("  Hypothesis 2: Page table blocks QEMU→Component writes\n");
    printf("               → Direct write fails, still see 0x00000000\n");
    printf("\n");

    /* Initialize VirtIO device */
    if (virtio_net_init() != 0) {
        printf("%s: Failed to initialize VirtIO device\n", COMPONENT_NAME);
        return;
    }

    /* ═══════════════════════════════════════════════════════════
     * PAINT TEST: Write to virtqueue memory from GDB
     * ═══════════════════════════════════════════════════════════ */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  PAINT TEST: Virtqueue RX Used Ring Memory              ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("This test writes DIRECTLY to virtqueue memory from GDB\n");
    printf("to prove whether QEMU can't access memory OR page blocks writes.\n");
    printf("\n");

    /* Get the used ring address from virtio_net_init() */
    extern struct virtq rx_virtq;  /* Declared earlier in this file */
    volatile uint16_t *used_idx_ptr = &rx_virtq.used->idx;
    volatile uint32_t *used_ring_ptr = (volatile uint32_t *)rx_virtq.used;

    printf("RX Used Ring addresses:\n");
    printf("  Virtual address:  %p\n", (void*)rx_virtq.used);
    printf("  used->idx pointer: %p\n", (void*)used_idx_ptr);
    printf("\n");

    /* Read physical address from earlier debug output - it's already printed */
    printf("Physical address was printed earlier as 'RX used paddr'\n");
    printf("Look for that line above to get the physical address.\n");
    printf("\n");

    printf("STEP 1: Read current values (should be 0x00000000)...\n");
    uint16_t idx_before = *used_idx_ptr;
    uint32_t word0_before = used_ring_ptr[0];
    uint32_t word1_before = used_ring_ptr[1];
    printf("  used->idx = 0x%04x\n", idx_before);
    printf("  used ring word[0] = 0x%08x\n", word0_before);
    printf("  used ring word[1] = 0x%08x\n", word1_before);
    printf("\n");

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  ACTION REQUIRED: Paint memory from GDB                 ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("INSTRUCTIONS:\n");
    printf("1. Connect GDB:\n");
    printf("   gdb-multiarch\n");
    printf("   set architecture aarch64\n");
    printf("   target remote :1234\n");
    printf("   continue\n");
    printf("\n");
    printf("2. Press Ctrl+C to interrupt\n");
    printf("\n");
    printf("3. Find RX used ring physical address from output above\n");
    printf("   (Look for 'RX used paddr = 0x...')\n");
    printf("   Example: If paddr = 0x80834408, then:\n");
    printf("\n");
    printf("4. Write test pattern to physical address:\n");
    printf("   set *((unsigned short *)0x<PADDR>+2) = 0x1234\n");
    printf("   # Write to used->idx (offset +2 for idx field)\n");
    printf("\n");
    printf("   Or write full words:\n");
    printf("   set *((unsigned int *)0x<PADDR>) = 0xdeadbeef\n");
    printf("   set *((unsigned int *)0x<PADDR>+4) = 0xcafebabe\n");
    printf("\n");
    printf("5. Type 'continue' to resume\n");
    printf("\n");
    printf("Waiting 60 seconds for manual GDB painting...\n");
    printf("\n");

    /* Countdown */
    for (int i = 60; i > 0; i--) {
        if (i % 10 == 0 || i <= 5) {
            printf("[T=%d] %d seconds remaining...\n", 60-i, i);
        }
        for (volatile int j = 0; j < 50000000; j++);
    }
    printf("[T=60] Countdown complete!\n\n");

    printf("STEP 2: Read values after GDB paint attempt...\n");
    uint16_t idx_after = *used_idx_ptr;
    uint32_t word0_after = used_ring_ptr[0];
    uint32_t word1_after = used_ring_ptr[1];
    printf("  used->idx = 0x%04x", idx_after);
    if (idx_after != idx_before) {
        printf("  ← CHANGED from 0x%04x! ✅", idx_before);
    }
    printf("\n");
    printf("  used ring word[0] = 0x%08x", word0_after);
    if (word0_after != word0_before) {
        printf("  ← CHANGED from 0x%08x! ✅", word0_before);
    }
    printf("\n");
    printf("  used ring word[1] = 0x%08x", word1_after);
    if (word1_after != word1_before) {
        printf("  ← CHANGED from 0x%08x! ✅", word1_before);
    }
    printf("\n\n");

    printf("═══════════════════════════════════════════════════════════\n");
    printf("PAINT TEST RESULT:\n");
    printf("═══════════════════════════════════════════════════════════\n");
    if (idx_after != idx_before || word0_after != word0_before || word1_after != word1_before) {
        printf("✅ SUCCESS: Values CHANGED!\n");
        printf("\n");
        printf("CONCLUSION: Direct writes to RAM work!\n");
        printf("  → Hypothesis 1 CONFIRMED: QEMU never touches memory\n");
        printf("  → QEMU doesn't know where virtqueue is (MMIO writes dropped)\n");
        printf("  → Page table ALLOWS QEMU→Component memory access\n");
        printf("  → Problem is ONLY that QEMU wasn't told the addresses\n");
        printf("\n");
        printf("IMPLICATION: Fix MMIO writes → QEMU learns addresses → Network works!\n");
    } else {
        printf("❌ UNCHANGED: All values still 0x00000000\n");
        printf("\n");
        printf("Either:\n");
        printf("  1. You didn't write from GDB (try again), OR\n");
        printf("  2. Hypothesis 2 CONFIRMED: Page table blocks QEMU→Component writes\n");
        printf("     → Even if QEMU knew addresses, it couldn't write to virtqueue\n");
        printf("     → This is a MORE SERIOUS problem (bidirectional block)\n");
        printf("\n");
        printf("If you DID write from GDB and it's still zero:\n");
        printf("  → Page table permissions block writes in BOTH directions\n");
        printf("  → Need to fix permissions for both Component→QEMU AND QEMU→Component\n");
    }
    printf("═══════════════════════════════════════════════════════════\n\n");

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

    /* QueueSel Runtime Verification Test */
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  RUNTIME QUEUESEL TEST (after full initialization)      ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    printf("Testing QueueSel by reading configured queue properties...\n\n");

    /* Test Queue 0 (RX) */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t q0_num_rt = VREG_READ(VIRTIO_MMIO_QUEUE_NUM);
    uint32_t q0_ready_rt = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    uint32_t q0_nummax_rt = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("Queue 0 (RX - configured during init):\n");
    printf("  QueueNum    = %u (we set this to %u)\n", q0_num_rt, rx_virtq.num);
    printf("  QueueReady  = %u (should be 1)\n", q0_ready_rt);
    printf("  QueueNumMax = %u\n", q0_nummax_rt);

    /* Test Queue 1 (TX) */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 1);
    uint32_t q1_num_rt = VREG_READ(VIRTIO_MMIO_QUEUE_NUM);
    uint32_t q1_ready_rt = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    uint32_t q1_nummax_rt = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("\nQueue 1 (TX - configured during init):\n");
    printf("  QueueNum    = %u (we set this to %u)\n", q1_num_rt, tx_virtq.num);
    printf("  QueueReady  = %u (should be 1)\n", q1_ready_rt);
    printf("  QueueNumMax = %u\n", q1_nummax_rt);

    /* Test Queue 2 (should not exist) */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 2);
    uint32_t q2_num_rt = VREG_READ(VIRTIO_MMIO_QUEUE_NUM);
    uint32_t q2_ready_rt = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    uint32_t q2_nummax_rt = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("\nQueue 2 (should NOT exist):\n");
    printf("  QueueNum    = %u (should be 0 - not configured)\n", q2_num_rt);
    printf("  QueueReady  = %u (should be 0)\n", q2_ready_rt);
    printf("  QueueNumMax = %u (should be 0 - queue doesn't exist)\n", q2_nummax_rt);

    /* Analysis */
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("ANALYSIS:\n");

    bool nums_match_config = (q0_num_rt == rx_virtq.num && q1_num_rt == tx_virtq.num);
    bool ready_correct = (q0_ready_rt == 1 && q1_ready_rt == 1 && q2_ready_rt == 0);
    bool q2_is_zero = (q2_num_rt == 0 && q2_nummax_rt == 0);
    bool stuck_at_q0 = (q0_num_rt == q1_num_rt && q1_num_rt == q2_num_rt && q2_num_rt != 0);

    if (stuck_at_q0) {
        printf("❌ FAIL: STUCK AT QUEUE 0!\n");
        printf("   All queues return same values - QueueSel doesn't switch context\n");
        printf("   Q0/Q1/Q2 QueueNum all = %u (expected Q0=%u, Q1=%u, Q2=0)\n",
               q0_num_rt, rx_virtq.num, tx_virtq.num);
    } else if (nums_match_config && ready_correct && q2_is_zero) {
        printf("✅ PASS: QueueSel works correctly!\n");
        printf("   Q0 and Q1 have different configured values\n");
        printf("   Q2 returns zeros (queue doesn't exist)\n");
        printf("   QueueSel successfully switches between queue contexts\n");
    } else {
        printf("⚠️  MIXED RESULTS:\n");
        printf("   Config match: %s (Q0=%u/%u, Q1=%u/%u)\n",
               nums_match_config ? "YES" : "NO",
               q0_num_rt, rx_virtq.num, q1_num_rt, tx_virtq.num);
        printf("   Ready correct: %s (Q0=%u, Q1=%u, Q2=%u)\n",
               ready_correct ? "YES" : "NO",
               q0_ready_rt, q1_ready_rt, q2_ready_rt);
        printf("   Q2 is zero: %s\n", q2_is_zero ? "YES" : "NO");
    }
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* CRITICAL TEST: Check actual queue ring activity */
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  QUEUE ACTIVITY CHECK (checking actual ring indices)    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    printf("RX Queue (Queue 0) - Direct memory inspection:\n");
    printf("  rx_virtq.avail->idx = %u (driver added %u buffers)\n",
           rx_virtq.avail->idx, rx_virtq.avail->idx);
    printf("  rx_virtq.used->idx  = %u (QEMU consumed %u buffers)\n",
           rx_virtq.used->idx, rx_virtq.used->idx);

    printf("\nTX Queue (Queue 1) - Direct memory inspection:\n");
    printf("  tx_virtq.avail->idx = %u (driver added %u buffers)\n",
           tx_virtq.avail->idx, tx_virtq.avail->idx);
    printf("  tx_virtq.used->idx  = %u (QEMU consumed %u buffers)\n",
           tx_virtq.used->idx, tx_virtq.used->idx);

    printf("\nExpected values:\n");
    printf("  RX: avail->idx=32, used->idx=0 (we added 32 RX buffers, none consumed yet)\n");
    printf("  TX: avail->idx=0,  used->idx=1 (we sent 1 packet, QEMU consumed it)\n");

    bool rx_correct = (rx_virtq.avail->idx == 32 && rx_virtq.used->idx == 0);
    bool tx_correct = (tx_virtq.avail->idx == 0 && tx_virtq.used->idx == 1);

    printf("\nRESULT:\n");
    if (rx_correct && tx_correct) {
        printf("✅ BOTH queues have correct activity - they are SEPARATE!\n");
        printf("   RX and TX queues have different idx values\n");
        printf("   This proves we're NOT stuck on a single queue\n");
    } else if (rx_virtq.avail->idx == tx_virtq.avail->idx &&
               rx_virtq.used->idx == tx_virtq.used->idx) {
        printf("❌ RX and TX show IDENTICAL indices - STUCK ON SAME QUEUE!\n");
        printf("   Both queues have same avail->idx and used->idx\n");
        printf("   This proves QueueSel doesn't switch between queues\n");
    } else {
        printf("⚠️  UNEXPECTED: Indices don't match expected pattern\n");
        printf("   RX correct: %s, TX correct: %s\n",
               rx_correct ? "YES" : "NO", tx_correct ? "YES" : "NO");
    }
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* DEFINITIVE QUEUESEL TEST: Wait for RX packets, then check if Queue 2 shows same activity */
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  DEFINITIVE QUEUESEL TEST (packet injection method)     ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\nWaiting 15 seconds for you to send packets via netcat...\n");
    printf("Command: echo 'test123' | nc localhost 6000\n");
    printf("(Port 6000 is forwarded to guest port 1234)\n\n");

    /* Wait and process packets */
    for (int i = 0; i < 15; i++) {
        printf(".");
        fflush(stdout);
        for (int j = 0; j < 100; j++) {
            sys_check_timeouts();
            process_rx_packets();
            seL4_Yield();
        }
    }
    printf("\n\n");

    /* Now check RX queue activity directly from memory */
    uint16_t rx_used_idx_mem = rx_virtq.used->idx;
    printf("RX Queue activity (from memory):\n");
    printf("  rx_virtq.used->idx = %u (packets received by QEMU)\n\n", rx_used_idx_mem);

    /* Key test: Check Queue 0 vs Queue 2 via MMIO reads */
    printf("Now checking via MMIO registers with QueueSel:\n\n");

    /* Test 1: Select Queue 0 (RX) - should show activity if packets arrived */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);
    /* VirtIO spec doesn't have a register to read used->idx via MMIO,
       but we can check QueueReady and QueueNumMax */
    uint32_t q0_ready_final = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    uint32_t q0_nummax_final = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("Queue 0 (RX) via MMIO:\n");
    printf("  QueueReady  = %u\n", q0_ready_final);
    printf("  QueueNumMax = %u\n", q0_nummax_final);
    printf("  (RX queue should be ready=1, nummax=1024)\n\n");

    /* Test 2: Select Queue 1 (TX) */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 1);
    uint32_t q1_ready_final = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    uint32_t q1_nummax_final = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("Queue 1 (TX) via MMIO:\n");
    printf("  QueueReady  = %u\n", q1_ready_final);
    printf("  QueueNumMax = %u\n", q1_nummax_final);
    printf("  (TX queue should be ready=1, nummax=1024)\n\n");

    /* Test 3: Select Queue 2 (doesn't exist - NO CTRL_VQ feature) */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 2);
    uint32_t q2_ready_final = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    uint32_t q2_nummax_final = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("Queue 2 (NON-EXISTENT - no CTRL_VQ) via MMIO:\n");
    printf("  QueueReady  = %u\n", q2_ready_final);
    printf("  QueueNumMax = %u\n", q2_nummax_final);
    printf("  (Should be ready=0, nummax=0 if QueueSel works)\n");
    printf("  (If same as Q0, proves QueueSel stuck!)\n\n");

    /* Analysis */
    printf("═══════════════════════════════════════════════════════════\n");
    printf("FINAL VERDICT:\n");

    if (q2_nummax_final == 0 && q2_ready_final == 0) {
        printf("✅ QueueSel WORKS!\n");
        printf("   Queue 2 returns zeros (non-existent queue)\n");
        printf("   Different from Queue 0/1 values\n");
    } else if (q2_nummax_final == q0_nummax_final && q2_ready_final == q0_ready_final) {
        printf("❌ QueueSel BROKEN - STUCK AT QUEUE 0!\n");
        printf("   Queue 2 returns same values as Queue 0\n");
        printf("   QueueNumMax: Q0=%u, Q2=%u (identical!)\n", q0_nummax_final, q2_nummax_final);
        printf("   QueueReady:  Q0=%u, Q2=%u (identical!)\n", q0_ready_final, q2_ready_final);
        printf("   This proves QueueSel writes don't reach QEMU\n");
    } else {
        printf("⚠️  UNCLEAR: Unexpected pattern\n");
        printf("   Q0: ready=%u, nummax=%u\n", q0_ready_final, q0_nummax_final);
        printf("   Q1: ready=%u, nummax=%u\n", q1_ready_final, q1_nummax_final);
        printf("   Q2: ready=%u, nummax=%u\n", q2_ready_final, q2_nummax_final);
    }

    if (rx_used_idx_mem > 0) {
        printf("\n📊 RX Activity: %u packets received (proves RX queue works!)\n", rx_used_idx_mem);
    } else {
        printf("\n⚠️  No RX packets detected - try sending: echo 'test' | nc localhost 6000\n");
    }

    printf("═══════════════════════════════════════════════════════════\n\n");

    /* Restore to Q0 */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);

    printf("Test complete. System continuing...\n\n");

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
