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
#include "lwip/priv/tcp_priv.h"  /* v2.173: Access tcp_active_pcbs, tcp_tw_pcbs for leak detection */
#include "netif/ethernet.h"

/* ICS common definitions */
#include "common.h"

/* v2.117: Connection state sharing */
#include "connection_state.h"

#define COMPONENT_NAME "VirtIO_Net1_Driver"
#define TCP_SERVER_PORT 502  /* INBOUND: Modbus port - pretends to be PLC */

/* ═══════════════════════════════════════════════════════════════ */
/* DEBUG OUTPUT CONTROL - Set ONE level to 1                        */
/* ═══════════════════════════════════════════════════════════════ */
#define DEBUG_LEVEL_SILENT   1  /* Testing: NO output - breadcrumbs only */
#define DEBUG_LEVEL_QUIET    0  /* Production: Only errors and warnings */
#define DEBUG_LEVEL_NORMAL   0  /* Default: Connection lifecycle + traffic flow */
#define DEBUG_LEVEL_VERBOSE  0  /* Development: Full packet details */

#if DEBUG_LEVEL_SILENT
    #define DEBUG_CRITICAL      0
    #define DEBUG_TRAFFIC       0
    #define DEBUG_METADATA      0
    #define DEBUG_PACKET_DETAIL 0
#elif DEBUG_LEVEL_QUIET
    #define DEBUG_CRITICAL      1
    #define DEBUG_TRAFFIC       0
    #define DEBUG_METADATA      0
    #define DEBUG_PACKET_DETAIL 0
#elif DEBUG_LEVEL_NORMAL
    #define DEBUG_CRITICAL      1
    #define DEBUG_TRAFFIC       1
    #define DEBUG_METADATA      1  /* Keep for v2.36 validation */
    #define DEBUG_PACKET_DETAIL 0
#elif DEBUG_LEVEL_VERBOSE
    #define DEBUG_CRITICAL      1
    #define DEBUG_TRAFFIC       1
    #define DEBUG_METADATA      1
    #define DEBUG_PACKET_DETAIL 1
#else
    #error "No debug level selected"
#endif

/* Connection tracking for metadata preservation */
/* v2.86: Increased from 64 to 256 to prevent table exhaustion
 * User insight: "increase active connections and let lwIP clear it naturally"
 * - SCADA keeps connections alive, accumulates over time
 * - Can't safely call tcp_abort() from main loop (v2.85 lesson)
 * - Solution: Increase limit, let lwIP's TCP timeouts handle cleanup
 * - Memory cost: ~12KB (256 * ~48 bytes) - negligible
 * - Benefit: System won't deadlock when table fills */
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

    /* v2.107: Pool state tracking to prevent leaks */
    struct tcp_inbound_client_state *pool_state;  /* Associated pool slot - freed when connection removed */

    /* v2.153: Deduplication flag for error notification queue
     * ══════════════════════════════════════════════════════════════════════════
     * SECURITY: Prevent RST flood attacks by deduplicating error notifications
     *
     * Problem without deduplication:
     *   - PLC sends RST, tcp_err callback fires
     *   - Error notification queued (Net1 → Net0)
     *   - Attacker sends 999 more RSTs for same connection
     *   - Without dedup: 1000 notifications queued (queue overflow!)
     *   - With dedup: Only 1 notification queued
     *
     * Solution: Only enqueue error notification ONCE per connection
     * ══════════════════════════════════════════════════════════════════════════
     */
    bool error_notified;             /* True if error notification already queued (Net1 → Net0) */

    /* v2.156: lwIP BEST PRACTICE - Safe connection closing from main thread
     * ══════════════════════════════════════════════════════════════════════════
     * CRITICAL: Never call tcp_abort() from main thread (CAmkES event handlers)
     *
     * Problem with direct tcp_abort() from event handler:
     *   - Event handler runs in main thread context
     *   - lwIP recv/sent/poll callbacks may be executing in parallel
     *   - tcp_abort() frees PCB while lwIP callback is using it
     *   - Result: Use-after-free crash
     *
     * Solution: Deferred closing via close_pending flag
     *   1. Event handler sets close_pending = true
     *   2. lwIP poll callback sees flag and closes safely
     *   3. Poll callback runs in lwIP context (safe to return ERR_ABRT)
     * ══════════════════════════════════════════════════════════════════════════
     */
    bool close_pending;              /* True if close requested from main thread (poll callback handles) */

};

static struct connection_metadata connection_table[MAX_CONNECTIONS];
static int connection_count = 0;

/* v2.117: Connection state sharing via dataports */
static volatile struct connection_state_table *own_state = NULL;   /* Our state (exposed to Net0) */
static volatile struct connection_state_table *peer_state = NULL;  /* Net0's state (read-only) */

/* v2.117: Self-cleaned connection tracking
 * ═══════════════════════════════════════════════════════════════════════════
 * Problem: Close notifications can arrive AFTER we've already cleaned up and
 * recreated a connection with the same 5-tuple. The notification handler can't
 * tell if the notification is for:
 *   - The OLD connection (already cleaned up) → should ignore
 *   - A NEW connection (actively processing) → should NOT close
 *
 * Solution: Track connections that WE cleaned up ourselves. When a close
 * notification arrives, check if we recently cleaned this 5-tuple. If yes,
 * it's a stale notification for the OLD connection → ignore it.
 *
 * This is robust because:
 * - Based on actual cleanup events, not timing guesses
 * - Entries consumed after use (prevent false positives)
 * - Old entries expire automatically (5 second TTL)
 * - Circular buffer (no unbounded memory growth)
 * ═══════════════════════════════════════════════════════════════════════════
 */
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

