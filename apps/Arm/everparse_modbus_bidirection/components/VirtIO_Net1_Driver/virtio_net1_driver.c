/*
 * VirtIO_Net1_Driver - Internal Network (Bidirectional)
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
 *   INBOUND:  Internal TCP:6000 => lwIP => extract metadata+payload => ICS_Inbound
 *   OUTBOUND: ICS_Outbound => create TCP packet => lwIP => Internal
 *
 * v2.241 (2025-11-02): FIX PCB CORRUPTION - Triple-decrement and double-close bugs
 *   - Error callback: Set meta->pcb=NULL, enqueue cleanup, remove counter decrement
 *   - Recv callback (p=NULL): Set meta->pcb=NULL, remove counter decrement
 *   - Close notification: Set meta->pcb=NULL BEFORE tcp_close/abort
 *   - Make process_cleanup_queue() the SINGLE source of truth for counter decrements
 *   - Prevents: "ERROR: active_connections already 0" and PCB corruption crashes
 */

/* v2.207: New industry-standard 5-level debug system */
#define DEBUG_LEVEL DEBUG_LEVEL_INFO  /* v2.255: Reduced - sentinel fix verified */
#include "debug_levels.h"

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
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/stats.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/icmp.h"  /* v2.242: ICMP header for proxy replies */
#include "lwip/priv/tcp_priv.h"  /* v2.173: Access tcp_active_pcbs, tcp_tw_pcbs for leak detection */
#include "netif/ethernet.h"

/* ICS common definitions */
#include "common.h"
#include "version.h"  /* v2.241: Unified version management */

/* v2.117: Connection state sharing */
#include "connection_state.h"

#define COMPONENT_NAME "VirtIO_Net1_Driver"
#define TCP_SERVER_PORT 502  /* INBOUND: Modbus port - pretends to be PLC */

#define MAX_CONNECTIONS 150  /* v2.182: Reverted to prevent PLC crash during leak testing */

struct connection_metadata {
    struct tcp_pcb *pcb;           /* lwIP connection pointer (SCADA→Net1 connection) */
    uint32_t session_id;           /* v2.150: Session ID from Net0 (links SCADA ↔ PLC connections) */
    uint32_t original_src_ip;      /* Original source IP (e.g., 192.168.90.5 SCADA) */
    uint32_t original_dest_ip;     /* Original destination IP (e.g., 192.168.95.2 PLC) */
    uint16_t src_port;             /* Source port (SCADA's ephemeral port) */
    uint16_t dest_port;            /* Destination port (502) */
    uint16_t lwip_ephemeral_port;  /* lwIP's ephemeral port for outbound connection */
    bool active;                   /* Is this slot in use? */

    /* v2.50: Connection validation fields for robust reuse */
    uint32_t tcp_seq_num;          /* Initial TCP sequence number - detects port reuse */
    uint32_t timestamp;            /* Creation time - for metadata consistency with Net0 */
    uint32_t last_activity;        /* Last activity timestamp - for idle timeout (v2.59) */

    struct tcp_inbound_client_state *pool_state;  /* Associated pool slot - freed when connection removed */

    bool error_notified;             /* True if error notification already queued (Net1 → Net0) */

    bool close_pending;              /* True if close requested from main thread (poll callback handles) */

    bool metadata_close_pending;     /* True if SCADA closed but metadata persists for TX */
    uint32_t close_timestamp;        /* When metadata_close_pending was set (for grace period) */
    uint32_t last_tx_timestamp;      /* Last TX path activity (for fast-track cleanup) */

    bool awaiting_response;          /* True if we're waiting for PLC response */
    bool response_received;          /* True if PLC response arrived */

    bool closing;                    /* True if close initiated, waiting for PCB free */

    uint8_t *pending_outbound_data;  /* Queued outbound data awaiting send */
    uint16_t pending_outbound_len;   /* Length of queued data */
    bool has_pending_outbound;       /* True if data needs to be sent */

    bool cleanup_in_progress;        /* Guard: prevents double-cleanup */

    bool close_notified;             /* True if close notification already queued */

    bool pcb_closed;                 /* True if PCB has been closed/aborted (don't close again) */

};

static struct connection_metadata connection_table[MAX_CONNECTIONS];
static int connection_count = 0;
static uint32_t active_connections = 0;
static uint32_t total_connections_created = 0;  /* v2.256: Moved up for cleanup queue access */
static uint32_t total_connections_closed = 0;   /* v2.256: Moved up for cleanup queue access */
uint32_t sys_now(void);

#define MAX_ICMP_METADATA 16  /* Track up to 16 concurrent pings */

struct icmp_metadata {
    uint16_t icmp_id;           /* ICMP echo identifier */
    uint16_t icmp_seq;          /* ICMP echo sequence number */
    uint32_t original_dest_ip;  /* Original destination IP (what was pinged) */
    uint32_t timestamp;         /* When request was received (for aging) */
    bool active;                /* Is this slot in use? */
};

static struct icmp_metadata icmp_table[MAX_ICMP_METADATA];

/* Helper: Store ICMP request metadata */
static void icmp_metadata_store(uint32_t dest_ip, uint16_t icmp_id, uint16_t icmp_seq)
{
    /* Find existing entry or free slot */
    for (int i = 0; i < MAX_ICMP_METADATA; i++) {
        if (!icmp_table[i].active ||
            (icmp_table[i].icmp_id == icmp_id && icmp_table[i].icmp_seq == icmp_seq)) {
            icmp_table[i].icmp_id = icmp_id;
            icmp_table[i].icmp_seq = icmp_seq;
            icmp_table[i].original_dest_ip = dest_ip;
            icmp_table[i].timestamp = sys_now();
            icmp_table[i].active = true;
            return;
        }
    }

    DEBUG_WARN("%s: [WARN] ICMP metadata table full! (id=%u, seq=%u)\n",
               COMPONENT_NAME, icmp_id, icmp_seq);
}

/* Helper: Lookup ICMP metadata by id/seq */
static struct icmp_metadata* icmp_metadata_lookup(uint16_t icmp_id, uint16_t icmp_seq)
{
    for (int i = 0; i < MAX_ICMP_METADATA; i++) {
        if (icmp_table[i].active &&
            icmp_table[i].icmp_id == icmp_id &&
            icmp_table[i].icmp_seq == icmp_seq) {
            /* Mark as consumed (one-time use) */
            icmp_table[i].active = false;
            return &icmp_table[i];
        }
    }
    return NULL;
}

/* Helper: Clean up old ICMP metadata (called periodically) */
static void icmp_metadata_cleanup(void)
{
    uint32_t now = sys_now();
    for (int i = 0; i < MAX_ICMP_METADATA; i++) {
        if (icmp_table[i].active && (now - icmp_table[i].timestamp) > 5000) {
            /* Entry older than 5 seconds - clean it up */
            icmp_table[i].active = false;
        }
    }
}

#define CLEANUP_QUEUE_SIZE 512
#define CLEANUP_QUEUE_MASK (CLEANUP_QUEUE_SIZE - 1)

struct cleanup_request {
    uint32_t session_id;
    uint32_t timestamp;  /* sys_now() when enqueued */
};

struct cleanup_queue {
    struct cleanup_request requests[CLEANUP_QUEUE_SIZE];
    volatile uint32_t head;  /* Producer writes here */
    volatile uint32_t tail;  /* Consumer reads here */
};

static struct cleanup_queue cleanup_queue = {0};

/* Cleanup statistics */
struct cleanup_stats {
    uint32_t enqueued;
    uint32_t processed;
    uint32_t duplicates;
};
static struct cleanup_stats cleanup_stats = {0};

static volatile struct connection_state_table *own_state = NULL;   /* Our state (exposed to Net0) */
static volatile struct connection_state_table *peer_state = NULL;  /* Net0's state (read-only) */

#define MAX_SELF_CLEANED_TRACKING 32  /* Circular buffer size */
#define SELF_CLEANED_TTL_MS 5000      /* Expire after 5 seconds */

struct self_cleaned_entry {
    uint32_t src_ip;      /* SCADA IP */
    uint16_t src_port;    /* SCADA port */
    uint16_t dst_port;    /* PLC port (502) */
    uint32_t timestamp;   /* When we cleaned it */
    bool valid;           /* Entry is valid */
};

static struct self_cleaned_entry self_cleaned_connections[MAX_SELF_CLEANED_TRACKING];
static int self_cleaned_index = 0;  /* Circular buffer index */

#define OUTBOUND_FORWARD_IP "192.168.95.1"        /* Forward to Net1 (private network) */
#define OUTBOUND_FORWARD_PORT 502              /* Modbus TCP port */
#define INBOUND_FORWARD_PORT 502               /* Unused - Net1 handles inbound */

/*
 * LEGACY DEBUG CONFIGURATION - Now controlled by DEBUG_LEVEL above
 * Keeping GDB/test flags separate from DEBUG_LEVEL system
 */
#define ENABLE_GDB_WAIT 0         /* Enable 60-second GDB wait during init */
#define ENABLE_PAINT_TEST 0       /* Enable virtqueue memory paint test */

/*
 * PROTOCOL FILTER CONFIGURATION
 * Control which protocols to show in debug output
 * Set to 1 to SHOW protocol, 0 to HIDE protocol
 */
#define FILTER_SHOW_ARP 0         /* Show ARP packets (0x0806) */
#define FILTER_SHOW_IPV6 0        /* Show IPv6 packets (0x86dd) */
#define FILTER_SHOW_TCP 1         /* Show TCP packets */
#define FILTER_SHOW_UDP 1         /* Show UDP packets */
#define FILTER_SHOW_ICMP 1        /* Show ICMP packets */
#define FILTER_SHOW_OTHER 1       /* Show all other protocols */

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

/* v2.241: Removed hex_dump_packet() and print_ascii_payload() -
 * excessive debug output for single-CPU seL4 image.
 * Use external packet capture (tcpdump) if needed.
 */

static void process_rx_packets(void);
static void refill_rx_queue(void);
static err_t netif_output(struct netif *netif, struct pbuf *p);
static err_t custom_netif_init(struct netif *netif);
static void netif_status_callback(struct netif *netif);
static void setup_tcp_echo_server(void);
static inline uint16_t ip_fast_csum(const void *iph, unsigned int ihl);
static uint16_t tcp_checksum(struct iphdr *ip, struct tcphdr *tcp, uint16_t tcp_len);

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
    static uint32_t refill_call_count = 0;
    int buffers_added = 0;

    refill_call_count++;

    /* Debug: count how many buffers are free (available for refill) */
    int free_count = 0;
    for (int i = 0; i < MAX_PACKETS; i++) {
        if (!rx_buffer_used[i]) free_count++;
    }

    #if DEBUG_ENABLED_DEBUG
    if (first_call || free_count > 0) {
        DEBUG("%s: refill_rx_queue() call #%u: %d/%d buffers free (available to refill)\n",
               COMPONENT_NAME, refill_call_count, free_count, MAX_PACKETS);
        first_call = false;
    }
    #else
    /* Only warn if buffers are critically low */
    if (free_count > MAX_PACKETS / 2) {
        DEBUG_WARN("%s: [WARN]  RX buffers low: %d/%d free\n", COMPONENT_NAME, free_count, MAX_PACKETS);
    }
    #endif

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

    if (buffers_added > 0) {
        #if DEBUG_ENABLED_DEBUG
        DEBUG_INFO("%s: [OK] Refilled RX queue with %d buffers (avail_idx now=%u)\n",
               COMPONENT_NAME, buffers_added, vq->avail->idx);
        #endif
        /* Notify device of new buffers */
        VREG_WRITE(VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_NET_RX_QUEUE);
    } else if (free_count > 0) {
        DEBUG_WARN("%s: [WARN]  WARNING: %d buffers were free but refill added 0! (avail_idx=%u)\n",
               COMPONENT_NAME, free_count, vq->avail->idx);
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

    #if DEBUG_TRAFFIC
    /* CRITICAL DEBUG: Confirm this function is being called */
    if (tx_count <= 20) {
        DEBUG("%s: ⚡ netif_output() CALLED - tx_count=%u, pbuf len=%u\n",
               COMPONENT_NAME, tx_count, p->tot_len);
    }

    /* Detailed TX logging for first 10 packets */
    if (tx_count <= 10) {
        uint32_t timestamp_ms = sys_now();
        DEBUG("\n╔══════════════════════════════════════════════════════════╗\n");
        DEBUG("║  [TX] OUTGOING PACKET #%u [T=%u.%03us]                      ║\n",
               tx_count, timestamp_ms / 1000, timestamp_ms % 1000);
        DEBUG("╚══════════════════════════════════════════════════════════╝\n");
        DEBUG("  Size: %u bytes\n", p->tot_len);
    }
    #endif

    /* Get TX descriptor pair (header + packet) - need 2 consecutive descriptors */
    uint16_t hdr_desc_idx = next_tx_desc;
    uint16_t pkt_desc_idx = (next_tx_desc + 1) % vq->num;
    next_tx_desc = (next_tx_desc + 2) % vq->num;  /* Advance by 2 for chaining */

    int tx_buf_idx = (hdr_desc_idx + MAX_PACKETS/2) % MAX_PACKETS;

    /* CRITICAL: Validate TX buffer index */
    if (tx_buf_idx < 0 || tx_buf_idx >= MAX_PACKETS) {
        DEBUG_ERROR("%s: [ERR] FATAL: Invalid TX buffer index %d (hdr_desc=%u, max=%d)\n",
               COMPONENT_NAME, tx_buf_idx, hdr_desc_idx, MAX_PACKETS);
        return ERR_BUF;
    }

    if (packet_buffers[tx_buf_idx] == NULL) {
        DEBUG_ERROR("%s: [ERR] FATAL: TX Buffer[%d] is NULL!\n", COMPONENT_NAME, tx_buf_idx);
        return ERR_BUF;
    }

    /* Copy pbuf chain to TX buffer */
    uint16_t copied = pbuf_copy_partial(p, packet_buffers[tx_buf_idx],
                                        p->tot_len, 0);

    if (copied != p->tot_len) {
        DEBUG("%s: Failed to copy pbuf: %u/%u bytes\n",
               COMPONENT_NAME, copied, p->tot_len);
        return ERR_BUF;
    }

    /*
     * NOTE: This IP restoration logic is DISABLED (metadata lookup intentionally fails)
     *
     * Net1's role: Forward SCADA requests to PLC (outbound connections)
     * Packet flow: Net1 (192.168.95.1) → PLC (192.168.95.2)
     *
     * IP restoration should NOT happen for outbound connections:
     * - Correct: src=192.168.95.1 (Net1 interface), dest=192.168.95.2 (PLC)
     * - Wrong: src=192.168.95.2 (PLC's own IP!), dest=192.168.95.2 (PLC)
     *
     * Current state: Metadata lookup ALWAYS FAILS (wrong port matching logic)
     * Result: No IP restoration, packets sent with correct IPs, communication works!
     *
     * TODO: Remove this entire IP restoration block - it's not needed for Net1
     */
    uint8_t *tx_data = packet_buffers[tx_buf_idx];
    if (p->tot_len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
        struct ethhdr *eth = (struct ethhdr *)tx_data;
        if (ntohs(eth->h_proto) == 0x0800) {  /* IPv4 */
            struct iphdr *ip = (struct iphdr *)(tx_data + sizeof(struct ethhdr));

            if (ip->protocol == 6) {  /* TCP */
                /* Extract current IPs and ports */
                uint32_t current_src = ntohl(ip->saddr);  /* 192.168.95.1 (Net1 interface) */
                uint32_t current_dest = ntohl(ip->daddr); /* 192.168.95.2 (PLC) */

                size_t ip_hdr_len = (ip->ihl) * 4;
                if (p->tot_len >= sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct tcphdr)) {
                    struct tcphdr *tcp = (struct tcphdr *)(tx_data + sizeof(struct ethhdr) + ip_hdr_len);
                    uint16_t src_port = ntohs(tcp->source);  /* lwIP's ephemeral port (e.g., 64023) */
                    uint16_t dest_port = ntohs(tcp->dest);   /* PLC Modbus port (502) */

                    struct connection_metadata *meta = NULL;
                    struct connection_metadata *partial_match = NULL;  /* Track partial matches for debugging */
                    for (int i = 0; i < MAX_CONNECTIONS; i++) {
                        /* Defensive check: ensure index is valid */
                        if (i >= MAX_CONNECTIONS) {
                            DEBUG_WARN("%s: [WARN]  TX: Invalid connection table index %d\n", COMPONENT_NAME, i);
                            break;
                        }

                        if (connection_table[i].active) {
                            /* Method 1: Lookup by ephemeral port (normal case after port is stored) */
                            if (connection_table[i].lwip_ephemeral_port == src_port &&
                                connection_table[i].src_port == dest_port) {
                                meta = &connection_table[i];
                                break;
                            }

                            /* Debug: Check for partial matches (ephemeral port matches but src_port doesn't) */
                            if (connection_table[i].lwip_ephemeral_port == src_port &&
                                connection_table[i].src_port == dest_port) {
                                /* This is actually a full match, captured above */
                            } else if (connection_table[i].lwip_ephemeral_port == src_port) {
                                /* Partial match: ephemeral port matches but src_port doesn't */
                                partial_match = &connection_table[i];
                                /* Don't break - keep searching for full match */
                            }
                        }
                    }

                    if (meta != NULL && meta->active) {
                        /* v2.210: Update TX timestamp for delayed metadata cleanup */
                        meta->last_tx_timestamp = sys_now();

                        /* Double-check metadata is valid before using */
                        if (meta->original_dest_ip == 0) {
                            DEBUG_WARN("%s: [WARN]  TX: Invalid metadata - original_dest_ip is 0\n", COMPONENT_NAME);
                        } else {
                            /* Restore original destination IP (PLC IP) as source */
                            ip->saddr = htonl(meta->original_dest_ip);  /* 192.168.95.2 */

                            #if DEBUG_TRAFFIC
                            DEBUG("%s: [RETRY] TX: Restored source IP: %u.%u.%u.%u → %u.%u.%u.%u\n",
                                   COMPONENT_NAME,
                                   (current_src >> 24) & 0xFF, (current_src >> 16) & 0xFF,
                                   (current_src >> 8) & 0xFF, current_src & 0xFF,
                                   (meta->original_dest_ip >> 24) & 0xFF, (meta->original_dest_ip >> 16) & 0xFF,
                                   (meta->original_dest_ip >> 8) & 0xFF, meta->original_dest_ip & 0xFF);
                            #endif

                            /* Recalculate IP checksum using lwIP's inet_chksum */
                            uint16_t old_ip_check = ip->check;
                            ip->check = 0;
                            uint16_t new_ip_check = inet_chksum(ip, ip->ihl * 4);
                            ip->check = new_ip_check;

                            #if DEBUG_TRAFFIC
                            DEBUG("%s: [FIX] TX: IP checksum: 0x%04x → 0x%04x\n",
                                   COMPONENT_NAME, ntohs(old_ip_check), ntohs(new_ip_check));
                            #endif

                            /* Recalculate TCP checksum with pseudo-header */
                            uint16_t old_tcp_check = tcp->check;
                            tcp->check = 0;
                            uint16_t tcp_len = ntohs(ip->tot_len) - (ip->ihl * 4);
                            uint16_t new_tcp_check = tcp_checksum(ip, tcp, tcp_len);
                            tcp->check = new_tcp_check;

                            #if DEBUG_TRAFFIC
                            DEBUG("%s: [FIX] TX: TCP checksum: 0x%04x → 0x%04x\n",
                                   COMPONENT_NAME, ntohs(old_tcp_check), ntohs(new_tcp_check));
                            #endif
                        }
                    } else {
                        static uint32_t tx_error_count = 0;
                        tx_error_count++;
                    }
                }
            } else if (ip->protocol == 1) {  /* v2.242: ICMP */
                /* Extract ICMP header */
                size_t ip_hdr_len = (ip->ihl) * 4;
                if (p->tot_len >= sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct icmp_echo_hdr)) {
                    struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)(tx_data + sizeof(struct ethhdr) + ip_hdr_len);

                    /* Only handle ICMP echo replies (type 0) */
                    if (icmp->type == 0) {  /* ICMP_ECHO_REPLY */
                        uint16_t icmp_id = ntohs(icmp->id);
                        uint16_t icmp_seq = ntohs(icmp->seqno);

                        /* Lookup original destination IP from metadata */
                        struct icmp_metadata *meta = icmp_metadata_lookup(icmp_id, icmp_seq);

                        if (meta != NULL) {
                            uint32_t current_src = ntohl(ip->saddr);  /* Currently 192.168.95.1 */
                            uint32_t new_src = meta->original_dest_ip;  /* Should be 192.168.90.5 */

                            DEBUG("%s: [ICMP-TX] Restoring source IP: %u.%u.%u.%u → %u.%u.%u.%u (id=%u, seq=%u)\n",
                                   COMPONENT_NAME,
                                   (current_src >> 24) & 0xFF, (current_src >> 16) & 0xFF,
                                   (current_src >> 8) & 0xFF, current_src & 0xFF,
                                   (new_src >> 24) & 0xFF, (new_src >> 16) & 0xFF,
                                   (new_src >> 8) & 0xFF, new_src & 0xFF,
                                   icmp_id, icmp_seq);

                            /* Restore source IP to original destination (pretend to be SCADA) */
                            ip->saddr = htonl(new_src);

                            /* Recalculate IP checksum using lwIP's inet_chksum */
                            ip->check = 0;
                            ip->check = inet_chksum(ip, ip_hdr_len);

                            /* Recalculate ICMP checksum */
                            icmp->chksum = 0;
                            icmp->chksum = inet_chksum(icmp, p->tot_len - sizeof(struct ethhdr) - ip_hdr_len);
                        }
                    }
                }
            }
        }
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

    #if DEBUG_ENABLED_DEBUG
    /* DEBUG: Log descriptor setup for first TX */
    if (tx_count == 1) {
        DEBUG("%s: DEBUG TX descriptor chain:\n", COMPONENT_NAME);
        DEBUG("  Desc[%u] (header): addr=0x%lx, len=%u, flags=0x%x, next=%u\n",
               hdr_desc_idx, hdr_paddr, VIRTIO_NET_HDR_SIZE, VIRTQ_DESC_F_NEXT, pkt_desc_idx);
        DEBUG("  Desc[%u] (packet): addr=0x%lx, len=%u, flags=0x%x, next=%u\n",
               pkt_desc_idx, packet_buffers_paddr[tx_buf_idx], p->tot_len, 0, 0);
        DEBUG("  avail->ring[%u] = %u (head of chain)\n", avail_idx, hdr_desc_idx);
    }
    #endif
    __sync_synchronize();
    vq->avail->idx++;

    /* Notify device */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_NET_TX_QUEUE);

    packets_sent++;
    return ERR_OK;
}