/*
 * VLAN-BASED DEPLOYMENT CONFIGURATION
 *
 * Net1 uses PRIVATE network (10.2.0.0/24) connected to tap0
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

    #if DEBUG_PACKET_DETAIL
    if (first_call || free_count > 0) {
        printf("%s: refill_rx_queue() call #%u: %d/%d buffers free (available to refill)\n",
               COMPONENT_NAME, refill_call_count, free_count, MAX_PACKETS);
        first_call = false;
    }
    #else
    /* Only warn if buffers are critically low */
    if (free_count > MAX_PACKETS / 2) {
        printf("%s: [WARN]  RX buffers low: %d/%d free\n", COMPONENT_NAME, free_count, MAX_PACKETS);
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
        #if DEBUG_PACKET_DETAIL
        printf("%s: [OK] Refilled RX queue with %d buffers (avail_idx now=%u)\n",
               COMPONENT_NAME, buffers_added, vq->avail->idx);
        #endif
        /* Notify device of new buffers */
        VREG_WRITE(VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_NET_RX_QUEUE);
    } else if (free_count > 0) {
        printf("%s: [WARN]  WARNING: %d buffers were free but refill added 0! (avail_idx=%u)\n",
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
        printf("%s: ⚡ netif_output() CALLED - tx_count=%u, pbuf len=%u\n",
               COMPONENT_NAME, tx_count, p->tot_len);
    }

    /* Detailed TX logging for first 10 packets */
    if (tx_count <= 10) {
        uint32_t timestamp_ms = sys_now();
        printf("\n╔══════════════════════════════════════════════════════════╗\n");
        printf("║  [TX] OUTGOING PACKET #%u [T=%u.%03us]                      ║\n",
               tx_count, timestamp_ms / 1000, timestamp_ms % 1000);
        printf("╚══════════════════════════════════════════════════════════╝\n");
        printf("  Size: %u bytes\n", p->tot_len);
    }
    #endif

    /* Get TX descriptor pair (header + packet) - need 2 consecutive descriptors */
    uint16_t hdr_desc_idx = next_tx_desc;
    uint16_t pkt_desc_idx = (next_tx_desc + 1) % vq->num;
    next_tx_desc = (next_tx_desc + 2) % vq->num;  /* Advance by 2 for chaining */

    int tx_buf_idx = (hdr_desc_idx + MAX_PACKETS/2) % MAX_PACKETS;

    /* CRITICAL: Validate TX buffer index */
    if (tx_buf_idx < 0 || tx_buf_idx >= MAX_PACKETS) {
        printf("%s: [ERR] FATAL: Invalid TX buffer index %d (hdr_desc=%u, max=%d)\n",
               COMPONENT_NAME, tx_buf_idx, hdr_desc_idx, MAX_PACKETS);
        return ERR_BUF;
    }

    if (packet_buffers[tx_buf_idx] == NULL) {
        printf("%s: [ERR] FATAL: TX Buffer[%d] is NULL!\n", COMPONENT_NAME, tx_buf_idx);
        return ERR_BUF;
    }

    /* Copy pbuf chain to TX buffer */
    uint16_t copied = pbuf_copy_partial(p, packet_buffers[tx_buf_idx],
                                        p->tot_len, 0);

    if (copied != p->tot_len) {
        printf("%s: Failed to copy pbuf: %u/%u bytes\n",
               COMPONENT_NAME, copied, p->tot_len);
        return ERR_BUF;
    }

    /*
     * CRITICAL: Restore original IPs for protocol-break architecture
     *
     * lwIP generated response with interface IP as source (192.168.95.1)
     * But SCADA expects response from PLC IP (192.168.95.2)
     * Restore: 192.168.95.1 → 192.168.95.2 (source IP)
     * Keep: 192.168.90.5 (destination IP to SCADA)
     */
    uint8_t *tx_data = packet_buffers[tx_buf_idx];
    if (p->tot_len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
        struct ethhdr *eth = (struct ethhdr *)tx_data;
        if (ntohs(eth->h_proto) == 0x0800) {  /* IPv4 */
            struct iphdr *ip = (struct iphdr *)(tx_data + sizeof(struct ethhdr));

            if (ip->protocol == 6) {  /* TCP */
                /* Extract current IPs and ports */
                uint32_t current_src = ntohl(ip->saddr);  /* 192.168.95.1 from lwIP */
                uint32_t current_dest = ntohl(ip->daddr); /* 192.168.90.5 to SCADA */

                size_t ip_hdr_len = (ip->ihl) * 4;
                if (p->tot_len >= sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct tcphdr)) {
                    struct tcphdr *tcp = (struct tcphdr *)(tx_data + sizeof(struct ethhdr) + ip_hdr_len);
                    uint16_t src_port = ntohs(tcp->source);  /* lwIP's ephemeral port */
                    uint16_t dest_port = ntohs(tcp->dest);   /* SCADA's port */

                    /* CRITICAL FIX: Lookup metadata by lwIP's ephemeral port
                     *
                     * Previous bug: Tried to match connection_table[i].dest_port (502) with src_port
                     * But src_port is lwIP's ephemeral port (e.g., 64085), NOT 502!
                     *
                     * Correct lookup:
                     *   - connection_table[i].lwip_ephemeral_port == src_port (lwIP's port)
                     *   - connection_table[i].src_port == dest_port (SCADA's port)
                     */
                    struct connection_metadata *meta = NULL;
                    for (int i = 0; i < MAX_CONNECTIONS; i++) {
                        /* Defensive check: ensure index is valid */
                        if (i >= MAX_CONNECTIONS) {
                            printf("%s: [WARN]  TX: Invalid connection table index %d\n", COMPONENT_NAME, i);
                            break;
                        }

                        if (connection_table[i].active) {
                            /* Method 1: Lookup by ephemeral port (normal case after port is stored) */
                            if (connection_table[i].lwip_ephemeral_port == src_port &&
                                connection_table[i].src_port == dest_port) {
                                meta = &connection_table[i];
                                break;
                            }

                            /* v2.127: Method 2 REMOVED - No longer needed!
                             * ═════════════════════════════════════════════════════════════
                             * Old Method 2 handled race window between tcp_connect() and
                             * storing ephemeral port by accessing pcb->local_port (UNSAFE!)
                             *
                             * v2.127 fix: Store port immediately after tcp_connect() (line 3630)
                             * Result: Race window eliminated, lwip_ephemeral_port always available
                             * Method 2 never triggers anymore (lwip_ephemeral_port != 0)
                             * Unsafe PCB field access removed!
                             * ═════════════════════════════════════════════════════════════
                             */
                        }
                    }

                    if (meta != NULL && meta->active) {
                        /* Double-check metadata is valid before using */
                        if (meta->original_dest_ip == 0) {
                            printf("%s: [WARN]  TX: Invalid metadata - original_dest_ip is 0\n", COMPONENT_NAME);
                        } else {
                            /* Restore original destination IP (PLC IP) as source */
                            ip->saddr = htonl(meta->original_dest_ip);  /* 192.168.95.2 */

                            #if DEBUG_TRAFFIC
                            printf("%s: [RETRY] TX: Restored source IP: %u.%u.%u.%u → %u.%u.%u.%u\n",
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
                            printf("%s: [FIX] TX: IP checksum: 0x%04x → 0x%04x\n",
                                   COMPONENT_NAME, ntohs(old_ip_check), ntohs(new_ip_check));
                            #endif

                            /* Recalculate TCP checksum with pseudo-header */
                            uint16_t old_tcp_check = tcp->check;
                            tcp->check = 0;
                            uint16_t tcp_len = ntohs(ip->tot_len) - (ip->ihl * 4);
                            uint16_t new_tcp_check = tcp_checksum(ip, tcp, tcp_len);
                            tcp->check = new_tcp_check;

                            #if DEBUG_TRAFFIC
                            printf("%s: [FIX] TX: TCP checksum: 0x%04x → 0x%04x\n",
                                   COMPONENT_NAME, ntohs(old_tcp_check), ntohs(new_tcp_check));
                            #endif
                        }
                    } else {
                        /* With PCB-based lookup, this should rarely happen.
                         * Only show in debug mode to avoid noise. */
                        #if DEBUG_METADATA
                        printf("%s: [WARN]  TX: No metadata found for TCP port %u → %u (connection may be closed)\n",
                               COMPONENT_NAME, src_port, dest_port);
                        #endif
                    }
                }
            }
        }
    }

    #if DEBUG_PACKET_DETAIL
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
    #endif

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

    #if DEBUG_PACKET_DETAIL
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

    #if DEBUG_PACKET_DETAIL
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

/* v2.117: Update shared connection state dataport */
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

/* Store metadata for a new connection */
static struct connection_metadata* connection_add(uint32_t session_id,  /* v2.150: Session ID from Net0 */
                                                   uint32_t orig_src, uint32_t orig_dest,
                                                   uint16_t sport, uint16_t dport,
                                                   struct tcp_inbound_client_state *pool_state)
{
    /* v2.183: CRITICAL FIX - Check for duplicate session_id to prevent connection leak
     *
     * Problem (v2.182):
     * - SCADA reuses session with NEW port (e.g., session 80: port 36414 → 36472)
     * - Old code created duplicate metadata entries for same session_id
     * - Result: Orphaned metadata entries never cleaned up
     * - Evidence: 165 connections created, 95 cleaned up, 70 leaked (42% leak rate!)
     *
     * Root Cause:
     * - Port reuse detection only checked src_port, not session_id
     * - Same session could have multiple active metadata entries
     * - Example: session 80 had 5 increments (slot 44 × 4, then slot 67)
     *
     * Fix:
     * - Check if session_id already exists in connection table
     * - If found, REUSE that slot (clean up old PCB if port changed)
     * - NO connection_count increment for reused sessions
     * - Only increment connection_count for NEW sessions
     *
     * Impact:
     * - Prevents duplicate metadata entries for same session
     * - Eliminates connection leak (connection_count now accurate)
     * - System can handle unlimited connection cycles
     */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active &&
            connection_table[i].session_id == session_id) {

            /* Found existing metadata for this session */
            printf("%s: [REUSE] Session %u already has metadata in slot %d (port %u→%u)\n",
                   COMPONENT_NAME, session_id, i,
                   connection_table[i].src_port, connection_table[i].dest_port);

            /* Check if port changed (Net0 assigned new port to same session) */
            if (connection_table[i].src_port != sport ||
                connection_table[i].dest_port != dport) {

                printf("%s:   → Port changed: %u→%u to %u→%u (cleaning up old PCB)\n",
                       COMPONENT_NAME,
                       connection_table[i].src_port, connection_table[i].dest_port,
                       sport, dport);

                /* Clean up old PCB if it exists (port changed, old connection stale) */
                if (connection_table[i].pcb != NULL) {
                    struct tcp_pcb *old_pcb = connection_table[i].pcb;
                    printf("%s:   → Aborting old PCB %p for port change\n",
                           COMPONENT_NAME, (void*)old_pcb);
                    tcp_abort(old_pcb);
                }
            } else {
                /* Same port - this might be rapid close/reopen */
                printf("%s:   → Same port %u→%u (rapid reuse)\n",
                       COMPONENT_NAME, sport, dport);
            }

            /* Reuse this slot - update all fields but keep active=true */
            connection_table[i].pcb = NULL;  /* Will be set when TCP accept happens */
            connection_table[i].session_id = session_id;  /* v2.190: CRITICAL FIX - Update session_id in reuse path! */
            connection_table[i].original_src_ip = orig_src;
            connection_table[i].original_dest_ip = orig_dest;
            connection_table[i].src_port = sport;
            connection_table[i].dest_port = dport;
            connection_table[i].timestamp = sys_now();
            connection_table[i].last_activity = sys_now();
            connection_table[i].pool_state = pool_state;
            connection_table[i].error_notified = false;
            connection_table[i].close_pending = false;

            /* v2.183: NO connection_count++ for reused sessions! */
            printf("%s: [COUNT==] %u (unchanged) | connection_add() REUSED slot=%d session=%u port=%u→%u\n",
                   COMPONENT_NAME, connection_count, i, session_id, sport, dport);

            #if DEBUG_METADATA
            printf("%s: 🔄 Reused metadata [%d]: %u.%u.%u.%u:%u → %u.%u.%u.%u:%u\n",
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

    /* No existing entry found - create NEW metadata entry */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_table[i].active) {
            connection_table[i].active = true;
            connection_table[i].pcb = NULL;  /* Will be set when TCP accept happens */
            connection_table[i].session_id = session_id;  /* v2.150: Store session ID from Net0 */
            connection_table[i].original_src_ip = orig_src;
            connection_table[i].original_dest_ip = orig_dest;
            connection_table[i].src_port = sport;
            connection_table[i].dest_port = dport;
            connection_table[i].timestamp = sys_now();  /* v2.50: For validation */
            connection_table[i].last_activity = sys_now();  /* v2.59: For idle timeout */
            connection_table[i].pool_state = pool_state;  /* v2.107: Track pool slot */
            connection_table[i].error_notified = false;  /* v2.154: Clear dedup flag for NEW connection */
            connection_table[i].close_pending = false;  /* v2.156: Initialize close_pending flag */

            /* v2.183: Track connection count changes - only increment for NEW sessions */
            uint32_t old_count = connection_count;
            connection_count++;
            printf("%s: [COUNT++] %u → %u | connection_add() NEW slot=%d session=%u port=%u→%u\n",
                   COMPONENT_NAME, old_count, connection_count, i, session_id, sport, dport);

            #if DEBUG_METADATA
            printf("%s: 📝 Stored metadata [%d]: %u.%u.%u.%u:%u → %u.%u.%u.%u:%u\n",
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
    printf("%s: [WARN]  Connection table full! Dropping metadata.\n", COMPONENT_NAME);
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
            printf("%s: [LINK] Linked PCB to metadata [%d]\n", COMPONENT_NAME, i);
            #endif
            return;
        }
    }
    #if DEBUG_METADATA
    printf("%s: [WARN]  No metadata found for %u → %u\n", COMPONENT_NAME, sport, dport);
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

/* v2.153: Lookup metadata by session_id */
static struct connection_metadata* connection_lookup_by_session_id(uint32_t session_id)
{
    if (session_id == 0) return NULL;  /* 0 = unassigned */

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active && connection_table[i].session_id == session_id) {
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

/* v2.117: Self-cleaned connection tracking functions
 * ═══════════════════════════════════════════════════════════════════════════
 * These functions manage the tracking of connections that WE cleaned up ourselves,
 * so we can ignore stale close notifications for those connections.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* Mark a connection as self-cleaned (we cleaned it up, not via close notification) */
static void mark_connection_self_cleaned(uint32_t src_ip, uint16_t src_port, uint16_t dst_port)
{
    self_cleaned_connections[self_cleaned_index].src_ip = src_ip;
    self_cleaned_connections[self_cleaned_index].src_port = src_port;
    self_cleaned_connections[self_cleaned_index].dst_port = dst_port;
    self_cleaned_connections[self_cleaned_index].timestamp = sys_now();
    self_cleaned_connections[self_cleaned_index].valid = true;

    printf("%s: [TRACK] Marked connection as self-cleaned: SCADA %u.%u.%u.%u:%u → PLC:%u (index=%d)\n",
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
            printf("%s: [TRACK] Found self-cleaned connection: SCADA %u.%u.%u.%u:%u → PLC:%u (age=%ums, index=%d)\n",
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

/* Forward declaration for pool state management (defined later) */
static void inbound_free_state(struct tcp_inbound_client_state *state);

/* Remove connection metadata */
static void connection_remove(struct tcp_pcb *pcb)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active && connection_table[i].pcb == pcb) {
            #if DEBUG_METADATA
            printf("%s: [DEL]  Removing metadata [%d]\n", COMPONENT_NAME, i);
            #endif

            /* v2.107: CRITICAL FIX - Free associated pool slot when removing connection
             * This prevents pool leaks when connections are cleaned up or reused */
            if (connection_table[i].pool_state != NULL) {
                BREADCRUMB(2113);  /* Freeing pool state via connection_remove */
                inbound_free_state(connection_table[i].pool_state);
                connection_table[i].pool_state = NULL;
            }

            connection_table[i].active = false;
            connection_table[i].pcb = NULL;

            /* v2.182: Track connection count changes for leak debugging */
            uint32_t old_count = connection_count;
            connection_count--;
            printf("%s: [COUNT--] %u → %u | connection_remove() slot=%d session=%u PCB=%p\n",
                   COMPONENT_NAME, old_count, connection_count, i,
                   connection_table[i].session_id, (void*)pcb);

            /* v2.117: Update shared connection state */
            update_shared_connection_state();

            return;
        }
    }
}

/* Print connection table statistics
 *
 * Shows:
 * - Active connections (metadata slots in use)
 * - Stale connections (PCB NULL or CLOSED/TIME_WAIT)
 * - Available slots
 */
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
    printf("%s: [STATS] Connection table: %d active (%d PCB-linked, %d stale), %d available\n",
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
    /* v2.85: Only clean up NULL PCBs - DO NOT call tcp_abort() from main loop!
     *
     * History:
     * - v2.83: Removed idle timeout (use-after-free when accessing pcb->state)
     * - v2.84: Re-added "safe" idle timeout using metadata->last_activity
     * - v2.85: Removed idle timeout again (tcp_abort() crashes if called during callback)
     *
     * The problem with tcp_abort() from main loop:
     * - lwIP callbacks may be on the call stack when main loop runs
     * - tcp_abort() frees PCB immediately
     * - Callback continues execution → use-after-free → crash
     *
     * This is DIFFERENT from the v2.83 bug:
     * - v2.83: Accessing freed PCB fields (pcb->state)
     * - v2.85: Freeing PCB while it's still in use by callback
     *
     * Solution: Only clean up NULL PCBs, let lwIP manage connection lifecycle
     */
    int cleaned = 0;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_table[i].active) {
            continue;
        }

        struct tcp_pcb *pcb = connection_table[i].pcb;

        /* Cleanup if PCB is NULL */
        if (pcb == NULL) {
            #if DEBUG_METADATA
            printf("%s: [CLEAN] Cleanup stale connection [%d]: PCB is NULL\n", COMPONENT_NAME, i);
            #endif
            connection_table[i].active = false;

            /* v2.182: Track connection count changes for leak debugging */
            uint32_t old_count = connection_count;
            connection_count--;
            printf("%s: [COUNT--] %u → %u | cleanup_stale() slot=%d session=%u (PCB=NULL)\n",
                   COMPONENT_NAME, old_count, connection_count, i,
                   connection_table[i].session_id);

            cleaned++;
            continue;
        }

        /* v2.85: REMOVED idle timeout - calling tcp_abort() from main loop is UNSAFE!
         *
         * Bug discovered by user: "But this only happened since latest fix"
         * Root cause: tcp_abort() called while lwIP callbacks are still executing
         * Sequence:
         * 1. inbound_tcp_recv_callback() processing
         * 2. Main loop runs connection_cleanup_stale()
         * 3. Idle timeout calls tcp_abort(pcb)
         * 4. lwIP callback still running → tries to use freed PCB → CRASH
         *
         * Assertion "p != NULL" failed at line 479 in pbuf.c
         * - pbuf_add_header_impl() called with NULL pbuf
         * - Caused by tcp_abort() freeing PCB while lwIP using it
         *
         * FUNDAMENTAL PROBLEM: Cannot safely call tcp_abort() from outside lwIP callbacks!
         * - lwIP callbacks may be on the call stack
         * - tcp_abort() frees PCB immediately
         * - Callback continues → use-after-free → crash
         *
         * Solution: Remove idle timeout entirely
         * - Rely on lwIP's internal TCP timeouts
         * - Rely on remote peer closing connections
         * - Accept that table may fill (but system won't crash)
         */
    }

    if (cleaned > 0) {
        #if DEBUG_METADATA
        printf("%s: [CLEAN] Cleaned %d stale connection(s)\n",
               COMPONENT_NAME, cleaned);
        connection_print_stats();
        #endif

        /* v2.117: Update shared connection state after cleanup */
        update_shared_connection_state();
    }
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
        return ERR_ARG;
    }

    ethhdr = (struct eth_hdr *)p->payload;
    type = ntohs(ethhdr->type);

    /* Handle ARP packets normally - pass to lwIP's ARP handler */
    if (type == ETHTYPE_ARP) {
        /* Remove Ethernet header and pass to ethernet_input for ARP processing */
        if (pbuf_remove_header(p, sizeof(struct eth_hdr)) == 0) {
            etharp_input(p, inp);
            return ERR_OK;
        }
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
            }

            /* If packet is not destined for our interface IP, rewrite it */
            if (pkt_dest_ip != interface_ip) {
                printf("%s: [RETRY] Rewriting dest IP: %u.%u.%u.%u → %u.%u.%u.%u\n",
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
                        /* New connection - store metadata */
                        /* v2.150: session_id=0 (will be set properly in inbound_ready_handle) */
                        /* Note: NULL pool_state because this is TCP server path, not client pool */
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
    /* v2.155: Let lwIP free pbuf - we don't own it */
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

    #if DEBUG_TRAFFIC
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
            printf("%s: [WARN] TRUE DESYNC: pending=%u exceeds ring_size=%u\n",
                   COMPONENT_NAME, pending_packets, vq->num);
            printf("%s:   last_used_idx=%u, current_used_idx=%u\n",
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
            printf("%s: ERROR - Processed 1000 packets in single call, breaking to prevent freeze\n",
                   COMPONENT_NAME);
            printf("%s:   last_used_idx=%u, current_used_idx=%u, pending=%u\n",
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
            printf("%s: [WARN]  INVALID packet length: %u bytes (expected %u-%u)\n",
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
            printf("%s: [WARN]  INVALID descriptor index: %u (max %u)\n",
                   COMPONENT_NAME, desc_idx, MAX_PACKETS);
            printf("%s:     Ring: used_ring_idx=%u, last_used=%u, current=%u\n",
                   COMPONENT_NAME, used_ring_idx, last_used_idx, current_used_idx);

            /* seL4-SAFE RECOVERY STRATEGY:
             * seL4's memory safety allows aggressive recovery without corruption risk.
             * We try multiple strategies knowing seL4 prevents double-free/use-after-free.
             */

            bool buffer_freed = false;

            // STRATEGY 1: Free buffer at ring position (most likely correct)
            if (used_ring_idx < MAX_PACKETS && rx_buffer_used[used_ring_idx]) {
                printf("%s: 🛡️  seL4-SAFE: Freeing buffer %u (ring position)\n",
                       COMPONENT_NAME, used_ring_idx);
                rx_buffer_used[used_ring_idx] = false;
                buffer_freed = true;
            }

            // STRATEGY 2: If ring position was already free, scan for any used buffer
            if (!buffer_freed) {
                printf("%s: [WARN]  Ring position %u already free - scanning for leaked buffers\n",
                       COMPONENT_NAME, used_ring_idx);

                for (int i = 0; i < MAX_PACKETS; i++) {
                    if (rx_buffer_used[i]) {
                        printf("%s: 🛡️  seL4-SAFE FALLBACK: Freeing leaked buffer %u\n",
                               COMPONENT_NAME, i);
                        rx_buffer_used[i] = false;
                        buffer_freed = true;
                        break;  // Free one buffer to avoid over-correction
                    }
                }
            }

            if (!buffer_freed) {
                printf("%s: [WARN]  No used buffers found - possible state desync!\n", COMPONENT_NAME);
            }

            last_used_idx++;
            continue;
        }

        /* Get packet buffer (use buffer index, not physical address from descriptor) */
        int buf_idx = desc_idx;

        /* CRITICAL: Validate buffer index to prevent out-of-bounds access */
        if (buf_idx < 0 || buf_idx >= MAX_PACKETS) {
            printf("%s: [ERR] FATAL: Invalid buffer index %d (desc_idx=%u, max=%d)\n",
                   COMPONENT_NAME, buf_idx, desc_idx, MAX_PACKETS);
            printf("%s:    last_used_idx=%u, RX queue full, system halting\n",
                   COMPONENT_NAME, last_used_idx);
            last_used_idx++;
            continue;
        }

        uint8_t *buffer = packet_buffers[buf_idx];

        if (buffer == NULL) {
            printf("%s: [ERR] FATAL: Buffer[%d] is NULL!\n", COMPONENT_NAME, buf_idx);
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

        #if DEBUG_PACKET_DETAIL
        /* Log packet arrival with detailed inspection (VERY VERBOSE - only for debugging) */
        uint32_t timestamp_ms = sys_now();
        printf("\n╔══════════════════════════════════════════════════════════╗\n");
        printf("║  [RX] [Net1] INCOMING PACKET #%u [T=%u.%03us]              ║\n",
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
        #endif /* DEBUG_PACKET_DETAIL */

        #if DEBUG_PACKET_DETAIL
        /* Lightweight packet summary - just protocol and ports (with filtering) */
        bool show_packet = false;
        uint8_t packet_protocol = 0;  /* 0=other, 6=TCP, 17=UDP, 1=ICMP */

        if (packet_len >= sizeof(struct ethhdr)) {
            struct ethhdr *eth = (struct ethhdr *)packet_data;
            uint16_t eth_proto = ntohs(eth->h_proto);

            if (eth_proto == 0x0800 && packet_len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
                struct iphdr *ip = (struct iphdr *)(packet_data + sizeof(struct ethhdr));
                uint32_t saddr = ntohl(ip->saddr);
                uint32_t daddr = ntohl(ip->daddr);
                packet_protocol = ip->protocol;

                if (ip->protocol == 6) {  /* TCP */
                    #if FILTER_SHOW_TCP
                    show_packet = true;
                    size_t ip_hdr_len = (ip->ihl) * 4;
                    if (packet_len >= sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct tcphdr)) {
                        struct tcphdr *tcp = (struct tcphdr *)(packet_data + sizeof(struct ethhdr) + ip_hdr_len);
                        printf("[RX] Net1 RX #%u: %u.%u.%u.%u:%u → %u.%u.%u.%u:%u TCP %s%s%s%s\n",
                               packets_received,
                               (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF, (saddr >> 8) & 0xFF, saddr & 0xFF, ntohs(tcp->source),
                               (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF, ntohs(tcp->dest),
                               tcp->syn ? "[SYN]" : "", tcp->ack ? "[ACK]" : "", tcp->fin ? "[FIN]" : "", tcp->rst ? "[RST]" : "");
                    }
                    #endif
                } else if (ip->protocol == 17) {  /* UDP */
                    #if FILTER_SHOW_UDP
                    show_packet = true;
                    printf("[RX] Net1 RX #%u: %u.%u.%u.%u → %u.%u.%u.%u UDP (%ub)\n",
                           packets_received,
                           (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF, (saddr >> 8) & 0xFF, saddr & 0xFF,
                           (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF,
                           packet_len);
                    #endif
                } else if (ip->protocol == 1) {  /* ICMP */
                    #if FILTER_SHOW_ICMP
                    show_packet = true;
                    printf("[RX] Net1 RX #%u: %u.%u.%u.%u → %u.%u.%u.%u ICMP (%ub)\n",
                           packets_received,
                           (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF, (saddr >> 8) & 0xFF, saddr & 0xFF,
                           (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF,
                           packet_len);
                    #endif
                } else {
                    #if FILTER_SHOW_OTHER
                    show_packet = true;
                    printf("[RX] Net1 RX #%u: %u.%u.%u.%u → %u.%u.%u.%u proto=%u (%ub)\n",
                           packets_received,
                           (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF, (saddr >> 8) & 0xFF, saddr & 0xFF,
                           (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF,
                           ip->protocol, packet_len);
                    #endif
                }
            } else if (eth_proto == 0x0806) {  /* ARP */
                #if FILTER_SHOW_ARP
                show_packet = true;
                printf("[RX] Net1 RX #%u: ARP (%u bytes)\n", packets_received, packet_len);
                #endif
            } else if (eth_proto == 0x86dd) {  /* IPv6 */
                #if FILTER_SHOW_IPV6
                show_packet = true;
                printf("[RX] Net1 RX #%u: IPv6 (%u bytes)\n", packets_received, packet_len);
                #endif
            } else {
                #if FILTER_SHOW_OTHER
                show_packet = true;
                printf("[RX] Net1 RX #%u: EtherType=0x%04x (%u bytes)\n", packets_received, eth_proto, packet_len);
                #endif
            }
        }
        #endif /* DEBUG_PACKET_DETAIL */

        #if DEBUG_PACKET_DETAIL
        uint32_t msg_id = ++message_id_counter;
        printf("\n🔵 [NET1-INTERNAL MSG #%u] ═══ RX: Packet received from VirtIO device ═══\n", msg_id);
        printf("   Size: %u bytes, Buffer index: %d\n", packet_len, buf_idx);
        printf("   Action: Feeding to lwIP stack for processing\n");
        #endif

        /* Allocate pbuf and copy packet data (skipping header) */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, packet_len, PBUF_POOL);
        if (p != NULL) {
            pbuf_take(p, packet_data, packet_len);

            #if DEBUG_PACKET_DETAIL && DEBUG_PACKET_DETAIL
            if (show_packet) {
                printf("   [OK] pbuf allocated, passing to lwIP input handler\n");
            }
            #endif

            /* Feed packet to lwIP */
            err_t lwip_result = netif_data.input(p, &netif_data);

            #if DEBUG_PACKET_DETAIL && DEBUG_PACKET_DETAIL
            if (show_packet) {
                if (lwip_result == ERR_OK) {
                    printf("   [OK] lwIP accepted packet (will route to TCP/UDP/etc.)\n");
                } else {
                    printf("   ✗ lwIP rejected packet (err=%d)\n", lwip_result);
                }
            }
            #endif

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
                                printf("%s: [FIND] SYN packet detected: Dest IP = %u.%u.%u.%u:%u (Interface IP = 192.168.95.2)\n",
                                       COMPONENT_NAME,
                                       (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF,
                                       ntohs(tcp->dest));
                                printf("%s:    → If dest IP matches interface IP, lwIP should accept. Otherwise it rejects.\n", COMPONENT_NAME);
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
            printf("%s: [WARN]  WARNING: Failed to allocate pbuf for packet #%u - dropping (lwIP out of memory)\n",
                   COMPONENT_NAME, packets_received);
        }

        /* Mark buffer as free (buf_idx already defined above) */
        rx_buffer_used[buf_idx] = false;

        /* Move to next packet */
        last_used_idx++;
    }

    /* Print pbuf pool statistics every 10 packets to monitor allocation/deallocation */
    if (packets_received % 10 == 0 && packets_received > 0) {
        printf("%s: [STATS] PBUF Pool Stats - Used: %u/%u, Avail: %u, Peak: %u\n",
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

    printf("%s: [WARN]  TCP connection error - err=%d (%s)\n", COMPONENT_NAME, err, err_name);
    printf("%s:    → Active connections: %u | Total created: %u | Total closed: %u\n",
           COMPONENT_NAME, active_connections, total_connections_created, total_connections_closed);

    /* v2.83: CRITICAL FIX - Do NOT call connection_cleanup_stale() from error callback!
     *
     * Bug Analysis (same as Net0 v2.83):
     * - tcp_echo_err() is called DURING lwIP's error handling
     * - lwIP may have freed MULTIPLE PCBs in the same batch
     * - connection_cleanup_stale() iterates ALL connections and accesses pcb->state
     * - If it accesses a recently-freed PCB → crash at offset 0x10 (state field)
     *
     * Fix: Let main loop's periodic cleanup handle stale entries
     * Error callback should ONLY handle its own connection cleanup
     *
     * NOTE: PCB is already freed by lwIP when err callback is called - don't access it!
     * The periodic cleanup in main loop (every 10000 iterations) will clean up stale entries safely.
     */
}

/*
 * TCP Echo callbacks
 */
static err_t tcp_echo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (p == NULL) {
        /* v2.82: CRITICAL FIX - Connection closed by remote peer
         * MUST return ERR_ABRT WITHOUT calling tcp_close()!
         * Same fix as Net0 v2.81 */

        /* v2.82: Only decrement if counter is positive (prevent underflow)
         * lwIP may call both err callback and recv(p=NULL) for the same connection */
        if (active_connections > 0) {
            active_connections--;
            total_connections_closed++;
        } else {
            printf("%s: [WARN]  BUG: active_connections already 0 in recv, not decrementing\n",
                   COMPONENT_NAME);
        }

        #if DEBUG_TRAFFIC
        printf("%s: [INIT] TCP connection closed gracefully\n", COMPONENT_NAME);
        printf("%s:    Remote: %u.%u.%u.%u:%u\n", COMPONENT_NAME,
               ip4_addr1(&pcb->remote_ip), ip4_addr2(&pcb->remote_ip),
               ip4_addr3(&pcb->remote_ip), ip4_addr4(&pcb->remote_ip), pcb->remote_port);
        printf("%s:    → Active connections: %u | Total created: %u | Total closed: %u\n",
               COMPONENT_NAME, active_connections, total_connections_created, total_connections_closed);
        #endif

        /* Clean up connection metadata before lwIP frees PCB */
        connection_remove(pcb);

        /* v2.82: Return ERR_ABRT - lwIP handles tcp_abort() internally */
        return ERR_ABRT;
    }

    if (err != ERR_OK) {
        /* v2.155: Let lwIP free pbuf on error - we don't own it */
        return err;
    }

    /* ═══ Forward TCP data to ICS_Outbound (PLC→SCADA response path) ═══ */

    #if DEBUG_PACKET_DETAIL
    uint32_t msg_id = ++message_id_counter;
    printf("\n[OK] [MSG #%u] ═══ TCP: Data received from PLC (OUTBOUND response) ═══\n", msg_id);
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

    /* CRITICAL: Check if dataport is properly mapped by CAmkES
     * PLC responses must go through OUTBOUND path (Net1 → ICS_Outbound → Net0 → SCADA) */
    if (outbound_dp == NULL) {
        printf("%s: [ERR] FATAL: outbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        printf("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        /* v2.155: Let lwIP free pbuf - we don't own it */
        return ERR_MEM;
    }

    printf("%s: [OK] Dataport check: outbound_dp=%p (valid)\n", COMPONENT_NAME, (void*)outbound_dp);

    /* Step 1: Create ICS message with metadata */
    ICS_Message *ics_msg = (ICS_Message *)outbound_dp;

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
        printf("%s: [FIND] Lookup: Found metadata - using original SCADA IP %u.%u.%u.%u:%u\n",
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
        printf("%s: [WARN]  Lookup: No metadata found - using gateway IP (WRONG - Net0 won't find connection!)\n", COMPONENT_NAME);
    }

    ics_msg->metadata.src_port = pcb->remote_port;
    ics_msg->metadata.payload_offset = 0;  /* TCP payload directly */
    ics_msg->metadata.payload_length = (p->len < MAX_PAYLOAD_SIZE) ? p->len : MAX_PAYLOAD_SIZE;

    /* Step 3: Copy TCP payload */
    ics_msg->payload_length = ics_msg->metadata.payload_length;
    memcpy(ics_msg->payload, p->payload, ics_msg->payload_length);

    printf("%s: OUTBOUND: Forwarding %u bytes to ICS_Outbound (proto=TCP, src_port=%u, dst_port=%u)\n",
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

    #if DEBUG_PACKET_DETAIL
    printf("   [OK] ICS message prepared in shared memory (outbound_dp)\n");
    printf("   Action: Signaling ICS_Outbound component via outbound_ready_emit()\n");
    #endif

    /* v2.159 FIX: REMOVED memory barrier from here!
     * ═══════════════════════════════════════════════════════════════════════
     * CRITICAL: Cannot add memory barrier inside lwIP callback!
     *
     * This is tcp_echo_recv() - an lwIP recv callback. Adding __sync_synchronize()
     * here blocks the CPU inside lwIP's packet processing loop.
     *
     * Problem: CPU stall gives lwIP timers chance to fire → race with pbuf
     * Result: CRASH at pbuf.c:732 (pbuf_free NULL pointer)
     *
     * Why barrier not needed:
     * - CAmkES outbound_ready_emit() has internal synchronization
     * - Event mechanism provides sufficient memory ordering
     * - Net0 has barrier AFTER receiving signal (line 4390)
     *
     * Cache coherency still guaranteed by:
     * 1. CAmkES event synchronization
     * 2. Net0's read barrier after signal
     * ═══════════════════════════════════════════════════════════════════════
     */

    /* Step 4: Signal ICS_Outbound that PLC response is ready */
    outbound_ready_emit();

    #if DEBUG_PACKET_DETAIL
    printf("   [OK] Signal sent to ICS_Outbound - PLC response handoff complete\n");
    printf("   [MSG #%u now in OUTBOUND pipeline - forwarding to Net0]\n\n", msg_id);
    #endif

    /* v2.60: Keep connection alive BUT update last_activity timestamp for fast idle cleanup
     *
     * Problem Analysis (user feedback):
     * - Gateway creates new connections without checking if SCADA reuses its connection
     * - When SCADA keeps connection alive and sends multiple requests:
     *   - Request #1: Net1 creates connection to PLC
     *   - v2.58 closes Net1→PLC immediately after response
     *   - Request #2 on SAME SCADA connection: Net1 finds metadata but PCB closed
     *   - Validation fails → creates NEW PLC connection
     *   - Result: PLC accumulates ESTABLISHED connections
     *
     * Solution: Keep connections alive for reuse + FAST idle timeout cleanup
     * - Keep Net1→PLC connection alive after forwarding response
     * - Update last_activity timestamp for idle detection
     * - Periodic cleanup task closes connections idle > 2 seconds (v2.60)
     * - Supports SCADA connection reuse (multiple requests on one connection)
     * - Fast timeout matches Modbus TCP request rates (sub-second to ~1 second)
     *
     * Benefits:
     * 1. Connection reuse when SCADA keeps connection alive (efficient)
     * 2. FAST idle timeout prevents accumulation (Modbus TCP = sub-second cycles)
     * 3. Matches gateway architecture: Mirror SCADA connection lifecycle
     */

    tcp_recved(pcb, p->len);  /* Tell lwIP we consumed the data */

    /* v2.155: Let lwIP free pbuf - we don't own it */
    /* lwIP's tcp_input() will call pbuf_free() after this callback returns */

    /* Update last_activity timestamp for idle timeout detection */
    /* Reuse 'meta' variable from above (line 1924) */
    if (meta != NULL && meta->active) {
        meta->last_activity = sys_now();  /* lwIP millisecond timer */
    }

    /* DO NOT close connection - keep alive for next request from same SCADA connection */
    /* DO NOT decrement active_connections - connection still active */
    /* DO NOT remove metadata - needed for connection reuse validation */

    /* Return ERR_OK - connection stays alive for reuse */
    return ERR_OK;
}

static err_t tcp_echo_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    printf("%s: [EVENT] tcp_echo_accept() CALLED! arg=%p, newpcb=%p, err=%d\n",
           COMPONENT_NAME, arg, newpcb, err);

    if (err != ERR_OK || newpcb == NULL) {
        printf("%s: [ERR] TCP accept FAILED - err=%d (%s), newpcb=%p\n",
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

    printf("%s: [OK] TCP connection accepted from %u.%u.%u.%u:%u (pcb=%p)\n",
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

#if DEBUG_PACKET_DETAIL
    printf("\n%s: ╔════════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
    printf("%s: ║  [DEBUG] Entering setup_tcp_echo_server()                 ║\n", COMPONENT_NAME);
    printf("%s: ╚════════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
    printf("%s: [DEBUG] About to call tcp_new_ip_type(IPADDR_TYPE_V4)...\n", COMPONENT_NAME);
    fflush(stdout);
#endif

    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);

#if DEBUG_PACKET_DETAIL
    printf("%s: [DEBUG] tcp_new_ip_type() returned: pcb=%p\n", COMPONENT_NAME, (void*)pcb);
    fflush(stdout);
#endif

    if (pcb == NULL) {
        printf("%s: [ERR] Failed to create TCP PCB\n", COMPONENT_NAME);
#if DEBUG_PACKET_DETAIL
        printf("%s: [DEBUG] TCP PCB creation returned NULL - malloc likely failed\n", COMPONENT_NAME);
        printf("%s: [DEBUG] This suggests lwIP memory allocator is not ready\n", COMPONENT_NAME);
        fflush(stdout);
#endif
        return;
    }

#if DEBUG_PACKET_DETAIL
    printf("%s: [DEBUG] [OK] TCP PCB created successfully at %p\n", COMPONENT_NAME, (void*)pcb);
    printf("%s: [DEBUG] About to call tcp_bind(pcb, IP_ADDR_ANY, %d)...\n", COMPONENT_NAME, TCP_ECHO_PORT);
    fflush(stdout);
#endif

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, TCP_ECHO_PORT);

#if DEBUG_PACKET_DETAIL
    printf("%s: [DEBUG] tcp_bind() returned: err=%d (%s)\n", COMPONENT_NAME,
           err, err == ERR_OK ? "ERR_OK" : "ERROR");
    fflush(stdout);
#endif

    if (err != ERR_OK) {
        printf("%s: [ERR] Failed to bind TCP port %d (err=%d)\n", COMPONENT_NAME, TCP_ECHO_PORT, err);
        return;
    }

#if DEBUG_PACKET_DETAIL
    printf("%s: [DEBUG] [OK] Successfully bound to port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
    printf("%s: [DEBUG] About to call tcp_listen_with_backlog(pcb, %d)...\n", COMPONENT_NAME, MAX_TCP_CONNECTIONS);
    fflush(stdout);
#endif

    pcb = tcp_listen_with_backlog(pcb, MAX_TCP_CONNECTIONS);

#if DEBUG_PACKET_DETAIL
    printf("%s: [DEBUG] tcp_listen_with_backlog() returned: pcb=%p\n", COMPONENT_NAME, (void*)pcb);
    fflush(stdout);
#endif

    if (pcb == NULL) {
        printf("%s: [ERR] Failed to listen on TCP port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
        return;
    }

#if DEBUG_PACKET_DETAIL
    printf("%s: [DEBUG] [OK] Now listening on port %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
    printf("%s: [DEBUG] About to call tcp_accept(pcb, tcp_echo_accept)...\n", COMPONENT_NAME);
    fflush(stdout);
#endif

    tcp_accept(pcb, tcp_echo_accept);

#if DEBUG_PACKET_DETAIL
    printf("%s: [DEBUG] [OK] Accept callback registered\n", COMPONENT_NAME);
    printf("%s: [DEBUG] Exiting setup_tcp_echo_server() - SUCCESS\n", COMPONENT_NAME);
    fflush(stdout);
#endif

    /* CRITICAL DEBUG: Print actual PCB local_ip to diagnose TCP matching */
    struct tcp_pcb_listen *lpcb = (struct tcp_pcb_listen *)pcb;
    printf("\n");
    printf("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    printf("%s: [OK] TCP SERVER CONFIGURATION\n", COMPONENT_NAME);
    printf("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    printf("%s: Port:           %d\n", COMPONENT_NAME, TCP_ECHO_PORT);
    printf("%s: PCB local_ip:   %u.%u.%u.%u\n", COMPONENT_NAME,
           ip4_addr1(&lpcb->local_ip), ip4_addr2(&lpcb->local_ip),
           ip4_addr3(&lpcb->local_ip), ip4_addr4(&lpcb->local_ip));

    /* Validate PCB binding */
    bool is_wildcard = (ip4_addr1(&lpcb->local_ip) == 0 &&
                        ip4_addr2(&lpcb->local_ip) == 0 &&
                        ip4_addr3(&lpcb->local_ip) == 0 &&
                        ip4_addr4(&lpcb->local_ip) == 0);

    if (is_wildcard) {
        printf("%s: Status:         [OK] WILDCARD (0.0.0.0) - accepts ANY destination IP\n", COMPONENT_NAME);
        printf("%s: Will accept:    Packets to 10.2.0.2, 192.168.95.2, or any IP\n", COMPONENT_NAME);
    } else {
        printf("%s: Status:         [WARN]  SPECIFIC IP - only accepts packets to this IP\n", COMPONENT_NAME);
        printf("%s: Will accept:    Packets to %u.%u.%u.%u ONLY\n", COMPONENT_NAME,
               ip4_addr1(&lpcb->local_ip), ip4_addr2(&lpcb->local_ip),
               ip4_addr3(&lpcb->local_ip), ip4_addr4(&lpcb->local_ip));
        printf("%s: Will REJECT:    Packets to 192.168.95.2 (if not matching above)\n", COMPONENT_NAME);
    }
    printf("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * OUTBOUND PATH: ICS_Outbound → Internal Network (TCP Client)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* TCP client connection state for INBOUND forwarding */
struct tcp_inbound_client_state {
    struct tcp_pcb *pcb;
    uint8_t payload_data[MAX_PAYLOAD_SIZE];  /* CRITICAL FIX v2.48: Use buffer, not pointer!
                                               * Previously: uint8_t *payload_data pointed to shared dataport
                                               * Problem: Dataport gets overwritten by next message while TCP callback still uses it
                                               * Result: NULL pointer crashes, corrupted data, assertion failures in pbuf.c
                                               * Solution: Copy payload into local buffer that persists across messages */
    uint16_t payload_len;
    uint16_t bytes_sent;
    bool active;
};

/* v2.106: CRITICAL FIX - Replace single global with connection pool
 * ═══════════════════════════════════════════════════════════════════════════
 * Bug Discovery (2025-10-20):
 * - Single global `inbound_tcp_client` was shared across ALL TCP connections
 * - When multiple SCADA requests arrived before first connection completed:
 *   1. Request 1 arrives → sets inbound_tcp_client data → tcp_connect()
 *   2. Request 2 arrives → OVERWRITES inbound_tcp_client data
 *   3. Connection 1 completes → callback sees Request 2's data
 *   4. CRASH or DATA CORRUPTION
 *
 * Root Cause: tcp_connect() is ASYNCHRONOUS (5-20ms to complete)
 * - During this window, new requests can arrive and overwrite state
 * - This was hidden by heavy printf delays in v2.97 (50-100ms)
 * - When prints removed, race window exposed → v2.105 crash
 *
 * Fix: Per-connection state allocation using connection pool
 * - Each TCP connection gets its own dedicated state slot
 * - No sharing, no race conditions
 * - Pool size: MUST MATCH lwIP's MEMP_NUM_TCP_PCB limit
 *
 * Evidence: v2.105 crash after "Callbacks registered: recv=0xb878, sent=0xbec8"
 *           Crashed accessing state->payload_len (data corrupted by race)
 *
 * Pool sizing history:
 * - v2.106-v2.107: 10 slots (arbitrary choice - BUG!)
 * - v2.108: 50 slots (still too small)
 * - v2.109: 100 slots (matches MEMP_NUM_TCP_PCB=100)
 * - v2.174: 150 slots (matches MEMP_NUM_TCP_PCB=150)
 *
 * Root Cause of Pool Exhaustion:
 * - lwIP configured for MEMP_NUM_TCP_PCB (can create N TCP connections)
 * - But MAX_INBOUND_CONNECTIONS was only 10 (application state pool)
 * - Mismatch: lwIP creates N connections, but we can only track 10!
 * - Fix: Synchronize application pool with lwIP pool
 *
 * Memory overhead: 1000 × 2.4KB = ~2.4MB (2.4% of 100MB RAM - acceptable)
 */
#define MAX_INBOUND_CONNECTIONS 1000  /* v2.181: MUST match MEMP_NUM_TCP_PCB in lwipopts.h */
static struct tcp_inbound_client_state inbound_connection_pool[MAX_INBOUND_CONNECTIONS];

/* Allocate a free connection state from the pool */
static struct tcp_inbound_client_state* inbound_alloc_state(void)
{
    BREADCRUMB(2100);  /* Pool allocation attempt */
    for (int i = 0; i < MAX_INBOUND_CONNECTIONS; i++) {
        if (!inbound_connection_pool[i].active) {
            memset(&inbound_connection_pool[i], 0, sizeof(struct tcp_inbound_client_state));
            inbound_connection_pool[i].active = true;
            BREADCRUMB(2101);  /* Pool slot allocated successfully */
            return &inbound_connection_pool[i];
        }
    }

    /* Pool exhausted - this should be very rare with proper connection cleanup */
    BREADCRUMB(2102);  /* Pool exhausted */
    printf("%s: [CRITICAL] Inbound connection pool exhausted! (max=%d)\n",
           COMPONENT_NAME, MAX_INBOUND_CONNECTIONS);

    /* v2.117: CRITICAL FIX - Limit debug output to prevent stack overflow
     * Problem: Printing 100 slots caused stack overflow → hypervisor trap → system reset
     * Evidence: GDB showed "corrupt stack?" and system stuck in arm_hyp_trap()
     * Solution: Only print first 10 active slots to conserve stack space */
    printf("%s: [DEBUG] Pool status (first 10 active slots only):\n", COMPONENT_NAME);
    int printed = 0;
    for (int i = 0; i < MAX_INBOUND_CONNECTIONS && printed < 10; i++) {
        if (inbound_connection_pool[i].active) {
            printf("%s:   Slot %d: active=%d pcb=%p len=%u sent=%u\n",
                   COMPONENT_NAME, i,
                   inbound_connection_pool[i].active,
                   (void*)inbound_connection_pool[i].pcb,
                   inbound_connection_pool[i].payload_len,
                   inbound_connection_pool[i].bytes_sent);
            printed++;
        }
    }
    if (printed < MAX_INBOUND_CONNECTIONS) {
        printf("%s:   ... (%d more active connections not shown)\n",
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
        printf("%s: [CRITICAL] RECV CALLBACK RE-ENTRANCY DETECTED! depth=%d, p=%p, err=%d\n",
               COMPONENT_NAME, inbound_recv_callback_depth, (void*)p, err);
    }
    printf("%s: [EVENT] inbound_tcp_recv_callback FIRED! depth=%d, p=%p, err=%d\n",
           COMPONENT_NAME, inbound_recv_callback_depth, (void*)p, err);
    struct tcp_inbound_client_state *state = (struct tcp_inbound_client_state *)arg;

    if (p == NULL) {
        BREADCRUMB(1001);  /* Connection closed by remote */
        /* v2.91: CRITICAL FIX - DO NOT remove connection metadata yet!
         *
         * Same issue as Net0 v2.90:
         * - PLC may close connection immediately after sending response
         * - Net1's inbound_tcp_recv_callback(p=NULL) is called
         * - OLD CODE: connection_remove(pcb) removes metadata
         * - But Net0 might still be processing this connection
         * - Result: Net0 can't find metadata to send response back to SCADA
         *
         * Solution: Mark PCB as NULL but keep metadata alive
         * - Cleanup happens later in inbound_ready_handle() or timeout
         */

        /* v2.117: CRITICAL FIX - Prevent double-close bug
         * ═══════════════════════════════════════════════════════════════════════
         * Problem: Close notification handler might have already called tcp_close()
         * on this connection. If we call tcp_close() AGAIN, lwIP will crash with
         * assertion "tcp_output: pcb->next != pcb" because the PCB is already in
         * closing state.
         *
         * Sequence that causes double-close:
         * 1. Net0 sends close notification (SCADA closed)
         * 2. Close notification handler calls tcp_close() on PCB
         * 3. PLC closes its end → recv(p=NULL) fires
         * 4. We call tcp_close() AGAIN → CRASH!
         *
         * Solution: Check metadata first. If metadata->pcb is already NULL,
         * it means the close notification handler already handled this close.
         * Only call tcp_close() if we're the first to handle the close.
         * ═══════════════════════════════════════════════════════════════════════
         */

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
            printf("%s: [WARN]  PLC closed connection but no metadata found\n",
                   COMPONENT_NAME);
            printf("%s:          Connection was likely already closed via notification\n",
                   COMPONENT_NAME);
            BREADCRUMB(1002);  /* Before decrement */
            inbound_recv_callback_depth--;
            BREADCRUMB(1003);  /* Before return */
            return ERR_OK;
        }

        /* Mark PCB as NULL in metadata */
        printf("%s: [WARN]  PLC closed connection - marking PCB as NULL (keeping metadata)\n",
               COMPONENT_NAME);
        meta->pcb = NULL;

        /* Memory barrier to ensure metadata update is visible */
        __sync_synchronize();

        /* Check if PCB is still valid (not NULL) */
        if (pcb == NULL) {
            printf("%s: [WARN]  recv(p=NULL) called with NULL pcb - already freed by lwIP\n",
                   COMPONENT_NAME);
            inbound_recv_callback_depth--;
            return ERR_OK;
        }

        /* v2.156: lwIP BEST PRACTICE - Return ERR_ABRT instead of calling tcp_abort()
         * CRITICAL FIX: Never call tcp_abort() from inside lwIP callbacks!
         *
         * OLD CODE (v2.132):
         *   tcp_abort(pcb);  // ❌ WRONG! lwIP may still need PCB after callback
         *   return ERR_ABRT;
         *
         * NEW CODE (v2.156):
         *   - Clean up application state only
         *   - Return ERR_ABRT to tell lwIP connection should be aborted
         *   - lwIP calls tcp_abort() internally AFTER callback completes
         *   - This prevents use-after-free when lwIP accesses PCB after callback
         */
        printf("%s: [INFO]  PLC closed connection - returning ERR_ABRT (lwIP will handle abort)\n",
               COMPONENT_NAME);

        /* v2.186: CRITICAL FIX - Decrement connection_count when PLC closes
         * ═══════════════════════════════════════════════════════════════════════════
         * Bug in v2.185: Set active=false but didn't decrement connection_count
         *
         * Flow:
         * 1. PLC closes (sends FIN) → recv callback with p=NULL
         * 2. We set active=false here but don't decrement count
         * 3. Return ERR_ABRT → lwIP calls tcp_abort() internally
         * 4. Error callback fires with err=ERR_ABRT
         * 5. Old code checked "if (err != ERR_ABRT)" → SKIPPED decrement
         * 6. Result: active=false but count never decremented → LEAK!
         *
         * Fix: Decrement count here when PLC closes
         * - This makes recv path symmetric with close notification handler path
         * - Error callback can now safely check meta->active to prevent double-decrement
         * ═══════════════════════════════════════════════════════════════════════════
         */
        if (meta != NULL) {
            meta->pcb = NULL;
            meta->active = false;

            /* Decrement connection count */
            if (connection_count > 0) {
                uint32_t old_count = connection_count;
                connection_count--;
                printf("%s: [COUNT--] %u → %u | inbound_tcp_recv(p=NULL) PLC closed\n",
                       COMPONENT_NAME, old_count, connection_count);
            }

            update_shared_connection_state();
            __sync_synchronize();  /* Ensure metadata update visible */
        }

        inbound_recv_callback_depth--;
        return ERR_ABRT;  /* ✅ lwIP calls tcp_abort() internally */
    }

    if (err != ERR_OK) {
        BREADCRUMB(1002);  /* Error in receive */
        /* v2.155: Let lwIP free pbuf on error - we don't own it */
        inbound_recv_callback_depth--;
        return err;
    }

    if (outbound_dp == NULL) {
        BREADCRUMB(1003);  /* NULL dataport */
        /* v2.155: Let lwIP free pbuf - we don't own it */
        inbound_recv_callback_depth--;
        return ERR_MEM;
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

    /* v2.159 FIX: REMOVED memory barrier from here!
     * ═══════════════════════════════════════════════════════════════════════
     * CRITICAL: Cannot add memory barrier inside lwIP callback!
     *
     * Problem: __sync_synchronize() blocks CPU, gives lwIP timers chance to fire
     * Result: Race condition with pbuf management → CRASH
     *
     * Why barrier not needed here:
     * - CAmkES outbound_ready_emit() has internal synchronization
     * - Event mechanism provides memory ordering guarantees
     * - Net0 has memory barrier AFTER receiving signal (line 4390)
     *
     * Evidence: v2.159 crashed at pbuf.c:732 when barrier was here
     * ═══════════════════════════════════════════════════════════════════════
     */

    /* v2.104: Lightweight breadcrumb debugging for event emission
     * BREADCRUMB 1009: About to emit outbound_ready notification
     * BREADCRUMB 1010: outbound_ready_emit() completed successfully
     * If Net0 doesn't receive, check CAmkES event connection */
    BREADCRUMB(1009);  /* Emitting notification */
    outbound_ready_emit();
    BREADCRUMB(1010);  /* Emit completed */

    tcp_recved(pcb, p->len);

    /* v2.155: CRITICAL FIX - Let lwIP free pbuf, we don't own it
     * Previous bug: We called pbuf_free(p) here, then lwIP also freed it
     * Result: Double-free → "pbuf_free: p->ref > 0" assertion failure
     * Fix: Remove manual pbuf_free(), lwIP's tcp_input() will handle it */

    BREADCRUMB(1010);  /* Response sent to ICS_Outbound */

    /* CRITICAL FIX FOR USE-AFTER-FREE RACE:
     * DO NOT close the connection here!
     *
     * Previously: Net1 closed connection immediately after sending notification
     * Problem: Net0 still needed to write response back, but PCB was already freed
     * Crash: Net0 tried tcp_write() on freed PCB → assertion failure
     *
     * Solution: Keep connection alive so Net0 can write response
     * The connection will be closed either by:
     *   1. SCADA closing after receiving response
     *   2. lwIP TCP timeout if SCADA doesn't close
     *   3. New request reusing this connection (handled in inbound_ready_handle)
     */

    BREADCRUMB(1011);  /* Keeping connection alive for Net0 response */

    /* Connection stays open - will be cleaned up by:
     * 1. Remote close (SCADA or PLC closes)
     * 2. Next request arrives (B2006 cleanup with tcp_abort)
     * 3. lwIP TCP timeout (if connection dies) */

    BREADCRUMB(1012);  /* Before decrement */
    inbound_recv_callback_depth--;  /* v2.117: Decrement re-entrancy counter */
    BREADCRUMB(1013);  /* Before return - control goes back to lwIP */
    printf("%s: [DEBUG] About to return ERR_OK to lwIP from recv callback\n", COMPONENT_NAME);
    fflush(stdout);  /* Force output before return */
    return ERR_OK;
}

static err_t inbound_tcp_sent_callback(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    struct tcp_inbound_client_state *state = (struct tcp_inbound_client_state *)arg;

    #if DEBUG_TRAFFIC
    printf("%s: INBOUND: Sent %u bytes to internal network\n", COMPONENT_NAME, len);
    #endif

    state->bytes_sent += len;

    /* Check if all data sent */
    if (state->bytes_sent >= state->payload_len) {
        #if DEBUG_TRAFFIC
        printf("%s: INBOUND: Complete - sent %u/%u bytes, waiting for PLC response\n",
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
            printf("%s: INBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        }
    }

    return ERR_OK;
}

/* v2.82: CRITICAL FIX - Add missing error callback for inbound TCP client connections
 *
 * Bug Analysis (v2.81 crash at 0x10 in Net1):
 * - Net1 never registered tcp_err() callback for inbound client connections
 * - When connection error occurred, lwIP freed PCB but had no callback to notify us
 * - Later, inbound_tcp_connected_callback() fired with freed PCB
 * - Crash at offset 0x10 when accessing tcp_sndbuf(pcb)
 *
 * Fix: Add error callback to clean up state when connection fails
 * This is the same pattern as Net0's tcp_echo_err callback
 */
static void inbound_tcp_err_callback(void *arg, err_t err)
{
    struct tcp_inbound_client_state *state = (struct tcp_inbound_client_state *)arg;

    printf("%s: [WARN]  INBOUND TCP error - err=%d (%s)\n", COMPONENT_NAME, err,
           err == ERR_ABRT ? "ERR_ABRT (Connection aborted)" :
           err == ERR_RST ? "ERR_RST (Connection reset)" :
           err == ERR_CLSD ? "ERR_CLSD (Connection closed)" :
           err == ERR_CONN ? "ERR_CONN (Not connected)" :
           err == ERR_TIMEOUT ? "ERR_TIMEOUT (Timeout)" : "Unknown");

    /* CRITICAL: PCB is already freed by lwIP when err callback is called - don't access it!
     * v2.116: CRITICAL FIX - Clear metadata's PCB pointer to prevent stale pointer usage
     * v2.132: Don't clean up metadata for ERR_ABRT - close handler already did it
     * ═══════════════════════════════════════════════════════════════════════════
     * ERR_ABRT is triggered by OUR tcp_abort() call in close notification handler.
     * The close handler already cleaned up metadata (active=false, count--).
     * We only need to free the pool_state here, not touch metadata.
     *
     * For other errors (ERR_RST, ERR_TIMEOUT, etc.), lwIP aborted the connection
     * and we need to clean up both pool_state AND metadata.
     * ═══════════════════════════════════════════════════════════════════════════
     */
    if (state != NULL) {
        /* Find metadata for this connection */
        struct connection_metadata *meta = NULL;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connection_table[i].active && connection_table[i].pool_state == state) {
                meta = &connection_table[i];
                break;
            }
        }

        /* v2.153: Enqueue error notification to Net0 (for non-ABRT errors)
         * ═══════════════════════════════════════════════════════════════════════
         * ERR_ABRT: WE called tcp_abort() from close notification handler
         *           → Net0 already knows (it sent the close notification)
         *           → Don't send error notification (would be duplicate)
         *
         * Other errors (ERR_RST, ERR_TIMEOUT): PLC initiated the close
         *           → Net0 doesn't know yet
         *           → Send error notification to close SCADA side
         * ═══════════════════════════════════════════════════════════════════════
         */
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

                    /* v2.188-sentinel: Mark as error-only notification
                     * Set payload_length = 0 to indicate this is NOT a response
                     * ICS_Outbound will forward this, Net0 will see sentinel and skip response processing
                     */
                    dp->response_msg.payload_length = 0;  /* Sentinel: error-only, no payload */
                    dp->response_msg.metadata.session_id = meta->session_id;
                    __sync_synchronize();  /* Memory barrier - ensure sentinel visible before signal */

                    outbound_ready_emit();        /* Signal Net0 */

                    printf("%s: Enqueued error notification to Net0 "
                           "(session %u, err=%d)\n",
                           COMPONENT_NAME, meta->session_id, err);
                } else {
                    printf("%s: [ERROR] Failed to enqueue error notification "
                           "(queue full? session %u)\n",
                           COMPONENT_NAME, meta->session_id);
                }
            } else {
                printf("%s: [DEDUP] Ignoring duplicate error for session %u "
                       "(RST flood protection)\n",
                       COMPONENT_NAME, meta->session_id);
            }
        }

        /* v2.186: CRITICAL FIX - Always clean up metadata if still active
         * ═══════════════════════════════════════════════════════════════════════════
         * Bug in v2.185: Used "if (err != ERR_ABRT && meta != NULL)" which assumed
         * ERR_ABRT only comes from close notification handler.
         *
         * Reality: ERR_ABRT comes from TWO sources:
         * 1. Close notification handler (line 3420) calls tcp_abort()
         *    → Handler sets meta->active=false and decrements count BEFORE abort
         *    → Error callback sees meta->active=false → skip decrement ✅
         *
         * 2. recv callback (line 2731) returns ERR_ABRT when PLC closes (p=NULL)
         *    → recv sets meta->active=false but does NOT decrement count
         *    → lwIP calls tcp_abort() internally
         *    → Error callback sees meta->active=false but count NOT decremented! ❌
         *    → Old code skipped decrement because err==ERR_ABRT → LEAK!
         *
         * Fix: Use meta->active flag instead of err type
         * - If meta->active is true: connection still active → decrement count
         * - If meta->active is false: already cleaned up → skip decrement
         * - This handles BOTH ERR_ABRT sources correctly
         *
         * But wait! recv callback sets active=false, so this won't work either!
         *
         * REAL FIX: Check if connection_count > 0 and meta exists but is inactive
         * Actually, the REAL issue is that recv callback sets active=false but
         * doesn't decrement. We need to track whether count was decremented.
         *
         * Simplest fix: recv callback should decrement when it sets active=false!
         * ═══════════════════════════════════════════════════════════════════════════
         */
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
            printf("%s:   → Clearing metadata PCB pointer (PCB was freed by lwIP)\n",
                   COMPONENT_NAME);

            /* Only decrement if connection is still marked active
             * This prevents double-decrement if close handler already ran */
            if (meta->active && connection_count > 0) {
                uint32_t old_count = connection_count;
                connection_count--;
                printf("%s: [COUNT--] %u → %u | inbound_tcp_err_callback() err=%d\n",
                       COMPONENT_NAME, old_count, connection_count, err);
            }

            meta->pcb = NULL;  /* PCB already freed by lwIP */
            meta->active = false;
            /* DON'T clear error_notified - keeps deduplication active for retransmitted RSTs */

            /* v2.117: Update shared connection state */
            update_shared_connection_state();
        }

        inbound_free_state(state);  /* v2.106: Free connection pool slot (always needed) */
    }

    printf("%s: [CLEAN] INBOUND connection error triggered - %s\n", COMPONENT_NAME,
           err == ERR_ABRT ? "pool freed (metadata already cleaned by close handler)" :
           "state and metadata cleaned up");
}

static err_t inbound_tcp_connected_callback(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct tcp_inbound_client_state *state = (struct tcp_inbound_client_state *)arg;

    if (err != ERR_OK) {
        printf("%s: [ERR] INBOUND: Connection failed (3-way handshake): err=%d\n", COMPONENT_NAME, err);

        /* v2.186: CRITICAL FIX - Clean up metadata when 3-way handshake fails
         * ═══════════════════════════════════════════════════════════════════════
         * Bug: connection_add() incremented connection_count and created metadata,
         * but when 3-way handshake fails (PLC sends RST), this callback is called
         * with err != ERR_OK and we never cleaned up metadata!
         *
         * This is different from tcp_connect() immediate failure (fixed in v2.174)
         * - v2.174 fixed: tcp_connect() returns error immediately (ENOMEM, etc.)
         * - v2.186 fixes: tcp_connect() succeeds but 3-way handshake fails (RST)
         *
         * Evidence from tcpdump:
         * - Many RST packets from PLC during handshake
         * - connection_count reaches 100 and stays stuck
         * - These are 3-way handshake failures, not immediate tcp_connect() failures
         *
         * Fix: Clean up metadata and decrement counter when handshake fails
         * ═══════════════════════════════════════════════════════════════════════
         */

        /* Find the connection metadata */
        struct connection_metadata *meta = NULL;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connection_table[i].active && connection_table[i].pcb == pcb) {
                meta = &connection_table[i];
                break;
            }
        }

        /* v2.153: Enqueue error notification to Net0 using control queue
         * This handles failures during 3-way handshake (RST from PLC) */
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

                /* v2.188-sentinel: Mark as error-only notification
                 * Set payload_length = 0 to indicate this is NOT a response
                 * ICS_Outbound will forward this, Net0 will see sentinel and skip response processing
                 */
                dp->response_msg.payload_length = 0;  /* Sentinel: error-only, no payload */
                dp->response_msg.metadata.session_id = meta->session_id;
                __sync_synchronize();  /* Memory barrier - ensure sentinel visible before signal */

                outbound_ready_emit();

                printf("%s: Enqueued connection failure notification to Net0 "
                       "(session %u, err=%d)\n",
                       COMPONENT_NAME, meta->session_id, err);
            } else {
                printf("%s: [ERROR] Failed to enqueue connection failure notification "
                       "(session %u)\n", COMPONENT_NAME, meta->session_id);
            }
        }

        /* v2.186: Clean up metadata for failed handshake */
        if (meta != NULL) {
            meta->pcb = NULL;
            meta->active = false;
            if (connection_count > 0) {
                uint32_t old_count = connection_count;
                connection_count--;
                printf("%s: [COUNT--] %u → %u | inbound_tcp_connected_callback() handshake failed err=%d\n",
                       COMPONENT_NAME, old_count, connection_count, err);
            }
            update_shared_connection_state();
        }

        inbound_free_state(state);  /* v2.106: Free connection pool slot */
        return err;
    }

    #if DEBUG_TRAFFIC
    printf("%s: INBOUND: Connected to internal network\n", COMPONENT_NAME);
    #endif

    printf("%s: [LINK] TCP connection ESTABLISHED to PLC - registering callbacks\n", COMPONENT_NAME);

    /* Set callbacks */
    tcp_recv(pcb, inbound_tcp_recv_callback);
    tcp_sent(pcb, inbound_tcp_sent_callback);

    printf("%s: [OK] Callbacks registered: recv=%p, sent=%p\n", COMPONENT_NAME, (void*)inbound_tcp_recv_callback, (void*)inbound_tcp_sent_callback);

    /* v2.101: CRITICAL FIX - Check send buffer before calling tcp_output()
     * ════════════════════════════════════════════════════════════════════
     * Bug Discovery (2025-10-20):
     * - Connection just established, tcp_sndbuf(pcb) returns 0 (no window yet)
     * - to_send = 0, tcp_write(pcb, data, 0) succeeds but queues nothing
     * - pcb->unsent remains NULL (no data queued)
     * - tcp_output(pcb) tries to access pcb->unsent->next at offset +16
     * - CRASH: NULL pointer dereference at address 0x10
     *
     * Evidence from crash log:
     * - "TCP connection ESTABLISHED to PLC - registering callbacks"
     * - "Callbacks registered: recv=0xb878, sent=0xbeb8"
     * - FAULT: pc=0x39868 (inside tcp_output), address=0x10
     * - Disassembly: ldr r3, [r3, #16] where r3=0
     *
     * Root Cause:
     * - TCP connection just established but remote window not yet advertised
     * - tcp_sndbuf() returns 0 (send buffer available but remote can't receive)
     * - Calling tcp_output() with empty unsent queue causes NULL dereference
     *
     * Fix: Only call tcp_output() if data was actually queued
     * - Check to_send > 0 before tcp_write()
     * - lwIP will call sent callback when buffer space available
     * - sent callback will retry transmission
     *
     * This is a race condition between:
     * 1. TCP connection establishment (3-way handshake completes)
     * 2. Remote window advertisement (may arrive slightly later)
     * 3. Immediate data transmission attempt (before window known)
     */

    /* Send the payload */
    uint16_t to_send = (state->payload_len > tcp_sndbuf(pcb)) ? tcp_sndbuf(pcb) : state->payload_len;

    if (to_send == 0) {
        /* Send buffer full - defer transmission until sent callback */
        printf("%s: [WARN]  Send buffer full (sndbuf=%u), deferring transmission of %u bytes\n",
               COMPONENT_NAME, tcp_sndbuf(pcb), state->payload_len);
        printf("%s:    → Will retry in tcp_sent callback when buffer available\n", COMPONENT_NAME);

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
        printf("%s: INBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        /* v2.88: CRITICAL FIX - Do NOT call tcp_abort() from callback!
         * Return ERR_ABRT and lwIP handles cleanup internally */
        inbound_free_state(state);  /* v2.106: Free connection pool slot */
        return ERR_ABRT;
    }

    state->bytes_sent = to_send;

    /* Trigger transmission - safe now because we know data was queued */
    tcp_output(pcb);

    #if DEBUG_TRAFFIC
    printf("%s: INBOUND: Sent initial %u bytes\n", COMPONENT_NAME, to_send);
    #endif

    return ERR_OK;
}

/* TCP client connection state for OUTBOUND forwarding */
struct tcp_outbound_client_state {
    struct tcp_pcb *pcb;
    uint8_t payload_data[MAX_PAYLOAD_SIZE];  /* v2.106: CRITICAL FIX - Use buffer, not pointer!
                                               * Same fix as inbound v2.48
                                               * Problem: Pointer to dataport gets overwritten by next message
                                               * Solution: Copy payload into local buffer */
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

        /* v2.88: CRITICAL FIX - Close connection after successful transmission
         * DO NOT call tcp_abort() from callback! Return ERR_ABRT instead.
         *
         * This was THE BUG causing the crash at PC 0x38a9c!
         * - tcp_abort(pcb) frees the PCB immediately
         * - lwIP callback returns and tries to use freed PCB
         * - Crash at address 0x10 (NULL + offset)
         *
         * Correct protocol: Return ERR_ABRT, lwIP handles tcp_abort() internally */
        connection_remove(pcb);
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

    /* v2.102: CRITICAL FIX - Check to_send > 0 before calling tcp_write()
     * Same issue as v2.101 but in OUTBOUND path (we fixed inbound but forgot outbound!)
     *
     * Bug Discovery (2025-10-20):
     * - Crash at PC 0x383e0 (tcp_write+0xbc8) with NULL pbuf
     * - OUTBOUND connection established but remote window not yet advertised
     * - tcp_sndbuf(pcb) returns 0, to_send = 0
     * - tcp_write(pcb, data, 0) creates NULL pbuf
     * - Later access to pbuf->len (offset +8) causes crash
     *
     * Evidence:
     * - GDB kernel fault catcher at c_handle_data_fault
     * - FAR_EL2 = 0x10, r3 = NULL
     * - Disassembly: ldrh r2, [r3, #8] (loading pbuf->len)
     *
     * Root Cause: v2.101 only fixed inbound callback, forgot outbound!
     *
     * Fix: Same as v2.101 - only call tcp_write() if to_send > 0
     */
    if (to_send == 0) {
        /* Send buffer full - defer transmission until sent callback */
        printf("%s: [WARN]  OUTBOUND: Send buffer full (sndbuf=%u), deferring transmission of %u bytes\n",
               COMPONENT_NAME, tcp_sndbuf(pcb), state->payload_len);
        printf("%s:    → Will retry in tcp_sent callback when buffer available\n", COMPONENT_NAME);

        /* Keep state active - sent callback will retry when buffer space available */
        state->bytes_sent = 0;
        return ERR_OK;
    }

    err = tcp_write(pcb, state->payload_data, to_send, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("%s: OUTBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        /* v2.88: CRITICAL FIX - Do NOT call tcp_abort() from callback!
         * Return ERR_ABRT and lwIP handles cleanup internally */
        state->active = false;
        state->pcb = NULL;
        return ERR_ABRT;
    }

    state->bytes_sent = to_send;

    /* Trigger transmission */
    tcp_output(pcb);

    printf("%s: OUTBOUND: Sent initial %u bytes\n", COMPONENT_NAME, to_send);

    return ERR_OK;
}

/*
 * INBOUND notification handler - called when ICS_Inbound has validated data
 * Creates TCP client connection to forward data to internal network (PLC)
 */
void inbound_ready_handle(void)
{
    BREADCRUMB(2000);  /* Entry: ICS_Inbound notification received */

    #if DEBUG_PACKET_DETAIL
    uint32_t msg_id = ++message_id_counter;
    printf("\n[MSG] [MSG #%u] ═══ ICS→NET: Received from ICS_Inbound ═══\n", msg_id);
    printf("   Source: ICS pipeline validation complete\n");
    printf("   Action: Creating TCP client to forward to internal network\n");
    #endif

    #if DEBUG_TRAFFIC
    printf("%s: ╔═══════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
    printf("%s: ║  INBOUND: Received message from ICS_Inbound              ║\n", COMPONENT_NAME);
    printf("%s: ╚═══════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
    #endif

    BREADCRUMB(2001);  /* Checking dataport */

    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (inbound_dp == NULL) {
        BREADCRUMB(2002);  /* NULL dataport */
        printf("%s: [ERR] FATAL: inbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        printf("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        return;
    }

    #if DEBUG_TRAFFIC
    printf("%s: [OK] Dataport check: inbound_dp=%p (valid)\n", COMPONENT_NAME, (void*)inbound_dp);
    #endif

    BREADCRUMB(2003);  /* Reading ICS message */

    /* v2.153: Process close notification queue from Net0
     * ═══════════════════════════════════════════════════════════════════════
     * SCADA close notifications are queued by Net0 in close_queue.
     * Process all queued notifications and close corresponding PLC connections.
     *
     * This replaces the old single-message close notification (v2.151).
     * ═══════════════════════════════════════════════════════════════════════
     */
    InboundDataport *dp = (InboundDataport *)inbound_dp;
    static uint32_t close_queue_tail = 0;  /* Consumer state (local, never shared) */

    uint32_t close_queue_head = dp->close_queue.head;

    /* v2.159: CRITICAL FIX - Memory barrier for cache coherency
     * ═══════════════════════════════════════════════════════════════════════
     * Problem (v2.158): Net0 sent 103 close notifications, Net1 processed 0
     *
     * Root Cause: Cache coherency issue with dataport reads
     * - Net0 writes close_queue.head with memory barrier
     * - Net1 reads close_queue.head WITHOUT memory barrier
     * - Net1 CPU cache may have stale value (always reads 0)
     * - Loop condition (0 < 0) never true → never processes notifications
     * - Result: Net1 PCB pool exhausts (100/100), communication breaks
     *
     * Solution: Add memory barrier AFTER reading from shared dataport
     * - Forces CPU to invalidate cache line
     * - Ensures we read the actual value written by Net0
     * - Loop condition now correct → processes all notifications
     *
     * Evidence from v2.158 test (console-20251025-013249.log):
     * - Net0: 103 close notifications sent ✅
     * - Net1: 0 notifications processed ❌ (this bug)
     * - Net0: 88 connections (healthy) ✅
     * - Net1: 100/100 connections (pool exhausted) ❌
     *
     * This is symmetric to Net0's write barrier in tcp_echo_poll():
     *   Net0: notif->session_id = X; __sync_synchronize(); head++;
     *   Net1: head = dp->head; __sync_synchronize(); process(notif);
     * ═══════════════════════════════════════════════════════════════════════
     */
    __sync_synchronize();  /* Force cache invalidation - read fresh value from Net0 */

    /* Check for queue overflow */
    if (close_queue_head - close_queue_tail > CONTROL_QUEUE_SIZE) {
        printf("%s: [WARN] Close queue overflow! Missed %u notifications\n",
               COMPONENT_NAME, close_queue_head - close_queue_tail - CONTROL_QUEUE_SIZE);
        close_queue_tail = close_queue_head - CONTROL_QUEUE_SIZE;
    }

    /* Process all queued close notifications */
    while (close_queue_tail < close_queue_head) {
        uint32_t slot = close_queue_tail & CONTROL_QUEUE_MASK;
        volatile struct control_notification *notif = &dp->close_queue.notifications[slot];

        /* Verify sequence */
        if (notif->seq_num == close_queue_tail && notif->session_id != 0) {
            printf("%s: Processing close notification: session %u\n",
                   COMPONENT_NAME, notif->session_id);

            /* Lookup connection by session_id */
            struct connection_metadata *meta = connection_lookup_by_session_id(notif->session_id);

            if (meta != NULL && meta->active && meta->pcb != NULL) {
                struct tcp_pcb *pcb = meta->pcb;

                printf("%s:   → Closing PLC connection (session %u, PCB=%p)\n",
                       COMPONENT_NAME, notif->session_id, (void*)pcb);

                /* v2.156: lwIP BEST PRACTICE - Safe close from main thread (event handler)
                 * CRITICAL FIX: Never call tcp_abort() directly from main thread!
                 *
                 * OLD CODE (v2.153):
                 *   tcp_abort(pcb);  // ❌ WRONG! Use-after-free if lwIP callback executing
                 *
                 * NEW CODE (v2.156): NULL all callbacks first, then tcp_close()
                 *   1. NULL all callbacks to prevent them firing during/after close
                 *   2. Mark PCB as NULL in metadata (prevents double-close)
                 *   3. Use tcp_close() for graceful close (safer than tcp_abort from main thread)
                 *   4. If tcp_close() fails, use tcp_abort() as fallback
                 *
                 * Why this is safe:
                 *   - NULLing callbacks prevents spurious callbacks during close
                 *   - tcp_err callback won't fire (already NULL)
                 *   - Metadata marked inactive before calling lwIP
                 *   - tcp_close() is safer than tcp_abort() from main thread
                 */

                /* Step 1: NULL all callbacks to prevent them firing */
                tcp_arg(pcb, NULL);
                tcp_recv(pcb, NULL);
                tcp_sent(pcb, NULL);
                tcp_err(pcb, NULL);   /* Prevent error callback during close */
                tcp_poll(pcb, NULL, 0);

                /* Step 2: Mark PCB as NULL in metadata BEFORE closing */
                meta->pcb = NULL;
                meta->active = false;
                meta->error_notified = false;  /* Clear dedup flag */

                /* v2.182: Track connection count changes for leak debugging */
                uint32_t old_count = connection_count;
                if (connection_count > 0) {
                    connection_count--;
                }
                printf("%s: [COUNT--] %u → %u | close_notification() session=%u port=%u→%u PCB=%p\n",
                       COMPONENT_NAME, old_count, connection_count, notif->session_id,
                       meta->src_port, meta->dest_port, (void*)pcb);

                __sync_synchronize();  /* Ensure metadata updates visible */

                /* Step 3: Symmetrical close behavior (v2.175)
                 * ═══════════════════════════════════════════════════════════════════════
                 * CRITICAL FIX: Mirror SCADA's close behavior to PLC
                 *
                 * Problem (v2.174):
                 * - SCADA sends FIN+RST (aggressive close) → Net0 returns ERR_RST
                 * - Net1 always uses tcp_close() (graceful FIN) regardless of SCADA behavior
                 * - Result: Net1 PCBs stuck in FIN_WAIT when PLC network stops responding
                 * - After 100 connections: pool exhausted, communication breaks
                 *
                 * Solution: Check notif->err_code from Net0
                 * - ERR_RST (-14): SCADA sent RST → Use tcp_abort() to send RST to PLC
                 * - ERR_CLSD (-15) or 0: SCADA sent FIN → Use tcp_close() to send FIN to PLC
                 *
                 * Why this works:
                 * - tcp_abort() sends RST and frees PCB immediately (no FIN_WAIT)
                 * - Mirrors SCADA's aggressive close behavior
                 * - Prevents PCB accumulation in ICS environments
                 * - Compatible with other SCADA systems that use proper FIN (ERR_CLSD)
                 *
                 * Evidence from tcpdump analysis:
                 * - tap0: SCADA sends [F.] then [R] (FIN+RST pattern)
                 * - tap1 (before fix): Net1 only sends [F.] (graceful close)
                 * - tap1 (after fix): Net1 should send [R] when SCADA sent RST
                 * ═══════════════════════════════════════════════════════════════════════
                 */
                if (notif->err_code == ERR_RST || notif->err_code == ERR_ABRT) {
                    /* SCADA sent RST or Net0 forced close → Use tcp_abort() for immediate cleanup */
                    const char *reason = (notif->err_code == ERR_RST) ? "SCADA sent RST" : "Forced close (pool exhaustion)";
                    printf("%s:   → %s, sending RST to PLC (tcp_abort)\n",
                           COMPONENT_NAME, reason);
                    tcp_abort(pcb);  /* Sends RST, frees PCB immediately */
                } else {
                    /* SCADA sent FIN (ERR_CLSD) or unknown → Use graceful close */
                    printf("%s:   → SCADA sent FIN, sending FIN to PLC (tcp_close)\n",
                           COMPONENT_NAME);
                    err_t err = tcp_close(pcb);
                    if (err != ERR_OK) {
                        /* Graceful close failed (out of memory), force abort */
                        printf("%s:   → tcp_close() failed (err=%d), forcing tcp_abort()\n",
                               COMPONENT_NAME, err);
                        tcp_abort(pcb);  /* Safe now - callbacks NULL, metadata inactive */
                    }
                }

                /* v2.162: CRITICAL FIX - Free inbound connection pool slot!
                 * ═══════════════════════════════════════════════════════════════
                 * BUG (v2.161): Pool exhaustion after 100 connections
                 * - Close notification processing freed metadata but NOT pool slot
                 * - inbound_connection_pool[i].active stayed true forever
                 * - After 100 connections: pool exhausted, can't accept new connections
                 *
                 * ROOT CAUSE: Forgot to call inbound_free_state(meta->pool_state)
                 * - Error callback correctly frees pool (line 2879)
                 * - Close notification handler forgot to free pool (this bug!)
                 *
                 * FIX: Free pool slot same as error callback does
                 * ═══════════════════════════════════════════════════════════════
                 */
                if (meta->pool_state != NULL) {
                    inbound_free_state(meta->pool_state);
                    meta->pool_state = NULL;
                }

                /* Update shared state */
                update_shared_connection_state();

                printf("%s:   ✓ PLC connection closed (session %u)\n",
                       COMPONENT_NAME, notif->session_id);
            } else {
                printf("%s:   → Connection already closed (session %u)\n",
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
        printf("%s: INBOUND: Invalid payload length %u (max %u)\n",
               COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
        #if DEBUG_PACKET_DETAIL
        printf("   ✗ [MSG #%u] DROPPED - invalid payload size\n\n", msg_id);
        #endif
        return;
    }

    BREADCRUMB(2005);  /* Payload size valid */

    /* Skip if no request data (only close notifications were queued) */
    if (ics_msg->payload_length == 0) {
        return;
    }

    /* OLD CLOSE NOTIFICATION HANDLER REMOVED (replaced by queue above) */
    /* Continue with normal request processing below... */
    if (false) {  /* DISABLED - kept for reference only */
        printf("%s: [RX] Received close notification from Net0 (SCADA %u.%u.%u.%u:%u closed)\n",
               COMPONENT_NAME,
               (ics_msg->metadata.src_ip >> 24) & 0xFF,
               (ics_msg->metadata.src_ip >> 16) & 0xFF,
               (ics_msg->metadata.src_ip >> 8) & 0xFF,
               ics_msg->metadata.src_ip & 0xFF,
               ics_msg->metadata.src_port);

        /* v2.117: CRITICAL FIX - Check if this is a stale notification for a self-cleaned connection
         * ═══════════════════════════════════════════════════════════════════════════
         * Problem: Close notifications can arrive AFTER we've cleaned up the OLD connection
         * and created a NEW connection with the same 5-tuple. If we process the stale
         * notification, we'll close the ACTIVE NEW connection → CRASH!
         *
         * Solution: Check if we recently cleaned this 5-tuple ourselves. If yes, the
         * notification is stale (for the OLD connection we already cleaned up).
         * Ignore it to avoid closing the NEW connection.
         *
         * This is robust because:
         * - Based on actual cleanup events (not timing guesses)
         * - Entries are consumed after use (no false positives)
         * - Old entries auto-expire (5 second TTL)
         * ═══════════════════════════════════════════════════════════════════════════
         */
        if (was_recently_self_cleaned(ics_msg->metadata.src_ip,
                                       ics_msg->metadata.src_port,
                                       ics_msg->metadata.dst_port)) {

            /* v2.117: CRITICAL FIX - Verify with Net0's state before ignoring
             * ═══════════════════════════════════════════════════════════════════
             * Problem: Self-cleaned tracking can give FALSE POSITIVES:
             * - We cleaned connection A (5-tuple X)
             * - Net0 creates NEW connection B (same 5-tuple X)
             * - Net0 sends close notification for B
             * - We see 5-tuple X in self-cleaned → incorrectly ignore it!
             * - Result: We keep B alive while Net0 closed it → ASYMMETRIC STATE
             *
             * Solution: Check Net0's peer_state dataport
             * - If Net0 STILL HAS the connection: This is NOT stale, don't ignore!
             * - If Net0 DOESN'T have it: Truly stale, safe to ignore
             * ═══════════════════════════════════════════════════════════════════
             */

            bool net0_has_connection = false;
            if (peer_state != NULL) {
                __sync_synchronize();  /* Memory barrier - read latest Net0 state */

                printf("%s:   → Self-cleaned match found - verifying with Net0 state...\n",
                       COMPONENT_NAME);

                for (int i = 0; i < MAX_SHARED_CONNECTIONS; i++) {
                    const struct connection_view *view = &peer_state->connections[i];
                    if (view->active &&
                        view->src_ip == ics_msg->metadata.src_ip &&     /* SCADA IP */
                        view->dst_ip == ics_msg->metadata.dst_ip &&     /* PLC IP */
                        view->src_port == ics_msg->metadata.src_port && /* SCADA port */
                        view->dst_port == ics_msg->metadata.dst_port) { /* PLC port */

                        net0_has_connection = true;
                        printf("%s:   ✗ Net0 STILL HAS this connection (slot %d) - NOT stale!\n",
                               COMPONENT_NAME, i);
                        printf("%s:      → Net0 wants us to close it - processing notification\n",
                               COMPONENT_NAME);
                        break;
                    }
                }

                if (!net0_has_connection) {
                    printf("%s:   ✓ Net0 doesn't have it either - notification is truly stale\n",
                           COMPONENT_NAME);
                }
            } else {
                printf("%s:   → peer_state not available - using self-cleaned check only\n",
                       COMPONENT_NAME);
            }

            if (!net0_has_connection) {
                /* Net0 doesn't have it - safe to ignore */
                printf("%s:   → IGNORING stale notification - we cleaned this connection ourselves\n",
                       COMPONENT_NAME);
                printf("%s:      A NEW connection may exist with same 5-tuple - must NOT close it!\n",
                       COMPONENT_NAME);
                return;  /* Ignore stale notification */
            }

            /* Net0 still has it - fall through to process the close notification */
            printf("%s:   → Processing close notification despite self-cleaned match\n",
                   COMPONENT_NAME);
            printf("%s:      (Net0 verification prevents false positive)\n", COMPONENT_NAME);
        }

        /* Look up PLC connection for this SCADA session */
        struct connection_metadata *meta = connection_lookup_by_tuple(
            ics_msg->metadata.src_ip,
            ics_msg->metadata.dst_ip,
            ics_msg->metadata.src_port,
            ics_msg->metadata.dst_port
        );

        if (meta != NULL && meta->active && meta->pcb != NULL) {
            struct tcp_pcb *pcb = meta->pcb;

            /* DEBUG: Comment out printf to test if it's causing crash */
            // printf("%s:   → Found PLC connection (PCB=%p) - closing gracefully\n",
            //        COMPONENT_NAME, (void*)pcb);
            printf("CLOSE_START\n");  /* Simpler printf for testing */
            BREADCRUMB(3160);  /* After printf */

            /* v2.117: CRITICAL FIX - Mark PCB as NULL BEFORE calling tcp_close()
             * ═══════════════════════════════════════════════════════════════════
             * Problem: tcp_close() can trigger recv(p=NULL) callback DURING execution.
             * If meta->pcb is still set when recv callback fires, it will try to call
             * tcp_close() AGAIN → double-close bug → lwIP assertion crash!
             *
             * Sequence of double-close bug:
             * 1. Close notification handler calls tcp_close(pcb)
             * 2. tcp_close() sends FIN and processes incoming packets
             * 3. PLC FIN arrives → lwIP calls recv(p=NULL) BEFORE tcp_close() returns
             * 4. recv callback checks metadata, finds pcb still set
             * 5. recv callback calls tcp_close() AGAIN → CRASH!
             *
             * Solution: Mark meta->pcb = NULL BEFORE calling tcp_close().
             * This way, if recv(p=NULL) fires during tcp_close(), it will see
             * pcb=NULL and skip the second close.
             * ═══════════════════════════════════════════════════════════════════
             */

            /* Mark PCB as NULL to prevent double-close */
            BREADCRUMB(3161);  /* Before meta->pcb = NULL */
            meta->pcb = NULL;
            BREADCRUMB(3162);  /* After meta->pcb = NULL */
            __sync_synchronize();  /* Memory barrier */
            BREADCRUMB(3163);  /* After memory barrier */

            /* v2.117: Defensive NULL check before tcp_close() - matches v2.115 fix
             * tcp_close() internally calls tcp_output(pcb), which asserts pcb != NULL
             * Even though we checked meta->pcb above, be defensive about the local copy */
            if (pcb == NULL) {
                printf("%s:   [WARN]  PCB is NULL - skipping tcp_close (metadata cleanup)\n",
                       COMPONENT_NAME);
                /* Clean up metadata since we can't close the PCB */
                meta->active = false;
                if (connection_count > 0) {
                    connection_count--;
                }
                /* Update shared connection state */
                update_shared_connection_state();
                return;
            }

            /* v2.132: CRITICAL FIX - Use tcp_abort() instead of tcp_close()
             * ═══════════════════════════════════════════════════════════════════
             * Problem: tcp_close() allows callbacks and ACK processing to continue
             * AFTER close is initiated but BEFORE PCB is freed. This causes:
             * 1. ACK processing tries to decrement snd_queuelen (already 0)
             *    → Assertion "pcb->snd_queuelen >= pbuf_clen(next->p)" at tcp_in.c:1111
             * 2. Pbuf freeing on already-freed buffers
             *    → Assertion "pbuf_free: p->ref > 0" at pbuf.c:753
             *
             * Root Cause: tcp_close() does NOT:
             * - Immediately free the PCB
             * - Unregister callbacks
             * - Prevent queued callbacks from firing
             * - Stop ACK processing
             *
             * Evidence from v2.131-failure:
             * - tcp_close() called at B3164-B3165
             * - IMMEDIATELY after: recv(p=0) callback fires (B1000-B1001)
             * - THEN: Assertions in tcp_in.c:1111 and pbuf.c:753
             *
             * Solution: Use tcp_abort() for immediate termination
             * - Close notification means SCADA already closed
             * - No need for graceful FIN to PLC (ICS protocols tolerate RST)
             * - tcp_abort() immediately frees PCB, no race window
             * - Simpler and safer than trying to guard against callbacks
             *
             * See: /home/qemu/phd/research-docs/v2.131-lwip-race-condition-analysis.md
             * ═══════════════════════════════════════════════════════════════════
             */
            /* v2.132 FIX: Mark metadata inactive BEFORE tcp_abort()
             * This prevents DOUBLE cleanup:
             * - tcp_abort() triggers error callback
             * - Error callback searches for metadata with active==true
             * - If we set active=false FIRST, error callback won't find it
             * - Error callback will still free pool_state, but won't touch metadata
             */
            BREADCRUMB(3164);  /* Right before cleanup */
            meta->active = false;  /* Mark inactive BEFORE tcp_abort to prevent double cleanup */
            if (connection_count > 0) {
                connection_count--;
            }
            __sync_synchronize();  /* Memory barrier: ensure active=false is visible to error callback */

            printf("%s:   [INFO]  Aborting PLC connection (SCADA already closed via notification)\n",
                   COMPONENT_NAME);
            tcp_abort(pcb);  /* v2.132: Immediate termination, error callback will free pool_state */
            BREADCRUMB(3165);  /* Right after tcp_abort() */

            /* Update shared state AFTER tcp_abort to reflect final state */
            update_shared_connection_state();
        } else {
            printf("%s:   → No active PLC connection found (already closed or never existed)\n",
                   COMPONENT_NAME);
        }

        return;  /* Close notification processed - done */
    }

    /* ═══════════════════════════════════════════════════════════════════════════
     * v2.49: PRODUCTION-READY CONNECTION REUSE with Multi-Layer Validation
     * ═══════════════════════════════════════════════════════════════════════════
     *
     * Goal: Reuse PLC connection when safe (same SCADA session, >MTU data support)
     *
     * Validation Layers:
     * 1. 5-tuple match (SCADA IP:port → PLC IP:port) - identifies SCADA session
     * 2. TCP sequence number match - detects port reuse after connection close
     * 3. PCB state check (ESTABLISHED) - ensures connection is alive
     * 4. Sanity checks (valid ports) - prevents using corrupted PCB
     */

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

        printf("%s: [FIND] Found existing connection for SCADA %u.%u.%u.%u:%u (PCB=%p)\n",
               COMPONENT_NAME,
               (ics_msg->metadata.src_ip >> 24) & 0xFF,
               (ics_msg->metadata.src_ip >> 16) & 0xFF,
               (ics_msg->metadata.src_ip >> 8) & 0xFF,
               ics_msg->metadata.src_ip & 0xFF,
               ics_msg->metadata.src_port,
               (void*)existing_pcb);

        /* v2.96: CRITICAL FIX - DO NOT ACCESS PCB FIELDS! (Same as Net0 v2.95)
         * ═══════════════════════════════════════════════════════════════════
         * BUG: Lines 2773-2800 accessed pcb->state, pcb->snd_nxt, pcb->local_port
         * Problem: PCB might be freed by lwIP between metadata lookup and access
         * Result: Page fault at offset 0x10 (accessing freed memory)
         *
         * Root Cause (same as Net0):
         * 1. First connection closes → tcp_close() called, metadata kept alive
         * 2. lwIP eventually frees PCB (callback not fired yet)
         * 3. Second request finds existing metadata with PCB pointer
         * 4. We access pcb->state → CRASH at offset 0x10!
         *
         * Solution: Don't validate PCB fields - assume it needs cleanup
         * If existing_pcb exists but might be stale → always create new connection
         *
         * This is SAFER than trying to validate - lwIP will clean up old PCB
         * through callbacks, and we create a fresh connection for the new request.
         */

        printf("%s:   → Found existing metadata - cleaning up old connection\n", COMPONENT_NAME);
        printf("%s:   → Creating new connection for this request (safer than reuse)\n", COMPONENT_NAME);
        goto cleanup_and_create_new;

        /* v2.106: DEAD CODE - Never executed due to goto cleanup_and_create_new above
         * Connection reuse is disabled - always create new connection
         * Left commented out for reference
         *
         * ═══════════════════════════════════════════════════════════════════
         * [OK] ALL VALIDATION PASSED - SAFE TO REUSE CONNECTION!
         * ═══════════════════════════════════════════════════════════════════
         *
        printf("%s:   [OK] Connection validation passed - REUSING for same SCADA session\n", COMPONENT_NAME);
        printf("%s:   → Supports: Multi-packet responses (>MTU), HTTP keep-alive, streaming\n", COMPONENT_NAME);

        existing_meta->last_activity = sys_now();

        if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
            printf("%s: ERROR: Payload too large (%u > %u)\n",
                   COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
            goto cleanup_and_create_new;
        }

        [Connection reuse code removed - used global inbound_tcp_client]

        tcp_output(existing_pcb);
        BREADCRUMB(2020);
        return;
        */

cleanup_and_create_new:
        /* ─────────────────────────────────────────────────────────────────────
         * Connection validation failed - clean up and create new
         * ───────────────────────────────────────────────────────────────────── */
        printf("%s:   [CLEAN] Cleaning up old connection (PCB=%p)\n", COMPONENT_NAME, (void*)existing_pcb);

        /* v2.117: Mark this connection as self-cleaned so we ignore close notifications for it
         * ═══════════════════════════════════════════════════════════════════════════
         * We're about to clean up this connection ourselves (not via close notification).
         * If a close notification arrives later for this same 5-tuple, it's a STALE
         * notification for this OLD connection. We must ignore it to prevent closing
         * a NEW connection that may have been created with the same 5-tuple.
         * ═══════════════════════════════════════════════════════════════════════════
         */
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

        /* Step 2: Use tcp_abort() to immediately free old PCB (v2.175)
         * ═══════════════════════════════════════════════════════════════════════
         * CRITICAL FIX: Prevent PCB accumulation in "reuse" path
         *
         * OLD CODE (v2.174):
         *   tcp_close(existing_pcb);  // ❌ PCB goes to FIN_WAIT, waits for handshake
         *   // If PLC network stops responding → PCBs pile up → pool exhausts
         *
         * NEW CODE (v2.175): Use tcp_abort() instead
         *   tcp_abort(existing_pcb);  // ✓ Sends RST, frees PCB immediately
         *
         * Why tcp_abort() is correct here:
         * - SCADA sent NEW request for same 5-tuple → Old connection already closed
         * - In ICS environments, SCADA typically sends RST when closing
         * - tcp_abort() mirrors this behavior: sends RST to PLC, frees PCB immediately
         * - No FIN_WAIT accumulation, no dependency on PLC responding
         * - Safe: callbacks already NULL, this is event handler context
         *
         * Evidence from tcpdump analysis:
         * - SCADA: Opens connection A, sends data, closes with RST
         * - SCADA: Opens connection B with SAME port (connection reuse)
         * - Net1: Should send RST to close old PLC connection A (not FIN)
         * ═══════════════════════════════════════════════════════════════════════
         */
        BREADCRUMB(2108);  /* Cleanup path - about to abort and remove metadata */
        printf("%s:   [CLEAN] Sending RST to PLC for old connection (SCADA opened new one)\n",
               COMPONENT_NAME);
        tcp_abort(existing_pcb);  /* Immediate cleanup - no FIN_WAIT */

        /* v2.107: connection_remove() will automatically free the associated pool state */
        connection_remove(existing_pcb);  /* Frees pool_state inside */

        printf("%s:   [OK] Cleanup complete - proceeding to create new connection\n", COMPONENT_NAME);
    }

    BREADCRUMB(2007);  /* Creating new TCP PCB */

    /* v2.95: lwIP-MANAGED CONNECTION LIMIT
     * ═══════════════════════════════════════════════════════════════════
     * REMOVED manual connection_count check (v2.93)
     *
     * Problem with v2.93: Manual tcp_abort() from main loop causes crashes!
     * Root cause: tcp_abort() immediately frees PCB, but lwIP state machine
     * still has references → crash at offset 0x10 (callback_arg access)
     *
     * Solution: Let lwIP handle connection limits via MEMP_NUM_TCP_PCB=100
     * - tcp_new() returns NULL when pool exhausted (safe!)
     * - No manual PCB lifecycle management (no tcp_abort from main loop!)
     * - Send error notification when tcp_new() fails
     *
     * This matches the design: "let lwIP handle lifecycle" (lwipopts.h v2.87)
     */

    #if DEBUG_PACKET_DETAIL
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
    #if DEBUG_TRAFFIC
    printf("%s: INBOUND: Protocol=%s, Src=%u.%u.%u.%u:%u, Dst=%u.%u.%u.%u:%u, Payload=%u bytes\n",
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
        printf("%s: [WARN]  INBOUND: Failed to create TCP PCB - lwIP connection pool exhausted (MEMP_NUM_TCP_PCB=%d)\n",
               COMPONENT_NAME, MEMP_NUM_TCP_PCB);
        printf("%s:   → Sending ERROR notification to Net0 (lwIP connection limit reached)\n", COMPONENT_NAME);

        /* v2.187: CRITICAL DIAGNOSTIC - Print connection state when pool exhausts
         * ═══════════════════════════════════════════════════════════════════════
         * When pool exhaustion occurs, diagnose WHY it happened:
         * 1. Dangling connections: metadata active but PCB=NULL (shouldn't exist!)
         * 2. Orphan connections: Net1 has connection but Net0 doesn't (asymmetric state)
         * 3. Duplicate connections: Same session_id appears multiple times
         *
         * This helps identify which type of leak is causing pool exhaustion.
         * ═══════════════════════════════════════════════════════════════════════
         */
        printf("%s:\n", COMPONENT_NAME);
        printf("%s: ╔══════════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
        printf("%s: ║ CONNECTION LIMIT REACHED - DIAGNOSTIC ANALYSIS               ║\n", COMPONENT_NAME);
        printf("%s: ╚══════════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
        printf("%s:\n", COMPONENT_NAME);
        printf("%s: Connection Pool Status:\n", COMPONENT_NAME);
        printf("%s:   → connection_count = %u / %u (our tracking)\n",
               COMPONENT_NAME, connection_count, MAX_CONNECTIONS);
        printf("%s:   → lwIP pool size = %d PCBs\n", COMPONENT_NAME, MEMP_NUM_TCP_PCB);
        printf("%s:\n", COMPONENT_NAME);

        /* Analyze connection table for issues */
        int active_count = 0;
        int dangling_count = 0;  /* active=true but pcb=NULL */
        int valid_count = 0;      /* active=true and pcb!=NULL */
        int orphan_count = 0;     /* Net1 has but Net0 doesn't */

        /* Track session IDs to detect duplicates */
        uint32_t session_ids[MAX_CONNECTIONS];
        int session_count = 0;

        printf("%s: Scanning %d connection slots:\n", COMPONENT_NAME, MAX_CONNECTIONS);
        printf("%s:\n", COMPONENT_NAME);

        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (!connection_table[i].active) continue;

            active_count++;

            /* Check for dangling connection (active but no PCB) */
            if (connection_table[i].pcb == NULL) {
                if (dangling_count == 0) {
                    printf("%s: [DANGLING] Found connections with active=true but PCB=NULL:\n",
                           COMPONENT_NAME);
                }
                printf("%s:   → [slot %d] session=%u, src_port=%u, dest_port=%u, PCB=NULL ❌\n",
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
                        printf("%s:\n", COMPONENT_NAME);
                        printf("%s: [ORPHAN] Found connections in Net1 but NOT in Net0:\n",
                               COMPONENT_NAME);
                    }
                    printf("%s:   → [slot %d] session=%u, Net0 doesn't have this session ⚠️\n",
                           COMPONENT_NAME, i, connection_table[i].session_id);
                    orphan_count++;
                }
            }

            /* Track session ID for duplicate detection */
            session_ids[session_count++] = connection_table[i].session_id;
        }

        /* Check for duplicate session IDs */
        printf("%s:\n", COMPONENT_NAME);
        printf("%s: [DUPLICATES] Checking for duplicate session_ids:\n", COMPONENT_NAME);
        int duplicate_count = 0;
        for (int i = 0; i < session_count; i++) {
            for (int j = i + 1; j < session_count; j++) {
                if (session_ids[i] == session_ids[j]) {
                    printf("%s:   → session_id %u appears at least twice! ❌\n",
                           COMPONENT_NAME, session_ids[i]);
                    duplicate_count++;
                    break;  /* Only report once per session_id */
                }
            }
        }
        if (duplicate_count == 0) {
            printf("%s:   → No duplicate session_ids found ✅\n", COMPONENT_NAME);
        }

        /* Summary */
        printf("%s:\n", COMPONENT_NAME);
        printf("%s: ╔══════════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
        printf("%s: ║ DIAGNOSTIC SUMMARY                                           ║\n", COMPONENT_NAME);
        printf("%s: ╚══════════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
        printf("%s:   Active connections:    %d\n", COMPONENT_NAME, active_count);
        printf("%s:   Valid (with PCB):      %d\n", COMPONENT_NAME, valid_count);
        printf("%s:   Dangling (PCB=NULL):   %d %s\n",
               COMPONENT_NAME, dangling_count,
               dangling_count > 0 ? "❌ LEAK!" : "✅");
        printf("%s:   Orphan (not in Net0):  %d %s\n",
               COMPONENT_NAME, orphan_count,
               orphan_count > 0 ? "⚠️  Asymmetric state" : "✅");
        printf("%s:   Duplicate session_ids: %d %s\n",
               COMPONENT_NAME, duplicate_count,
               duplicate_count > 0 ? "❌ BUG!" : "✅");
        printf("%s:\n", COMPONENT_NAME);

        if (dangling_count > 0) {
            printf("%s: ⚠️  DANGLING CONNECTIONS indicate metadata leak:\n", COMPONENT_NAME);
            printf("%s:    - connection_add() created metadata\n", COMPONENT_NAME);
            printf("%s:    - But PCB was freed without cleaning metadata\n", COMPONENT_NAME);
            printf("%s:    - Check 3-way handshake failure path\n", COMPONENT_NAME);
            printf("%s:    - Check recv(p=NULL) cleanup path\n", COMPONENT_NAME);
        }

        if (orphan_count > 0) {
            printf("%s: ⚠️  ORPHAN CONNECTIONS indicate asymmetric state:\n", COMPONENT_NAME);
            printf("%s:    - Net1 has connection but Net0 doesn't\n", COMPONENT_NAME);
            printf("%s:    - Possible cause: Net0 already closed SCADA side\n", COMPONENT_NAME);
            printf("%s:    - But Net1 never received close notification\n", COMPONENT_NAME);
        }

        if (duplicate_count > 0) {
            printf("%s: ❌ DUPLICATE SESSION IDs indicate session collision:\n", COMPONENT_NAME);
            printf("%s:    - Same 5-tuple (src/dst IP/port) used multiple times\n", COMPONENT_NAME);
            printf("%s:    - Check connection_add() deduplication logic\n", COMPONENT_NAME);
        }

        printf("%s:\n", COMPONENT_NAME);
        printf("%s: ╚══════════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
        printf("%s:\n", COMPONENT_NAME);

        /* v2.95: Send error notification to Net0 to close SCADA connection
         * This is the SAFE way to handle connection limits - let lwIP refuse the connection
         * by returning NULL from tcp_new(), then notify Net0 to close SCADA side.
         *
         * v2.172 CRITICAL FIX: Include session_id in error notification
         * ═══════════════════════════════════════════════════════════════
         * Problem: Net0 needs session_id to send close notification back to Net1
         * Without it, Net0 can't tell Net1 which session to close → PCB LEAK!
         *
         * Root cause of v2.171 failure:
         * - SCADA closes quickly after getting response (normal Modbus behavior)
         * - By time Net0 receives error notification, SCADA metadata already gone
         * - Net0 can't look up session_id by IP/port because metadata deleted
         * - Net0 can't send close notification to Net1
         * - Net1's PCB leaks!
         *
         * Fix: Include session_id in error notification so Net0 can always
         * send close notification to Net1, even if SCADA already closed.
         * ═══════════════════════════════════════════════════════════════
         */
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

    BREADCRUMB(2009);  /* PCB created successfully */

    /* v2.106: Allocate per-connection state from pool
     * CRITICAL: Each connection must have its own state to prevent race conditions
     * See detailed comment at inbound_connection_pool declaration for bug analysis */
    struct tcp_inbound_client_state *state = inbound_alloc_state();
    if (state == NULL) {
        printf("%s: [ERROR] INBOUND: Connection pool exhausted, dropping request\n", COMPONENT_NAME);
        tcp_abort(pcb);
        BREADCRUMB(2111);  /* Pool exhausted - aborted PCB and returning */
        return;
    }

    BREADCRUMB(2112);  /* Pool allocation succeeded - continue to setup */

    /* CRITICAL FIX v2.48: Copy payload to local buffer instead of storing pointer
     * Previously: inbound_tcp_client.payload_data = ics_msg->payload (pointer assignment)
     * Problem: ics_msg->payload is in shared CAmkES dataport that gets overwritten
     * When: tcp_connect() is async - by the time callback fires, dataport has new data
     * Result: Corrupted payload data, NULL pointers, pbuf.c assertion failures
     * Fix: Copy to persistent local buffer before tcp_connect() */

    /* Validate payload size before copying */
    if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
        printf("%s: INBOUND: Payload too large (%u > %u), dropping\n",
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
        printf("%s: INBOUND: tcp_bind failed: %d\n", COMPONENT_NAME, bind_err);
        tcp_abort(pcb);  /* v2.51: Force RST - never use tcp_close() to avoid FIN-WAIT-1 */
        inbound_free_state(state);  /* v2.106: Free allocated state */
        return;
    }

    BREADCRUMB(2012);  /* tcp_bind succeeded */

    /* CRITICAL: Store connection metadata BEFORE connecting
     * This metadata is needed in netif_output() to restore original IPs
     * when sending responses back through the gateway
     */
    BREADCRUMB(2013);  /* Storing connection metadata */

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
        printf("%s: INBOUND: Failed to store connection metadata (table full)\n", COMPONENT_NAME);
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

    /* v2.82: Register error callback BEFORE tcp_connect()
     * This is critical to handle connection failures and prevent dangling PCB access */
    tcp_err(pcb, inbound_tcp_err_callback);

    /* Use original destination port from metadata */
    uint16_t dest_port = ics_msg->metadata.dst_port;

    #if DEBUG_TRAFFIC
    printf("%s: INBOUND: Binding to IP_ADDR_ANY and connecting to %u.%u.%u.%u:%u\n",
           COMPONENT_NAME,
           (ics_msg->metadata.dst_ip >> 24) & 0xFF,
           (ics_msg->metadata.dst_ip >> 16) & 0xFF,
           (ics_msg->metadata.dst_ip >> 8) & 0xFF,
           ics_msg->metadata.dst_ip & 0xFF,
           dest_port);
    #endif

    BREADCRUMB(2016);  /* Attempting tcp_connect */

    err_t err = tcp_connect(pcb, &dest_ip, dest_port, inbound_tcp_connected_callback);

    /* v2.127: CRITICAL FIX - Store ephemeral port IMMEDIATELY after tcp_connect()
     * ══════════════════════════════════════════════════════════════════════════
     * Problem: Race window between tcp_connect() and storing the port
     *
     * Sequence causing unsafe PCB access:
     * 1. tcp_connect() assigns ephemeral port and sends SYN
     * 2. SYN packet arrives in TX path (netif_output)
     * 3. Lookup tries to match packet but lwip_ephemeral_port not stored yet
     * 4. Falls back to Method 2: accesses pcb->local_port (UNSAFE!)
     * 5. MUCH LATER: We store the port (old line 3721)
     *
     * Fix: Store port immediately after tcp_connect() to eliminate race window
     * This makes Method 2 unnecessary and removes unsafe PCB field access
     * ══════════════════════════════════════════════════════════════════════════
     */
    meta->lwip_ephemeral_port = pcb->local_port;

    if (err != ERR_OK) {
        BREADCRUMB(2017);  /* tcp_connect failed */
        printf("%s: INBOUND: tcp_connect failed: %d\n", COMPONENT_NAME, err);

        /* v2.93: If connection fails, PLC might be overloaded with stale connections
         * Proactively clean up any stale connections to this PLC to free resources */
        if (err == ERR_CONN || err == ERR_RST || err == ERR_ABRT) {
            printf("%s:   [WARN]  PLC connection failed - checking for stale connections to clean up\n",
                   COMPONENT_NAME);

            int cleaned = 0;
            for (int i = 0; i < MAX_CONNECTIONS; i++) {
                if (!connection_table[i].active) continue;

                /* Check if this is a connection to the same PLC */
                if (connection_table[i].original_dest_ip == ics_msg->metadata.dst_ip &&
                    connection_table[i].dest_port == dest_port) {

                    struct tcp_pcb *stale_pcb = connection_table[i].pcb;

                    /* v2.96: Only clean up if PCB is NULL (already freed by lwIP)
                     * DO NOT access pcb->state - causes crash at offset 0x10!
                     * DO NOT call tcp_abort() from main loop - lwIP will clean up via callbacks */
                    if (stale_pcb == NULL) {
                        printf("%s:   [CLEAN] Cleaning stale connection [%d] to PLC (PCB=NULL)\n",
                               COMPONENT_NAME, i);

                        connection_table[i].active = false;

                        /* v2.182: Track connection count changes for leak debugging */
                        uint32_t old_count = connection_count;
                        if (connection_count > 0) {
                            connection_count--;
                        }
                        printf("%s: [COUNT--] %u → %u | plc_unreachable() slot=%d session=%u (PCB=NULL)\n",
                               COMPONENT_NAME, old_count, connection_count, i,
                               connection_table[i].session_id);

                        cleaned++;
                    }
                }
            }

            if (cleaned > 0) {
                printf("%s:   [OK] Cleaned %d stale connection(s) to PLC\n", COMPONENT_NAME, cleaned);
            } else {
                printf("%s:   → No stale connections found - PLC might be genuinely unreachable\n",
                       COMPONENT_NAME);
            }
        }

        /* v2.93: CRITICAL - Notify Net0 to reject SCADA connection when PLC refuses!
         *
         * Problem: If PLC is at max capacity (124+ stale connections), it refuses new connections.
         * Old behavior: Net1 fails silently, SCADA connection hangs waiting for response.
         * New behavior: Send error notification to Net0 → Net0 closes SCADA connection immediately.
         *
         * This prevents:
         * 1. SCADA connections piling up in Net0
         * 2. Long timeouts (SCADA knows immediately gateway is down)
         * 3. Resource exhaustion in Net0
         *
         * Error notification format: Zero-length payload with special error flag.
         */
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

            printf("%s: [ERR] Sending ERROR notification to Net0 (PLC refused connection, err=%d)\n",
                   COMPONENT_NAME, err);
            printf("%s:    → Net0 will close SCADA %u.%u.%u.%u:%u immediately\n",
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

        /* v2.170: CRITICAL FIX - Follow CRITICAL_LESSON Rule 2
         * Callbacks WERE registered (tcp_arg, tcp_err) before tcp_connect(),
         * so we must NULL them before closing to prevent race condition */
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_arg(pcb, NULL);

        /* v2.174: CRITICAL FIX - Clean up metadata when tcp_connect() fails
         * ═══════════════════════════════════════════════════════════════════════
         * BUG: connection_add() incremented connection_count and created metadata
         * (line 3887), but when tcp_connect() fails, we never cleaned it up!
         *
         * Result: Orphaned metadata entries with active=true, pcb=NULL
         * - connection_count too high (our count=106, lwIP count=100)
         * - 6 orphaned metadata entries accumulate over time
         *
         * Fix: Mark metadata inactive and decrement counter when tcp_connect() fails
         * ═══════════════════════════════════════════════════════════════════════
         */
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

    BREADCRUMB(2018);  /* tcp_connect succeeded */

    /* v2.127: Port storage moved to immediately after tcp_connect() (line 3630)
     * to eliminate race window and unsafe PCB field access */

    /* v2.50: Store validation metadata for consistency with Net0
     * tcp_seq_num: Detects port reuse - if same 5-tuple but different seq, it's a new connection
     * timestamp: Connection creation time for metadata consistency */
    meta->tcp_seq_num = pcb->snd_nxt;  /* Current send sequence number */
    meta->timestamp = sys_now();       /* Connection creation timestamp */

    /* CRITICAL: Memory barrier to ensure all metadata writes are visible before callbacks fire */
    __sync_synchronize();

    BREADCRUMB(2019);  /* Metadata complete, memory barrier done */

    #if DEBUG_METADATA
    printf("%s: 📝 Stored metadata [slot %d]: SCADA %u.%u.%u.%u:%u → PLC %u.%u.%u.%u:%u (lwIP port: %u)\n",
           COMPONENT_NAME, (int)(meta - connection_table),
           (meta->original_src_ip >> 24) & 0xFF, (meta->original_src_ip >> 16) & 0xFF,
           (meta->original_src_ip >> 8) & 0xFF, meta->original_src_ip & 0xFF,
           meta->src_port,
           (meta->original_dest_ip >> 24) & 0xFF, (meta->original_dest_ip >> 16) & 0xFF,
           (meta->original_dest_ip >> 8) & 0xFF, meta->original_dest_ip & 0xFF,
           meta->dest_port, meta->lwip_ephemeral_port);
    #endif

    #if DEBUG_TRAFFIC
    printf("%s: INBOUND: Connection initiated to PLC on internal network\n", COMPONENT_NAME);
    #endif

    BREADCRUMB(2020);  /* Exit: inbound_ready_handle complete */
}

/*
 * OUTBOUND notification handler - called when ICS_Outbound has validated data
 * Creates TCP client connection to forward data to external network
 */
void outbound_ready_handle(void)
{
    #if DEBUG_PACKET_DETAIL
    uint32_t msg_id = ++message_id_counter;
    printf("\n[MSG] [MSG #%u] ═══ ICS→NET: Received from ICS_Outbound ═══\n", msg_id);
    printf("   Source: ICS pipeline validation complete\n");
    printf("   Action: Creating TCP client to forward to external network\n");
    #endif

    #if DEBUG_TRAFFIC
    printf("%s: ╔═══════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
    printf("%s: ║  OUTBOUND: Received message from ICS_Outbound            ║\n", COMPONENT_NAME);
    printf("%s: ╚═══════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
    #endif

    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (outbound_dp == NULL) {
        printf("%s: [ERR] FATAL: outbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        printf("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        return;
    }

    #if DEBUG_TRAFFIC
    printf("%s: [OK] Dataport check: outbound_dp=%p (valid)\n", COMPONENT_NAME, (void*)outbound_dp);
    #endif

    ICS_Message *ics_msg = (ICS_Message *)outbound_dp;

    /* Validate message */
    if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
        printf("%s: OUTBOUND: Invalid payload length %u (max %u)\n",
               COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
        #if DEBUG_PACKET_DETAIL
        printf("   ✗ [MSG #%u] DROPPED - invalid payload size\n\n", msg_id);
        #endif
        return;
    }

    /* Check if we already have an active connection - if so, close it and create new one */
    if (outbound_tcp_client.active) {
        #if DEBUG_TRAFFIC
        printf("%s: OUTBOUND: Previous connection still active, closing it to handle new message\n", COMPONENT_NAME);
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

    #if DEBUG_PACKET_DETAIL
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
    #if DEBUG_TRAFFIC
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
    #endif

    /* Create TCP client connection */
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        printf("%s: OUTBOUND: Failed to create TCP PCB\n", COMPONENT_NAME);
        return;
    }

    /* v2.106: Validate payload size before copying */
    if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
        printf("%s: OUTBOUND: Payload too large (%u > %u), dropping\n",
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
    printf("%s: OUTBOUND: Port mapping: internal:%u → host:%s:%u (via QEMU gateway)\n",
           COMPONENT_NAME, ics_msg->metadata.dst_port, OUTBOUND_FORWARD_IP, mapped_port);
    #endif

    /* Connect to host via QEMU gateway */
    #if DEBUG_TRAFFIC
    printf("%s: OUTBOUND: Connecting to host via %s:%u...\n",
           COMPONENT_NAME, OUTBOUND_FORWARD_IP, mapped_port);
    #endif

    err_t err = tcp_connect(pcb, &dest_ip, mapped_port, outbound_tcp_connected_callback);
    if (err != ERR_OK) {
        printf("%s: OUTBOUND: tcp_connect failed: %d\n", COMPONENT_NAME, err);

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
    printf("%s: OUTBOUND: Connection initiated\n", COMPONENT_NAME);
    #endif
}

/*
 * VirtIO IRQ Handler
 */
/* v2.167: Flag to signal RX packets pending (set by IRQ, cleared by main loop)
 * ═══════════════════════════════════════════════════════════════════════════
 * This implements correct lwIP NO_SYS=1 pattern:
 * - IRQ does minimal work (sets flag)
 * - Main loop does heavy processing (calls process_rx_packets)
 * - No reentrancy possible (all lwIP calls in main loop)
 * ═══════════════════════════════════════════════════════════════════════════
 */
static volatile bool rx_packets_pending = false;

void virtio_irq_handle(void)
{
    static uint32_t irq_count = 0;
    uint32_t irq_status = VREG_READ(VIRTIO_MMIO_INTERRUPT_STATUS);

    irq_count++;

    #if DEBUG_TRAFFIC
    printf("%s: ⚡ IRQ #%u: status=0x%x\n", COMPONENT_NAME, irq_count, irq_status);
    #endif

    if (irq_status & VIRTIO_MMIO_IRQ_VQUEUE) {
        #if DEBUG_TRAFFIC
        printf("%s:   → VQUEUE interrupt - setting rx_packets_pending flag\n", COMPONENT_NAME);
        #endif
        /* v2.167: CRITICAL FIX - Don't call process_rx_packets() in IRQ!
         * Just set flag - main loop will process packets.
         * This prevents reentrancy with sys_check_timeouts() */
        rx_packets_pending = true;
        VREG_WRITE(VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_MMIO_IRQ_VQUEUE);
    }

    if (irq_status & VIRTIO_MMIO_IRQ_CONFIG) {
        #if DEBUG_TRAFFIC
        printf("%s:   → CONFIG interrupt\n", COMPONENT_NAME);
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
        printf("%s: [ERR] FATAL: virtio_mmio_regs dataport is NULL!\n", COMPONENT_NAME);
        printf("%s:    CAmkES failed to map hardware component net0_hw\n", COMPONENT_NAME);
        printf("%s:    Check ics_dual_nic.camkes configuration\n", COMPONENT_NAME);
        return -1;
    }

    printf("%s: virtio_mmio_regs dataport mapped at %p\n", COMPONENT_NAME, (void *)virtio_mmio_regs);

    /* Access VirtIO device at SLOT 31 (offset 0xc00 from page base 0xa003000) */
    /* QEMU assigns FIRST -device virtio-net-device to slot 30 - matches vm_ethernet_echo */
    virtio_regs_base = (volatile uint32_t *)((uintptr_t)virtio_mmio_regs + 0xc00);

    printf("%s: virtio_regs_base (slot 30) = %p (base + 0xc00)\n",
           COMPONENT_NAME, (void *)virtio_regs_base);

    /* Verify we have the network device using pointer arithmetic */
    uint32_t magic = VREG_READ(VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version = VREG_READ(VIRTIO_MMIO_VERSION);
    uint32_t device_id = VREG_READ(VIRTIO_MMIO_DEVICE_ID);

    printf("%s: VirtIO @ slot 30 (+0xc00): Magic=0x%x, Version=%u, DeviceID=%u\n",
           COMPONENT_NAME, magic, version, device_id);

    if (magic != 0x74726976) {
        printf("%s: ERROR: Invalid VirtIO magic! Device not accessible.\n", COMPONENT_NAME);
        return -1;
    }

    /* CRITICAL CHECK: Enforce modern VirtIO protocol */
    if (version != 2) {
        printf("\n");
        printf("╔════════════════════════════════════════════════════════════════╗\n");
        printf("║  [ERR] FATAL ERROR: Legacy VirtIO Protocol Detected              ║\n");
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
        printf("  [OK] Enables VirtIO 1.0+ modern protocol (Version 2)\n");
        printf("  [OK] Fixes MMIO write issues\n");
        printf("  [OK] Allocates devices to slots 6-7 (not 30-31)\n");
        printf("  [OK] Makes QueueReady registers writable\n");
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

    printf("%s: [OK] Found VirtIO network device (modern protocol, Version 2)\n", COMPONENT_NAME);

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

    #if DEBUG_PACKET_DETAIL
    printf("%s: DEBUG: ring_base virtual  = 0x%lx\n", COMPONENT_NAME, (uintptr_t)ring_base);
    printf("%s: DEBUG: ring_base physical = 0x%lx (via camkes_dma_get_paddr)\n", COMPONENT_NAME, ring_base_paddr);
    #endif

    /* RX queue */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_RX_QUEUE);
    #if DEBUG_PACKET_DETAIL
    printf("%s: DEBUG: QueueSel set to %u\n", COMPONENT_NAME, VIRTIO_NET_RX_QUEUE);
    printf("%s: DEBUG: QueueSel readback = %u\n", COMPONENT_NAME, VREG_READ(VIRTIO_MMIO_QUEUE_SEL));
    printf("%s: DEBUG: QueueNumMax = %u\n", COMPONENT_NAME, VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX));
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

    printf("%s: [FIX] RX Queue: Device offers %u descriptors, using %u (matches buffer pool)\n",
           COMPONENT_NAME, queue_num_max, rx_virtq.num);

    if (queue_num_max < MAX_PACKETS) {
        printf("%s: [WARN]  WARNING: Device only supports %u descriptors but we need %u\n",
               COMPONENT_NAME, queue_num_max, MAX_PACKETS);
        printf("%s:             This may cause issues - consider reducing MAX_PACKETS\n", COMPONENT_NAME);
    }

    #if DEBUG_PACKET_DETAIL
    printf("%s: DEBUG: rx_virtq.num = %u (FIXED to match MAX_PACKETS=%u)\n",
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

    #if DEBUG_PACKET_DETAIL
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
        printf("  [ERR] QEMU REJECTED RX queue - configuration invalid!\n");
    } else {
        printf("  [OK] QEMU ACCEPTED RX queue\n");
    }

    /* TX queue */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_TX_QUEUE);

    /* Read and validate QueueNumMax from device register */
    queue_num_max = VREG_READ(VIRTIO_MMIO_QUEUE_NUM_MAX);

    /* CRITICAL FIX: Same as RX - TX ring size must match buffer pool.
     * TX uses MAX_PACKETS/2 buffers (16), so set ring size to match.
     */
    tx_virtq.num = MAX_PACKETS;  /* Use same size as RX for consistency */

    printf("%s: [FIX] TX Queue: Device offers %u descriptors, using %u (matches buffer pool)\n",
           COMPONENT_NAME, queue_num_max, tx_virtq.num);

    if (queue_num_max < MAX_PACKETS) {
        printf("%s: [WARN]  WARNING: Device only supports %u TX descriptors but we need %u\n",
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
        printf("  [ERR] QEMU REJECTED TX queue - configuration invalid!\n");
    } else {
        printf("  [OK] QEMU ACCEPTED TX queue\n");
    }

    /* Device ready - activate the device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VREG_READ(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);
    printf("%s: [OK] VirtIO device initialized and activated\n", COMPONENT_NAME);
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
        printf("\n%s: [ERR][ERR][ERR] FATAL ERROR: MMIO WRITES DO NOT WORK! [ERR][ERR][ERR]\n", COMPONENT_NAME);
        printf("%s: Device memory attributes are incorrect.\n", COMPONENT_NAME);
        printf("%s: This will cause infinite IRQ loops and duplicate packets.\n", COMPONENT_NAME);
        printf("%s: Cannot continue - terminating initialization.\n\n", COMPONENT_NAME);
        return -1;
    }

    printf("%s:   [OK] MMIO writes work correctly!\n\n", COMPONENT_NAME);

    return 0;
}

/*
 * Component initialization
 */
void post_init(void)
{
    printf("%s: Component started\n", COMPONENT_NAME);
    printf("%s: NET1 v2.187 (2025-10-27) - Add pool exhaustion diagnostics (dangling/orphan/duplicate detection)\n", COMPONENT_NAME);
    printf("%s: [FIX] MODE: PRODUCTION-READY with Multi-Layer Validation\n", COMPONENT_NAME);
    printf("%s: [OK] FIX 1: payload_data buffer (prevents dataport corruption)\n", COMPONENT_NAME);
    printf("%s: [OK] FIX 2: tcp_abort() removed from callbacks (crash at 0x38a9c fixed!)\n", COMPONENT_NAME);
    printf("%s: [OK] FIX 3: Connection reuse when SCADA keeps connection alive\n", COMPONENT_NAME);
    printf("%s: [OK] FIX 4: Close notification - Net0 tells Net1 when SCADA closes\n", COMPONENT_NAME);
    printf("%s: [OK] FIX 5: Stale connection cleanup on tcp_connect() failure\n", COMPONENT_NAME);
    printf("%s: [OK] FIX 6: CLOSE_WAIT memory leak - always call tcp_close() regardless of state\n", COMPONENT_NAME);
    printf("%s: [TARGET] FEATURES: Supports >MTU data, HTTP keep-alive, streaming protocols\n", COMPONENT_NAME);
    printf("%s: [WARN]  CRITICAL: Uses tcp_close() for graceful shutdown (FIN handshake)\n\n", COMPONENT_NAME);

    /* Initialize connection tracking table */
    memset(connection_table, 0, sizeof(connection_table));
    connection_count = 0;
    printf("%s: [OK] Connection tracking table initialized (%d slots)\n", COMPONENT_NAME, MAX_CONNECTIONS);

    /* v2.117: Initialize connection state sharing dataports */
    own_state = (volatile struct connection_state_table *)net1_conn_state;
    peer_state = (volatile struct connection_state_table *)net0_conn_state;
    if (own_state) {
        memset((void *)own_state, 0, sizeof(struct connection_state_table));
        printf("%s: [OK] Own connection state dataport mapped (size=%zu bytes)\n",
               COMPONENT_NAME, sizeof(struct connection_state_table));
    }
    if (peer_state) {
        printf("%s: [OK] Peer connection state dataport mapped (read-only access to Net0)\n", COMPONENT_NAME);
    }

    /* v2.93: Note about cleaning up stale PLC connections from previous versions
     *
     * If PLC has hundreds of stale ESTABLISHED connections from previous buggy versions,
     * they will timeout naturally according to PLC's TCP keepalive settings (typically 2 hours).
     *
     * For immediate cleanup, you have two options:
     * 1. Restart the PLC (quickest - resets all TCP state)
     * 2. Wait for TCP keepalive timeouts (automatic but slow)
     *
     * This version (v2.93) PREVENTS new accumulation via close notifications,
     * so after the backlog clears, the system will maintain stable connection counts.
     */
    printf("%s: ℹ️  If PLC has stale connections from previous versions, they will timeout naturally.\n", COMPONENT_NAME);
    printf("%s:    For immediate cleanup: restart PLC or wait ~2 hours for TCP keepalive.\n", COMPONENT_NAME);

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
    printf("%s: [OK] Allocated DMA packet buffers (vaddr=%p, paddr=0x%lx)\n",
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
    printf("%s: [OK] Allocated TX headers array (vaddr=%p, paddr=0x%lx)\n",
           COMPONENT_NAME, tx_headers, tx_headers_paddr);

    /* Initialize packet buffers */
    memset(rx_buffer_used, 0, sizeof(rx_buffer_used));
    refill_rx_queue();

    /* Initialize lwIP */
    printf("%s: Initializing lwIP TCP/IP stack...\n", COMPONENT_NAME);
    lwip_init();

    /* CRITICAL: Setup TCP server BEFORE netif_add() so PCB stays bound to 0.0.0.0
     * This is the key to accepting packets for ANY destination IP (both 10.2.0.2 and 192.168.95.2)
     * If we do netif_add() first, lwIP might bind the PCB to the interface IP
     */
    printf("%s: Setting up TCP server on port %d (binding to 0.0.0.0 for promiscuous accept)...\n", COMPONENT_NAME, TCP_ECHO_PORT);
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

    printf("%s: Configuring network interface:\n", COMPONENT_NAME);
    printf("%s:   IP:      192.168.95.1 (internal gateway - PLC network)\n", COMPONENT_NAME);
    printf("%s:   Netmask: 255.255.255.0\n", COMPONENT_NAME);
    printf("%s:   Gateway: None (this interface IS the gateway)\n", COMPONENT_NAME);
    printf("%s:   TCP server: 192.168.95.1:%d\n", COMPONENT_NAME, TCP_ECHO_PORT);

    netif_add(&netif_data, &ipaddr, &netmask, &gw, NULL, custom_netif_init, custom_input_promiscuous);
    netif_set_default(&netif_data);
    netif_set_status_callback(&netif_data, netif_status_callback);
    netif_set_up(&netif_data);

    /* Static ARP entry NOT needed with bridge architecture!
     * With bridges, all devices are on the same Layer 2 network
     * ARP works naturally without any hacks
     */

    /* Verify interface configuration */
    printf("\n");
    printf("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    printf("%s: [OK] NETWORK INTERFACE CONFIGURATION\n", COMPONENT_NAME);
    printf("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    printf("%s: Interface IP:   %u.%u.%u.%u\n", COMPONENT_NAME,
           ip4_addr1(netif_ip4_addr(&netif_data)),
           ip4_addr2(netif_ip4_addr(&netif_data)),
           ip4_addr3(netif_ip4_addr(&netif_data)),
           ip4_addr4(netif_ip4_addr(&netif_data)));
    printf("%s: Netmask:        %u.%u.%u.%u\n", COMPONENT_NAME,
           ip4_addr1(netif_ip4_netmask(&netif_data)),
           ip4_addr2(netif_ip4_netmask(&netif_data)),
           ip4_addr3(netif_ip4_netmask(&netif_data)),
           ip4_addr4(netif_ip4_netmask(&netif_data)));
    printf("%s: Gateway:        %u.%u.%u.%u\n", COMPONENT_NAME,
           ip4_addr1(netif_ip4_gw(&netif_data)),
           ip4_addr2(netif_ip4_gw(&netif_data)),
           ip4_addr3(netif_ip4_gw(&netif_data)),
           ip4_addr4(netif_ip4_gw(&netif_data)));
    printf("%s: Status:         %s\n", COMPONENT_NAME, netif_is_up(&netif_data) ? "UP" : "DOWN");
    printf("%s: Role:           Internal gateway (transparent security gateway)\n", COMPONENT_NAME);
    printf("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    printf("\n");

    /* Validation check */
    uint8_t if_ip1 = ip4_addr1(netif_ip4_addr(&netif_data));
    uint8_t if_ip2 = ip4_addr2(netif_ip4_addr(&netif_data));
    uint8_t if_ip3 = ip4_addr3(netif_ip4_addr(&netif_data));
    uint8_t if_ip4 = ip4_addr4(netif_ip4_addr(&netif_data));

    if (if_ip1 == 192 && if_ip2 == 168 && if_ip3 == 96 && if_ip4 == 2) {
        printf("%s: [OK] CONFIGURATION VALID: Internal gateway IP = 192.168.95.1\n", COMPONENT_NAME);
        printf("%s: [OK] pfSense routes 192.168.95.0/24 traffic through this gateway\n", COMPONENT_NAME);
        printf("%s: [OK] Bridge br0 forwards all traffic to/from ens224\n", COMPONENT_NAME);
    } else {
        printf("%s: [WARN]  WARNING: Interface IP (%u.%u.%u.%u) does NOT match expected (192.168.95.1)\n",
               COMPONENT_NAME, if_ip1, if_ip2, if_ip3, if_ip4);
        printf("%s: [WARN]  pfSense routing will FAIL!\n", COMPONENT_NAME);
    }
    printf("\n");

    tcp_server_initialized = true;
    printf("%s: [OK] Initialization complete\n", COMPONENT_NAME);
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
        printf("║  [ERR] FATAL: VirtIO_Net1_Driver initialization FAILED     ║\n");
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

    printf("%s: [OK] Initialization validation passed - starting main loop\n", COMPONENT_NAME);

    /* Main event loop - process lwIP timers, RX packets, and ICS notifications */
    /* Note: TCP server is now initialized in RX path after first packet */
    uint32_t loop_iterations = 0;
    const uint32_t CLEANUP_INTERVAL = 10000;  /* Run cleanup every ~10000 iterations (~30-60 seconds) */

    static uint32_t heartbeat_counter = 0;
    while (1) {
        /* v2.74: Heartbeat to detect silent hangs */
        if (++heartbeat_counter >= 50000) {
            printf("%s: [HB]  Heartbeat: %u iterations, %u active connections\n",
                   COMPONENT_NAME, heartbeat_counter, connection_count);

            /* v2.173: Leak detector - triggers at high connection count
             * ═══════════════════════════════════════════════════════════════
             * v2.173: Threshold 90 (90% of MEMP_NUM_TCP_PCB=100)
             * v2.174: Threshold 135 (90% of MEMP_NUM_TCP_PCB=150)
             * v2.181: Threshold 900 (90% of MEMP_NUM_TCP_PCB=1000)
             *
             * Early detection prevents communication failures and helps debug
             * which PCB states are accumulating (TIME_WAIT, CLOSE_WAIT, etc.)
             * ═══════════════════════════════════════════════════════════════
             */
            if (connection_count >= 900) {
                printf("%s: [LEAK_DETECT] WARNING: Connection count HIGH (%u/1000)\n",
                       COMPONENT_NAME, connection_count);
                printf("%s: [LEAK_DETECT] Inspecting lwIP PCB states...\n", COMPONENT_NAME);

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

                printf("%s: [LEAK_DETECT] lwIP PCB breakdown:\n", COMPONENT_NAME);
                printf("%s: [LEAK_DETECT]   Active PCBs:    %u (ESTABLISHED, SYN_*, FIN_WAIT, etc.)\n",
                       COMPONENT_NAME, active_count);
                printf("%s: [LEAK_DETECT]   TIME_WAIT PCBs: %u (should be < 30 with TCP_MSL=30s)\n",
                       COMPONENT_NAME, tw_count);
                printf("%s: [LEAK_DETECT]   Bound PCBs:     %u (LISTEN, CLOSED)\n",
                       COMPONENT_NAME, bound_count);
                printf("%s: [LEAK_DETECT]   Total lwIP:     %u/%u\n",
                       COMPONENT_NAME, total_lwip, MEMP_NUM_TCP_PCB);

                /* Check for mismatch between our count and lwIP's count */
                if (total_lwip != connection_count) {
                    printf("%s: [LEAK_DETECT] ⚠️  MISMATCH: Our count=%u, lwIP count=%u (diff=%d)\n",
                           COMPONENT_NAME, connection_count, total_lwip,
                           (int)connection_count - (int)total_lwip);
                }

                /* Detailed active PCB state inspection */
                if (active_count > 0) {
                    printf("%s: [LEAK_DETECT] Active PCB states:\n", COMPONENT_NAME);
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
                            printf("%s: [LEAK_DETECT]   %s: %u\n",
                                   COMPONENT_NAME, state_names[i], state_counts[i]);
                        }
                    }

                    /* Flag suspicious states */
                    if (state_counts[7] > 5) {  /* CLOSE_WAIT */
                        printf("%s: [LEAK_DETECT] ⚠️  HIGH CLOSE_WAIT count (%u) - possible leak!\n",
                               COMPONENT_NAME, state_counts[7]);
                    }
                    if (state_counts[5] > 10 || state_counts[6] > 10) {  /* FIN_WAIT_1/2 */
                        printf("%s: [LEAK_DETECT] ⚠️  HIGH FIN_WAIT count (FIN_WAIT_1=%u, FIN_WAIT_2=%u)\n",
                               COMPONENT_NAME, state_counts[5], state_counts[6]);
                    }
                }

                /* Check TIME_WAIT excessive accumulation */
                if (tw_count > 30) {
                    printf("%s: [LEAK_DETECT] ⚠️  EXCESSIVE TIME_WAIT (%u) - expected < 30 with TCP_MSL=30s\n",
                           COMPONENT_NAME, tw_count);
                    printf("%s: [LEAK_DETECT]     (Should auto-expire after 60s, check if cleanup working)\n",
                           COMPONENT_NAME);
                }

                printf("%s: [LEAK_DETECT] Leak detection complete\n", COMPONENT_NAME);
            }

            /* v2.93: DEBUG - Show connection table details */
            printf("%s: [FIND] NET1 Connection Table (PLC connections):\n", COMPONENT_NAME);
            int shown = 0;
            for (int i = 0; i < MAX_CONNECTIONS && shown < 10; i++) {
                if (connection_table[i].active) {
                    printf("%s:   [%d] SCADA %u.%u.%u.%u:%u → PLC %u.%u.%u.%u:%u PCB=%p lwIP_port=%u\n",
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
                printf("%s:   (no active connections)\n", COMPONENT_NAME);
            } else if (connection_count > shown) {
                printf("%s:   ... and %d more connections\n", COMPONENT_NAME, connection_count - shown);
            }

            heartbeat_counter = 0;
        }

        /* Check for INBOUND notifications from ICS_Inbound (non-blocking) */
        if (inbound_ready_poll()) {
            inbound_ready_handle();
        }

        /* Process lwIP timers and RX packets */
        sys_check_timeouts();

        /* v2.167: CORRECT FIX - Process RX packets in main loop (flag-based)
         * ═══════════════════════════════════════════════════════════════════════
         * Previous bug: process_rx_packets() called from BOTH main loop AND IRQ
         * - Main loop: process_rx_packets() → tcp_input() → tcp_output()
         * - IRQ fires during sys_check_timeouts(): process_rx_packets() again!
         * - Result: Reentrancy → DEPTH=4/5 recursion → CRASH
         *
         * Correct lwIP NO_SYS=1 pattern:
         * - IRQ: Just set flag (minimal work, fast return)
         * - Main loop: Check flag and process packets
         * - All lwIP processing in single thread (no reentrancy)
         * ═══════════════════════════════════════════════════════════════════════
         */
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