/*
 * ═══════════════════════════════════════════════════════════════
 * HELPER FUNCTIONS
 * ═══════════════════════════════════════════════════════════════
 */

/* Fast IP checksum calculation */
static inline uint16_t ip_fast_csum(const void *iph, unsigned int ihl)
{
    const uint16_t *ptr = (const uint16_t *)iph;
    uint32_t sum = 0;

    while (ihl > 1) {
        sum += *ptr++;
        if (sum & 0x80000000)
            sum = (sum & 0xFFFF) + (sum >> 16);
        ihl -= 2;
    }

    if (ihl > 0)
        sum += *(uint8_t *)ptr;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

/* TCP checksum calculation with pseudo-header */
static uint16_t tcp_checksum(struct iphdr *ip, struct tcphdr *tcp, uint16_t tcp_len)
{
    uint32_t sum = 0;
    uint16_t *ptr;
    int i;

    /* Pseudo-header (source IP) */
    sum += (ip->saddr >> 16) & 0xFFFF;
    sum += ip->saddr & 0xFFFF;

    /* Pseudo-header (dest IP) */
    sum += (ip->daddr >> 16) & 0xFFFF;
    sum += ip->daddr & 0xFFFF;

    /* Pseudo-header (protocol and TCP length) */
    sum += htons(IP_PROTO_TCP);
    sum += htons(tcp_len);

    /* TCP header and data */
    ptr = (uint16_t *)tcp;
    for (i = 0; i < tcp_len / 2; i++) {
        sum += ptr[i];
    }

    /* Handle odd byte */
    if (tcp_len & 1) {
        sum += ((uint8_t *)tcp)[tcp_len - 1];
    }

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

/*
 * ═══════════════════════════════════════════════════════════════
 * CONNECTION TRACKING FOR METADATA PRESERVATION
 * ═══════════════════════════════════════════════════════════════
 *
 * Problem: ICS validation needs original source/dest IPs
 * - Packets arrive: 192.168.90.5 (SCADA) → 192.168.95.2 (PLC)
 * - We rewrite: 192.168.90.5 → 192.168.95.1 (for lwIP acceptance)
 * - ICS pipeline needs to know original dest was 192.168.95.2
 * - TCP responses must restore: 192.168.95.2 → 192.168.90.5
 *
 * Solution: Connection tracking table
 * - Store original IPs when packet arrives
 * - Link to TCP PCB when connection established
 * - Lookup metadata when sending responses
 * - Restore original IPs before transmission
 */

static void update_shared_connection_state(void)
{
    if (!own_state) return;

    /* Update connection count and timestamp */
    ((struct connection_state_table *)own_state)->count = connection_count;
    ((struct connection_state_table *)own_state)->last_update = sys_now();

    /* Copy active connections to shared state */
    int shared_idx = 0;
    for (int i = 0; i < MAX_CONNECTIONS && shared_idx < MAX_SHARED_CONNECTIONS; i++) {
        if (connection_table[i].active) {
            struct connection_view *view = (struct connection_view *)&own_state->connections[shared_idx];
            view->session_id = connection_table[i].session_id;  /* v2.150: Share session ID */
            view->src_ip = connection_table[i].original_src_ip;
            view->dst_ip = connection_table[i].original_dest_ip;
            view->src_port = connection_table[i].src_port;
            view->dst_port = connection_table[i].dest_port;
            view->timestamp = connection_table[i].timestamp;
            view->active = true;
            shared_idx++;
        }
    }

    /* Clear remaining slots */
    for (int i = shared_idx; i < MAX_SHARED_CONNECTIONS; i++) {
        struct connection_view *view = (struct connection_view *)&own_state->connections[i];
        view->active = false;
    }

    /* Memory barrier to ensure updates are visible to Net0 */
    __sync_synchronize();
}

static void process_cleanup_queue(void);
static void check_pending_cleanups(void);  /* v2.210: Delayed metadata cleanup */
static void inbound_free_state(struct tcp_inbound_client_state *state);  /* Free pool state */

/* Store metadata for a new connection */
static struct connection_metadata* connection_add(uint32_t session_id,  /* v2.150: Session ID from Net0 */
                                                   uint32_t orig_src, uint32_t orig_dest,
                                                   uint16_t sport, uint16_t dport,
                                                   struct tcp_inbound_client_state *pool_state)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].session_id == session_id) {

            /* Found existing metadata for this session */
            DEBUG("%s: [REUSE] Session %u already has metadata in slot %d (port %u→%u)\n",
                   COMPONENT_NAME, session_id, i,
                   connection_table[i].src_port, connection_table[i].dest_port);

            /* Check if port changed (Net0 assigned new port to same session) */
            if (connection_table[i].src_port != sport ||
                connection_table[i].dest_port != dport) {

                DEBUG("%s:   → Port changed: %u→%u to %u→%u (cleaning up old PCB)\n",
                       COMPONENT_NAME,
                       connection_table[i].src_port, connection_table[i].dest_port,
                       sport, dport);

                /* Clean up old PCB if it exists (port changed, old connection stale) */
                if (connection_table[i].pcb != NULL) {
                    struct tcp_pcb *old_pcb = connection_table[i].pcb;
                    DEBUG("%s:   → Aborting old PCB %p for port change\n",
                           COMPONENT_NAME, (void*)old_pcb);
                    tcp_abort(old_pcb);
                }
            } else {
                /* Same port - this might be rapid close/reopen */
                DEBUG("%s:   → Same port %u→%u (rapid reuse)\n",
                       COMPONENT_NAME, sport, dport);
            }

            connection_table[i].active = true; 
            connection_table[i].pcb = NULL;
            connection_table[i].session_id = session_id; 
            connection_table[i].original_src_ip = orig_src;
            connection_table[i].original_dest_ip = orig_dest;
            connection_table[i].src_port = sport;
            connection_table[i].dest_port = dport;
            connection_table[i].timestamp = sys_now();
            connection_table[i].last_activity = sys_now();
            connection_table[i].pool_state = pool_state;
            connection_table[i].error_notified = false;
            connection_table[i].close_pending = false;

            DEBUG("%s: [COUNT==] %u (unchanged) | connection_add() REUSED slot=%d session=%u port=%u→%u\n",
                   COMPONENT_NAME, connection_count, i, session_id, sport, dport);

            #if DEBUG_METADATA
            DEBUG("%s: 🔄 Reused metadata [%d]: %u.%u.%u.%u:%u → %u.%u.%u.%u:%u\n",
                   COMPONENT_NAME, i,
                   (orig_src >> 24) & 0xFF, (orig_src >> 16) & 0xFF,
                   (orig_src >> 8) & 0xFF, orig_src & 0xFF, sport,
                   (orig_dest >> 24) & 0xFF, (orig_dest >> 16) & 0xFF,
                   (orig_dest >> 8) & 0xFF, orig_dest & 0xFF, dport);
            #endif

            /* v2.117: Update shared connection state */
            update_shared_connection_state();

            return &connection_table[i];
        }
    }

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_table[i].active && connection_table[i].session_id == 0) {
            connection_table[i].active = true;
            connection_table[i].pcb = NULL;  /* Will be set when TCP accept happens */
            connection_table[i].session_id = session_id;  /* v2.150: Store session ID from Net0 */
            connection_table[i].original_src_ip = orig_src;
            connection_table[i].original_dest_ip = orig_dest;
            connection_table[i].src_port = sport;
            connection_table[i].dest_port = dport;
            connection_table[i].timestamp = sys_now();
            connection_table[i].last_activity = sys_now();
            connection_table[i].pool_state = pool_state; 
            connection_table[i].error_notified = false; 
            connection_table[i].close_pending = false; 
            uint32_t old_count = connection_count;
            connection_count++;
            DEBUG("%s: [COUNT++] %u → %u | connection_add() NEW slot=%d session=%u port=%u→%u\n",
                   COMPONENT_NAME, old_count, connection_count, i, session_id, sport, dport);

            #if DEBUG_METADATA
            DEBUG("%s: 📝 Stored metadata [%d]: %u.%u.%u.%u:%u → %u.%u.%u.%u:%u\n",
                   COMPONENT_NAME, i,
                   (orig_src >> 24) & 0xFF, (orig_src >> 16) & 0xFF,
                   (orig_src >> 8) & 0xFF, orig_src & 0xFF, sport,
                   (orig_dest >> 24) & 0xFF, (orig_dest >> 16) & 0xFF,
                   (orig_dest >> 8) & 0xFF, orig_dest & 0xFF, dport);
            #endif

            update_shared_connection_state();

            return &connection_table[i];
        }
    }

    DEBUG("%s: [EMERGENCY] No clean slots available - processing cleanup queue immediately!\n",
           COMPONENT_NAME);
    DEBUG("%s:   → All 150 slots have cleanup pending (burst close detected)\n",
           COMPONENT_NAME);

    /* Synchronous cleanup - process all pending cleanup requests NOW */
    process_cleanup_queue();

    /* Try again - should find clean slots now */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_table[i].active && connection_table[i].session_id == 0) {
            connection_table[i].active = true;
            connection_table[i].pcb = NULL;
            connection_table[i].session_id = session_id;
            connection_table[i].original_src_ip = orig_src;
            connection_table[i].original_dest_ip = orig_dest;
            connection_table[i].src_port = sport;
            connection_table[i].dest_port = dport;
            connection_table[i].timestamp = sys_now();
            connection_table[i].last_activity = sys_now();
            connection_table[i].pool_state = pool_state;
            connection_table[i].error_notified = false;
            connection_table[i].close_pending = false;

            uint32_t old_count = connection_count;
            connection_count++;
            DEBUG("%s: [COUNT++] %u → %u | connection_add() EMERGENCY slot=%d session=%u port=%u→%u\n",
                   COMPONENT_NAME, old_count, connection_count, i, session_id, sport, dport);

            update_shared_connection_state();

            DEBUG("%s:   ✓ Emergency cleanup successful - slot %d allocated\n",
                   COMPONENT_NAME, i);
            return &connection_table[i];
        }
    }

    /* Still no slots after emergency cleanup - system critically overloaded! */
    DEBUG("%s: [CRITICAL] Connection table STILL full after emergency cleanup!\n",
           COMPONENT_NAME);
    DEBUG("%s:   → System overloaded: All slots active with PCBs\n", COMPONENT_NAME);
    DEBUG("%s:   → Cannot accept new connection (session %u)\n", COMPONENT_NAME, session_id);
    return NULL;
}

/* Link PCB to existing metadata entry */
static void connection_link_pcb(struct tcp_pcb *pcb, uint16_t sport, uint16_t dport)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active &&
            connection_table[i].src_port == sport &&
            connection_table[i].dest_port == dport &&
            connection_table[i].pcb == NULL) {
            connection_table[i].pcb = pcb;
            #if DEBUG_METADATA
            DEBUG("%s: [LINK] Linked PCB to metadata [%d]\n", COMPONENT_NAME, i);
            #endif
            return;
        }
    }
    #if DEBUG_METADATA
    DEBUG_WARN("%s: [WARN]  No metadata found for %u → %u\n", COMPONENT_NAME, sport, dport);
    #endif
}

/* Lookup metadata by PCB */
static struct connection_metadata* connection_lookup_by_pcb(struct tcp_pcb *pcb)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active && connection_table[i].pcb == pcb) {
            return &connection_table[i];
        }
    }
    return NULL;
}

static struct connection_metadata* connection_lookup_by_session_id(uint32_t session_id)
{
    if (session_id == 0) return NULL;  /* 0 = unassigned/cleaned */

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].session_id == session_id) {
            return &connection_table[i];
        }
    }
    return NULL;
}

/* Lookup metadata by 5-tuple (for SYN packets before PCB exists) */
static struct connection_metadata* connection_lookup_by_tuple(uint32_t src_ip, uint32_t dest_ip,
                                                               uint16_t sport, uint16_t dport)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active &&
            connection_table[i].original_src_ip == src_ip &&
            connection_table[i].src_port == sport &&
            connection_table[i].dest_port == dport) {
            return &connection_table[i];
        }
    }
    return NULL;
}

static inline void enqueue_cleanup(uint32_t session_id)
{
    if (session_id == 0) {
        DEBUG("%s: WARN: enqueue_cleanup called with session_id=0 (ignoring)\n",
               COMPONENT_NAME);
        return;
    }

    uint32_t head = cleanup_queue.head;
    uint32_t tail = cleanup_queue.tail;

    /* Check if queue is full */
    if (head - tail >= CLEANUP_QUEUE_SIZE) {
        DEBUG("%s: ERROR: Cleanup queue full! head=%u tail=%u (session %u dropped)\n",
               COMPONENT_NAME, head, tail, session_id);
        return;
    }

    /* Add to queue */
    uint32_t slot = head & CLEANUP_QUEUE_MASK;
    cleanup_queue.requests[slot].session_id = session_id;
    cleanup_queue.requests[slot].timestamp = sys_now();

    __sync_synchronize();  /* Memory barrier - ensure request is written before head update */

    cleanup_queue.head = head + 1;
    cleanup_stats.enqueued++;

    DEBUG("%s: [QUEUE] Enqueued cleanup session=%u (queue depth=%u)\n",
           COMPONENT_NAME, session_id, head + 1 - tail);
}

static void process_cleanup_queue(void)
{
    uint32_t tail = cleanup_queue.tail;
    uint32_t head = cleanup_queue.head;

    /* Process all pending requests */
    while (tail != head) {
        uint32_t slot = tail & CLEANUP_QUEUE_MASK;
        struct cleanup_request *req = &cleanup_queue.requests[slot];

        /* Skip if session_id is 0 (invalid/already cleaned) */
        if (req->session_id == 0) {
            DEBUG("%s: [QUEUE] SKIP: session_id=0 (invalid or already cleaned)\n",
                   COMPONENT_NAME);
            cleanup_stats.duplicates++;
            tail++;
            cleanup_queue.tail = tail;
            continue;
        }

        DEBUG("%s: [QUEUE] Processing cleanup session=%u (queued %ums ago)\n",
               COMPONENT_NAME, req->session_id, sys_now() - req->timestamp);

        /* Lookup connection by session_id */
        struct connection_metadata *meta = connection_lookup_by_session_id(req->session_id);

        if (meta == NULL) {
            /* Connection not found - already cleaned up */
            DEBUG("%s: [QUEUE] SKIP: session=%u not found (already cleaned)\n",
                   COMPONENT_NAME, req->session_id);
            cleanup_stats.duplicates++;
        } else {
            if (meta->metadata_close_pending) {
                uint32_t time_since_close = sys_now() - meta->close_timestamp;
                uint32_t time_since_tx = sys_now() - meta->last_tx_timestamp;
                bool tx_idle = (time_since_tx > 1000);       /* No TX for 1 second */
                bool timeout = (time_since_close > 5000);    /* Forced cleanup after 5 seconds */
                float pool_usage = (float)connection_count / MAX_CONNECTIONS;
                bool emergency = (pool_usage > 0.8);         /* Pool filling up */

                if (tx_idle || timeout || emergency) {
                    /* Conditions met - perform cleanup NOW */
                    const char *reason = tx_idle ? "TX idle 1000ms" :
                                        timeout ? "timeout 5000ms" :
                                        "emergency pool 80 percent";
                    DEBUG("%s:   → time_since_close=%ums, time_since_tx=%ums, pool=%d/%d\n",
                           COMPONENT_NAME, time_since_close, time_since_tx,
                           connection_count, MAX_CONNECTIONS);
                    /* Continue to cleanup below... */
                } else {
                    DEBUG("%s:   → time_since_close=%ums, time_since_tx=%ums, pool=%d/%d\n",
                           COMPONENT_NAME, time_since_close, time_since_tx,
                           connection_count, MAX_CONNECTIONS);

                    /* Advance tail to prevent infinite loop, then skip cleanup */
                    __sync_synchronize();
                    tail++;
                    cleanup_queue.tail = tail;
                    continue;
                }
            }

            /* Clean up pending outbound data */
            if (meta->pending_outbound_data != NULL) {
                DEBUG("%s: Freeing unsent pending data (%u bytes) for session %u\n",
                       COMPONENT_NAME, meta->pending_outbound_len, req->session_id);
                free(meta->pending_outbound_data);
                meta->pending_outbound_data = NULL;
            }

            /* Decrement counters */
            if (connection_count > 0) {
                connection_count--;
                DEBUG("%s: [COUNT--] %u → %u | cleanup session=%u (queued %ums ago)\n",
                       COMPONENT_NAME, connection_count + 1, connection_count,
                       req->session_id, sys_now() - req->timestamp);
            } else {
                DEBUG_WARN("%s: [WARN] connection_count already 0 (prevented underflow for session %u)!\n",
                       COMPONENT_NAME, req->session_id);
            }

            /* v2.256: REMOVED active_connections-- from cleanup queue
             * ═══════════════════════════════════════════════════════════════
             * Root cause of underflow: active_connections is only incremented
             * in tcp_echo_accept() for INBOUND TCP connections (SCADA→Net1 server).
             * But cleanup queue processes SESSION cleanup (Net0 session IDs),
             * which are for OUTBOUND connections (Net1→PLC). These are NOT 1:1.
             *
             * active_connections should only be decremented when the corresponding
             * tcp_echo_accept() connection closes (via tcp_err callback), NOT here.
             * ═══════════════════════════════════════════════════════════════
             */
            total_connections_closed++;

            meta->active = false;
            meta->pcb = NULL;
            meta->has_pending_outbound = false;
            meta->pending_outbound_len = 0;
            meta->awaiting_response = false;
            meta->response_received = false;
            meta->close_pending = false;
            meta->close_notified = false;
            meta->cleanup_in_progress = false;
            meta->metadata_close_pending = false;  /* v2.212: Clear deferred cleanup flag */
            meta->pcb_closed = false;              /* v2.212: Reset PCB state */

            /* Set session_id=0 for idempotency */
            meta->session_id = 0;

            /* Update shared connection state */
            update_shared_connection_state();

            cleanup_stats.processed++;
        }

        /* Advance tail */
        __sync_synchronize();
        tail++;
        cleanup_queue.tail = tail;
    }
}

/* Mark a connection as self-cleaned (we cleaned it up, not via close notification) */
static void mark_connection_self_cleaned(uint32_t src_ip, uint16_t src_port, uint16_t dst_port)
{
    self_cleaned_connections[self_cleaned_index].src_ip = src_ip;
    self_cleaned_connections[self_cleaned_index].src_port = src_port;
    self_cleaned_connections[self_cleaned_index].dst_port = dst_port;
    self_cleaned_connections[self_cleaned_index].timestamp = sys_now();
    self_cleaned_connections[self_cleaned_index].valid = true;

    DEBUG("%s: [TRACK] Marked connection as self-cleaned: SCADA %u.%u.%u.%u:%u → PLC:%u (index=%d)\n",
           COMPONENT_NAME,
           (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
           (src_ip >> 8) & 0xFF, src_ip & 0xFF,
           src_port, dst_port, self_cleaned_index);

    self_cleaned_index = (self_cleaned_index + 1) % MAX_SELF_CLEANED_TRACKING;
}

/* Check if a connection was recently self-cleaned (returns true if stale notification) */
static bool was_recently_self_cleaned(uint32_t src_ip, uint16_t src_port, uint16_t dst_port)
{
    uint32_t now = sys_now();

    for (int i = 0; i < MAX_SELF_CLEANED_TRACKING; i++) {
        if (!self_cleaned_connections[i].valid) {
            continue;
        }

        /* Expire old entries (> 5 seconds) */
        if (now - self_cleaned_connections[i].timestamp > SELF_CLEANED_TTL_MS) {
            self_cleaned_connections[i].valid = false;
            continue;
        }

        /* Check if this 5-tuple matches */
        if (self_cleaned_connections[i].src_ip == src_ip &&
            self_cleaned_connections[i].src_port == src_port &&
            self_cleaned_connections[i].dst_port == dst_port) {

            uint32_t age_ms = now - self_cleaned_connections[i].timestamp;
            DEBUG("%s: [TRACK] Found self-cleaned connection: SCADA %u.%u.%u.%u:%u → PLC:%u (age=%ums, index=%d)\n",
                   COMPONENT_NAME,
                   (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                   (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                   src_port, dst_port, age_ms, i);

            /* Mark as consumed to prevent duplicate matches */
            self_cleaned_connections[i].valid = false;
            return true;
        }
    }

    return false;
}

static void connection_print_stats(void)
{
    /* v2.83: CRITICAL FIX - Do NOT access pcb->state (can crash on freed PCB) */
    int active = 0;
    int stale = 0;
    int pcb_linked = 0;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active) {
            active++;
            struct tcp_pcb *pcb = connection_table[i].pcb;
            if (pcb != NULL) {
                pcb_linked++;
                /* v2.83: REMOVED pcb->state check - accessing freed PCB causes crashes! */
            } else {
                stale++;
            }
        }
    }

    int available = MAX_CONNECTIONS - active;

    #if DEBUG_METADATA
    DEBUG("%s: [STATS] Connection table: %d active (%d PCB-linked, %d stale), %d available\n",
           COMPONENT_NAME, active, pcb_linked, stale, available);
    #endif
}

/* Cleanup stale connections from the connection table
 *
 * This function removes connections where:
 * 1. PCB is NULL (connection already closed but metadata not cleaned up)
 * 2. PCB state is CLOSED or TIME_WAIT (connection finished)
 *
 * Called periodically from main loop to prevent table exhaustion
 */
static void connection_cleanup_stale(void)
{
    int cleaned = 0;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_table[i].active) {
            continue;
        }

        struct tcp_pcb *pcb = connection_table[i].pcb;

        /* Cleanup if PCB is NULL */
        if (pcb == NULL) {
            #if DEBUG_METADATA
            DEBUG("%s: [CLEAN] Cleanup stale connection [%d]: PCB is NULL\n", COMPONENT_NAME, i);
            #endif
            connection_table[i].active = false;

            /* v2.182: Track connection count changes for leak debugging */
            uint32_t old_count = connection_count;
            connection_count--;
            DEBUG("%s: [COUNT--] %u → %u | cleanup_stale() slot=%d session=%u (PCB=NULL)\n",
                   COMPONENT_NAME, old_count, connection_count, i,
                   connection_table[i].session_id);

            cleaned++;
            continue;
        }
    }

    if (cleaned > 0) {
        #if DEBUG_METADATA
        DEBUG("%s: [CLEAN] Cleaned %d stale connection(s)\n",
               COMPONENT_NAME, cleaned);
        connection_print_stats();
        #endif

        /* v2.117: Update shared connection state after cleanup */
        update_shared_connection_state();
    }
}

static bool arp_proxy_check_and_reply(struct pbuf *p, struct netif *inp)
{
    struct eth_hdr *ethhdr = (struct eth_hdr *)p->payload;

    /* Need room for Ethernet + ARP headers */
    if (p->len < sizeof(struct eth_hdr) + sizeof(struct etharp_hdr)) {
        return false;  /* Not a valid ARP packet */
    }

    /* Get ARP header (after Ethernet header) */
    struct etharp_hdr *arphdr = (struct etharp_hdr *)((uint8_t *)p->payload + sizeof(struct eth_hdr));

    /* Only handle ARP requests (opcode = 1) */
    if (ntohs(arphdr->opcode) != 1) {  /* ARP_REQUEST */
        return false;  /* Not a request */
    }

    /* Extract target IP address from ARP request */
    uint32_t target_ip = (arphdr->dipaddr.addrw[0] << 16) | arphdr->dipaddr.addrw[1];
    target_ip = ntohl(target_ip);

    /* Check if this is a request for SCADA IP (192.168.90.5) */
    #define SCADA_IP 0xC0A85A05  /* 192.168.90.5 in hex */
    if (target_ip != SCADA_IP) {
        return false;  /* Not for our proxied IP */
    }

    DEBUG("%s: [ARP-PROXY] Request for 192.168.90.5 (SCADA) - replying with our MAC\n",
           COMPONENT_NAME);

    /* Build ARP reply in the same pbuf (reuse the request packet) */
    arphdr->opcode = htons(2);  /* ARP_REPLY */

    /* Swap sender and target fields */
    /* Target becomes the original sender */
    memcpy(&arphdr->dhwaddr, &arphdr->shwaddr, sizeof(struct eth_addr));
    arphdr->dipaddr.addrw[0] = arphdr->sipaddr.addrw[0];
    arphdr->dipaddr.addrw[1] = arphdr->sipaddr.addrw[1];

    /* Sender becomes us (with SCADA's IP) */
    memcpy(&arphdr->shwaddr, &inp->hwaddr, sizeof(struct eth_addr));
    arphdr->sipaddr.addrw[0] = htons((SCADA_IP >> 16) & 0xFFFF);
    arphdr->sipaddr.addrw[1] = htons(SCADA_IP & 0xFFFF);

    /* Fix Ethernet header */
    memcpy(&ethhdr->dest, &ethhdr->src, sizeof(struct eth_addr));  /* Reply to sender */
    memcpy(&ethhdr->src, &inp->hwaddr, sizeof(struct eth_addr));   /* From us */

    /* Send the ARP reply directly via low-level output */
    inp->linkoutput(inp, p);

    DEBUG("%s: [ARP-PROXY] Sent ARP reply: 192.168.90.5 is at %02x:%02x:%02x:%02x:%02x:%02x\n",
           COMPONENT_NAME,
           inp->hwaddr[0], inp->hwaddr[1], inp->hwaddr[2],
           inp->hwaddr[3], inp->hwaddr[4], inp->hwaddr[5]);

    return true;  /* We handled it */
}

/*
 * Custom input function for protocol-break architecture WITH metadata preservation
 *
 * CRITICAL: Packets arrive with dest IP = 192.168.95.2 (PLC) but interface IP = 192.168.95.1
 * lwIP's ip_input() rejects packets not destined for interface IP
 *
 * Solution:
 * 1. Store original src/dest IPs in connection table
 * 2. Rewrite destination IP to match interface IP
 * 3. Pass to lwIP for processing
 * 4. Later restore original IPs when sending responses
 */
static err_t custom_input_promiscuous(struct pbuf *p, struct netif *inp)
{
    struct eth_hdr *ethhdr;
    u16_t type;

    /* Check Ethernet header */
    if (p->len < sizeof(struct eth_hdr)) {
        /* v2.222: REVERTED FIX #1 - Caller frees pbuf when we return ERR_ARG
         * Double-free was breaking connections!
         */
        return ERR_ARG;
    }

    ethhdr = (struct eth_hdr *)p->payload;
    type = ntohs(ethhdr->type);

    /* Handle ARP packets normally - pass to lwIP's ARP handler */
    if (type == ETHTYPE_ARP) {
        /* v2.242: Check if this is an ARP request for our proxied IP (192.168.90.5) */
        if (arp_proxy_check_and_reply(p, inp)) {
            /* We handled it - ARP reply sent */
            pbuf_free(p);  /* Free the request packet */
            return ERR_OK;
        }

        /* Remove Ethernet header and pass to etharp_input for ARP processing */
        if (pbuf_remove_header(p, sizeof(struct eth_hdr)) == 0) {
            /* v2.222: REVERTED FIX #2 - etharp_input() DOES free pbuf (line 741 in etharp.c)
             * Double-free was breaking connections!
             */
            etharp_input(p, inp);
            return ERR_OK;
        }
        /* pbuf_remove_header failed - return ERR_ARG, caller will free */
        return ERR_ARG;
    }

    /* Handle IPv6 - pass to ethernet_input */
    if (type == ETHTYPE_IPV6) {
        return ethernet_input(p, inp);
    }

    /* Handle IPv4 with IP rewriting for protocol-break */
    if (type == ETHTYPE_IP) {
        /* Remove Ethernet header first */
        if (pbuf_remove_header(p, sizeof(struct eth_hdr)) != 0) {
            return ERR_ARG;
        }

        /* Check if this is an IPv4 packet */
        if (p->len >= 20) {  /* Minimum IPv4 header size */
            struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;

            /* Extract source and destination IPs */
            uint32_t pkt_src_ip = ntohl(iphdr->src.addr);
            uint32_t pkt_dest_ip = ntohl(iphdr->dest.addr);
            uint32_t interface_ip = ntohl(inp->ip_addr.addr);

            /* Extract ports if this is TCP */
            uint16_t src_port = 0, dest_port = 0;
            if (IPH_PROTO(iphdr) == IP_PROTO_TCP && p->len >= 20 + 20) {  /* IP + TCP headers */
                struct tcp_hdr *tcphdr = (struct tcp_hdr *)((uint8_t *)iphdr + (IPH_HL(iphdr) * 4));
                src_port = ntohs(tcphdr->src);
                dest_port = ntohs(tcphdr->dest);

            } else if (IPH_PROTO(iphdr) == IP_PROTO_ICMP && p->len >= 20 + 8) {
                struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)((uint8_t *)iphdr + (IPH_HL(iphdr) * 4));

                if (icmp->type == ICMP_ECHO) {  /* Echo request (ping) */
                    uint16_t icmp_id = ntohs(icmp->id);
                    uint16_t icmp_seq = ntohs(icmp->seqno);

                    /* Store metadata so TX path can restore source IP */
                    icmp_metadata_store(pkt_dest_ip, icmp_id, icmp_seq);

                    DEBUG("%s: [ICMP-RX] Ping to %u.%u.%u.%u (id=%u, seq=%u) - metadata stored\n",
                           COMPONENT_NAME,
                           (pkt_dest_ip >> 24) & 0xFF, (pkt_dest_ip >> 16) & 0xFF,
                           (pkt_dest_ip >> 8) & 0xFF, pkt_dest_ip & 0xFF,
                           icmp_id, icmp_seq);
                }
            }

            /* If packet is not destined for our interface IP, rewrite it */
            if (pkt_dest_ip != interface_ip) {
                DEBUG("%s: [RETRY] Rewriting dest IP: %u.%u.%u.%u → %u.%u.%u.%u\n",
                       COMPONENT_NAME,
                       (pkt_dest_ip >> 24) & 0xFF, (pkt_dest_ip >> 16) & 0xFF,
                       (pkt_dest_ip >> 8) & 0xFF, pkt_dest_ip & 0xFF,
                       (interface_ip >> 24) & 0xFF, (interface_ip >> 16) & 0xFF,
                       (interface_ip >> 8) & 0xFF, interface_ip & 0xFF);

                /* CRITICAL: Store original IPs BEFORE rewriting */
                if (IPH_PROTO(iphdr) == IP_PROTO_TCP && src_port != 0 && dest_port != 0) {
                    /* Check if we already have metadata for this connection */
                    struct connection_metadata *meta = connection_lookup_by_tuple(
                        pkt_src_ip, pkt_dest_ip, src_port, dest_port);

                    if (!meta) {
                        connection_add(0, pkt_src_ip, pkt_dest_ip, src_port, dest_port, NULL);
                    }
                }

                /* Rewrite destination IP to interface IP */
                iphdr->dest.addr = inp->ip_addr.addr;

                /* Recalculate IP checksum */
                iphdr->_chksum = 0;
                iphdr->_chksum = inet_chksum(iphdr, IPH_HL(iphdr) * 4);
            }
        }

        return ip_input(p, inp);
    }

    /* Unknown protocol - drop */
    DEBUG_WARN("%s: [WARN] Unknown ethernet protocol: ethertype=0x%04x, pbuf=%p, p->ref=%d, p->len=%u\n",
           COMPONENT_NAME, type, (void*)p, p->ref, p->len);
    pbuf_free(p);
    
    
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
        DEBUG("\n");
        DEBUG("╔════════════════════════════════════════════════════════╗\n");
        DEBUG("║  🎉 DHCP SUCCESS! Network Interface Configured         ║\n");
        DEBUG("╚════════════════════════════════════════════════════════╝\n");
        DEBUG("%s: IP Address:  %s\n", COMPONENT_NAME,
               ip4addr_ntoa(netif_ip4_addr(netif)));
        DEBUG("%s: Netmask:     %s\n", COMPONENT_NAME,
               ip4addr_ntoa(netif_ip4_netmask(netif)));
        DEBUG("%s: Gateway:     %s\n", COMPONENT_NAME,
               ip4addr_ntoa(netif_ip4_gw(netif)));
        DEBUG("%s: MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
               COMPONENT_NAME,
               netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
               netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);
        DEBUG("\n");
        DEBUG("%s: TCP Echo Server listening on port %d\n",
               COMPONENT_NAME, TCP_ECHO_PORT);
        DEBUG("%s: Test with: telnet %s %d\n",
               COMPONENT_NAME, ip4addr_ntoa(netif_ip4_addr(netif)), TCP_ECHO_PORT);
        DEBUG("\n");

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

    #if DEBUG_TRAFFIC
    /* FUNDAMENTAL CHECK: Poll VirtIO device InterruptStatus register */
    if (check_count <= 5) {
        uint32_t irq_status = VREG_READ(VIRTIO_MMIO_INTERRUPT_STATUS);
        uint32_t dev_status = VREG_READ(VIRTIO_MMIO_STATUS);
        DEBUG("%s: RX check #%u: used_idx=%u, last_used=%u, IRQ_STATUS=0x%x, DEV_STATUS=0x%x, regs=%p\n",
               COMPONENT_NAME, check_count, vq->used->idx, last_used_idx, irq_status, dev_status, (void*)virtio_regs_base);
    } else if (vq->used->idx != last_used_idx) {
        DEBUG("%s: RX queue check #%u: used_idx=%u, last_used=%u\n",
               COMPONENT_NAME, check_count, vq->used->idx, last_used_idx);
    }
    #endif

    /* Process packets using correct wraparound arithmetic
     * CRITICAL: Re-read used->idx on EVERY iteration to avoid reading stale data
     * when we skip corrupted packets
     *
     * PACKET BURST LIMIT: Process at most 8 packets per call, then return to main loop
     * This ensures we check for ICS notifications regularly and don't starve other tasks
     */
    uint32_t loop_count = 0;
    const uint32_t MAX_PACKETS_PER_CALL = 8;

    while (loop_count < MAX_PACKETS_PER_CALL) {
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
            /* No more packets - exit IRQ handler and let timer handle refill */
            return;
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
                /* Don't refill here - let timer handle it to avoid IRQ storm */
                return;
            }

            /* True desync - should never happen with proper memory barriers */
            DEBUG_WARN("%s: [WARN] TRUE DESYNC: pending=%u exceeds ring_size=%u\n",
                   COMPONENT_NAME, pending_packets, vq->num);
            DEBUG("%s:   last_used_idx=%u, current_used_idx=%u\n",
                   COMPONENT_NAME, last_used_idx, current_used_idx);
            last_used_idx = current_used_idx;
            /* Don't refill here - let timer handle it to avoid IRQ storm */
            return;
        }

        uint16_t used_ring_idx = last_used_idx % vq->num;
        struct virtq_used_elem *used_elem = &vq->used->ring[used_ring_idx];

        uint16_t desc_idx = used_elem->id;
        uint32_t len = used_elem->len;

        /* Safety check: prevent infinite loops (should never happen with memory barrier) */
        loop_count++;
        if (loop_count > 1000) {
            DEBUG("%s: ERROR - Processed 1000 packets in single call, breaking to prevent freeze\n",
                   COMPONENT_NAME);
            DEBUG("%s:   last_used_idx=%u, current_used_idx=%u, pending=%u\n",
                   COMPONENT_NAME, last_used_idx, current_used_idx,
                   (uint16_t)(current_used_idx - last_used_idx));
            /* Don't refill here - let timer handle it to avoid IRQ storm */
            break;
        }

        /* CRITICAL: Validate VirtIO reported length before processing
         * Valid Ethernet frames: 60-1514 bytes + 12 byte VirtIO header = 72-1526 bytes
         * With memory barrier fix, invalid lengths should be VERY rare (real hardware corruption)
         */
        if (len < VIRTIO_NET_HDR_SIZE || len > (1514 + VIRTIO_NET_HDR_SIZE)) {
            DEBUG("%s:     desc_idx=%u, used_ring_idx=%u, last_used_idx=%u, current_used_idx=%u\n",
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
            DEBUG_WARN("%s: [WARN]  INVALID descriptor index: %u (max %u)\n",
                   COMPONENT_NAME, desc_idx, MAX_PACKETS);
            DEBUG("%s:     Ring: used_ring_idx=%u, last_used=%u, current=%u\n",
                   COMPONENT_NAME, used_ring_idx, last_used_idx, current_used_idx);

            /* seL4-SAFE RECOVERY STRATEGY:
             * seL4's memory safety allows aggressive recovery without corruption risk.
             * We try multiple strategies knowing seL4 prevents double-free/use-after-free.
             */

            bool buffer_freed = false;

            // STRATEGY 1: Free buffer at ring position (most likely correct)
            if (used_ring_idx < MAX_PACKETS && rx_buffer_used[used_ring_idx]) {
                DEBUG("%s: 🛡️  seL4-SAFE: Freeing buffer %u (ring position)\n",
                       COMPONENT_NAME, used_ring_idx);
                rx_buffer_used[used_ring_idx] = false;
                buffer_freed = true;
            }

            // STRATEGY 2: If ring position was already free, scan for any used buffer
            if (!buffer_freed) {
                DEBUG_WARN("%s: [WARN]  Ring position %u already free - scanning for leaked buffers\n",
                       COMPONENT_NAME, used_ring_idx);

                for (int i = 0; i < MAX_PACKETS; i++) {
                    if (rx_buffer_used[i]) {
                        DEBUG("%s: 🛡️  seL4-SAFE FALLBACK: Freeing leaked buffer %u\n",
                               COMPONENT_NAME, i);
                        rx_buffer_used[i] = false;
                        buffer_freed = true;
                        break;  // Free one buffer to avoid over-correction
                    }
                }
            }

            if (!buffer_freed) {
                DEBUG_WARN("%s: [WARN]  No used buffers found - possible state desync!\n", COMPONENT_NAME);
            }

            last_used_idx++;
            continue;
        }

        /* Get packet buffer (use buffer index, not physical address from descriptor) */
        int buf_idx = desc_idx;

        /* CRITICAL: Validate buffer index to prevent out-of-bounds access */
        if (buf_idx < 0 || buf_idx >= MAX_PACKETS) {
            DEBUG_ERROR("%s: [ERR] FATAL: Invalid buffer index %d (desc_idx=%u, max=%d)\n",
                   COMPONENT_NAME, buf_idx, desc_idx, MAX_PACKETS);
            DEBUG("%s:    last_used_idx=%u, RX queue full, system halting\n",
                   COMPONENT_NAME, last_used_idx);
            last_used_idx++;
            continue;
        }

        uint8_t *buffer = packet_buffers[buf_idx];

        if (buffer == NULL) {
            DEBUG_ERROR("%s: [ERR] FATAL: Buffer[%d] is NULL!\n", COMPONENT_NAME, buf_idx);
            last_used_idx++;
            continue;
        }

        /* Skip virtio_net_hdr at start of buffer */
        uint8_t *packet_data = buffer + VIRTIO_NET_HDR_SIZE;
        uint16_t packet_len = len - VIRTIO_NET_HDR_SIZE;

        packets_received++;

        /* NOTE: TCP server initialization moved to post_init()
         * The tcp_server_initialized flag is set there.
         * This deferred initialization code is no longer needed.
         */

        /* Allocate pbuf and copy packet data (skipping header)
         * v2.257: Removed verbose PBUF_POOL logs (RX-ALLOC, RX-ACCEPT) */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, packet_len, PBUF_POOL);
        if (p != NULL) {
            pbuf_take(p, packet_data, packet_len);

            /* Feed packet to lwIP */
            err_t lwip_result = netif_data.input(p, &netif_data);

            /* v2.257: Only log rejections (important), not acceptances (too verbose) */
            if (lwip_result != ERR_OK) {
                extern struct stats_ lwip_stats;
                uint32_t pbuf_after_input = lwip_stats.memp[MEMP_PBUF_POOL]->used;
                DEBUG_WARN("[Net1][PBUF_POOL][RX-REJECT] lwIP rejected (err=%d), pbuf=%p | PBUF: %u/800\n",
                           lwip_result, (void*)p, pbuf_after_input);
            }

            /* CRITICAL DIAGNOSTIC: Log TCP SYN packets to diagnose connection acceptance */
            if (packet_len >= sizeof(struct ethhdr)) {
                struct ethhdr *eth = (struct ethhdr *)packet_data;
                uint16_t eth_proto_check = ntohs(eth->h_proto);

                if (eth_proto_check == 0x0800 && packet_len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {  /* IPv4 */
                    struct iphdr *ip = (struct iphdr *)(packet_data + sizeof(struct ethhdr));
                    if (ip->protocol == 6) {  /* TCP */
                        size_t ip_hdr_len = (ip->ihl) * 4;
                        if (packet_len >= sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct tcphdr)) {
                            struct tcphdr *tcp = (struct tcphdr *)(packet_data + sizeof(struct ethhdr) + ip_hdr_len);
                            uint32_t daddr = ntohl(ip->daddr);

                            if (tcp->syn && !tcp->ack) {
                                /* This is a SYN packet (connection attempt) */
                                DEBUG("%s: [FIND] SYN packet detected: Dest IP = %u.%u.%u.%u:%u (Interface IP = 192.168.95.2)\n",
                                       COMPONENT_NAME,
                                       (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF,
                                       ntohs(tcp->dest));
                                DEBUG("%s:    → If dest IP matches interface IP, lwIP should accept. Otherwise it rejects.\n", COMPONENT_NAME);
                            }
                        }
                    }
                }
            }

            /* v2.155: Let lwIP free pbuf on error - we don't own it */
            if (lwip_result != ERR_OK) {
                /* lwIP handles pbuf cleanup */
            }
        } else {
            /* CRITICAL: pbuf allocation failed - this means lwIP is out of memory */
            DEBUG_WARN("%s: [WARN]  WARNING: Failed to allocate pbuf for packet #%u - dropping (lwIP out of memory)\n",
                   COMPONENT_NAME, packets_received);
        }

        /* Mark buffer as free (buf_idx already defined above) */
        rx_buffer_used[buf_idx] = false;

        /* Move to next packet */
        last_used_idx++;
    }

    /* Print pbuf pool statistics every 10 packets to monitor allocation/deallocation */
    if (packets_received % 10 == 0 && packets_received > 0) {
        DEBUG("%s: [STATS] PBUF Pool Stats - Used: %u/%u, Avail: %u, Peak: %u\n",
               COMPONENT_NAME,
               lwip_stats.memp[MEMP_PBUF_POOL]->used,
               PBUF_POOL_SIZE,
               lwip_stats.memp[MEMP_PBUF_POOL]->avail,
               lwip_stats.memp[MEMP_PBUF_POOL]->max);
    }

    refill_rx_queue();
}

/*
 * Connection tracking counters moved to line ~115 (before cleanup queue functions)
 * v2.256: total_connections_created and total_connections_closed declared earlier
 */

/*
 * TCP Error callback - handles connection errors and cleanup
 */
static void tcp_echo_err(void *arg, err_t err)
{
    struct connection_metadata *meta = (struct connection_metadata *)arg;

    const char *err_name;
    switch (err) {
        case ERR_ABRT:     err_name = "ERR_ABRT (Connection aborted)"; break;
        case ERR_RST:      err_name = "ERR_RST (Connection reset)"; break;
        case ERR_CLSD:     err_name = "ERR_CLSD (Connection closed)"; break;
        case ERR_CONN:     err_name = "ERR_CONN (Not connected)"; break;
        case ERR_TIMEOUT:  err_name = "ERR_TIMEOUT (Timeout)"; break;
        default:           err_name = "UNKNOWN"; break;
    }

    DEBUG_WARN("%s: [WARN]  TCP connection error - err=%d (%s)\n", COMPONENT_NAME, err, err_name);

    if (meta != NULL) {
        DEBUG("%s:    → session_id=%u, clearing PCB pointer (already freed by lwIP)\n",
               COMPONENT_NAME, meta->session_id);
        meta->pcb = NULL;  /* Mark PCB as freed */

        /* Enqueue cleanup for metadata (counters will be decremented there) */
        enqueue_cleanup(meta->session_id);
    } else {
        DEBUG_WARN("%s: [WARN]  Error callback called with NULL metadata (arg=%p)\n",
                   COMPONENT_NAME, arg);
    }
}

/*
 * TCP Echo callbacks
 */
static err_t tcp_echo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (p == NULL) {

        #if DEBUG_TRAFFIC
        DEBUG("%s: [INIT] TCP connection closed gracefully\n", COMPONENT_NAME);
        DEBUG("%s:    Remote: %u.%u.%u.%u:%u\n", COMPONENT_NAME,
               ip4_addr1(&pcb->remote_ip), ip4_addr2(&pcb->remote_ip),
               ip4_addr3(&pcb->remote_ip), ip4_addr4(&pcb->remote_ip), pcb->remote_port);
        #endif

        struct connection_metadata *meta = connection_lookup_by_pcb(pcb);
        if (meta != NULL) {
            /* v2.241: Clear PCB pointer - connection is closing */
            meta->pcb = NULL;
            meta->metadata_close_pending = true;
            meta->close_timestamp = sys_now();
            meta->pcb_closed = true;
            enqueue_cleanup(meta->session_id);
            DEBUG_INFO("[CLOSE-SOURCE-1] tcp_echo_recv(p=NULL) - session=%u, PBUF=%u/%u\n",
                   meta->session_id,
                   lwip_stats.memp[MEMP_PBUF_POOL]->used,
                   lwip_stats.memp[MEMP_PBUF_POOL]->avail);
        }

        return ERR_ABRT;
    }

    err_t result = ERR_OK;  /* Default return value */

    if (err != ERR_OK) {
        /* Error case - lwIP will handle pbuf */
        result = err;
        goto cleanup;
    }

    /* ═══ Forward TCP data to ICS_Outbound (PLC→SCADA response path) ═══ */


    /* CRITICAL: Check if dataport is properly mapped by CAmkES
     * PLC responses must go through OUTBOUND path (Net1 → ICS_Outbound → Net0 → SCADA) */
    if (outbound_dp == NULL) {
        DEBUG_ERROR("%s: [ERR] FATAL: outbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        DEBUG("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        result = ERR_MEM;  /* v2.240: lwIP will store in refused_data, don't free */
        goto cleanup;
    }

    DEBUG("%s: [OK] Dataport check: outbound_dp=%p (valid)\n", COMPONENT_NAME, (void*)outbound_dp);

    /* Step 1: Create ICS message with metadata */
    ICS_Message *ics_msg = (ICS_Message *)outbound_dp;

    /* Step 2: Populate FrameMetadata (Phase 1: basic info, Phase 2: full header parsing) */
    DEBUG("%s: About to memset ics_msg->metadata at %p (size=%zu)\n",
           COMPONENT_NAME, (void*)&ics_msg->metadata, sizeof(FrameMetadata));
    memset(&ics_msg->metadata, 0, sizeof(FrameMetadata));

    /* Basic metadata - will be enhanced with full frame parsing */
    ics_msg->metadata.ethertype = 0x0800;  /* IPv4 */
    ics_msg->metadata.ip_protocol = 6;     /* TCP */
    ics_msg->metadata.is_ip = 1;
    ics_msg->metadata.is_tcp = 1;

    /* Extract IP addresses from lwIP pcb (network byte order -> host byte order) */
    ics_msg->metadata.src_ip = ntohl(ip4_addr_get_u32(&pcb->remote_ip));  /* PLC IP (192.168.95.2) */

    /* CRITICAL: Look up original SCADA IP from connection tracking table
     * pcb->local_ip is the gateway IP (192.168.95.1)
     * We need the ORIGINAL SCADA IP (e.g., 192.168.90.5) for Net0 to send response to */
    struct connection_metadata *meta = connection_lookup_by_pcb(pcb);
    if (meta != NULL && meta->active) {
        /* Use original SCADA IP from request metadata */
        ics_msg->metadata.dst_ip = meta->original_src_ip;  /* Original SCADA IP */
        ics_msg->metadata.dst_port = meta->src_port;       /* Original SCADA port */
        #if DEBUG_METADATA
        DEBUG("%s: [FIND] Lookup: Found metadata - using original SCADA IP %u.%u.%u.%u:%u\n",
               COMPONENT_NAME,
               (meta->original_src_ip >> 24) & 0xFF,
               (meta->original_src_ip >> 16) & 0xFF,
               (meta->original_src_ip >> 8) & 0xFF,
               meta->original_src_ip & 0xFF,
               meta->src_port);
        #endif
    } else {
        /* Fallback: use local IP if lookup fails (will cause Net0 to fail lookup) */
        ics_msg->metadata.dst_ip = ntohl(ip4_addr_get_u32(&pcb->local_ip));
        ics_msg->metadata.dst_port = pcb->local_port;
        DEBUG_WARN("%s: [WARN]  Lookup: No metadata found - using gateway IP (WRONG - Net0 won't find connection!)\n", COMPONENT_NAME);
    }

    ics_msg->metadata.src_port = pcb->remote_port;
    ics_msg->metadata.payload_offset = 0;  /* TCP payload directly */
    ics_msg->metadata.payload_length = (p->len < MAX_PAYLOAD_SIZE) ? p->len : MAX_PAYLOAD_SIZE;

    /* Step 3: Copy TCP payload */
    ics_msg->payload_length = ics_msg->metadata.payload_length;
    memcpy(ics_msg->payload, p->payload, ics_msg->payload_length);

    /* v2.250: Minimal packet flow logging */
    DEBUG_INFO("[N1→ICS] session=%u, %u bytes from PLC\n",
               ics_msg->metadata.session_id, ics_msg->payload_length);

    /* Always show RAW payload for debugging */
    DEBUG("%s: RAW PAYLOAD (%u bytes): \"", COMPONENT_NAME, ics_msg->payload_length);
    for (uint16_t i = 0; i < ics_msg->payload_length && i < 200; i++) {
        char c = ics_msg->payload[i];
        if (c >= 32 && c <= 126) DEBUG("%c", c);
        else if (c == '\n') DEBUG("\\n");
        else if (c == '\r') DEBUG("\\r");
        else if (c == '\t') DEBUG("\\t");
        else DEBUG("[0x%02x]", (unsigned char)c);
    }
    if (ics_msg->payload_length > 200) DEBUG("... (%u more bytes)", ics_msg->payload_length - 200);
    DEBUG("\"\n");

    #if DEBUG_ENABLED_DEBUG
    DEBUG("   [OK] ICS message prepared in shared memory (outbound_dp)\n");
    DEBUG("   Action: Signaling ICS_Outbound component via outbound_ready_emit()\n");
    #endif

    /* Step 4: Signal ICS_Outbound that PLC response is ready */
    outbound_ready_emit();

    #if DEBUG_ENABLED_DEBUG
    DEBUG("   [OK] Signal sent to ICS_Outbound - PLC response handoff complete\n");
    /* DEBUG("   [MSG #%u now in OUTBOUND pipeline - forwarding to Net0]\n\n", msg_id); */ /* msg_id undefined */
    #endif

    tcp_recved(pcb, p->len);  /* Tell lwIP we consumed the data */

    /* Update last_activity timestamp for idle timeout detection */
    /* Reuse 'meta' variable from above (line 1924) */
    if (meta != NULL && meta->active) {
        meta->last_activity = sys_now();  /* lwIP millisecond timer */
    }

    /* DO NOT close connection - keep alive for next request from same SCADA connection */
    /* DO NOT decrement active_connections - connection still active */
    /* DO NOT remove metadata - needed for connection reuse validation */

    /* Normal path - data processed successfully */
    result = ERR_OK;
    /* Fall through to cleanup */

cleanup:
    if (result == ERR_OK && p != NULL) {
        pbuf_free(p);
    }

    return result;
}

static err_t tcp_echo_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    DEBUG("%s: [EVENT] tcp_echo_accept() CALLED! arg=%p, newpcb=%p, err=%d\n",
           COMPONENT_NAME, arg, newpcb, err);

    if (err != ERR_OK || newpcb == NULL) {
        DEBUG_ERROR("%s: [ERR] TCP accept FAILED - err=%d (%s), newpcb=%p\n",
               COMPONENT_NAME, err,
               err == -1 ? "OUT OF MEMORY (ERR_MEM)" :
               err == -13 ? "CONNECTION ABORTED (ERR_ABRT)" : "UNKNOWN",
               newpcb);
        if (err == -1) {
            DEBUG("%s:    → lwIP ran out of TCP PCBs! Check MEMP_NUM_TCP_PCB in lwipopts.h\n",
                   COMPONENT_NAME);
            DEBUG("%s:    → Current active connections: %u\n", COMPONENT_NAME, active_connections);
        }
        return err != ERR_OK ? err : ERR_VAL;
    }

    uint16_t local_port = newpcb->local_port;
    uint16_t remote_port = newpcb->remote_port;

    if (local_port != TCP_SERVER_PORT && remote_port != TCP_SERVER_PORT) {
        DEBUG("%s: [REJECT-TCP] Non-Modbus connection from %u.%u.%u.%u:%u -> local:%u (aborting)\n",
               COMPONENT_NAME,
               ip4_addr1(&newpcb->remote_ip), ip4_addr2(&newpcb->remote_ip),
               ip4_addr3(&newpcb->remote_ip), ip4_addr4(&newpcb->remote_ip),
               remote_port, local_port);
        tcp_abort(newpcb);  /* Tell lwIP to abort and clean up */
        return ERR_ABRT;
    }

    active_connections++;
    total_connections_created++;

    DEBUG_INFO("%s: [OK] TCP connection accepted from %u.%u.%u.%u:%u (pcb=%p)\n",
           COMPONENT_NAME,
           ip4_addr1(&newpcb->remote_ip), ip4_addr2(&newpcb->remote_ip),
           ip4_addr3(&newpcb->remote_ip), ip4_addr4(&newpcb->remote_ip), newpcb->remote_port,
           newpcb);
    DEBUG("%s:    → Active connections: %u | Total created: %u | Total closed: %u\n",
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


    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);

#if DEBUG_ENABLED_DEBUG
    DEBUG("%s: [DEBUG] tcp_new_ip_type() returned: pcb=%p\n", COMPONENT_NAME, (void*)pcb);
    fflush(stdout);
#endif

    if (pcb == NULL) {
        DEBUG_ERROR("%s: [ERR] Failed to create TCP PCB\n", COMPONENT_NAME);
#if DEBUG_ENABLED_DEBUG
        DEBUG("%s: [DEBUG] TCP PCB creation returned NULL - malloc likely failed\n", COMPONENT_NAME);
        DEBUG("%s: [DEBUG] This suggests lwIP memory allocator is not ready\n", COMPONENT_NAME);
        fflush(stdout);
#endif
        return;
    }

#if DEBUG_ENABLED_DEBUG
    DEBUG("%s: [DEBUG] [OK] TCP PCB created successfully at %p\n", COMPONENT_NAME, (void*)pcb);
    DEBUG("%s: [DEBUG] About to call tcp_bind(pcb, IP_ADDR_ANY, %d)...\n", COMPONENT_NAME, TCP_ECHO_PORT);
    fflush(stdout);
#endif

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, TCP_ECHO_PORT);

#if DEBUG_ENABLED_DEBUG
    DEBUG("%s: [DEBUG] tcp_bind() returned: err=%d (%s)\n", COMPONENT_NAME,
           err, err == ERR_OK ? "ERR_OK" : "ERROR");
    fflush(stdout);
#endif

    if (err != ERR_OK) {
        DEBUG_ERROR("%s: [ERR] Failed to bind TCP port %d (err=%d)\n", COMPONENT_NAME, TCP_ECHO_PORT, err);
        return;
    }

#if DEBUG_ENABLED_DEBUG
    DEBUG("%s: [DEBUG] [OK] Successfully bound to port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
    DEBUG("%s: [DEBUG] About to call tcp_listen_with_backlog(pcb, %d)...\n", COMPONENT_NAME, MAX_TCP_CONNECTIONS);
    fflush(stdout);
#endif

    pcb = tcp_listen_with_backlog(pcb, MAX_TCP_CONNECTIONS);

#if DEBUG_ENABLED_DEBUG
    DEBUG("%s: [DEBUG] tcp_listen_with_backlog() returned: pcb=%p\n", COMPONENT_NAME, (void*)pcb);
    fflush(stdout);
#endif

    if (pcb == NULL) {
        DEBUG_ERROR("%s: [ERR] Failed to listen on TCP port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
        return;
    }

#if DEBUG_ENABLED_DEBUG
    DEBUG("%s: [DEBUG] [OK] Now listening on port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
    DEBUG("%s: [DEBUG] About to call tcp_accept(pcb, tcp_echo_accept)...\n", COMPONENT_NAME);
    fflush(stdout);
#endif

    tcp_accept(pcb, tcp_echo_accept);

#if DEBUG_ENABLED_DEBUG
    DEBUG("%s: [DEBUG] [OK] Accept callback registered\n", COMPONENT_NAME);
    DEBUG("%s: [DEBUG] Exiting setup_tcp_echo_server() - SUCCESS\n", COMPONENT_NAME);
    fflush(stdout);
#endif

    /* CRITICAL DEBUG: Print actual PCB local_ip to diagnose TCP matching */
    struct tcp_pcb_listen *lpcb = (struct tcp_pcb_listen *)pcb;
    DEBUG("\n");
    DEBUG("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    DEBUG_INFO("%s: [OK] TCP SERVER CONFIGURATION\n", COMPONENT_NAME);
    DEBUG("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    DEBUG("%s: Port:           %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
    DEBUG("%s: PCB local_ip:   %u.%u.%u.%u\n", COMPONENT_NAME,
           ip4_addr1(&lpcb->local_ip), ip4_addr2(&lpcb->local_ip),
           ip4_addr3(&lpcb->local_ip), ip4_addr4(&lpcb->local_ip));

    /* Validate PCB binding */
    bool is_wildcard = (ip4_addr1(&lpcb->local_ip) == 0 &&
                        ip4_addr2(&lpcb->local_ip) == 0 &&
                        ip4_addr3(&lpcb->local_ip) == 0 &&
                        ip4_addr4(&lpcb->local_ip) == 0);

    if (is_wildcard) {
        DEBUG_INFO("%s: Status:         [OK] WILDCARD (0.0.0.0) - accepts ANY destination IP\n", COMPONENT_NAME);
        DEBUG("%s: Will accept:    Packets to 10.2.0.2, 192.168.95.2, or any IP\n", COMPONENT_NAME);
    } else {
        DEBUG_WARN("%s: Status:         [WARN]  SPECIFIC IP - only accepts packets to this IP\n", COMPONENT_NAME);
        DEBUG("%s: Will accept:    Packets to %u.%u.%u.%u ONLY\n", COMPONENT_NAME,
               ip4_addr1(&lpcb->local_ip), ip4_addr2(&lpcb->local_ip),
               ip4_addr3(&lpcb->local_ip), ip4_addr4(&lpcb->local_ip));
        DEBUG("%s: Will REJECT:    Packets to 192.168.95.2 (if not matching above)\n", COMPONENT_NAME);
    }
    DEBUG("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    DEBUG("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * OUTBOUND PATH: ICS_Outbound → Internal Network (TCP Client)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* TCP client connection state for INBOUND forwarding */
struct tcp_inbound_client_state {
    struct tcp_pcb *pcb;
    uint8_t payload_data[MAX_PAYLOAD_SIZE];  
    uint16_t payload_len;
    uint16_t bytes_sent;
    bool active;
};

#define MAX_INBOUND_CONNECTIONS 1000 
static struct tcp_inbound_client_state inbound_connection_pool[MAX_INBOUND_CONNECTIONS];

/* Allocate a free connection state from the pool */
static struct tcp_inbound_client_state* inbound_alloc_state(void)
{
    for (int i = 0; i < MAX_INBOUND_CONNECTIONS; i++) {
        if (!inbound_connection_pool[i].active) {
            memset(&inbound_connection_pool[i], 0, sizeof(struct tcp_inbound_client_state));
            inbound_connection_pool[i].active = true;
            BREADCRUMB(2101);  /* Pool slot allocated successfully */
            return &inbound_connection_pool[i];
        }
    }

    BREADCRUMB(2102);  /* Pool exhausted */
    DEBUG("%s: [CRITICAL] Inbound connection pool exhausted! (max=%d)\n",
           COMPONENT_NAME, MAX_INBOUND_CONNECTIONS);
    DEBUG("%s: [DEBUG] Pool status (first 10 active slots only):\n", COMPONENT_NAME);
    int printed = 0;
    for (int i = 0; i < MAX_INBOUND_CONNECTIONS && printed < 10; i++) {
        if (inbound_connection_pool[i].active) {
            DEBUG("%s:   Slot %d: active=%d pcb=%p len=%u sent=%u\n",
                   COMPONENT_NAME, i,
                   inbound_connection_pool[i].active,
                   (void*)inbound_connection_pool[i].pcb,
                   inbound_connection_pool[i].payload_len,
                   inbound_connection_pool[i].bytes_sent);
            printed++;
        }
    }
    if (printed < MAX_INBOUND_CONNECTIONS) {
        DEBUG("%s:   ... (%d more active connections not shown)\n",
               COMPONENT_NAME, MAX_INBOUND_CONNECTIONS - printed);
    }

    return NULL;
}

/* Free a connection state back to the pool */
static void inbound_free_state(struct tcp_inbound_client_state *state)
{
    BREADCRUMB(2103);  /* Pool free called */
    if (state != NULL) {
        state->active = false;
        state->pcb = NULL;
        state->payload_len = 0;
        state->bytes_sent = 0;
        BREADCRUMB(2104);  /* Pool free completed */
    }
}

/*
 * TCP client callbacks for INBOUND path
 */

/**
 * Receive callback for INBOUND TCP client (receives PLC responses)
 */
static volatile int inbound_recv_callback_depth = 0;  /* v2.117: Track re-entrancy */
static err_t inbound_tcp_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    BREADCRUMB(1000);  /* Entry: PLC response received */
    inbound_recv_callback_depth++;
    if (inbound_recv_callback_depth > 1) {
        DEBUG("%s: [CRITICAL] RECV CALLBACK RE-ENTRANCY DETECTED! depth=%d, p=%p, err=%d\n",
               COMPONENT_NAME, inbound_recv_callback_depth, (void*)p, err);
    }
    DEBUG("%s: [EVENT] inbound_tcp_recv_callback FIRED! depth=%d, p=%p, err=%d\n",
           COMPONENT_NAME, inbound_recv_callback_depth, (void*)p, err);
    struct tcp_inbound_client_state *state = (struct tcp_inbound_client_state *)arg;

    if (p == NULL) {
     
        /* Find metadata to check if connection was already closed */
        struct connection_metadata *meta = NULL;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connection_table[i].active && connection_table[i].pcb == pcb) {
                meta = &connection_table[i];
                break;
            }
        }

        if (meta == NULL) {
            /* No metadata found - connection was already cleaned up */
            DEBUG_WARN("%s: [WARN]  PLC closed connection but no metadata found\n",
                   COMPONENT_NAME);
            DEBUG("%s:          Connection was likely already closed via notification\n",
                   COMPONENT_NAME);
            BREADCRUMB(1002);  /* Before decrement */
            inbound_recv_callback_depth--;
            BREADCRUMB(1003);  /* Before return */
            return ERR_OK;
        }

        DEBUG_WARN("%s: [WARN]  PLC closed connection - keeping metadata for FIN-ACK TX\n",
               COMPONENT_NAME);

        /* Check if PCB is still valid (not NULL) */
        if (pcb == NULL) {
            DEBUG_WARN("%s: [WARN]  recv(p=NULL) called with NULL pcb - already freed by lwIP\n",
                   COMPONENT_NAME);
            inbound_recv_callback_depth--;
            return ERR_OK;
        }

        DEBUG("%s: [INFO]  PLC closed connection - returning ERR_ABRT (lwIP will handle abort)\n",
               COMPONENT_NAME);

        if (meta != NULL) {
            meta->metadata_close_pending = true;
            meta->close_timestamp = sys_now();

            /* v2.210: Log when delayed cleanup flag is set */
            DEBUG_INFO("[CLOSE-SOURCE-2] inbound_tcp_recv_callback(p=NULL) - PLC FIN session=%u, lwip_port=%u | PBUF: %u/%u\n",
                   meta->session_id, meta->lwip_ephemeral_port,
                   lwip_stats.memp[MEMP_PBUF_POOL]->used,
                   lwip_stats.memp[MEMP_PBUF_POOL]->avail);

            /* DON'T decrement connection_count here - check_pending_cleanups() will handle it */
            /* DON'T set active=false - lwIP needs metadata for FIN-ACK transmission */
            enqueue_cleanup(meta->session_id);

            update_shared_connection_state();
            __sync_synchronize();  /* Ensure metadata update visible */
        }

        inbound_recv_callback_depth--;
        return ERR_ABRT;  /* ✅ lwIP calls tcp_abort() internally */
    }

    err_t result = ERR_OK;  /* Default return value */

    if (err != ERR_OK) {
        BREADCRUMB(1002);  /* Error in receive */
        result = err;
        goto cleanup;  /* lwIP will handle pbuf */
    }

    if (outbound_dp == NULL) {
        BREADCRUMB(1003);  /* NULL dataport */
        result = ERR_MEM;  /* v2.240: lwIP will store in refused_data, don't free */
        goto cleanup;
    }

    BREADCRUMB(1004);  /* Preparing ICS message */
    ICS_Message *ics_msg = (ICS_Message *)outbound_dp;
    memset(&ics_msg->metadata, 0, sizeof(FrameMetadata));

    ics_msg->metadata.ethertype = 0x0800;
    ics_msg->metadata.ip_protocol = 6;
    ics_msg->metadata.is_ip = 1;
    ics_msg->metadata.is_tcp = 1;
    ics_msg->metadata.src_ip = ntohl(ip4_addr_get_u32(&pcb->remote_ip));

    BREADCRUMB(1005);  /* Looking up metadata */
    /* Look up original SCADA IP */
    struct connection_metadata *meta = connection_lookup_by_pcb(pcb);
    if (meta != NULL && meta->active) {
        BREADCRUMB(1006);  /* Metadata found */
        ics_msg->metadata.dst_ip = meta->original_src_ip;
        ics_msg->metadata.dst_port = meta->src_port;
        ics_msg->metadata.session_id = meta->session_id;  /* v2.251: Fix missing session_id in response */
    } else {
        BREADCRUMB(1007);  /* Metadata NOT found */
        ics_msg->metadata.dst_ip = ntohl(ip4_addr_get_u32(&pcb->local_ip));
        ics_msg->metadata.dst_port = pcb->local_port;
    }

    ics_msg->metadata.src_port = pcb->remote_port;
    ics_msg->metadata.payload_offset = 0;
    ics_msg->metadata.payload_length = (p->len < MAX_PAYLOAD_SIZE) ? p->len : MAX_PAYLOAD_SIZE;

    BREADCRUMB(1008);  /* Copying payload */
    ics_msg->payload_length = ics_msg->metadata.payload_length;
    memcpy(ics_msg->payload, p->payload, ics_msg->payload_length);

    outbound_ready_emit();

    tcp_recved(pcb, p->len);

    /* Connection stays open - will be cleaned up by:
     * 1. Remote close (SCADA or PLC closes)
     * 2. Next request arrives (B2006 cleanup with tcp_abort)
     * 3. lwIP TCP timeout (if connection dies) */

    /* Normal path - data processed successfully */
    result = ERR_OK;
    /* Fall through to cleanup */

cleanup:
    if (result == ERR_OK && p != NULL) {
        pbuf_free(p);
    }

    BREADCRUMB(1012);  /* Before decrement */
    inbound_recv_callback_depth--;  /* v2.117: Decrement re-entrancy counter */
    BREADCRUMB(1013);  /* Before return - control goes back to lwIP */
    DEBUG("%s: [DEBUG] About to return ERR_OK to lwIP from recv callback\n", COMPONENT_NAME);
    fflush(stdout);  /* Force output before return */
    return result;
}

static err_t inbound_tcp_sent_callback(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    struct tcp_inbound_client_state *state = (struct tcp_inbound_client_state *)arg;

    #if DEBUG_TRAFFIC
    DEBUG("%s: INBOUND: Sent %u bytes to internal network\n", COMPONENT_NAME, len);
    #endif

    state->bytes_sent += len;

    /* Check if all data sent */
    if (state->bytes_sent >= state->payload_len) {
        #if DEBUG_TRAFFIC
        DEBUG("%s: INBOUND: Complete - sent %u/%u bytes, waiting for PLC response\n",
               COMPONENT_NAME, state->bytes_sent, state->payload_len);
        #endif
        /* Do NOT close - keep connection open to receive PLC response */
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
            DEBUG("%s: INBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        }
    }

    return ERR_OK;
}

static void inbound_tcp_err_callback(void *arg, err_t err)
{
    struct tcp_inbound_client_state *state = (struct tcp_inbound_client_state *)arg;

    DEBUG_WARN("%s: [WARN]  INBOUND TCP error - err=%d (%s)\n", COMPONENT_NAME, err,
           err == ERR_ABRT ? "ERR_ABRT (Connection aborted)" :
           err == ERR_RST ? "ERR_RST (Connection reset)" :
           err == ERR_CLSD ? "ERR_CLSD (Connection closed)" :
           err == ERR_CONN ? "ERR_CONN (Not connected)" :
           err == ERR_TIMEOUT ? "ERR_TIMEOUT (Timeout)" : "Unknown");

    if (state != NULL) {
        /* Find metadata for this connection */
        struct connection_metadata *meta = NULL;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connection_table[i].active && connection_table[i].pool_state == state) {
                meta = &connection_table[i];
                break;
            }
        }

        if (err != ERR_ABRT && meta != NULL && outbound_dp != NULL) {
            /* DEDUPLICATION: Check if already notified (RST flood protection) */
            if (!meta->error_notified) {
                OutboundDataport *dp = (OutboundDataport *)outbound_dp;

                /* Enqueue error notification */
                bool success = control_queue_enqueue(
                    &dp->error_queue,
                    meta->session_id,
                    (int8_t)err,
                    0  /* flags - future use */
                );

                if (success) {
                    meta->error_notified = true;  /* Set dedup flag */

                    dp->response_msg.payload_length = 0;  /* Sentinel: error-only, no payload */
                    dp->response_msg.metadata.payload_length = 0;  /* v2.254: Must match payload_length! */
                    dp->response_msg.metadata.session_id = meta->session_id;
                    __sync_synchronize();  /* Memory barrier - ensure sentinel visible before signal */

                    outbound_ready_emit();        /* Signal Net0 */

                    DEBUG("%s: Enqueued error notification to Net0 "
                           "(session %u, err=%d)\n",
                           COMPONENT_NAME, meta->session_id, err);
                } else {
                    DEBUG("%s: [ERROR] Failed to enqueue error notification "
                           "(queue full? session %u)\n",
                           COMPONENT_NAME, meta->session_id);
                }
            } else {
                DEBUG("%s: [DEDUP] Ignoring duplicate error for session %u "
                       "(RST flood protection)\n",
                       COMPONENT_NAME, meta->session_id);
            }
        }

        if (meta != NULL) {
            /* Only decrement if connection is still active
             * - Close handler sets active=false BEFORE calling tcp_abort() → skip
             * - recv(p=NULL) sets active=false but doesn't decrement → PROBLEM!
             *
             * Wait, both scenarios set active=false. How do we distinguish?
             * Answer: We can't! The real fix is to make recv callback decrement.
             *
             * But for backward compatibility, let's keep this logic:
             * - If active is true: definitely need to decrement
             * - If active is false: might have been decremented already (close handler)
             *                       or might not have been (recv callback) ← ambiguous!
             *
             * Actually, looking at the code again:
             * - Close handler (line 3399): decrements BEFORE setting active=false
             * - recv callback (line 2728): sets active=false but does NOT decrement
             *
             * So when error callback sees meta->active=false, it could be:
             * 1. Close handler: count already decremented
             * 2. recv callback: count NOT decremented yet
             *
             * We need a way to tell them apart. But wait - close handler NULLs the
             * error callback (line 3386)! So if this callback fires, it CAN'T be
             * from close handler path!
             *
             * That means: if we're in this callback, it's NEVER from close handler.
             * So we should ALWAYS clean up metadata!
             */
            meta->metadata_close_pending = true;  /* Intent flag: cleanup needed */
            meta->close_timestamp = sys_now();     /* For deferred cleanup timeout */
            meta->pcb_closed = true;               /* PCB already freed by lwIP */

            DEBUG_INFO("[CLOSE-SOURCE-3] inbound_tcp_err_callback err=%d - session=%u | PBUF=%u/%u\n",
                   err, meta->session_id,
                   lwip_stats.memp[MEMP_PBUF_POOL]->used,
                   lwip_stats.memp[MEMP_PBUF_POOL]->avail);
            /* DON'T clear error_notified - keeps deduplication active for retransmitted RSTs */

            update_shared_connection_state();

            enqueue_cleanup(meta->session_id);
        }

        inbound_free_state(state);  /* v2.106: Free connection pool slot (always needed) */
    }

    DEBUG("%s: [CLEAN] INBOUND connection error triggered - %s\n", COMPONENT_NAME,
           err == ERR_ABRT ? "pool freed (metadata already cleaned by close handler)" :
           "state and metadata cleaned up");
}

static err_t inbound_tcp_connected_callback(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct tcp_inbound_client_state *state = (struct tcp_inbound_client_state *)arg;

    if (err != ERR_OK) {
        DEBUG_ERROR("%s: [ERR] INBOUND: Connection failed (3-way handshake): err=%d\n", COMPONENT_NAME, err);

        /* Find the connection metadata */
        struct connection_metadata *meta = NULL;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connection_table[i].active && connection_table[i].pcb == pcb) {
                meta = &connection_table[i];
                break;
            }
        }

        if (outbound_dp != NULL && meta != NULL && !meta->error_notified) {
            OutboundDataport *dp = (OutboundDataport *)outbound_dp;

            /* Enqueue error notification */
            bool success = control_queue_enqueue(
                &dp->error_queue,
                meta->session_id,
                (int8_t)err,
                0  /* flags */
            );

            if (success) {
                meta->error_notified = true;

                dp->response_msg.payload_length = 0;  /* Sentinel: error-only, no payload */
                dp->response_msg.metadata.payload_length = 0;  /* v2.254: Must match payload_length! */
                dp->response_msg.metadata.session_id = meta->session_id;
                __sync_synchronize();  /* Memory barrier - ensure sentinel visible before signal */

                outbound_ready_emit();

                DEBUG("%s: Enqueued connection failure notification to Net0 "
                       "(session %u, err=%d)\n",
                       COMPONENT_NAME, meta->session_id, err);
            } else {
                DEBUG("%s: [ERROR] Failed to enqueue connection failure notification "
                       "(session %u)\n", COMPONENT_NAME, meta->session_id);
            }
        }
    
        if (meta != NULL) {
            meta->metadata_close_pending = true;  /* Intent: cleanup needed */
            meta->close_timestamp = sys_now();     /* For timeout tracking */
            meta->pcb_closed = true;               /* Handshake failed, PCB invalid */

            DEBUG_INFO("[CLOSE-SOURCE-4] inbound_connected_callback failed - session=%u | PBUF=%u/%u\n",
                   meta->session_id,
                   lwip_stats.memp[MEMP_PBUF_POOL]->used,
                   lwip_stats.memp[MEMP_PBUF_POOL]->avail);

            /* Enqueue for centralized cleanup */
            enqueue_cleanup(meta->session_id);

            DEBUG("%s: [ENQUEUE] Handshake failed (err=%d), enqueued cleanup for session %u\n",
                   COMPONENT_NAME, err, meta->session_id);

            update_shared_connection_state();
        }

        inbound_free_state(state);  /* v2.106: Free connection pool slot */
        return err;
    }

    #if DEBUG_TRAFFIC
    DEBUG("%s: INBOUND: Connected to internal network\n", COMPONENT_NAME);
    #endif

    DEBUG("%s: [LINK] TCP connection ESTABLISHED to PLC - registering callbacks\n", COMPONENT_NAME);

    /* Set callbacks */
    tcp_recv(pcb, inbound_tcp_recv_callback);
    tcp_sent(pcb, inbound_tcp_sent_callback);

    DEBUG("%s: [OK] Callbacks registered: recv=%p, sent=%p\n", COMPONENT_NAME, (void*)inbound_tcp_recv_callback, (void*)inbound_tcp_sent_callback);

    /* Send the payload */
    uint16_t to_send = (state->payload_len > tcp_sndbuf(pcb)) ? tcp_sndbuf(pcb) : state->payload_len;

    if (to_send == 0) {
        /* Send buffer full - defer transmission until sent callback */
        DEBUG_WARN("%s: [WARN]  Send buffer full (sndbuf=%u), deferring transmission of %u bytes\n",
               COMPONENT_NAME, tcp_sndbuf(pcb), state->payload_len);
        DEBUG("%s:    → Will retry in tcp_sent callback when buffer available\n", COMPONENT_NAME);

        /* Keep state active - sent callback will retry when buffer space available
         * This is safe because:
         * 1. Connection is established (callbacks registered)
         * 2. State is marked active with pending data
         * 3. lwIP will call sent callback when ACKs arrive and free buffer space
         * 4. sent callback checks state->bytes_sent < state->payload_len and retries
         */
        state->bytes_sent = 0;
        return ERR_OK;
    }

    err = tcp_write(pcb, state->payload_data, to_send, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        DEBUG("%s: INBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        inbound_free_state(state); 
        return ERR_ABRT;
    }

    state->bytes_sent = to_send;

    /* Trigger transmission - safe now because we know data was queued */
    tcp_output(pcb);

    /* v2.250: Minimal packet flow logging - sent to PLC */
    {
        struct connection_metadata *tx_meta = connection_lookup_by_pcb(pcb);
        if (tx_meta != NULL) {
            DEBUG_INFO("[N1-TX] session=%u, %u bytes → PLC\n",
                       tx_meta->session_id, to_send);
        }
    }

    return ERR_OK;
}

/* TCP client connection state for OUTBOUND forwarding */
struct tcp_outbound_client_state {
    struct tcp_pcb *pcb;
    uint8_t payload_data[MAX_PAYLOAD_SIZE]; 
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

    DEBUG("%s: OUTBOUND: Sent %u bytes to external network\n", COMPONENT_NAME, len);

    state->bytes_sent += len;

    /* Check if all data sent */
    if (state->bytes_sent >= state->payload_len) {
        DEBUG("%s: OUTBOUND: Complete - sent %u/%u bytes\n",
               COMPONENT_NAME, state->bytes_sent, state->payload_len);

        struct connection_metadata *meta = connection_lookup_by_pcb(pcb);
        if (meta != NULL) {
            meta->metadata_close_pending = true;
            meta->close_timestamp = sys_now();
            meta->pcb_closed = true;
            enqueue_cleanup(meta->session_id);
            DEBUG_INFO("[CLOSE-SOURCE-5] outbound_tcp_sent_callback close - session=%u | PBUF=%u/%u\n",
                   meta->session_id,
                   lwip_stats.memp[MEMP_PBUF_POOL]->used,
                   lwip_stats.memp[MEMP_PBUF_POOL]->avail);
        }
        state->active = false;  /* v2.106: Mark outbound client as free */

        return ERR_ABRT;  /* Let lwIP handle tcp_abort() internally */
    }

    /* Send remaining data if needed */
    uint16_t remaining = state->payload_len - state->bytes_sent;
    uint16_t to_send = (remaining > tcp_sndbuf(pcb)) ? tcp_sndbuf(pcb) : remaining;

    if (to_send > 0) {
        err_t err = tcp_write(pcb, state->payload_data + state->bytes_sent, to_send, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK) {
            tcp_output(pcb);
        } else {
            DEBUG("%s: OUTBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        }
    }

    return ERR_OK;
}

static err_t outbound_tcp_connected_callback(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct tcp_outbound_client_state *state = (struct tcp_outbound_client_state *)arg;

    if (err != ERR_OK) {
        DEBUG("%s: OUTBOUND: Connection failed: %d\n", COMPONENT_NAME, err);
        state->active = false;
        state->pcb = NULL;
        return err;
    }

    DEBUG("%s: OUTBOUND: Connected to external network\n", COMPONENT_NAME);

    /* Set sent callback */
    tcp_sent(pcb, outbound_tcp_sent_callback);

    /* Send the payload */
    uint16_t to_send = (state->payload_len > tcp_sndbuf(pcb)) ? tcp_sndbuf(pcb) : state->payload_len;

    if (to_send == 0) {
        /* Send buffer full - defer transmission until sent callback */
        DEBUG_WARN("%s: [WARN]  OUTBOUND: Send buffer full (sndbuf=%u), deferring transmission of %u bytes\n",
               COMPONENT_NAME, tcp_sndbuf(pcb), state->payload_len);
        DEBUG("%s:    → Will retry in tcp_sent callback when buffer available\n", COMPONENT_NAME);

        state->bytes_sent = 0;
        return ERR_OK;
    }

    err = tcp_write(pcb, state->payload_data, to_send, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        DEBUG("%s: OUTBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        state->active = false;
        state->pcb = NULL;
        return ERR_ABRT;
    }

    state->bytes_sent = to_send;

    /* Trigger transmission */
    tcp_output(pcb);

    DEBUG("%s: OUTBOUND: Sent initial %u bytes\n", COMPONENT_NAME, to_send);

    return ERR_OK;
}

/*
 * INBOUND notification handler - called when ICS_Inbound has validated data
 * Creates TCP client connection to forward data to internal network (PLC)
 */
void inbound_ready_handle(void)
{
    BREADCRUMB(2000);  /* Entry: ICS_Inbound notification received */


    /* v2.250: Minimal packet flow logging - box drawing removed */

    BREADCRUMB(2001);  /* Checking dataport */

    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (inbound_dp == NULL) {
        BREADCRUMB(2002);  /* NULL dataport */
        DEBUG_ERROR("%s: [ERR] FATAL: inbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        DEBUG("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        return;
    }


    InboundDataport *dp = (InboundDataport *)inbound_dp;
    static uint32_t close_queue_tail = 0;  /* Consumer state (local, never shared) */

    uint32_t close_queue_head = dp->close_queue.head;

    __sync_synchronize();  /* Force cache invalidation - read fresh value from Net0 */

    /* Check for queue overflow */
    if (close_queue_head - close_queue_tail > CONTROL_QUEUE_SIZE) {
        DEBUG_WARN("%s: [WARN] Close queue overflow! Missed %u notifications\n",
               COMPONENT_NAME, close_queue_head - close_queue_tail - CONTROL_QUEUE_SIZE);
        close_queue_tail = close_queue_head - CONTROL_QUEUE_SIZE;
    }

    /* Process all queued close notifications */
    while (close_queue_tail < close_queue_head) {
        uint32_t slot = close_queue_tail & CONTROL_QUEUE_MASK;
        volatile struct control_notification *notif = &dp->close_queue.notifications[slot];

        /* Verify sequence */
        if (notif->seq_num == close_queue_tail && notif->session_id != 0) {
            DEBUG("%s: Processing close notification: session %u\n",
                   COMPONENT_NAME, notif->session_id);

            /* Lookup connection by session_id */
            struct connection_metadata *meta = connection_lookup_by_session_id(notif->session_id);

            if (meta != NULL && meta->pcb != NULL && !meta->pcb_closed) {
                struct tcp_pcb *pcb = meta->pcb;

                DEBUG("%s:   → Closing PLC connection (session %u, PCB=%p)\n",
                       COMPONENT_NAME, notif->session_id, (void*)pcb);

                /* Step 1: NULL all callbacks to prevent them firing */
                tcp_arg(pcb, NULL);
                tcp_recv(pcb, NULL);
                tcp_sent(pcb, NULL);
                tcp_err(pcb, NULL);   /* Prevent error callback during close */
                tcp_poll(pcb, NULL, 0);

                /* v2.241: CRITICAL FIX - Clear PCB pointer BEFORE closing
                 * ═══════════════════════════════════════════════════════════════
                 * This prevents race conditions if tcp_close() triggers callbacks
                 * (even though we NULLed them above, better safe than sorry)
                 */
                meta->pcb = NULL;

                /* Step 2: v2.212-phase1: Mark metadata for deferred cleanup */
                meta->metadata_close_pending = true;   /* Intent: cleanup needed */
                meta->close_timestamp = sys_now();     /* For timeout tracking */
                meta->pcb_closed = true;               /* PCB will be closed below */
                meta->error_notified = false;          /* Clear dedup flag */

                DEBUG_INFO("[CLOSE-SOURCE-6] inbound_ready_handle close notif - session=%u | PBUF=%u/%u\n",
                       meta->session_id,
                       lwip_stats.memp[MEMP_PBUF_POOL]->used,
                       lwip_stats.memp[MEMP_PBUF_POOL]->avail);

                __sync_synchronize();  /* Ensure metadata updates visible */

                /* Step 3: Symmetrical close */
                if (notif->err_code == ERR_RST || notif->err_code == ERR_ABRT) {
                    /* SCADA sent RST or Net0 forced close → Use tcp_abort() for immediate cleanup */
                    const char *reason = (notif->err_code == ERR_RST) ? "SCADA sent RST" : "Forced close (pool exhaustion)";
                    DEBUG("%s:   → %s, sending RST to PLC (tcp_abort)\n",
                           COMPONENT_NAME, reason);
                    tcp_abort(pcb);  /* Sends RST, frees PCB immediately */
                } else {
                    /* SCADA sent FIN (ERR_CLSD) or unknown → Use graceful close */
                    DEBUG("%s:   → SCADA sent FIN, sending FIN to PLC (tcp_close)\n",
                           COMPONENT_NAME);
                    err_t err = tcp_close(pcb);
                    if (err != ERR_OK) {
                        /* Graceful close failed (out of memory), force abort */
                        DEBUG("%s:   → tcp_close() failed (err=%d), forcing tcp_abort()\n",
                               COMPONENT_NAME, err);
                        tcp_abort(pcb);  /* Safe now - callbacks NULL, metadata inactive */
                    }
                }

                if (meta->pool_state != NULL) {
                    inbound_free_state(meta->pool_state);
                    meta->pool_state = NULL;
                }

                /* Update shared state */
                update_shared_connection_state();

                enqueue_cleanup(notif->session_id);

                DEBUG("%s:   ✓ PLC connection closed, cleanup enqueued (session %u)\n",
                       COMPONENT_NAME, notif->session_id);
            } else if (meta != NULL) {
                DEBUG("%s:   → PCB already closed, but metadata exists (session %u) - enqueueing cleanup\n",
                       COMPONENT_NAME, notif->session_id);
                enqueue_cleanup(notif->session_id);
            } else {
                /* meta == NULL - session not found at all, already fully cleaned */
                DEBUG("%s:   → Session %u not found (already fully cleaned)\n",
                       COMPONENT_NAME, notif->session_id);
            }
        }

        close_queue_tail++;  /* Move to next notification */
    }

    /* Now process request data (if any) */
    ICS_Message *ics_msg = &dp->request_msg;

    /* Validate message */
    if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
        BREADCRUMB(2004);  /* Invalid payload size */
        DEBUG("%s: INBOUND: Invalid payload length %u (max %u)\n",
               COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
        #if DEBUG_ENABLED_DEBUG
        /* DEBUG("   ✗ [MSG #%u] DROPPED - invalid payload size\n\n", msg_id); */ /* msg_id undefined */
        #endif
        return;
    }

    /* Skip if no request data (only close notifications were queued) */
    if (ics_msg->payload_length == 0) {
        return;
    }

    /* v2.250: Minimal packet flow logging - received from ICS_Inbound */
    DEBUG_INFO("[N1←ICS] session=%u, %u bytes from SCADA\n",
               ics_msg->metadata.session_id, ics_msg->payload_length);

    /* Look up if we already have a connection for this SCADA client (by 5-tuple) */
    struct connection_metadata *existing_meta = connection_lookup_by_tuple(
        ics_msg->metadata.src_ip,    /* SCADA IP */
        ics_msg->metadata.dst_ip,    /* PLC IP */
        ics_msg->metadata.src_port,  /* SCADA port - unique per SCADA session */
        ics_msg->metadata.dst_port   /* PLC port (502) */
    );

    if (existing_meta != NULL && existing_meta->active && existing_meta->pcb != NULL) {
        BREADCRUMB(2006);  /* Found existing connection - validating */

        struct tcp_pcb *existing_pcb = existing_meta->pcb;

        DEBUG("%s: [FIND] Found existing connection for SCADA %u.%u.%u.%u:%u (PCB=%p, state=%d)\n",
               COMPONENT_NAME,
               (ics_msg->metadata.src_ip >> 24) & 0xFF,
               (ics_msg->metadata.src_ip >> 16) & 0xFF,
               (ics_msg->metadata.src_ip >> 8) & 0xFF,
               ics_msg->metadata.src_ip & 0xFF,
               ics_msg->metadata.src_port,
               (void*)existing_pcb, existing_pcb->state);

        /* v2.260: CONNECTION REUSE - Check if connection is in ESTABLISHED state
         *
         * Root cause of connection churn:
         *   ModScan opens ONE connection, sends multiple Modbus requests on it.
         *   Old code: Created NEW backend connection for EVERY request, sending RST to abort previous.
         *   This overwhelmed the PLC's single-threaded accept loop.
         *
         * Fix: Reuse ESTABLISHED connections by sending data on existing PCB.
         *   - If PCB state is ESTABLISHED (4), send request on existing connection
         *   - If PCB state is anything else (connecting, closing, etc.), cleanup and create new
         */
        if (existing_pcb->state == ESTABLISHED) {
            BREADCRUMB(2106);  /* Reusing ESTABLISHED connection */

            DEBUG_INFO("[N1-REUSE] session=%u → Reusing ESTABLISHED connection to PLC (PCB=%p)\n",
                   ics_msg->metadata.session_id, (void*)existing_pcb);

            /* Get the existing pool state and update with new payload */
            struct tcp_inbound_client_state *state = existing_meta->pool_state;

            if (state == NULL) {
                /* Pool state was freed but connection still up - allocate new state */
                DEBUG_WARN("%s: [WARN]  Reuse: pool_state is NULL, allocating new state\n", COMPONENT_NAME);
                state = inbound_alloc_state();
                if (state == NULL) {
                    DEBUG_ERROR("%s: [ERR] Failed to allocate state for connection reuse\n", COMPONENT_NAME);
                    goto cleanup_and_create_new;
                }
                existing_meta->pool_state = state;
                state->pcb = existing_pcb;
                tcp_arg(existing_pcb, state);
            }

            /* Validate payload size */
            if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
                DEBUG_WARN("%s: [WARN]  Reuse: Payload too large (%u > %u)\n",
                       COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
                /* Don't abort connection - just drop this request */
                return;
            }

            /* Copy new payload to state */
            memcpy(state->payload_data, ics_msg->payload, ics_msg->payload_length);
            state->payload_len = ics_msg->payload_length;
            state->bytes_sent = 0;

            /* Update session_id if needed (same SCADA connection, same session) */
            existing_meta->session_id = ics_msg->metadata.session_id;
            existing_meta->last_activity = sys_now();

            /* Send the payload on existing connection */
            uint16_t to_send = (state->payload_len > tcp_sndbuf(existing_pcb))
                               ? tcp_sndbuf(existing_pcb) : state->payload_len;

            if (to_send > 0) {
                err_t write_err = tcp_write(existing_pcb, state->payload_data, to_send, TCP_WRITE_FLAG_COPY);
                if (write_err == ERR_OK) {
                    state->bytes_sent = to_send;
                    tcp_output(existing_pcb);
                    DEBUG_INFO("[N1-REUSE] Sent %u/%u bytes on existing connection\n",
                           to_send, state->payload_len);
                    return;  /* Done - request sent on reused connection */
                } else {
                    DEBUG_WARN("%s: [WARN]  tcp_write failed on reuse (err=%d), falling back to new connection\n",
                           COMPONENT_NAME, write_err);
                    /* Fall through to cleanup_and_create_new */
                }
            } else {
                DEBUG_WARN("%s: [WARN]  Send buffer full on reuse, falling back to new connection\n",
                       COMPONENT_NAME);
                /* Fall through to cleanup_and_create_new */
            }
        } else {
            DEBUG("%s:   → Connection not ESTABLISHED (state=%d), creating new\n",
                   COMPONENT_NAME, existing_pcb->state);
        }

cleanup_and_create_new:
        /* ─────────────────────────────────────────────────────────────────────
         * Connection validation failed - clean up and create new
         * ───────────────────────────────────────────────────────────────────── */
        DEBUG("%s:   [CLEAN] Cleaning up old connection (PCB=%p)\n", COMPONENT_NAME, (void*)existing_pcb);

        mark_connection_self_cleaned(
            ics_msg->metadata.src_ip,
            ics_msg->metadata.src_port,
            ics_msg->metadata.dst_port
        );

        /* v2.170: CRITICAL FIX - Follow CRITICAL_LESSON Rule 2 for event handler context */
        BREADCRUMB(2107);  /* Cleanup path - clearing callbacks */

        /* Step 1: Clear callbacks to prevent them from firing during cleanup */
        tcp_recv(existing_pcb, NULL);
        tcp_sent(existing_pcb, NULL);
        tcp_err(existing_pcb, NULL);
        tcp_arg(existing_pcb, NULL);

        /* Step 2: Use tcp_abort() 
         */
        DEBUG("%s:   [CLEAN] Sending RST to PLC for old connection (SCADA opened new one)\n",
               COMPONENT_NAME);

        /* v2.212-phase3: Use centralized cleanup instead of connection_remove() */
        struct connection_metadata *existing_meta = connection_lookup_by_pcb(existing_pcb);
        if (existing_meta != NULL) {
            existing_meta->metadata_close_pending = true;
            existing_meta->close_timestamp = sys_now();
            existing_meta->pcb_closed = true;  /* Will be closed below */

            DEBUG_INFO("[CLOSE-SOURCE-7] tcp_connect existing PCB cleanup - session=%u | PBUF=%u/%u\n",
                   existing_meta->session_id,
                   lwip_stats.memp[MEMP_PBUF_POOL]->used,
                   lwip_stats.memp[MEMP_PBUF_POOL]->avail);

            /* Free pool state before tcp_abort (it needs to be freed before PCB is freed) */
            if (existing_meta->pool_state != NULL) {
                inbound_free_state(existing_meta->pool_state);
                existing_meta->pool_state = NULL;
            }

            enqueue_cleanup(existing_meta->session_id);
            DEBUG("%s:   [CLEAN] Enqueued cleanup for old session %u\n",
                   COMPONENT_NAME, existing_meta->session_id);
        }

        tcp_abort(existing_pcb);  /* Immediate cleanup - no FIN_WAIT */

        DEBUG("%s:   [OK] Cleanup complete - proceeding to create new connection\n", COMPONENT_NAME);
    }

    BREADCRUMB(2007);  /* Creating new TCP PCB */

    #if DEBUG_ENABLED_DEBUG
    DEBUG("   Payload size: %u bytes\n", ics_msg->payload_length);
    DEBUG("   Destination: %u.%u.%u.%u:%u\n",
           (ics_msg->metadata.dst_ip >> 24) & 0xFF,
           (ics_msg->metadata.dst_ip >> 16) & 0xFF,
           (ics_msg->metadata.dst_ip >> 8) & 0xFF,
           ics_msg->metadata.dst_ip & 0xFF,
           ics_msg->metadata.dst_port);

    /* Print ASCII payload preview */
    DEBUG("   Payload preview: \"");
    for (uint16_t i = 0; i < (ics_msg->payload_length < 60 ? ics_msg->payload_length : 60); i++) {
        char c = ics_msg->payload[i];
        if (c >= 32 && c <= 126) DEBUG("%c", c);
        else if (c == '\n') DEBUG("\\n");
        else if (c == '\r') DEBUG("\\r");
        else DEBUG(".");
    }
    if (ics_msg->payload_length > 60) DEBUG("...");
    DEBUG("\"\n");
    #endif

    /* Print metadata */
    #if DEBUG_TRAFFIC
    DEBUG("%s: INBOUND: Protocol=%s, Src=%u.%u.%u.%u:%u, Dst=%u.%u.%u.%u:%u, Payload=%u bytes\n",
           COMPONENT_NAME,
           ics_msg->metadata.is_tcp ? "TCP" : (ics_msg->metadata.is_udp ? "UDP" : "Other"),
           (ics_msg->metadata.src_ip >> 24) & 0xFF, (ics_msg->metadata.src_ip >> 16) & 0xFF,
           (ics_msg->metadata.src_ip >> 8) & 0xFF, ics_msg->metadata.src_ip & 0xFF,
           ics_msg->metadata.src_port,
           (ics_msg->metadata.dst_ip >> 24) & 0xFF, (ics_msg->metadata.dst_ip >> 16) & 0xFF,
           (ics_msg->metadata.dst_ip >> 8) & 0xFF, ics_msg->metadata.dst_ip & 0xFF,
           ics_msg->metadata.dst_port,
           ics_msg->payload_length);
    #endif

    /* Create TCP client connection */
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        BREADCRUMB(2008);  /* Failed to create PCB */
        DEBUG_WARN("%s: [WARN]  INBOUND: Failed to create TCP PCB - lwIP connection pool exhausted (MEMP_NUM_TCP_PCB=%d)\n",
               COMPONENT_NAME, MEMP_NUM_TCP_PCB);
        DEBUG("%s:   → Sending ERROR notification to Net0 (lwIP connection limit reached)\n", COMPONENT_NAME);

        DEBUG("%s:\n", COMPONENT_NAME);
        DEBUG("%s: ╔══════════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
        DEBUG("%s: ║ CONNECTION LIMIT REACHED - DIAGNOSTIC ANALYSIS               ║\n", COMPONENT_NAME);
        DEBUG("%s: ╚══════════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
        DEBUG("%s:\n", COMPONENT_NAME);
        DEBUG("%s: Connection Pool Status:\n", COMPONENT_NAME);
        DEBUG("%s:   → connection_count = %u / %u (our tracking)\n",
               COMPONENT_NAME, connection_count, MAX_CONNECTIONS);
        DEBUG("%s:   → lwIP pool size = %d PCBs\n", COMPONENT_NAME, MEMP_NUM_TCP_PCB);
        DEBUG("%s:\n", COMPONENT_NAME);

        /* Analyze connection table for issues */
        int active_count = 0;
        int dangling_count = 0;  /* active=true but pcb=NULL */
        int valid_count = 0;      /* active=true and pcb!=NULL */
        int orphan_count = 0;     /* Net1 has but Net0 doesn't */

        /* Track session IDs to detect duplicates */
        uint32_t session_ids[MAX_CONNECTIONS];
        int session_count = 0;

        DEBUG("%s: Scanning %d connection slots:\n", COMPONENT_NAME, MAX_CONNECTIONS);
        DEBUG("%s:\n", COMPONENT_NAME);

        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (!connection_table[i].active) continue;

            active_count++;

            /* Check for dangling connection (active but no PCB) */
            if (connection_table[i].pcb == NULL) {
                if (dangling_count == 0) {
                    DEBUG("%s: [DANGLING] Found connections with active=true but PCB=NULL:\n",
                           COMPONENT_NAME);
                }
                DEBUG("%s:   → [slot %d] session=%u, src_port=%u, dest_port=%u, PCB=NULL ❌\n",
                       COMPONENT_NAME, i,
                       connection_table[i].session_id,
                       connection_table[i].src_port,
                       connection_table[i].dest_port);
                dangling_count++;
            } else {
                valid_count++;
            }

            /* Check for orphan connections (Net1 has but Net0 doesn't) */
            if (peer_state != NULL) {
                bool found_in_net0 = false;
                for (int j = 0; j < peer_state->count && j < 256; j++) {
                    if (peer_state->connections[j].active &&
                        peer_state->connections[j].session_id == connection_table[i].session_id) {
                        found_in_net0 = true;
                        break;
                    }
                }

                if (!found_in_net0) {
                    if (orphan_count == 0) {
                        DEBUG("%s:\n", COMPONENT_NAME);
                        DEBUG("%s: [ORPHAN] Found connections in Net1 but NOT in Net0:\n",
                               COMPONENT_NAME);
                    }
                    DEBUG("%s:   → [slot %d] session=%u, Net0 doesn't have this session ⚠️\n",
                           COMPONENT_NAME, i, connection_table[i].session_id);
                    orphan_count++;
                }
            }

            /* Track session ID for duplicate detection */
            session_ids[session_count++] = connection_table[i].session_id;
        }

        /* Check for duplicate session IDs */
        DEBUG("%s:\n", COMPONENT_NAME);
        DEBUG("%s: [DUPLICATES] Checking for duplicate session_ids:\n", COMPONENT_NAME);
        int duplicate_count = 0;
        for (int i = 0; i < session_count; i++) {
            for (int j = i + 1; j < session_count; j++) {
                if (session_ids[i] == session_ids[j]) {
                    DEBUG("%s:   → session_id %u appears at least twice! ❌\n",
                           COMPONENT_NAME, session_ids[i]);
                    duplicate_count++;
                    break;  /* Only report once per session_id */
                }
            }
        }
        if (duplicate_count == 0) {
            DEBUG("%s:   → No duplicate session_ids found ✅\n", COMPONENT_NAME);
        }

        /* Summary */
        DEBUG("%s:\n", COMPONENT_NAME);
        DEBUG("%s: ╔══════════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
        DEBUG("%s: ║ DIAGNOSTIC SUMMARY                                           ║\n", COMPONENT_NAME);
        DEBUG("%s: ╚══════════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
        DEBUG("%s:   Active connections:    %d\n", COMPONENT_NAME, active_count);
        DEBUG("%s:   Valid (with PCB):      %d\n", COMPONENT_NAME, valid_count);
        DEBUG("%s:   Dangling (PCB=NULL):   %d %s\n",
               COMPONENT_NAME, dangling_count,
               dangling_count > 0 ? "❌ LEAK!" : "✅");
        DEBUG("%s:   Orphan (not in Net0):  %d %s\n",
               COMPONENT_NAME, orphan_count,
               orphan_count > 0 ? "⚠️  Asymmetric state" : "✅");
        DEBUG("%s:   Duplicate session_ids: %d %s\n",
               COMPONENT_NAME, duplicate_count,
               duplicate_count > 0 ? "❌ BUG!" : "✅");
        DEBUG("%s:\n", COMPONENT_NAME);

        if (dangling_count > 0) {
            DEBUG("%s: ⚠️  DANGLING CONNECTIONS indicate metadata leak:\n", COMPONENT_NAME);
            DEBUG("%s:    - connection_add() created metadata\n", COMPONENT_NAME);
            DEBUG("%s:    - But PCB was freed without cleaning metadata\n", COMPONENT_NAME);
            DEBUG("%s:    - Check 3-way handshake failure path\n", COMPONENT_NAME);
            DEBUG("%s:    - Check recv(p=NULL) cleanup path\n", COMPONENT_NAME);
        }

        if (orphan_count > 0) {
            DEBUG("%s: ⚠️  ORPHAN CONNECTIONS indicate asymmetric state:\n", COMPONENT_NAME);
            DEBUG("%s:    - Net1 has connection but Net0 doesn't\n", COMPONENT_NAME);
            DEBUG("%s:    - Possible cause: Net0 already closed SCADA side\n", COMPONENT_NAME);
            DEBUG("%s:    - But Net1 never received close notification\n", COMPONENT_NAME);
        }

        if (duplicate_count > 0) {
            DEBUG("%s: ❌ DUPLICATE SESSION IDs indicate session collision:\n", COMPONENT_NAME);
            DEBUG("%s:    - Same 5-tuple (src/dst IP/port) used multiple times\n", COMPONENT_NAME);
            DEBUG("%s:    - Check connection_add() deduplication logic\n", COMPONENT_NAME);
        }

        DEBUG("%s:\n", COMPONENT_NAME);
        DEBUG("%s: ╚══════════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
        DEBUG("%s:\n", COMPONENT_NAME);

        if (outbound_dp != NULL) {
            ICS_Message *error_msg = (ICS_Message *)outbound_dp;

            memset(&error_msg->metadata, 0, sizeof(FrameMetadata));
            error_msg->metadata.ethertype = 0x0800;
            error_msg->metadata.ip_protocol = 6;
            error_msg->metadata.is_ip = 1;
            error_msg->metadata.is_tcp = 1;

            error_msg->metadata.src_ip = ics_msg->metadata.src_ip;
            error_msg->metadata.dst_ip = ics_msg->metadata.dst_ip;
            error_msg->metadata.src_port = ics_msg->metadata.src_port;
            error_msg->metadata.dst_port = ics_msg->metadata.dst_port;

            /* v2.172: Include session_id so Net0 can send close notification to Net1 */
            error_msg->metadata.session_id = ics_msg->metadata.session_id;

            error_msg->payload_length = 0;
            error_msg->metadata.payload_length = 0;
            error_msg->metadata.payload_offset = 0xFFFF;  /* Error marker */

            __sync_synchronize();
            outbound_ready_emit();
        }

        return;
    }

    struct tcp_inbound_client_state *state = inbound_alloc_state();
    if (state == NULL) {
        DEBUG("%s: [ERROR] INBOUND: Connection pool exhausted, dropping request\n", COMPONENT_NAME);
        tcp_abort(pcb);
        BREADCRUMB(2111);  /* Pool exhausted - aborted PCB and returning */
        return;
    }

    /* Validate payload size before copying */
    if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
        DEBUG("%s: INBOUND: Payload too large (%u > %u), dropping\n",
               COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
        tcp_abort(pcb);  /* v2.51: Force RST - never use tcp_close() to avoid FIN-WAIT-1 */
        inbound_free_state(state);  /* v2.106: Free allocated state */
        return;
    }

    /* Set up client state with COPIED payload */
    state->pcb = pcb;
    memcpy(state->payload_data, ics_msg->payload, ics_msg->payload_length);  /* COPY, not pointer! */
    state->payload_len = ics_msg->payload_length;
    state->bytes_sent = 0;
    /* state->active already set to true by inbound_alloc_state() */

    /* Extract source and destination from metadata
     * This preserves end-to-end IP addresses through the gateway
     * Example: SCADA (192.168.90.5) → PLC (192.168.95.2)
     *   Net0 receives: src=192.168.90.5, dst=192.168.95.2
     *   Net1 binds to: 192.168.90.5 (appears as SCADA to PLC)
     *   Net1 connects to: 192.168.95.2 (real PLC)
     */
    ip_addr_t src_ip, dest_ip;
    IP4_ADDR(&src_ip,
             (ics_msg->metadata.src_ip >> 24) & 0xFF,
             (ics_msg->metadata.src_ip >> 16) & 0xFF,
             (ics_msg->metadata.src_ip >> 8) & 0xFF,
             ics_msg->metadata.src_ip & 0xFF);

    IP4_ADDR(&dest_ip,
             (ics_msg->metadata.dst_ip >> 24) & 0xFF,
             (ics_msg->metadata.dst_ip >> 16) & 0xFF,
             (ics_msg->metadata.dst_ip >> 8) & 0xFF,
             ics_msg->metadata.dst_ip & 0xFF);

    /* NOTE: Cannot use tcp_bind() with external IP (192.168.90.5) because lwIP
     * only allows binding to IPs configured on the local interface (192.168.95.1)
     *
     * WORKAROUND: We bind to IP_ADDR_ANY and let lwIP use 192.168.95.1 as source.
     * This means the PLC will see the connection coming from the gateway (192.168.95.1),
     * not from the original SCADA IP (192.168.90.5).
     *
     * Alternative future solution: Use raw sockets or modify lwIP to allow arbitrary source IPs
     */
    BREADCRUMB(2010);  /* Attempting tcp_bind */

    err_t bind_err = tcp_bind(pcb, IP_ADDR_ANY, 0);  /* Bind to any, port 0 = ephemeral */
    if (bind_err != ERR_OK) {
        BREADCRUMB(2011);  /* tcp_bind failed */
        DEBUG("%s: INBOUND: tcp_bind failed: %d\n", COMPONENT_NAME, bind_err);
        tcp_abort(pcb);  /* v2.51: Force RST - never use tcp_close() to avoid FIN-WAIT-1 */
        inbound_free_state(state);  /* v2.106: Free allocated state */
        return;
    }

    struct connection_metadata *meta = connection_add(
        ics_msg->metadata.session_id, /* v2.150: Session ID from Net0 */
        ics_msg->metadata.src_ip,     /* Original SCADA IP (e.g., 192.168.90.5) */
        ics_msg->metadata.dst_ip,     /* Original PLC IP (e.g., 192.168.95.2) */
        ics_msg->metadata.src_port,   /* SCADA port */
        ics_msg->metadata.dst_port,   /* PLC port (502) */
        state                         /* v2.107: Track pool state for cleanup */
    );

    if (meta == NULL) {
        BREADCRUMB(2014);  /* Metadata storage failed */
        DEBUG("%s: INBOUND: Failed to store connection metadata (table full)\n", COMPONENT_NAME);
        tcp_abort(pcb);  /* v2.51: Force RST - never use tcp_close() to avoid FIN-WAIT-1 */
        inbound_free_state(state);  /* v2.106: Free allocated state */
        return;
    }

    BREADCRUMB(2015);  /* Metadata stored successfully */

    /* Store PCB pointer immediately so netif_output() can find metadata by PCB */
    meta->pcb = pcb;

    /* CRITICAL: Set callback argument BEFORE tcp_connect()
     * This prevents null pointer dereference in inbound_tcp_sent_callback
     * v2.106: Pass allocated state instead of global variable */
    tcp_arg(pcb, state);

    tcp_err(pcb, inbound_tcp_err_callback);

    /* Use original destination port from metadata */
    uint16_t dest_port = ics_msg->metadata.dst_port;

    /* v2.259: Always log PLC connection attempt at INFO level for debugging */
    DEBUG_INFO("[N1-CONNECT] session=%u → PLC %u.%u.%u.%u:%u\n",
           ics_msg->metadata.session_id,
           (ics_msg->metadata.dst_ip >> 24) & 0xFF,
           (ics_msg->metadata.dst_ip >> 16) & 0xFF,
           (ics_msg->metadata.dst_ip >> 8) & 0xFF,
           ics_msg->metadata.dst_ip & 0xFF,
           dest_port);

    BREADCRUMB(2016);  /* Attempting tcp_connect */

    err_t err = tcp_connect(pcb, &dest_ip, dest_port, inbound_tcp_connected_callback);

    meta->lwip_ephemeral_port = pcb->local_port;

    if (err != ERR_OK) {
        BREADCRUMB(2017);  /* tcp_connect failed */
        DEBUG("%s: INBOUND: tcp_connect failed: %d\n", COMPONENT_NAME, err);

        if (err == ERR_CONN || err == ERR_RST || err == ERR_ABRT) {
            DEBUG_WARN("%s:   [WARN]  PLC connection failed - checking for stale connections to clean up\n",
                   COMPONENT_NAME);

            int cleaned = 0;
            for (int i = 0; i < MAX_CONNECTIONS; i++) {
                if (!connection_table[i].active) continue;

                /* Check if this is a connection to the same PLC */
                if (connection_table[i].original_dest_ip == ics_msg->metadata.dst_ip &&
                    connection_table[i].dest_port == dest_port) {

                    struct tcp_pcb *stale_pcb = connection_table[i].pcb;

                    if (stale_pcb == NULL) {
                        DEBUG("%s:   [CLEAN] Cleaning stale connection [%d] to PLC (PCB=NULL)\n",
                               COMPONENT_NAME, i);

                        connection_table[i].active = false;

                        /* v2.182: Track connection count changes for leak debugging */
                        uint32_t old_count = connection_count;
                        if (connection_count > 0) {
                            connection_count--;
                        }
                        DEBUG("%s: [COUNT--] %u → %u | plc_unreachable() slot=%d session=%u (PCB=NULL)\n",
                               COMPONENT_NAME, old_count, connection_count, i,
                               connection_table[i].session_id);

                        cleaned++;
                    }
                }
            }

            if (cleaned > 0) {
                DEBUG("%s:   [OK] Cleaned %d stale connection(s) to PLC\n", COMPONENT_NAME, cleaned);
            } else {
                DEBUG("%s:   → No stale connections found - PLC might be genuinely unreachable\n",
                       COMPONENT_NAME);
            }
        }

        if (outbound_dp != NULL) {
            ICS_Message *error_msg = (ICS_Message *)outbound_dp;

            /* Prepare error notification */
            memset(&error_msg->metadata, 0, sizeof(FrameMetadata));
            error_msg->metadata.ethertype = 0x0800;
            error_msg->metadata.ip_protocol = 6;
            error_msg->metadata.is_ip = 1;
            error_msg->metadata.is_tcp = 1;

            /* Copy connection 5-tuple so Net0 knows which SCADA connection to close */
            error_msg->metadata.src_ip = ics_msg->metadata.src_ip;  /* SCADA IP */
            error_msg->metadata.dst_ip = ics_msg->metadata.dst_ip;  /* PLC IP */
            error_msg->metadata.src_port = ics_msg->metadata.src_port;  /* SCADA port */
            error_msg->metadata.dst_port = ics_msg->metadata.dst_port;  /* PLC port */

            /* Zero-length payload + error code in metadata = "PLC refused connection" */
            error_msg->payload_length = 0;
            error_msg->metadata.payload_length = 0;
            error_msg->metadata.payload_offset = 0xFFFF;  /* Special marker: 0xFFFF = ERROR */

            DEBUG_ERROR("%s: [ERR] Sending ERROR notification to Net0 (PLC refused connection, err=%d)\n",
                   COMPONENT_NAME, err);
            DEBUG("%s:    → Net0 will close SCADA %u.%u.%u.%u:%u immediately\n",
                   COMPONENT_NAME,
                   (error_msg->metadata.src_ip >> 24) & 0xFF,
                   (error_msg->metadata.src_ip >> 16) & 0xFF,
                   (error_msg->metadata.src_ip >> 8) & 0xFF,
                   error_msg->metadata.src_ip & 0xFF,
                   error_msg->metadata.src_port);

            /* Force cache flush before notification */
            __sync_synchronize();

            /* Signal ICS_Outbound to pass error notification to Net0 */
            outbound_ready_emit();
        }

        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_arg(pcb, NULL);

        meta->pcb = NULL;  /* Clear PCB from metadata before close */
        meta->active = false;  /* Mark metadata inactive */
        if (connection_count > 0) {
            connection_count--;  /* Decrement counter */
        }

        err_t close_err = tcp_close(pcb);
        if (close_err != ERR_OK) {
            /* tcp_close failed, safe to abort now (callbacks NULL) */
            tcp_abort(pcb);
        }

        inbound_free_state(state);  /* v2.106: Free allocated state */
        return;
    }

    meta->tcp_seq_num = pcb->snd_nxt;  /* Current send sequence number */
    meta->timestamp = sys_now();       /* Connection creation timestamp */

    /* CRITICAL: Memory barrier to ensure all metadata writes are visible before callbacks fire */
    __sync_synchronize();

    BREADCRUMB(2019);  /* Metadata complete, memory barrier done */

    #if DEBUG_METADATA
    DEBUG("%s: 📝 Stored metadata [slot %d]: SCADA %u.%u.%u.%u:%u → PLC %u.%u.%u.%u:%u (lwIP port: %u)\n",
           COMPONENT_NAME, (int)(meta - connection_table),
           (meta->original_src_ip >> 24) & 0xFF, (meta->original_src_ip >> 16) & 0xFF,
           (meta->original_src_ip >> 8) & 0xFF, meta->original_src_ip & 0xFF,
           meta->src_port,
           (meta->original_dest_ip >> 24) & 0xFF, (meta->original_dest_ip >> 16) & 0xFF,
           (meta->original_dest_ip >> 8) & 0xFF, meta->original_dest_ip & 0xFF,
           meta->dest_port, meta->lwip_ephemeral_port);
    #endif

    /* v2.250: Minimal packet flow logging already done at entry */

    BREADCRUMB(2020);  /* Exit: inbound_ready_handle complete */
}

/*
 * OUTBOUND notification handler - called when ICS_Outbound has validated data
 * Creates TCP client connection to forward data to external network
 */
void outbound_ready_handle(void)
{

    /* v2.250: Minimal packet flow logging - box drawing removed */

    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (outbound_dp == NULL) {
        DEBUG_ERROR("%s: [ERR] FATAL: outbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        DEBUG("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        return;
    }

    ICS_Message *ics_msg = (ICS_Message *)outbound_dp;

    /* Validate message */
    if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
        DEBUG("%s: OUTBOUND: Invalid payload length %u (max %u)\n",
               COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
        #if DEBUG_ENABLED_DEBUG
        /* DEBUG("   ✗ [MSG #%u] DROPPED - invalid payload size\n\n", msg_id); */ /* msg_id undefined */
        #endif
        return;
    }

    /* Check if we already have an active connection - if so, close it and create new one */
    if (outbound_tcp_client.active) {
        #if DEBUG_TRAFFIC
        DEBUG("%s: OUTBOUND: Previous connection still active, closing it to handle new message\n", COMPONENT_NAME);
        #endif
        if (outbound_tcp_client.pcb != NULL) {
            /* v2.170: CRITICAL FIX - Follow CRITICAL_LESSON Rule 2 */
            struct tcp_pcb *old_pcb = outbound_tcp_client.pcb;
            tcp_recv(old_pcb, NULL);
            tcp_sent(old_pcb, NULL);
            tcp_err(old_pcb, NULL);
            tcp_arg(old_pcb, NULL);

            err_t close_err = tcp_close(old_pcb);
            if (close_err != ERR_OK) {
                tcp_abort(old_pcb);  /* Safe - callbacks NULL */
            }
        }
        outbound_tcp_client.active = false;
        outbound_tcp_client.pcb = NULL;
    }

    #if DEBUG_ENABLED_DEBUG
    DEBUG("   Payload size: %u bytes\n", ics_msg->payload_length);
    DEBUG("   Destination: %u.%u.%u.%u:%u\n",
           (ics_msg->metadata.dst_ip >> 24) & 0xFF,
           (ics_msg->metadata.dst_ip >> 16) & 0xFF,
           (ics_msg->metadata.dst_ip >> 8) & 0xFF,
           ics_msg->metadata.dst_ip & 0xFF,
           ics_msg->metadata.dst_port);

    /* Print ASCII payload preview */
    DEBUG("   Payload preview: \"");
    for (uint16_t i = 0; i < (ics_msg->payload_length < 60 ? ics_msg->payload_length : 60); i++) {
        char c = ics_msg->payload[i];
        if (c >= 32 && c <= 126) DEBUG("%c", c);
        else if (c == '\n') DEBUG("\\n");
        else if (c == '\r') DEBUG("\\r");
        else DEBUG(".");
    }
    if (ics_msg->payload_length > 60) DEBUG("...");
    DEBUG("\"\n");
    #endif

    /* Print metadata */
    #if DEBUG_TRAFFIC
    DEBUG("%s: OUTBOUND: Protocol=%s, Src=%u.%u.%u.%u:%u, Dst=%u.%u.%u.%u:%u, Payload=%u bytes\n",
           COMPONENT_NAME,
           ics_msg->metadata.is_tcp ? "TCP" : (ics_msg->metadata.is_udp ? "UDP" : "Other"),
           (ics_msg->metadata.src_ip >> 24) & 0xFF, (ics_msg->metadata.src_ip >> 16) & 0xFF,
           (ics_msg->metadata.src_ip >> 8) & 0xFF, ics_msg->metadata.src_ip & 0xFF,
           ics_msg->metadata.src_port,
           (ics_msg->metadata.dst_ip >> 24) & 0xFF, (ics_msg->metadata.dst_ip >> 16) & 0xFF,
           (ics_msg->metadata.dst_ip >> 8) & 0xFF, ics_msg->metadata.dst_ip & 0xFF,
           ics_msg->metadata.dst_port,
           ics_msg->payload_length);
    #endif

    /* Create TCP client connection */
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        DEBUG("%s: OUTBOUND: Failed to create TCP PCB\n", COMPONENT_NAME);
        return;
    }

    /* v2.106: Validate payload size before copying */
    if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
        DEBUG("%s: OUTBOUND: Payload too large (%u > %u), dropping\n",
               COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
        tcp_abort(pcb);
        return;
    }

    /* Set up client state with COPIED payload (not pointer!) */
    outbound_tcp_client.pcb = pcb;
    memcpy(outbound_tcp_client.payload_data, ics_msg->payload, ics_msg->payload_length);  /* v2.106: COPY, not pointer! */
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

    #if DEBUG_TRAFFIC
    DEBUG("%s: OUTBOUND: Port mapping: internal:%u → host:%s:%u (via QEMU gateway)\n",
           COMPONENT_NAME, ics_msg->metadata.dst_port, OUTBOUND_FORWARD_IP, mapped_port);
    #endif

    /* Connect to host via QEMU gateway */
    #if DEBUG_TRAFFIC
    DEBUG("%s: OUTBOUND: Connecting to host via %s:%u...\n",
           COMPONENT_NAME, OUTBOUND_FORWARD_IP, mapped_port);
    #endif

    err_t err = tcp_connect(pcb, &dest_ip, mapped_port, outbound_tcp_connected_callback);
    if (err != ERR_OK) {
        DEBUG("%s: OUTBOUND: tcp_connect failed: %d\n", COMPONENT_NAME, err);

        /* v2.170: CRITICAL FIX - Follow CRITICAL_LESSON Rule 2
         * tcp_arg was set before tcp_connect(), so NULL it before close */
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_arg(pcb, NULL);

        err_t close_err = tcp_close(pcb);
        if (close_err != ERR_OK) {
            tcp_abort(pcb);  /* Safe - callbacks NULL */
        }

        outbound_tcp_client.active = false;
        outbound_tcp_client.pcb = NULL;
        return;
    }

    #if DEBUG_TRAFFIC
    DEBUG("%s: OUTBOUND: Connection initiated\n", COMPONENT_NAME);
    #endif
}

static volatile bool rx_packets_pending = false;

void virtio_irq_handle(void)
{
    static uint32_t irq_count = 0;
    uint32_t irq_status = VREG_READ(VIRTIO_MMIO_INTERRUPT_STATUS);

    irq_count++;

    #if DEBUG_TRAFFIC
    DEBUG("%s: ⚡ IRQ #%u: status=0x%x\n", COMPONENT_NAME, irq_count, irq_status);
    #endif

    if (irq_status & VIRTIO_MMIO_IRQ_VQUEUE) {
        #if DEBUG_TRAFFIC
        DEBUG("%s:   → VQUEUE interrupt - setting rx_packets_pending flag\n", COMPONENT_NAME);
        #endif
        rx_packets_pending = true;
        VREG_WRITE(VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_MMIO_IRQ_VQUEUE);
    }

    if (irq_status & VIRTIO_MMIO_IRQ_CONFIG) {
        #if DEBUG_TRAFFIC
        DEBUG("%s:   → CONFIG interrupt\n", COMPONENT_NAME);
        #endif
        VREG_WRITE(VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_MMIO_IRQ_CONFIG);
    }

    virtio_irq_acknowledge();
}

/*
 * Initialize VirtIO device (same as Tier 2)
 */
static int virtio_net_init(void)
{
    DEBUG("\n");
    DEBUG("╔══════════════════════════════════════════════════════════╗\n");
    DEBUG("║         EthernetDriver Component - Tier 4                ║\n");
    DEBUG("║      TCP Echo Server with lwIP TCP/IP Stack               ║\n");
    DEBUG("║              (CAmkES Port of sDDF Driver)                 ║\n");
    DEBUG("╚══════════════════════════════════════════════════════════╝\n");
    DEBUG("\n");

    DEBUG("\n╔═══════════════════════════════════════════════════════════════╗\n");
    DEBUG("║  [DISABLED] SCANNING MAPPED VIRTIO MMIO SLOTS FOR ACTIVE DEVICES         ║\n");
    DEBUG("║  Base: 0x0a000000, Each slot: 0x200 bytes apart               ║\n");
    DEBUG("║  Scanning slots 24-31 only (one 4KB page mapped)                ║\n");
    DEBUG("╚═══════════════════════════════════════════════════════════════╝\n\n");

    DEBUG("\n");

    /* CRITICAL: Check if CAmkES dataport is properly mapped */
    if (virtio_mmio_regs == NULL) {
        DEBUG_ERROR("%s: [ERR] FATAL: virtio_mmio_regs dataport is NULL!\n", COMPONENT_NAME);
        DEBUG("%s:    CAmkES failed to map hardware component net0_hw\n", COMPONENT_NAME);
        DEBUG("%s:    Check ics_dual_nic.camkes configuration\n", COMPONENT_NAME);
        return -1;
    }

    DEBUG("%s: virtio_mmio_regs dataport mapped at %p\n", COMPONENT_NAME, (void *)virtio_mmio_regs);

    /* Access VirtIO device at SLOT 31 (offset 0xc00 from page base 0xa003000) */
    /* QEMU assigns FIRST -device virtio-net-device to slot 30 - matches vm_ethernet_echo */
    virtio_regs_base = (volatile uint32_t *)((uintptr_t)virtio_mmio_regs + 0xc00);

    DEBUG("%s: virtio_regs_base (slot 30) = %p (base + 0xc00)\n",
           COMPONENT_NAME, (void *)virtio_regs_base);

    /* Verify we have the network device using pointer arithmetic */
    uint32_t magic = VREG_READ(VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version = VREG_READ(VIRTIO_MMIO_VERSION);
    uint32_t device_id = VREG_READ(VIRTIO_MMIO_DEVICE_ID);

    DEBUG("%s: VirtIO @ slot 30 (+0xc00): Magic=0x%x, Version=%u, DeviceID=%u\n",
           COMPONENT_NAME, magic, version, device_id);

    if (magic != 0x74726976) {
        DEBUG("%s: ERROR: Invalid VirtIO magic! Device not accessible.\n", COMPONENT_NAME);
        return -1;
    }

    /* CRITICAL CHECK: Enforce modern VirtIO protocol */
    if (version != 2) {
        DEBUG("\n");
        DEBUG("╔════════════════════════════════════════════════════════════════╗\n");
        DEBUG_ERROR("║  [ERR] FATAL ERROR: Legacy VirtIO Protocol Detected              ║\n");
        DEBUG("╚════════════════════════════════════════════════════════════════╝\n");
        DEBUG("\n");
        DEBUG("%s: VirtIO Version=%u (expected 2 for modern protocol)\n", COMPONENT_NAME, version);
        return -1;
    }

    if (device_id != 1) {
        DEBUG("%s: ERROR: DeviceID=%u (expected 1 for network)\n", COMPONENT_NAME, device_id);
        DEBUG("%s: QEMU may have allocated the device to a different slot.\n", COMPONENT_NAME);
        DEBUG("%s: With force-legacy=false, devices should be at slots 6-7.\n", COMPONENT_NAME);
        return -1;
    }

    DEBUG_INFO("%s: [OK] Found VirtIO network device (modern protocol, Version 2)\n", COMPONENT_NAME);

    /* Reset device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, 0);

    /* Acknowledge device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* ═══════════════════════════════════════════════════════════
     * VirtIO Device Initialization Summary
     * ═══════════════════════════════════════════════════════════ */

    uint32_t device_features = VREG_READ(VIRTIO_MMIO_DEVICE_FEATURES);

    DEBUG("\n");
    DEBUG("╔══════════════════════════════════════════════════════════╗\n");
    DEBUG("║  VirtIO Network Device Initialization                   ║\n");
    DEBUG("╚══════════════════════════════════════════════════════════╝\n");
    DEBUG("%s: Device ID: 0x%x (VirtIO-Net)\n", COMPONENT_NAME, device_id);
    DEBUG("%s: DeviceFeatures: 0x%08x (CTRL_VQ %s)\n", COMPONENT_NAME,
           device_features, (device_features & (1<<18)) ? "enabled" : "disabled");
    DEBUG("\n");


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

    DEBUG("%s: MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", COMPONENT_NAME,
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5]);

    /* Setup virtqueues using CAmkES DMA allocation (sDDF equivalent) */
    /* Allocate 64KB DMA buffer for virtqueue rings, 4K-aligned, uncached for device DMA */
    uint8_t *ring_base = camkes_dma_alloc(0x10000, 4096, false);
    if (!ring_base) {
        DEBUG("%s: ERROR: Failed to allocate DMA buffer for virtqueues\n", COMPONENT_NAME);
        return -1;
    }
    memset(ring_base, 0, 0x10000);

    /* Get physical address for VirtIO device DMA access (sDDF: device_resources.regions[1].io_addr) */
    uintptr_t ring_base_paddr = camkes_dma_get_paddr(ring_base);

    #if DEBUG_ENABLED_DEBUG
    DEBUG("%s: DEBUG: ring_base virtual  = 0x%lx\n", COMPONENT_NAME, (uintptr_t)ring_base);
    DEBUG("%s: DEBUG: ring_base physical = 0x%lx (via camkes_dma_get_paddr)\n", COMPONENT_NAME, ring_base_paddr);
    #endif

    /* RX queue */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_RX_QUEUE);
    #if DEBUG_ENABLED_DEBUG
    DEBUG("%s: DEBUG: QueueSel set to %u\n", COMPONENT_NAME, VIRTIO_NET_RX_QUEUE);
    DEBUG("%s: DEBUG: QueueSel readback = %u\n", COMPONENT_NAME, VREG_READ(VIRTIO_MMIO_QUEUE_SEL));
    DEBUG("%s: DEBUG: QueueNumMax = %u\n", COMPONENT_NAME, VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX));
    #endif

    /* Read and validate QueueNumMax from device register
     * VirtIO spec: Max queue size is typically 1024 for network devices
     */
    uint32_t queue_num_max = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);

    /* CRITICAL FIX: Ring size MUST match buffer pool size to prevent accessing
     * uninitialized descriptors. Previous bug: QEMU offered 256 descriptors but
     * we only had 32 buffers, causing descriptor index wraparound to hit
     * uninitialized memory after ~352 packets.
     *
     * Solution: Tell QEMU to use exactly MAX_PACKETS descriptors (32).
     * This ensures QEMU never tries to use descriptors we haven't initialized.
     */
    rx_virtq.num = MAX_PACKETS;

    DEBUG("%s: [FIX] RX Queue: Device offers %u descriptors, using %u (matches buffer pool)\n",
           COMPONENT_NAME, queue_num_max, rx_virtq.num);

    if (queue_num_max < MAX_PACKETS) {
        DEBUG_WARN("%s: [WARN]  WARNING: Device only supports %u descriptors but we need %u\n",
               COMPONENT_NAME, queue_num_max, MAX_PACKETS);
        DEBUG("%s:             This may cause issues - consider reducing MAX_PACKETS\n", COMPONENT_NAME);
    }

    #if DEBUG_ENABLED_DEBUG
    DEBUG("%s: DEBUG: rx_virtq.num = %u (FIXED to match MAX_PACKETS=%u)\n",
           COMPONENT_NAME, rx_virtq.num, MAX_PACKETS);
    #endif

    /* Virtual addresses for driver access */
    rx_virtq.desc = (struct virtq_desc *)ring_base;
    rx_virtq.avail = (struct virtq_avail *)(ring_base + 0x2000);
    rx_virtq.used = (struct virtq_used *)(ring_base + 0x2408);

    /* Physical addresses for device DMA access */
    uintptr_t desc_paddr = ring_base_paddr;
    uintptr_t avail_paddr = ring_base_paddr + 0x2000;
    uintptr_t used_paddr = ring_base_paddr + 0x2408;

    #if DEBUG_ENABLED_DEBUG
    DEBUG("%s: DEBUG: RX desc paddr  = 0x%lx\n", COMPONENT_NAME, desc_paddr);
    DEBUG("%s: DEBUG: RX avail paddr = 0x%lx\n", COMPONENT_NAME, avail_paddr);
    DEBUG("%s: DEBUG: RX used paddr  = 0x%lx\n", COMPONENT_NAME, used_paddr);
    #endif

    VREG_WRITE(VIRTIO_MMIO_QUEUE_NUM, rx_virtq.num);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail_paddr >> 32));
    VREG_WRITE(VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)used_paddr);
    VREG_WRITE(VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used_paddr >> 32));

    /* v2.241: Removed verbose VirtIO register dumps - excessive for single-CPU image */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);
    DMB();

    uint32_t rx_ready_after = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    if (rx_ready_after == 0) {
        DEBUG_ERROR("%s: [ERR] QEMU REJECTED RX queue\n", COMPONENT_NAME);
    } else {
        DEBUG_INFO("%s: RX queue configured (size=%u)\n", COMPONENT_NAME, rx_virtq.num);
    }

    /* TX queue */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_TX_QUEUE);

    /* Read and validate QueueNumMax from device register */
    queue_num_max = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);

    /* CRITICAL FIX: Same as RX - TX ring size must match buffer pool.
     * TX uses MAX_PACKETS/2 buffers (16), so set ring size to match.
     */
    tx_virtq.num = MAX_PACKETS;  /* Use same size as RX for consistency */

    DEBUG("%s: [FIX] TX Queue: Device offers %u descriptors, using %u (matches buffer pool)\n",
           COMPONENT_NAME, queue_num_max, tx_virtq.num);

    if (queue_num_max < MAX_PACKETS) {
        DEBUG_WARN("%s: [WARN]  WARNING: Device only supports %u TX descriptors but we need %u\n",
               COMPONENT_NAME, queue_num_max, MAX_PACKETS);
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

    /* v2.241: Removed verbose VirtIO register dumps - excessive for single-CPU image */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);
    DMB();

    uint32_t tx_ready_after = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    if (tx_ready_after == 0) {
        DEBUG_ERROR("%s: [ERR] QEMU REJECTED TX queue\n", COMPONENT_NAME);
    } else {
        DEBUG_INFO("%s: TX queue configured (size=%u)\n", COMPONENT_NAME, tx_virtq.num);
    }

    /* Device ready - activate the device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VREG_READ(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);
    DEBUG_INFO("%s: VirtIO device initialized and activated\n", COMPONENT_NAME);

    return 0;
}

/*
 * Component initialization
 */
void post_init(void)
{
    DEBUG_INFO("%s: %s (%s) started\n", COMPONENT_NAME, NET1_VERSION, ICS_VERSION_DATE);

    /* Initialize connection tracking table */
    memset(connection_table, 0, sizeof(connection_table));
    connection_count = 0;


    DEBUG_INFO("%s: [OK] Connection tracking table initialized (%d slots, all states=FREE)\n",
           COMPONENT_NAME, MAX_CONNECTIONS);

    /* v2.117: Initialize connection state sharing dataports */
    own_state = (volatile struct connection_state_table *)net1_conn_state;
    peer_state = (volatile struct connection_state_table *)net0_conn_state;
    if (own_state) {
        memset((void *)own_state, 0, sizeof(struct connection_state_table));
        DEBUG_INFO("%s: [OK] Own connection state dataport mapped (size=%zu bytes)\n",
               COMPONENT_NAME, sizeof(struct connection_state_table));
    }
    if (peer_state) {
        DEBUG_INFO("%s: [OK] Peer connection state dataport mapped (read-only access to Net0)\n", COMPONENT_NAME);
    }

    DEBUG("%s: ℹ️  If PLC has stale connections from previous versions, they will timeout naturally.\n", COMPONENT_NAME);
    DEBUG("%s:    For immediate cleanup: restart PLC or wait ~2 hours for TCP keepalive.\n", COMPONENT_NAME);

    /* Initialize VirtIO device */
    if (virtio_net_init() != 0) {
        DEBUG("%s: Failed to initialize VirtIO device\n", COMPONENT_NAME);
        return;
    }

    /* Allocate packet buffers from DMA memory */
    DEBUG("%s: Allocating %d DMA packet buffers (%d bytes each)...\n",
           COMPONENT_NAME, MAX_PACKETS, PACKET_BUFFER_SIZE);
    for (int i = 0; i < MAX_PACKETS; i++) {
        packet_buffers[i] = camkes_dma_alloc(PACKET_BUFFER_SIZE, 64, false);
        if (!packet_buffers[i]) {
            DEBUG("%s: ERROR: Failed to allocate DMA buffer %d\n", COMPONENT_NAME, i);
            return;
        }
        packet_buffers_paddr[i] = camkes_dma_get_paddr(packet_buffers[i]);
    }
    DEBUG_INFO("%s: [OK] Allocated DMA packet buffers (vaddr=%p, paddr=0x%lx)\n",
           COMPONENT_NAME, packet_buffers[0], packet_buffers_paddr[0]);

    /* Allocate TX headers array */
    size_t tx_headers_size = MAX_PACKETS * sizeof(virtio_net_hdr_t);
    tx_headers = camkes_dma_alloc(tx_headers_size, 16, false);
    if (!tx_headers) {
        DEBUG("%s: ERROR: Failed to allocate TX headers DMA memory\n", COMPONENT_NAME);
        return;
    }
    tx_headers_paddr = camkes_dma_get_paddr(tx_headers);
    memset(tx_headers, 0, tx_headers_size);
    DEBUG_INFO("%s: [OK] Allocated TX headers array (vaddr=%p, paddr=0x%lx)\n",
           COMPONENT_NAME, tx_headers, tx_headers_paddr);

    /* Initialize packet buffers */
    memset(rx_buffer_used, 0, sizeof(rx_buffer_used));
    refill_rx_queue();

    /* Initialize lwIP */
    DEBUG_INFO("%s: Initializing lwIP TCP/IP stack...\n", COMPONENT_NAME);
    lwip_init();

    /* CRITICAL: Setup TCP server BEFORE netif_add() so PCB stays bound to 0.0.0.0
     * This is the key to accepting packets for ANY destination IP (both 10.2.0.2 and 192.168.95.2)
     * If we do netif_add() first, lwIP might bind the PCB to the interface IP
     */
    DEBUG("%s: Setting up TCP server on port %d (binding to 0.0.0.0 for promiscuous accept)...\n", COMPONENT_NAME, TCP_ECHO_PORT);
    setup_tcp_echo_server();

    /* Add network interface - BRIDGE ARCHITECTURE
     * nic0 IS the external gateway (192.168.95.1) that pfSense routes through
     * No gateway needed - we ARE the gateway!
     * TCP server listens on 192.168.95.1:502
     */
    struct ip4_addr ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 192, 168, 95, 1);    /* Static IP: 192.168.95.1 (internal gateway) */
    IP4_ADDR(&netmask, 255, 255, 255, 0);  /* Netmask: 255.255.255.0 */
    IP4_ADDR(&gw, 0, 0, 0, 0);              /* NO Gateway - this interface IS the gateway */

    DEBUG("%s: Configuring network interface:\n", COMPONENT_NAME);
    DEBUG("%s:   IP:      192.168.95.1 (internal gateway - PLC network)\n", COMPONENT_NAME);
    DEBUG("%s:   Netmask: 255.255.255.0\n", COMPONENT_NAME);
    DEBUG("%s:   Gateway: None (this interface IS the gateway)\n", COMPONENT_NAME);
    DEBUG("%s:   TCP server: 192.168.95.1:%d\n", COMPONENT_NAME, TCP_ECHO_PORT);

    netif_add(&netif_data, &ipaddr, &netmask, &gw, NULL, custom_netif_init, custom_input_promiscuous);
    netif_set_default(&netif_data);
    netif_set_status_callback(&netif_data, netif_status_callback);
    netif_set_up(&netif_data);

    /* Static ARP entry NOT needed with bridge architecture!
     * With bridges, all devices are on the same Layer 2 network
     * ARP works naturally without any hacks
     */

    /* Verify interface configuration */
    DEBUG("\n");
    DEBUG("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    DEBUG_INFO("%s: [OK] NETWORK INTERFACE CONFIGURATION\n", COMPONENT_NAME);
    DEBUG("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    DEBUG("%s: Interface IP:   %u.%u.%u.%u\n", COMPONENT_NAME,
           ip4_addr1(netif_ip4_addr(&netif_data)),
           ip4_addr2(netif_ip4_addr(&netif_data)),
           ip4_addr3(netif_ip4_addr(&netif_data)),
           ip4_addr4(netif_ip4_addr(&netif_data)));
    DEBUG("%s: Netmask:        %u.%u.%u.%u\n", COMPONENT_NAME,
           ip4_addr1(netif_ip4_netmask(&netif_data)),
           ip4_addr2(netif_ip4_netmask(&netif_data)),
           ip4_addr3(netif_ip4_netmask(&netif_data)),
           ip4_addr4(netif_ip4_netmask(&netif_data)));
    DEBUG("%s: Gateway:        %u.%u.%u.%u\n", COMPONENT_NAME,
           ip4_addr1(netif_ip4_gw(&netif_data)),
           ip4_addr2(netif_ip4_gw(&netif_data)),
           ip4_addr3(netif_ip4_gw(&netif_data)),
           ip4_addr4(netif_ip4_gw(&netif_data)));
    DEBUG("%s: Status:         %s\n", COMPONENT_NAME, netif_is_up(&netif_data) ? "UP" : "DOWN");
    DEBUG("%s: Role:           Internal gateway (transparent security gateway)\n", COMPONENT_NAME);
    DEBUG("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    DEBUG("\n");

    /* Validation check */
    uint8_t if_ip1 = ip4_addr1(netif_ip4_addr(&netif_data));
    uint8_t if_ip2 = ip4_addr2(netif_ip4_addr(&netif_data));
    uint8_t if_ip3 = ip4_addr3(netif_ip4_addr(&netif_data));
    uint8_t if_ip4 = ip4_addr4(netif_ip4_addr(&netif_data));

    if (if_ip1 == 192 && if_ip2 == 168 && if_ip3 == 96 && if_ip4 == 2) {
        DEBUG_INFO("%s: [OK] CONFIGURATION VALID: Internal gateway IP = 192.168.95.1\n", COMPONENT_NAME);
        DEBUG_INFO("%s: [OK] pfSense routes 192.168.95.0/24 traffic through this gateway\n", COMPONENT_NAME);
        DEBUG_INFO("%s: [OK] Bridge br0 forwards all traffic to/from ens224\n", COMPONENT_NAME);
    } else {
        DEBUG_WARN("%s: [WARN]  WARNING: Interface IP (%u.%u.%u.%u) does NOT match expected (192.168.95.1)\n",
               COMPONENT_NAME, if_ip1, if_ip2, if_ip3, if_ip4);
        DEBUG_WARN("%s: [WARN]  pfSense routing will FAIL!\n", COMPONENT_NAME);
    }
    DEBUG("\n");

    tcp_server_initialized = true;
    DEBUG_INFO("%s: [OK] Initialization complete\n", COMPONENT_NAME);
    DEBUG("%s: Network ready\n\n", COMPONENT_NAME);

    /* Mark initialization as successful */
    initialization_successful = true;

    DEBUG("%s: post_init() complete - returning to allow pipeline to start\n", COMPONENT_NAME);
}

int run(void)
{
    /* Validate initialization completed successfully */
    if (!initialization_successful) {
        DEBUG("\n");
        DEBUG("╔══════════════════════════════════════════════════════════╗\n");
        DEBUG_ERROR("║  [ERR] FATAL: VirtIO_Net1_Driver initialization FAILED     ║\n");
        DEBUG("╚══════════════════════════════════════════════════════════╝\n");
        DEBUG("\n");
        DEBUG("%s: Initialization did not complete successfully\n", COMPONENT_NAME);
        DEBUG("%s: Common causes:\n", COMPONENT_NAME);
        DEBUG("%s:   - DMA memory pool exhausted (check MAX_PACKETS setting)\n", COMPONENT_NAME);
        DEBUG("%s:   - VirtIO device not found or misconfigured\n", COMPONENT_NAME);
        DEBUG("%s:   - Network interface setup failed\n", COMPONENT_NAME);
        DEBUG("\n");
        DEBUG("%s: SYSTEM HALTED - cannot continue without working network driver\n", COMPONENT_NAME);
        DEBUG("\n");
        while (1) {
            seL4_Yield();  /* Halt forever */
        }
    }

    DEBUG_INFO("%s: [OK] Initialization validation passed - starting main loop\n", COMPONENT_NAME);

    /* Main event loop - process lwIP timers, RX packets, and ICS notifications */
    /* Note: TCP server is now initialized in RX path after first packet */
    uint32_t loop_iterations = 0;
    const uint32_t CLEANUP_INTERVAL = 10000;  /* Run cleanup every ~10000 iterations (~30-60 seconds) */

    static uint32_t heartbeat_counter = 0;
    while (1) {
        process_cleanup_queue();

        if (++heartbeat_counter >= 50000) {
            DEBUG("%s: [HB]  Heartbeat: %u iterations, %u active connections | PBUF: %u/%u\n",
                   COMPONENT_NAME, heartbeat_counter, connection_count,
                   lwip_stats.memp[MEMP_PBUF_POOL]->used,
                   lwip_stats.memp[MEMP_PBUF_POOL]->avail);

            if (connection_count >= 750) {
                DEBUG("%s: [LEAK_DETECT] WARNING: Connection count HIGH (%u/1000)\n",
                       COMPONENT_NAME, connection_count);
                DEBUG("%s: [LEAK_DETECT] Inspecting lwIP PCB states...\n", COMPONENT_NAME);

                /* Count PCBs in each lwIP list */
                uint32_t active_count = 0;
                uint32_t tw_count = 0;
                uint32_t bound_count = 0;

                /* Count active PCBs (ESTABLISHED, SYN_SENT, SYN_RCVD, etc.) */
                struct tcp_pcb *pcb;
                for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
                    active_count++;
                }

                /* Count TIME_WAIT PCBs */
                for (pcb = tcp_tw_pcbs; pcb != NULL; pcb = pcb->next) {
                    tw_count++;
                }

                /* Count bound PCBs (LISTEN, CLOSED) */
                for (pcb = tcp_bound_pcbs; pcb != NULL; pcb = pcb->next) {
                    bound_count++;
                }

                uint32_t total_lwip = active_count + tw_count + bound_count;

                DEBUG("%s: [LEAK_DETECT] lwIP PCB breakdown:\n", COMPONENT_NAME);
                DEBUG("%s: [LEAK_DETECT]   Active PCBs:    %u (ESTABLISHED, SYN_*, FIN_WAIT, etc.)\n",
                       COMPONENT_NAME, active_count);
                DEBUG("%s: [LEAK_DETECT]   TIME_WAIT PCBs: %u (should be < 30 with TCP_MSL=30s)\n",
                       COMPONENT_NAME, tw_count);
                DEBUG("%s: [LEAK_DETECT]   Bound PCBs:     %u (LISTEN, CLOSED)\n",
                       COMPONENT_NAME, bound_count);
                DEBUG("%s: [LEAK_DETECT]   Total lwIP:     %u/%u\n",
                       COMPONENT_NAME, total_lwip, MEMP_NUM_TCP_PCB);

                /* Check for mismatch between our count and lwIP's count */
                if (total_lwip != connection_count) {
                    DEBUG("%s: [LEAK_DETECT] ⚠️  MISMATCH: Our count=%u, lwIP count=%u (diff=%d)\n",
                           COMPONENT_NAME, connection_count, total_lwip,
                           (int)connection_count - (int)total_lwip);
                }

                /* Detailed active PCB state inspection */
                if (active_count > 0) {
                    DEBUG("%s: [LEAK_DETECT] Active PCB states:\n", COMPONENT_NAME);
                    uint32_t state_counts[11] = {0};  /* TCP states: 0-10 */
                    const char *state_names[] = {
                        "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED",
                        "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING",
                        "LAST_ACK", "TIME_WAIT"
                    };

                    for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
                        if (pcb->state < 11) {
                            state_counts[pcb->state]++;
                        }
                    }

                    for (int i = 0; i < 11; i++) {
                        if (state_counts[i] > 0) {
                            DEBUG("%s: [LEAK_DETECT]   %s: %u\n",
                                   COMPONENT_NAME, state_names[i], state_counts[i]);
                        }
                    }

                    /* Flag suspicious states */
                    if (state_counts[7] > 5) {  /* CLOSE_WAIT */
                        DEBUG("%s: [LEAK_DETECT] ⚠️  HIGH CLOSE_WAIT count (%u) - possible leak!\n",
                               COMPONENT_NAME, state_counts[7]);
                    }
                    if (state_counts[5] > 10 || state_counts[6] > 10) {  /* FIN_WAIT_1/2 */
                        DEBUG("%s: [LEAK_DETECT] ⚠️  HIGH FIN_WAIT count (FIN_WAIT_1=%u, FIN_WAIT_2=%u)\n",
                               COMPONENT_NAME, state_counts[5], state_counts[6]);
                    }
                }

                /* Check TIME_WAIT excessive accumulation */
                if (tw_count > 30) {
                    DEBUG("%s: [LEAK_DETECT] ⚠️  EXCESSIVE TIME_WAIT (%u) - expected < 30 with TCP_MSL=30s\n",
                           COMPONENT_NAME, tw_count);
                    DEBUG("%s: [LEAK_DETECT]     (Should auto-expire after 60s, check if cleanup working)\n",
                           COMPONENT_NAME);
                }

                DEBUG("%s: [LEAK_DETECT] Leak detection complete\n", COMPONENT_NAME);
            }

            /* v2.93: DEBUG - Show connection table details */
            DEBUG("%s: [FIND] NET1 Connection Table (PLC connections):\n", COMPONENT_NAME);
            int shown = 0;
            for (int i = 0; i < MAX_CONNECTIONS && shown < 10; i++) {
                if (connection_table[i].active) {
                    DEBUG("%s:   [%d] SCADA %u.%u.%u.%u:%u → PLC %u.%u.%u.%u:%u PCB=%p lwIP_port=%u\n",
                           COMPONENT_NAME, i,
                           (connection_table[i].original_src_ip >> 24) & 0xFF,
                           (connection_table[i].original_src_ip >> 16) & 0xFF,
                           (connection_table[i].original_src_ip >> 8) & 0xFF,
                           connection_table[i].original_src_ip & 0xFF,
                           connection_table[i].src_port,
                           (connection_table[i].original_dest_ip >> 24) & 0xFF,
                           (connection_table[i].original_dest_ip >> 16) & 0xFF,
                           (connection_table[i].original_dest_ip >> 8) & 0xFF,
                           connection_table[i].original_dest_ip & 0xFF,
                           connection_table[i].dest_port,
                           (void*)connection_table[i].pcb,
                           connection_table[i].lwip_ephemeral_port);
                    shown++;
                }
            }
            if (shown == 0) {
                DEBUG("%s:   (no active connections)\n", COMPONENT_NAME);
            } else if (connection_count > shown) {
                DEBUG("%s:   ... and %d more connections\n", COMPONENT_NAME, connection_count - shown);
            }

            heartbeat_counter = 0;
        }

        /* Check for INBOUND notifications from ICS_Inbound (non-blocking) */
        if (inbound_ready_poll()) {
            inbound_ready_handle();
        }

        /* Process lwIP timers and RX packets */
        sys_check_timeouts();

        if (rx_packets_pending) {
            rx_packets_pending = false;
            process_rx_packets();
        }

        /* Refill RX buffers OUTSIDE IRQ context to avoid IRQ storm
         * This happens in main loop after processing completes */
        refill_rx_queue();

        /* Periodic connection table cleanup to prevent exhaustion */
        loop_iterations++;
        if (loop_iterations >= CLEANUP_INTERVAL) {
            connection_cleanup_stale();
            loop_iterations = 0;
        }

        seL4_Yield();
    }

    return 0;
}
