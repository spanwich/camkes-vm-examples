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

/* v2.207: New industry-standard 5-level debug system */
#define DEBUG_LEVEL DEBUG_LEVEL_INFO  /* v2.210: INFO level (DEBUG has compilation errors) */
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
#include "lwip/priv/tcp_priv.h"  /* v2.205: Access tcp_active_pcbs for ooseq diagnostics */
#include "netif/ethernet.h"

/* ICS common definitions */
#include "common.h"

/* v2.117: Connection state sharing */
#include "connection_state.h"

/* v2.222: Comprehensive pbuf lifecycle tracking */
#include "pbuf_tracking.h"

#define COMPONENT_NAME "VirtIO_Net0_Driver"
#define TCP_SERVER_PORT 502  /* INBOUND: Modbus port - pretends to be PLC */

/* ═══════════════════════════════════════════════════════════════ */
/* OLD DEBUG SYSTEM (v2.206 and earlier) - DEPRECATED               */
/* v2.207: Replaced with industry-standard 5-level system           */
/* See debug_levels.h for new system (ERROR/WARN/INFO/DEBUG)        */
/* ═══════════════════════════════════════════════════════════════ */
/* OLD:
#define DEBUG_LEVEL_SILENT   1
#define DEBUG_LEVEL_QUIET    0
#define DEBUG_LEVEL_NORMAL   0
#define DEBUG_LEVEL_VERBOSE  0
...
(Old conditional blocks removed - see debug_levels.h for new mappings)
*/

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
    struct tcp_pcb *pcb;           /* lwIP connection pointer (key) */
    uint32_t session_id;           /* v2.150: Unique session ID (0 = unassigned) */
    uint32_t original_src_ip;      /* Original source IP (e.g., 192.168.90.5 SCADA) */
    uint32_t original_dest_ip;     /* Original destination IP (e.g., 192.168.95.2 PLC) */
    uint16_t src_port;             /* Source port */
    uint16_t dest_port;            /* Destination port */
    bool active;                   /* Is this slot in use? */
    /* v2.50: Connection validation - matches Net1 structure for consistency */
    uint32_t tcp_seq_num;          /* Initial TCP sequence number - detects connection reuse */
    uint32_t timestamp;            /* Creation time - for metadata consistency with Net1 */
    /* v2.92: Response lifecycle tracking */
    bool awaiting_response;        /* True if we're waiting for PLC response (don't cleanup yet!) */
    /* v2.189: Response arrival tracking (fix race condition)
     * ══════════════════════════════════════════════════════════════════════════
     * Problem: tcp_echo_recv(p=NULL) sets awaiting_response=true when SCADA closes,
     *          even if response already arrived and was processed.
     *
     * Solution: Track when response arrives separately from awaiting state
     *   1. outbound_ready_handle(): response_received = true (response arrived)
     *   2. tcp_echo_recv(p=NULL): check response_received before setting awaiting_response
     *   3. If response_received=true: proceed with normal close (don't wait)
     * ══════════════════════════════════════════════════════════════════════════
     */
    bool response_received;        /* True if PLC response arrived (even if not sent yet) */

    /* v2.158: Deferred connection close (lwIP best practice)
     * ══════════════════════════════════════════════════════════════════════════
     * Problem: Can't call tcp_close() from recv callback (Rule 5 violation)
     *
     * Solution: Deferred close via close_pending flag + poll callback
     *   1. After sending response: recv callback sets close_pending = true
     *   2. Poll callback (tcp_echo_poll) detects flag
     *   3. Poll callback sends close notification to Net1
     *   4. Poll callback returns ERR_ABRT → lwIP handles tcp_abort() internally
     *
     * This follows lwIP Rule 6 Solution A: close_pending flag pattern
     * ══════════════════════════════════════════════════════════════════════════
     */
    bool close_pending;            /* True if connection should close (poll handles) */

    /* v2.180: Deferred metadata cleanup (fix metadata/lwIP packet race)
     * ══════════════════════════════════════════════════════════════════════════
     * Problem: Poll callback removed metadata but lwIP still had PCB closing
     *   1. Poll callback: connection_cleanup_atomic() → metadata removed
     *   2. Poll callback: return ERR_ABRT
     *   3. lwIP: tcp_abort() internally → sends RST packet
     *   4. TX callback: looks up metadata → NOT FOUND!
     *   5. TX callback: sends packet with WRONG source IP (192.168.96.2)
     *   6. SCADA: ignores packet (expected 192.168.95.2)
     *   7. Result: 1,170 metadata failures, retransmissions, PCB leak
     *
     * Solution: Keep metadata alive until lwIP frees PCB
     *   1. Poll callback: tcp_close() + mark closing=true (DON'T cleanup!)
     *   2. lwIP: completes close handshake (FIN/ACK exchange)
     *   3. lwIP: frees PCB → error callback fires
     *   4. Error callback: connection_cleanup_atomic() (NOW safe!)
     *   5. Result: Metadata available for ALL lwIP packets during close
     *
     * Benefits:
     *   - Fixes 1,170 metadata lookup failures
     *   - All packets sent with correct source IP (192.168.95.2)
     *   - No SCADA retransmissions
     *   - No duplicate connection detection
     *   - No PCB leaks
     * ══════════════════════════════════════════════════════════════════════════
     */
    bool closing;                  /* True if close initiated, waiting for PCB free */
    /* NOTE: close_timestamp moved to v2.209 section below (reused for metadata_close_pending) */

    /* v2.111: Pending outbound data (fix for pbuf ref count bug)
     * ══════════════════════════════════════════════════════════════════════════
     * FIX: Don't call tcp_write() from main loop (outbound_ready_handle)
     *      Instead, queue data here and send from tcp_sent() callback.
     *
     * REASON: Calling tcp_write() from main loop causes race with lwIP timers:
     *   - Main loop: tcp_write() creates pbuf (ref=1)
     *   - Timer fires: lwIP increments ref for retransmission (ref=2)
     *   - tcp_output(): Asserts p->ref == 1 → FAILS → abort() → Net0 DEAD!
     *
     * SOLUTION: Only call tcp_write() from lwIP callback (proper threading model)
     * ══════════════════════════════════════════════════════════════════════════
     */
    uint8_t *pending_outbound_data;  /* Queued outbound data awaiting send */
    uint16_t pending_outbound_len;   /* Length of queued data */
    bool has_pending_outbound;       /* True if data needs to be sent */

    /* v2.143: Guard flag to prevent double-cleanup/double-decrement
     * ══════════════════════════════════════════════════════════════════════════
     * TICKING TIME BOMB FIX: Prevent double-decrement of active_connections
     *
     * Problem (v2.142):
     *   - tcp_close() called from main loop (ERROR notification path)
     *   - Main loop decrements active_connections
     *   - tcp_close() triggers recv(p=NULL) callback
     *   - Callback ALSO decrements active_connections
     *   - Result: Counter underflows to 0, unlimited connections accepted!
     *
     * Solution: Guard flag ensures cleanup logic runs exactly ONCE
     * ══════════════════════════════════════════════════════════════════════════
     */
    bool cleanup_in_progress;        /* Guard: prevents double-cleanup */

    /* v2.153: Deduplication flags for control message queue
     * ══════════════════════════════════════════════════════════════════════════
     * SECURITY: Prevent RST flood attacks by deduplicating control messages
     *
     * Problem without deduplication:
     *   - Attacker sends 1000 RST packets for same connection
     *   - Each RST triggers error notification enqueue
     *   - Queue fills up with duplicates
     *   - Legitimate notifications get dropped
     *
     * Solution: Only enqueue ONCE per connection
     *   - Set flag when first notification queued
     *   - Ignore subsequent notifications for same connection
     *   - Clear flag when connection cleaned up
     * ══════════════════════════════════════════════════════════════════════════
     */
    bool close_notified;             /* True if close notification already queued (Net0 → Net1) */

    /* v2.209: Delayed metadata cleanup (fix pbuf leak race condition)
     * ══════════════════════════════════════════════════════════════════════════
     * Problem (v2.208): Metadata deleted immediately when SCADA closes
     *   T+92.979 ms: PLC response arrives at Net0 (packet on wire)
     *   T+141.610 ms: SCADA closes (FIN) → tcp_echo_recv(p=NULL)
     *                 → connection_cleanup_atomic() → metadata.active = FALSE
     *   T+145.000 ms: TX path executes (processing delay)
     *                 → find_connection_by_port() → metadata NOT FOUND!
     *                 → Sends with WRONG source IP (192.168.96.2 vs 192.168.95.2)
     *                 → SCADA rejects → no ACK → pbuf LEAKED!
     *
     *   Result: 939 TX failures (78%), 1192 pbuf leaks (99%)
     *
     * Solution (v2.209): Delay metadata cleanup until TX completes
     *   T+141.610 ms: SCADA closes → metadata_close_pending = TRUE
     *                 (DON'T delete metadata yet!)
     *   T+145.000 ms: TX path executes
     *                 → find_connection_by_port() → FOUND! ✅
     *                 → Sends with CORRECT source IP (192.168.95.2)
     *                 → last_tx_timestamp updated
     *   T+1145.000 ms: check_pending_cleanups() runs (main loop)
     *                  → tx_idle = now - last_tx_timestamp = 1000 ms
     *                  → Fast-track cleanup (tx_idle > 1000 ms)
     *                  → connection_cleanup_atomic() (NOW safe!)
     *
     * Two-tier cleanup:
     *   1. Fast-track: Cleanup after 1 second of TX idle (normal case)
     *   2. Grace period: Cleanup after 5 seconds max (safety net)
     *
     * Benefits:
     *   - 0% TX failures (metadata persists until TX completes)
     *   - 0% pbuf leaks (correct IP → SCADA accepts → ACK → pbuf freed)
     *   - No connection pool exhaustion (1s cleanup keeps usage at 19.5%)
     *   - 103× safety margin (5s grace / 48ms race window)
     * ══════════════════════════════════════════════════════════════════════════
     */
    bool metadata_close_pending;     /* True if SCADA closed but metadata persists for TX */
    uint32_t close_timestamp;        /* When metadata_close_pending was set (for grace period) */
    uint32_t last_tx_timestamp;      /* Last TX path activity (for fast-track cleanup) */
};

static struct connection_metadata connection_table[MAX_CONNECTIONS];
static int connection_count = 0;

/* v2.143: Connection tracking counters (moved here for connection_cleanup_atomic()) */
static uint32_t active_connections = 0;
static uint32_t total_connections_created = 0;
static uint32_t total_connections_closed = 0;

/* v2.150: Session ID counter for unique connection identification
 * ══════════════════════════════════════════════════════════════════════════════
 * Purpose: Provide unique, persistent identifier for each SCADA connection
 * - Counter starts at 1 (0 means unassigned)
 * - Monotonically increasing (never reused)
 * - Wraps at UINT32_MAX (4 billion connections)
 * - Assigned in Net0 (SCADA side), passed to Net1 via shared dataport
 * - Enables reliable SCADA ↔ PLC connection mapping across components
 * ══════════════════════════════════════════════════════════════════════════════
 */
static uint32_t next_session_id = 1;

/* v2.117: Connection state sharing via dataports */
static volatile struct connection_state_table *own_state = NULL;   /* Our state (exposed to Net1) */
static volatile struct connection_state_table *peer_state = NULL;  /* Net1's state (read-only) */

/* v2.193: Queue-based cleanup architecture
 * ══════════════════════════════════════════════════════════════════════════════
 * Problem with direct cleanup calls (v2.192):
 * - Multiple callbacks (recv, poll, err) all call connection_cleanup_atomic()
 * - Guard flag (cleanup_in_progress) blocks duplicate calls
 * - But guard NEVER cleared → blocks ALL subsequent cleanups → CONNECTION LEAK!
 *
 * Queue-based solution:
 * - Callbacks enqueue cleanup requests (never blocks)
 * - Main loop processes queue (guaranteed to run)
 * - Natural deduplication: check meta->active flag
 * - No guard blocking issues
 * - Single cleanup enforcement point
 *
 * Benefits:
 * ✅ No more guard blocking
 * ✅ Reliable cleanup (main loop always runs)
 * ✅ Simple deduplication (active flag)
 * ✅ Easy debugging (single processing point)
 * ✅ Clean architecture (producer/consumer pattern)
 * ══════════════════════════════════════════════════════════════════════════════
 */
#define CLEANUP_QUEUE_SIZE 512  /* Must be power of 2 */
#define CLEANUP_QUEUE_MASK (CLEANUP_QUEUE_SIZE - 1)

struct cleanup_request {
    uint32_t session_id;
    uint32_t timestamp;  /* For age tracking and debugging */
};

struct cleanup_queue {
    volatile uint32_t head;  /* Producer writes here (callbacks) */
    volatile uint32_t tail;  /* Consumer reads here (main loop) */
    struct cleanup_request requests[CLEANUP_QUEUE_SIZE];
};

static struct cleanup_queue cleanup_queue = {
    .head = 0,
    .tail = 0
};

/* Queue statistics for debugging */
static struct {
    uint32_t enqueued;        /* Total cleanup requests enqueued */
    uint32_t processed;       /* Total requests processed */
    uint32_t duplicates;      /* Requests for already-inactive connections */
    uint32_t max_depth;       /* Maximum queue depth seen */
    uint32_t overflows;       /* Queue full events */
} cleanup_stats = {0};

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

/* v2.203: Pbuf leak diagnostics */
static uint32_t pbuf_allocated_count = 0;
static uint32_t pbuf_freed_count = 0;
static uint32_t pbuf_leaked_to_lwip = 0;  /* Pbufs passed to lwIP successfully */
static uint32_t pbuf_arp_count = 0;
static uint32_t pbuf_tcp_count = 0;
static uint32_t pbuf_udp_count = 0;
static uint32_t pbuf_other_count = 0;
static uint32_t pbuf_error_count = 0;  /* Pbufs we had to free due to errors */

/* v2.205: Out-of-order segment tracking */
typedef struct {
    uint32_t pcbs_with_ooseq;      /* Number of PCBs that have ooseq queue */
    uint32_t total_ooseq_segments;  /* Total segments across all PCBs */
    uint32_t total_ooseq_pbufs;     /* Approximate pbuf count in all ooseq */
    uint32_t total_active_pcbs;     /* Total PCBs in tcp_active_pcbs list */
} ooseq_stats_t;

/* v2.205: Count out-of-order segments and pbufs across all PCBs
 * This diagnostic helps identify if pbufs are stuck in ooseq queues
 */
static void get_ooseq_stats(ooseq_stats_t *stats)
{
    struct tcp_pcb *pcb;

    stats->pcbs_with_ooseq = 0;
    stats->total_ooseq_segments = 0;
    stats->total_ooseq_pbufs = 0;
    stats->total_active_pcbs = 0;

    /* Iterate through all active TCP PCBs */
    for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
        stats->total_active_pcbs++;

        if (pcb->ooseq != NULL) {
            stats->pcbs_with_ooseq++;

            /* Count segments and pbufs in this PCB's ooseq */
            struct tcp_seg *seg = pcb->ooseq;
            while (seg != NULL) {
                stats->total_ooseq_segments++;

                /* Count pbufs in this segment's pbuf chain */
                if (seg->p != NULL) {
                    struct pbuf *p = seg->p;
                    while (p != NULL) {
                        stats->total_ooseq_pbufs++;
                        p = p->next;
                    }
                }

                seg = seg->next;
            }
        }
    }
}

/* v2.205: PCB state breakdown tracking */
typedef struct {
    uint32_t pcb_listen;
    uint32_t pcb_syn_sent;
    uint32_t pcb_syn_rcvd;
    uint32_t pcb_established;
    uint32_t pcb_fin_wait_1;
    uint32_t pcb_fin_wait_2;
    uint32_t pcb_close_wait;
    uint32_t pcb_closing;
    uint32_t pcb_last_ack;
    uint32_t pcb_time_wait;
} pcb_state_stats_t;

/* v2.205: Count PCBs by TCP state
 * Helps identify if specific states are accumulating pbufs
 */
static void get_pcb_state_stats(pcb_state_stats_t *stats)
{
    struct tcp_pcb *pcb;

    stats->pcb_listen = 0;
    stats->pcb_syn_sent = 0;
    stats->pcb_syn_rcvd = 0;
    stats->pcb_established = 0;
    stats->pcb_fin_wait_1 = 0;
    stats->pcb_fin_wait_2 = 0;
    stats->pcb_close_wait = 0;
    stats->pcb_closing = 0;
    stats->pcb_last_ack = 0;
    stats->pcb_time_wait = 0;

    /* Count active PCBs by state */
    for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
        switch (pcb->state) {
            case LISTEN:       stats->pcb_listen++; break;
            case SYN_SENT:     stats->pcb_syn_sent++; break;
            case SYN_RCVD:     stats->pcb_syn_rcvd++; break;
            case ESTABLISHED:  stats->pcb_established++; break;
            case FIN_WAIT_1:   stats->pcb_fin_wait_1++; break;
            case FIN_WAIT_2:   stats->pcb_fin_wait_2++; break;
            case CLOSE_WAIT:   stats->pcb_close_wait++; break;
            case CLOSING:      stats->pcb_closing++; break;
            case LAST_ACK:     stats->pcb_last_ack++; break;
            case TIME_WAIT:    stats->pcb_time_wait++; break;
            default: break;
        }
    }
}

/* v2.205: Connection metadata vs lwIP PCB matching */
typedef struct {
    uint32_t metadata_active_with_pcb;     /* Active metadata that has a PCB */
    uint32_t metadata_active_without_pcb;  /* Active metadata with no PCB (orphaned) */
    uint32_t metadata_inactive;            /* Inactive metadata slots */
    uint32_t pcb_without_metadata;         /* PCBs that we don't track (orphaned PCBs) */
} connection_match_stats_t;

/* v2.206: Orphan PCB buffer diagnostics */
typedef struct {
    void *pcb_addr;                   /* PCB address */
    uint32_t state;                   /* TCP state */
    uint32_t rcv_wnd;                 /* Receive window */
    uint32_t snd_buf;                 /* Send buffer space */
    uint32_t refused_data_pbufs;      /* Pbufs in refused_data queue */
    uint32_t unacked_segments;        /* Segments in unacked queue */
    uint32_t unsent_segments;         /* Segments in unsent queue */
    uint32_t ooseq_segments;          /* Segments in ooseq queue */
    uint32_t unacked_pbufs;           /* Pbufs in unacked queue */
    uint32_t unsent_pbufs;            /* Pbufs in unsent queue */
} orphan_pcb_diag_t;

/* v2.205: Check for metadata/PCB mismatches
 * Identifies orphaned connections (metadata without PCB or PCB without metadata)
 */
static void get_connection_match_stats(connection_match_stats_t *stats)
{
    struct tcp_pcb *pcb;

    stats->metadata_active_with_pcb = 0;
    stats->metadata_active_without_pcb = 0;
    stats->metadata_inactive = 0;
    stats->pcb_without_metadata = 0;

    /* Count our connection metadata */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active) {
            if (connection_table[i].pcb != NULL) {
                stats->metadata_active_with_pcb++;
            } else {
                stats->metadata_active_without_pcb++;
            }
        } else {
            stats->metadata_inactive++;
        }
    }

    /* Count PCBs that don't have our metadata */
    for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
        bool found = false;

        /* Check if this PCB is tracked in our metadata */
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connection_table[i].active && connection_table[i].pcb == pcb) {
                found = true;
                break;
            }
        }

        if (!found) {
            stats->pcb_without_metadata++;
        }
    }
}

/* v2.206: Diagnose orphan PCB internal buffers
 * Shows WHERE pbufs are stuck in PCBs we no longer track
 */
static int diagnose_orphan_pcbs(orphan_pcb_diag_t *diags, int max_diags)
{
    struct tcp_pcb *pcb;
    int orphan_count = 0;

    for (pcb = tcp_active_pcbs; pcb != NULL && orphan_count < max_diags; pcb = pcb->next) {
        bool tracked = false;

        /* Check if this PCB is tracked in our metadata */
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connection_table[i].active && connection_table[i].pcb == pcb) {
                tracked = true;
                break;
            }
        }

        if (!tracked) {
            /* Found an orphan - diagnose its buffer state */
            orphan_pcb_diag_t *diag = &diags[orphan_count++];

            diag->pcb_addr = pcb;
            diag->state = pcb->state;
            diag->rcv_wnd = pcb->rcv_wnd;
            diag->snd_buf = pcb->snd_buf;

            /* Count pbufs in refused_data (application didn't consume) */
            diag->refused_data_pbufs = 0;
            if (pcb->refused_data != NULL) {
                struct pbuf *p = pcb->refused_data;
                while (p != NULL) {
                    diag->refused_data_pbufs++;
                    p = p->next;
                }
            }

            /* Count segments and pbufs in unacked queue (sent but not ACKed) */
            diag->unacked_segments = 0;
            diag->unacked_pbufs = 0;
            if (pcb->unacked != NULL) {
                struct tcp_seg *seg = pcb->unacked;
                while (seg != NULL) {
                    diag->unacked_segments++;
                    if (seg->p != NULL) {
                        struct pbuf *p = seg->p;
                        while (p != NULL) {
                            diag->unacked_pbufs++;
                            p = p->next;
                        }
                    }
                    seg = seg->next;
                }
            }

            /* Count segments and pbufs in unsent queue (not yet sent) */
            diag->unsent_segments = 0;
            diag->unsent_pbufs = 0;
            if (pcb->unsent != NULL) {
                struct tcp_seg *seg = pcb->unsent;
                while (seg != NULL) {
                    diag->unsent_segments++;
                    if (seg->p != NULL) {
                        struct pbuf *p = seg->p;
                        while (p != NULL) {
                            diag->unsent_pbufs++;
                            p = p->next;
                        }
                    }
                    seg = seg->next;
                }
            }

            /* Count ooseq segments (should be 0 based on v2.205 results) */
            diag->ooseq_segments = 0;
            if (pcb->ooseq != NULL) {
                struct tcp_seg *seg = pcb->ooseq;
                while (seg != NULL) {
                    diag->ooseq_segments++;
                    seg = seg->next;
                }
            }
        }
    }

    return orphan_count;
}

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
    DEBUG("%s: ", prefix);

    size_t display_len = (len < max_display) ? len : max_display;

    for (size_t i = 0; i < display_len; i++) {
        DEBUG("%02x ", data[i]);
        if ((i + 1) % 16 == 0 && (i + 1) < display_len) {
            DEBUG("\n%*s", (int)strlen(prefix) + 2, "");
        }
    }

    if (len > max_display) {
        DEBUG("... (%zu more bytes)", len - max_display);
    }
    DEBUG("\n");
}

static void print_ascii_payload(const uint8_t *data, size_t len)
{
    DEBUG("  ASCII: \"");
    for (size_t i = 0; i < len && i < 80; i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            DEBUG("%c", data[i]);
        } else {
            DEBUG(".");
        }
    }
    if (len > 80) DEBUG("...");
    DEBUG("\"\n");
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
        /* This is a warning - always show it */
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

    #if DEBUG_ENABLED_DEBUG
    /* CRITICAL DEBUG: Confirm this function is being called */
    if (tx_count <= 20) {
        DEBUG("%s: ⚡ netif_output() CALLED - tx_count=%u, pbuf len=%u\n",
               COMPONENT_NAME, tx_count, p->tot_len);
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
     * CRITICAL: Restore original IPs for protocol-break architecture
     *
     * lwIP generated response with interface IP as source (192.168.96.2)
     * But SCADA expects response from PLC IP (192.168.95.2)
     * Restore: 192.168.96.2 → 192.168.95.2 (source IP)
     * Keep: 192.168.90.5 (destination IP to SCADA)
     */
    uint8_t *tx_data = packet_buffers[tx_buf_idx];
    if (p->tot_len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
        struct ethhdr *eth = (struct ethhdr *)tx_data;
        if (ntohs(eth->h_proto) == 0x0800) {  /* IPv4 */
            struct iphdr *ip = (struct iphdr *)(tx_data + sizeof(struct ethhdr));

            if (ip->protocol == 6) {  /* TCP */
                /* Extract current IPs and ports */
                uint32_t current_src = ntohl(ip->saddr);  /* 192.168.96.2 from lwIP */
                uint32_t current_dest = ntohl(ip->daddr); /* 192.168.90.5 to SCADA */

                size_t ip_hdr_len = (ip->ihl) * 4;
                if (p->tot_len >= sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct tcphdr)) {
                    struct tcphdr *tcp = (struct tcphdr *)(tx_data + sizeof(struct ethhdr) + ip_hdr_len);
                    uint16_t src_port = ntohs(tcp->source);  /* 502 */
                    uint16_t dest_port = ntohs(tcp->dest);   /* SCADA's port */

                    /* Lookup original metadata by destination (SCADA) port */
                    struct connection_metadata *meta = NULL;
                    for (int i = 0; i < MAX_CONNECTIONS; i++) {
                        /* Defensive check: ensure index is valid */
                        if (i >= MAX_CONNECTIONS) {
                            DEBUG_WARN("%s: [WARN]  TX: Invalid connection table index %d\n", COMPONENT_NAME, i);
                            break;
                        }

                        if (connection_table[i].active &&
                            connection_table[i].dest_port == src_port &&  /* Our port 502 */
                            connection_table[i].src_port == dest_port) {  /* SCADA's port */
                            meta = &connection_table[i];
                            break;
                        }
                    }

                    if (meta != NULL && meta->active) {
                        /* Double-check metadata is valid before using */
                        if (meta->original_dest_ip == 0) {
                            DEBUG_WARN("%s: [WARN]  TX: Invalid metadata - original_dest_ip is 0\n", COMPONENT_NAME);
                        } else {
                            /* Restore original destination IP (PLC IP) as source */
                            ip->saddr = htonl(meta->original_dest_ip);  /* 192.168.95.2 */

                            #if DEBUG_ENABLED_DEBUG
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

                            #if DEBUG_ENABLED_DEBUG
                            DEBUG("%s: [FIX] TX: IP checksum: 0x%04x → 0x%04x\n",
                                   COMPONENT_NAME, ntohs(old_ip_check), ntohs(new_ip_check));
                            #endif

                            /* Recalculate TCP checksum with pseudo-header */
                            uint16_t old_tcp_check = tcp->check;
                            tcp->check = 0;
                            uint16_t tcp_len = ntohs(ip->tot_len) - (ip->ihl * 4);
                            uint16_t new_tcp_check = tcp_checksum(ip, tcp, tcp_len);
                            tcp->check = new_tcp_check;

                            #if DEBUG_ENABLED_DEBUG
                            DEBUG("%s: [FIX] TCP checksum: 0x%04x → 0x%04x\n",
                                   COMPONENT_NAME, ntohs(old_tcp_check), ntohs(new_tcp_check));
                            #endif
                        }
                    } else {
                        /* v2.104: Removed verbose connection table dump - uses too much stack */
                        DEBUG_WARN("%s: [WARN]  TX: No metadata for port %u->%u (conns:%d)\n",
                               COMPONENT_NAME, src_port, dest_port, connection_count);
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
 * - We rewrite: 192.168.90.5 → 192.168.96.2 (for lwIP acceptance)
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

    /* Memory barrier to ensure updates are visible to Net1 */
    __sync_synchronize();
}

/* Store metadata for a new connection */
static struct connection_metadata* connection_add(uint32_t orig_src, uint32_t orig_dest,
                                                   uint16_t sport, uint16_t dport)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_table[i].active) {
            connection_table[i].active = true;
            connection_table[i].pcb = NULL;  /* Will be set when TCP accept happens */

            /* v2.150: Assign unique session ID for this SCADA connection */
            connection_table[i].session_id = next_session_id++;

            connection_table[i].original_src_ip = orig_src;
            connection_table[i].original_dest_ip = orig_dest;
            connection_table[i].src_port = sport;
            connection_table[i].dest_port = dport;

            /* v2.111: Initialize pending outbound fields */
            connection_table[i].pending_outbound_data = NULL;
            connection_table[i].pending_outbound_len = 0;
            connection_table[i].has_pending_outbound = false;

            /* v2.158: Initialize connection lifecycle flags */
            connection_table[i].awaiting_response = false;
            connection_table[i].response_received = false;  /* v2.189: Response arrival tracking */
            connection_table[i].close_pending = false;
            connection_table[i].closing = false;  /* v2.180: Deferred metadata cleanup */

            /* v2.149: Reset guard flag for new connection
             * Note: This allows stale callbacks to pass guard check if slot is reused quickly
             * But the meta->active check above will catch stale callbacks before reuse
             */
            connection_table[i].cleanup_in_progress = false;

            /* v2.200: CRITICAL FIX - Initialize close_notified for new/reused slots
             * ═══════════════════════════════════════════════════════════════════════════
             * BUG: Slot lifecycle - close_notified persists when slot is reused
             *
             * Problem: When slot is reused by new session, close_notified flag inherits
             * value from previous session, causing new session to skip notifications.
             *
             * Evidence from v2.199 test:
             *   - Net0 created 150 connections, sent only 25 close notifications
             *   - Net1 has 125 orphan connections (never notified)
             *   - 125 sessions inherited close_notified=true from previous sessions
             *   - Result: Net1 pool exhaustion, communication failure
             *
             * Fix: Initialize to false for every new connection (new or reused slot)
             * ═══════════════════════════════════════════════════════════════════════════
             */
            connection_table[i].close_notified = false;

            /* v2.209: Initialize delayed metadata cleanup fields (fix pbuf leak)
             * ═══════════════════════════════════════════════════════════════════════════
             * Purpose: Support two-tier cleanup strategy to prevent "TX: No metadata"
             *
             * Fields:
             *   - metadata_close_pending: False initially, set when SCADA closes
             *   - close_timestamp: 0 initially, set when metadata_close_pending=true
             *   - last_tx_timestamp: Current time (connection creation time)
             *
             * Why initialize last_tx_timestamp to sys_now():
             *   - Prevents premature fast-track cleanup for new connections
             *   - Gives new connection at least 1 second to complete TX
             *   - After first TX, this gets updated to actual TX completion time
             * ═══════════════════════════════════════════════════════════════════════════
             */
            connection_table[i].metadata_close_pending = false;
            connection_table[i].close_timestamp = 0;
            connection_table[i].last_tx_timestamp = sys_now();

            connection_count++;

            /* v2.193: Log connection creation for debugging */
            DEBUG("%s: [COUNT++] %u → %u | connection_add() session=%u port=%u→%u\n",
                   COMPONENT_NAME, connection_count - 1, connection_count,
                   connection_table[i].session_id, sport, dport);

            #if DEBUG_METADATA
            DEBUG("%s: 📝 Stored metadata [%d]: %u.%u.%u.%u:%u → %u.%u.%u.%u:%u\n",
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
    DEBUG_WARN("%s: [WARN]  Connection table full! Dropping metadata.\n", COMPONENT_NAME);
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
            /* v2.50: Store validation metadata for consistency with Net1 */
            connection_table[i].tcp_seq_num = pcb->snd_nxt;
            connection_table[i].timestamp = sys_now();
            #if DEBUG_METADATA
            DEBUG("%s: [LINK] Linked PCB to metadata [%d] (seq=%u, ts=%u)\n",
                   COMPONENT_NAME, i, pcb->snd_nxt, connection_table[i].timestamp);
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

/* v2.153: Lookup metadata by session_id */
static struct connection_metadata* connection_lookup_by_session_id(uint32_t session_id)
{
    if (session_id == 0) return NULL;  /* 0 = unassigned/cleaned */

    /* v2.197: REMOVED active flag check for cleanup queue compatibility
     * ═══════════════════════════════════════════════════════════════════════════
     * Old behavior: Only return if active==true
     * Problem: Pool exhaustion handler sets active=false BEFORE enqueueing cleanup
     * Result: Cleanup queue can't find connection → counter never decremented!
     *
     * New behavior: Match by session_id only, ignore active flag
     * - If found: Return metadata (cleanup queue will handle it)
     * - If not found: Return NULL (already cleaned or invalid)
     * - Idempotency: After cleanup, session_id set to 0 → future lookups fail
     * ═══════════════════════════════════════════════════════════════════════════
     */
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
    /* v2.193: DON'T check active flag to prevent duplicate creation during cleanup!
     * ═══════════════════════════════════════════════════════════════════════════
     * Race condition (v2.193 initial):
     * 1. SYN arrives → connection_add(port=X) → active=true, session=678
     * 2. Close notification → enqueue_cleanup(678)
     * 3. process_cleanup_queue() → active=false (marks inactive immediately!)
     * 4. SYN retransmit arrives (SCADA retrying)
     * 5. connection_lookup_by_tuple() checks active → returns NULL!
     * 6. connection_add() creates NEW metadata (session=679, 680...)
     * 7. Only session 678 gets cleanup → 679, 680 LEAKED!
     *
     * Fix: Match by port tuple regardless of active status
     * - Prevents duplicate metadata during cleanup window
     * - Reuses metadata slot even if being cleaned
     * - Connection count stays accurate
     * ═══════════════════════════════════════════════════════════════════════════
     */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        /* v2.193: Check port tuple ONLY - don't check active! */
        if (connection_table[i].original_src_ip == src_ip &&
            connection_table[i].src_port == sport &&
            connection_table[i].dest_port == dport) {

            /* v2.193: Debug - show if we're reusing a being-cleaned connection */
            if (!connection_table[i].active) {
                DEBUG("%s: [FIND] Reusing slot %d for port %u→%u (was being cleaned, active=false)\n",
                       COMPONENT_NAME, i, sport, dport);
            }

            return &connection_table[i];
        }
    }
    return NULL;
}

/* Remove connection metadata */
static void connection_remove(struct tcp_pcb *pcb)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active && connection_table[i].pcb == pcb) {
            #if DEBUG_METADATA
            DEBUG("%s: [DEL]  Removing metadata [%d]\n", COMPONENT_NAME, i);
            #endif

            /* v2.111: Clean up pending outbound data if exists */
            if (connection_table[i].pending_outbound_data != NULL) {
                DEBUG_WARN("%s: [WARN]  Freeing unsent pending data (%u bytes)\n",
                       COMPONENT_NAME, connection_table[i].pending_outbound_len);
                free(connection_table[i].pending_outbound_data);
                connection_table[i].pending_outbound_data = NULL;
            }

            connection_table[i].active = false;
            connection_table[i].pcb = NULL;
            connection_table[i].has_pending_outbound = false;
            connection_table[i].pending_outbound_len = 0;
            connection_count--;

            /* v2.117: Update shared connection state */
            update_shared_connection_state();

            return;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * v2.193: Queue-based cleanup functions
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * enqueue_cleanup() - Producer: Enqueue cleanup request (called from callbacks)
 *
 * This function is called from lwIP callbacks (recv, poll, err) to request
 * cleanup of a connection. Requests are placed in a lock-free queue and
 * processed by the main loop.
 *
 * Benefits:
 * - Natural deduplication (duplicate requests just update timestamp)
 * - No blocking (if queue is full, we drop request - cleanup will retry)
 * - Single producer, single consumer (SPSC) pattern
 *
 * @param session_id Session ID of connection to cleanup
 */
static inline void enqueue_cleanup(uint32_t session_id)
{
    if (session_id == 0) {
        DEBUG("%s: WARN: enqueue_cleanup called with session_id=0 (ignoring)\n",
               COMPONENT_NAME);
        return;
    }

    uint32_t head = cleanup_queue.head;
    uint32_t tail = cleanup_queue.tail;
    uint32_t next_head = head + 1;

    /* Check if queue is full */
    if ((next_head & CLEANUP_QUEUE_MASK) == (tail & CLEANUP_QUEUE_MASK)) {
        cleanup_stats.overflows++;
        DEBUG("%s: WARN: Cleanup queue full (dropped session %u, overflow #%u)\n",
               COMPONENT_NAME, session_id, cleanup_stats.overflows);
        return;
    }

    /* Enqueue request */
    uint32_t slot = head & CLEANUP_QUEUE_MASK;
    cleanup_queue.requests[slot].session_id = session_id;
    cleanup_queue.requests[slot].timestamp = sys_now();

    /* Memory barrier: ensure writes complete before updating head */
    __sync_synchronize();

    cleanup_queue.head = next_head;
    cleanup_stats.enqueued++;

    /* Track max queue depth for debugging */
    uint32_t depth = next_head - tail;
    if (depth > cleanup_stats.max_depth) {
        cleanup_stats.max_depth = depth;
    }

    /* v2.193: Debug logging */
    DEBUG("%s: [QUEUE] Enqueued cleanup session=%u (queue depth=%u)\n",
           COMPONENT_NAME, session_id, depth);
}

/**
 * process_cleanup_queue() - Consumer: Process cleanup requests (called from main loop)
 *
 * This function runs in the main loop (guaranteed execution) and processes
 * all pending cleanup requests from the queue.
 *
 * v2.197: Natural deduplication using session_id:
 * - First request for session_id X: Lookup succeeds → cleanup, set session_id=0
 * - Duplicate requests: Lookup fails (session_id=0) → skip (already done)
 *
 * No guard flag or active flag needed:
 * - session_id=0 after cleanup provides idempotency
 * - Works even if active=false was set before enqueue (pool exhaustion case)
 * - No blocking or race conditions
 * - Simple and robust
 */
static void process_cleanup_queue(void)
{
    uint32_t tail = cleanup_queue.tail;
    uint32_t head = cleanup_queue.head;

    /* Process all pending requests */
    while (tail != head) {
        uint32_t slot = tail & CLEANUP_QUEUE_MASK;
        struct cleanup_request *req = &cleanup_queue.requests[slot];

        /* v2.197: Skip if session_id is 0 (invalid/already cleaned) */
        if (req->session_id == 0) {
            DEBUG("%s: [QUEUE] SKIP: session_id=0 (invalid or already cleaned)\n",
                   COMPONENT_NAME);
            cleanup_stats.duplicates++;
            tail++;
            cleanup_queue.tail = tail;
            continue;
        }

        /* v2.193: Debug - show what we're trying to cleanup */
        DEBUG("%s: [QUEUE] Processing cleanup session=%u (queued %ums ago)\n",
               COMPONENT_NAME, req->session_id, sys_now() - req->timestamp);

        /* Lookup connection by session_id (v2.197: ignores active flag) */
        struct connection_metadata *meta = connection_lookup_by_session_id(req->session_id);

        if (meta == NULL) {
            /* Connection not found - already cleaned up (session_id was set to 0) */
            DEBUG("%s: [QUEUE] SKIP: session=%u not found (already cleaned)\n",
                   COMPONENT_NAME, req->session_id);
            cleanup_stats.duplicates++;
        } else {
            /* Perform cleanup */

            /* v2.195: ORPHAN DETECTION - Check if Net1 also has this connection
             * ═══════════════════════════════════════════════════════════════════════════
             * Purpose: Detect asymmetric cleanup (Net0 has connection, Net1 doesn't)
             * This helps identify:
             * 1. Orphan connections (Net1 never created connection)
             * 2. Timing issues (Net1 cleaned up first)
             * 3. Forwarding failures (request never reached Net1)
             * ═══════════════════════════════════════════════════════════════════════════
             */
            bool found_on_net1 = false;

            if (peer_state != NULL) {
                for (uint32_t i = 0; i < peer_state->count && i < MAX_SHARED_CONNECTIONS; i++) {
                    if (peer_state->connections[i].session_id == req->session_id &&
                        peer_state->connections[i].active) {
                        found_on_net1 = true;
                        break;
                    }
                }
            }

            /* Diagnostic output based on orphan detection */
            if (!found_on_net1) {
                DEBUG("%s: *** ORPHAN DETECTED *** session=%u exists on Net0 but NOT on Net1\n",
                       COMPONENT_NAME, req->session_id);
                DEBUG("%s:    Net0: port=%u→%u, active=%d, pcb=%p\n",
                       COMPONENT_NAME, meta->src_port, meta->dest_port,
                       meta->active, (void*)meta->pcb);
                DEBUG("%s:    Net1: count=%u (checked peer_state)\n",
                       COMPONENT_NAME, peer_state ? peer_state->count : 0);
                DEBUG("%s:    Possible reasons:\n", COMPONENT_NAME);
                DEBUG("%s:      1. Net1 never created connection (request never forwarded?)\n", COMPONENT_NAME);
                DEBUG("%s:      2. Net1 already cleaned up (Net1 closed first?)\n", COMPONENT_NAME);
                DEBUG("%s:      3. Timing race (Net1 cleanup in progress?)\n", COMPONENT_NAME);
                DEBUG("%s:    Meta state: awaiting=%d, response_received=%d, close_pending=%d, close_notified=%d\n",
                       COMPONENT_NAME, meta->awaiting_response, meta->response_received,
                       meta->close_pending, meta->close_notified);
            } else {
                DEBUG("%s: [QUEUE] Normal cleanup: session=%u exists on BOTH Net0 and Net1\n",
                       COMPONENT_NAME, req->session_id);
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
                DEBUG("%s: ERROR: connection_count already 0 (prevented underflow for session %u)!\n",
                       COMPONENT_NAME, req->session_id);
            }

            if (active_connections > 0) {
                active_connections--;
            } else {
                DEBUG("%s: ERROR: active_connections already 0 (prevented underflow for session %u)!\n",
                       COMPONENT_NAME, req->session_id);
            }

            /* v2.209 Complete: CRITICAL FIX - Defer cleanup if metadata_close_pending is set
             * ═══════════════════════════════════════════════════════════════════════════
             * BUG FIX: Prevent immediate cleanup when transmission may still be pending
             *
             * Root cause of "TX: No metadata" errors:
             * - When SCADA FIN arrives with response_received=true, enqueue_cleanup() was called
             * - But response_received just means tcp_write() was called, NOT transmission complete
             * - process_cleanup_queue() would immediately set active=false
             * - Then netif_output() can't find metadata → "TX: No metadata" error
             *
             * Fix: Check metadata_close_pending flag before cleanup
             * - If set: TX may still be pending, defer cleanup
             * - Let check_pending_cleanups() handle it after TX idle >1s (fast-track)
             * - Or after 5s grace period (safety net)
             * - Or at 80% pool usage (emergency cleanup)
             *
             * This prevents the bypass where enqueue_cleanup() would cause immediate cleanup
             * before transmission completed, eliminating the "TX: No metadata" errors.
             * ═══════════════════════════════════════════════════════════════════════════
             */
            if (meta->metadata_close_pending) {
                DEBUG("%s: [v2.209] DEFERRED cleanup: session=%u has metadata_close_pending=true\n",
                       COMPONENT_NAME, req->session_id);
                DEBUG("%s:   → TX may still be pending, letting check_pending_cleanups() handle it\n",
                       COMPONENT_NAME);
                DEBUG("%s:   → Will cleanup after TX idle >1s (fast-track) or 5s grace period\n",
                       COMPONENT_NAME);

                /* Advance tail to prevent infinite loop, then skip cleanup */
                __sync_synchronize();  /* Memory barrier */
                tail++;
                cleanup_queue.tail = tail;

                /* check_pending_cleanups() will handle this later */
                continue;
            }

            /* Mark metadata as inactive */
            meta->active = false;
            meta->pcb = NULL;
            meta->has_pending_outbound = false;
            meta->pending_outbound_len = 0;
            meta->awaiting_response = false;
            meta->response_received = false;
            meta->close_pending = false;

            /* v2.200: CRITICAL FIX - Reset slot lifecycle flags for reuse
             * ═══════════════════════════════════════════════════════════════════════════
             * BUG: When slot is freed, session-specific flags must be reset
             *
             * Missing resets cause slot reuse bugs:
             * - close_notified: Next session inherits "already notified" state
             * - cleanup_in_progress: Next session might skip cleanup (guard triggered)
             *
             * Rule: When session_id becomes 0 (slot freed), ALL session-specific state
             * must be reset so next session starts with clean slate.
             * ═══════════════════════════════════════════════════════════════════════════
             */
            meta->close_notified = false;
            meta->cleanup_in_progress = false;

            /* v2.197: Set session_id=0 for idempotency
             * ═══════════════════════════════════════════════════════════════════════════
             * CRITICAL: Prevents double-cleanup if enqueued multiple times
             * - Next cleanup attempt: lookup fails (session_id=0) → skip
             * - Works even without active flag check
             * - Simpler and more robust than guard flags
             * ═══════════════════════════════════════════════════════════════════════════
             */
            meta->session_id = 0;

            /* Update shared connection state */
            update_shared_connection_state();

            cleanup_stats.processed++;
        }

        /* Advance tail */
        __sync_synchronize();  /* Memory barrier */
        tail++;
        cleanup_queue.tail = tail;
    }
}

/* v2.143: Atomic cleanup function - SINGLE source of truth for counter decrements
 * ═══════════════════════════════════════════════════════════════════════════
 * CRITICAL: This is the ONLY place where active_connections is decremented!
 *
 * Problem Solved:
 *   v2.142 had multiple cleanup paths that each decremented counters:
 *   - tcp_echo_recv(p=NULL) callback
 *   - tcp_echo_err() callback
 *   - ERROR notification handler in main loop
 *
 *   This caused double-decrement when:
 *   1. Main loop called tcp_close() and decremented counter
 *   2. tcp_close() triggered recv(p=NULL) callback
 *   3. Callback tried to decrement AGAIN
 *   Result: Counter underflowed to 0, unlimited connections accepted!
 *
 * Solution:
 *   - Guard flag (cleanup_in_progress) prevents double-cleanup
 *   - All cleanup code paths call this function
 *   - Counters only decremented once (here!)
 *
 * Usage:
 *   connection_cleanup_atomic(&connection_table[i]);
 * ═══════════════════════════════════════════════════════════════════════════
 */
static void connection_cleanup_atomic(struct connection_metadata *meta)
{
    if (meta == NULL) {
        DEBUG("%s: ERROR: connection_cleanup_atomic called with NULL meta\n",
               COMPONENT_NAME);
        return;
    }

    /* Guard flag check - prevent double-cleanup */
    if (meta->cleanup_in_progress) {
        BREADCRUMB(9300);  /* Double-cleanup attempt blocked */
        DEBUG("%s: GUARD: Cleanup already in progress for this connection (prevented double-decrement)\n",
               COMPONENT_NAME);
        return;
    }

    /* Set guard flag FIRST (before any cleanup) */
    meta->cleanup_in_progress = true;
    BREADCRUMB(9301);  /* Entering atomic cleanup */

    /* Clean up pending outbound data */
    if (meta->pending_outbound_data != NULL) {
        BREADCRUMB(9302);  /* Freeing pending data */
        DEBUG("%s: Freeing unsent pending data (%u bytes)\n",
               COMPONENT_NAME, meta->pending_outbound_len);
        free(meta->pending_outbound_data);
        meta->pending_outbound_data = NULL;
    }

    /* Decrement counters - ONLY place this happens! */
    if (connection_count > 0) {
        connection_count--;
        BREADCRUMB(9303);  /* connection_count decremented */
    }

    if (active_connections > 0) {
        active_connections--;
        BREADCRUMB(9304);  /* active_connections decremented */
        DEBUG("%s: active_connections decremented: %u → %u\n",
               COMPONENT_NAME, active_connections + 1, active_connections);
    } else {
        BREADCRUMB(9305);  /* ERROR: active_connections already 0! */
        DEBUG("%s: ERROR: active_connections already 0 (prevented underflow)!\n",
               COMPONENT_NAME);
    }

    /* Mark metadata as inactive */
    meta->active = false;
    meta->pcb = NULL;
    meta->has_pending_outbound = false;
    meta->pending_outbound_len = 0;
    meta->awaiting_response = false;
    meta->response_received = false;  /* v2.189: Reset response tracking */

    /* v2.147: DO NOT clear guard flag!
     * ═══════════════════════════════════════════════════════════════════════
     * Problem in v2.146:
     * 1. tcp_echo_err() calls connection_cleanup_atomic()
     * 2. cleanup_in_progress = true → cleanup happens → cleanup_in_progress = false
     * 3. Then recv(p=NULL) is ALSO called for same connection
     * 4. Guard check passes (flag is false now!) → cleanup AGAIN
     * 5. Result: active_connections underflow (0 → -1, prevented by guard in decrement)
     *
     * Root Cause:
     * - lwIP can call BOTH tcp_echo_err() AND recv(p=NULL) for same connection
     * - Clearing guard flag allows second cleanup
     *
     * Fix: Keep guard flag SET forever
     * - Once cleanup_in_progress = true, it stays true
     * - Any future cleanup attempts will be blocked
     * - Metadata entry will be reused for new connection anyway (active=false)
     * - New connection will reset cleanup_in_progress = false during initialization
     * ═══════════════════════════════════════════════════════════════════════
     */
    /* REMOVED (v2.147): meta->cleanup_in_progress = false; */
    BREADCRUMB(9306);  /* Cleanup complete (guard flag stays set) */

    /* v2.117: Update shared connection state */
    update_shared_connection_state();
}

/* Print connection table statistics
 *
 * Shows how many connections are:
 * - Active (metadata stored)
 * - PCB-linked (associated with active lwIP PCB)
 * - Stale (PCB is NULL or in closed state)
 */
static void connection_print_stats(void)
{
    /* v2.83: CRITICAL FIX - Do NOT access pcb->state (can crash on freed PCB) */
    int active = 0;
    int pcb_linked = 0;
    int stale = 0;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active) {
            active++;
            if (connection_table[i].pcb != NULL) {
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
    /* v2.92: CRITICAL FIX - Don't cleanup if awaiting_response!
     *
     * Lifecycle:
     * 1. SCADA connects, sends request → metadata created
     * 2. Request forwarded to PLC → awaiting_response = true
     * 3. SCADA closes connection → pcb = NULL (but metadata kept!)
     * 4. PLC response arrives → response sent → awaiting_response = false, metadata removed
     *
     * Bug (v2.90): connection_cleanup_stale() removed metadata at step 3
     * Result: Step 4 fails with "No metadata found"
     *
     * Fix: Only cleanup if pcb == NULL AND awaiting_response == false
     */
    int cleaned = 0;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_table[i].active) {
            continue;
        }

        struct tcp_pcb *pcb = connection_table[i].pcb;

        /* v2.92: ONLY cleanup if PCB is NULL AND we're not awaiting a response */
        if (pcb == NULL && !connection_table[i].awaiting_response) {
            #if DEBUG_METADATA
            DEBUG("%s: [CLEAN] Cleanup stale connection [%d]: PCB is NULL and no response pending\n", COMPONENT_NAME, i);
            #endif
            connection_table[i].active = false;

            /* v2.122: CRITICAL FIX - Do NOT decrement counter here!
             * ═══════════════════════════════════════════════════════════════════
             * Problem: Both error callback AND periodic cleanup decrement counter
             * Result: Double-decrement → counter underflow
             *
             * Root Cause:
             * - When connection error occurs, lwIP calls tcp_echo_err()
             * - Error callback decrements connection_count-- (line 1850)
             * - Later, periodic cleanup finds pcb==NULL and decrements AGAIN!
             *
             * Fix: Let error callback be authoritative for counter decrement
             * Periodic cleanup only marks metadata inactive (no counter change)
             *
             * Why This Works:
             * - Error callback is called by lwIP when connection fails
             * - It decrements counter immediately (authoritative cleanup)
             * - Periodic cleanup only marks metadata->active=false (lazy cleanup)
             * - No double-decrement, accurate counting
             * ═══════════════════════════════════════════════════════════════════
             */

            /* REMOVED (v2.122): connection_count--; */

            cleaned++;
        }

        /* v2.83: REMOVED pcb->state check - accessing freed PCB causes crashes!
         * Rely on lwIP callbacks (tcp_echo_recv with p=NULL, tcp_echo_err)
         * to set pcb=NULL when connection closes. */
    }

    if (cleaned > 0) {
        #if DEBUG_METADATA
        DEBUG("%s: [CLEAN] Cleaned %d stale connection(s)\n", COMPONENT_NAME, cleaned);
        connection_print_stats();
        #endif
    }
}

/* v2.209: Check and cleanup connections with metadata_close_pending
 * ══════════════════════════════════════════════════════════════════════════
 * Purpose: Process delayed metadata cleanups (fix pbuf leak race condition)
 *
 * This function implements two-tier cleanup strategy:
 *
 * Tier 1: Fast-track cleanup (normal case, ~99% of connections)
 *   - Wait for TX path to finish (1 second of TX inactivity)
 *   - Typical cleanup time: ~1 second after SCADA close
 *   - Pool usage: 19.5% at 100 conn/sec
 *
 * Tier 2: Grace period cleanup (safety net, ~1% of connections)
 *   - Wait maximum 5 seconds after close_pending flag set
 *   - Handles edge cases where TX timestamp doesn't update
 *   - Pool usage: 97.7% at 100 conn/sec (triggers emergency cleanup)
 *
 * Emergency Protection:
 *   - Force cleanup at 80% pool threshold (410 connections)
 *   - Prevents pool exhaustion even if everything goes wrong
 *
 * Why This Works:
 *   - Metadata persists until TX definitely completes (no more "TX: No metadata")
 *   - Fast-track keeps normal cleanup delay at 1 second (not 5 seconds!)
 *   - Grace period provides 103× safety margin (5000ms / 48ms race window)
 *   - Emergency cleanup prevents pool exhaustion
 *
 * Called from: Main loop (periodic check)
 * ══════════════════════════════════════════════════════════════════════════
 */
static void check_pending_cleanups(void)
{
    uint32_t now = sys_now();
    int active_count = 0;
    int pending_count = 0;
    int cleaned = 0;

    /* Count active and pending connections */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active) {
            active_count++;
            if (connection_table[i].metadata_close_pending) {
                pending_count++;
            }
        }
    }

    /* Emergency cleanup if pool filling up
     * Trigger at 80% threshold (410 out of 512 connections) */
    float pool_usage = (float)active_count / MAX_CONNECTIONS;
    if (pool_usage > 0.8) {
        DEBUG_WARN("%s: [WARN] Connection pool at %.1f%% (%d/%d), forcing emergency cleanup\n",
                   COMPONENT_NAME, pool_usage * 100, active_count, MAX_CONNECTIONS);

        /* Force cleanup of oldest pending connections (up to 10) */
        int emergency_cleaned = 0;
        for (int i = 0; i < MAX_CONNECTIONS && emergency_cleaned < 10; i++) {
            struct connection_metadata *meta = &connection_table[i];

            if (meta->active && meta->metadata_close_pending) {
                DEBUG_WARN("%s:    → Emergency cleanup: session_id=%u, pending for %u ms\n",
                           COMPONENT_NAME, meta->session_id,
                           (unsigned int)(now - meta->close_timestamp));
                connection_cleanup_atomic(meta);
                emergency_cleaned++;
                cleaned++;
            }
        }
        DEBUG_WARN("%s:    → Emergency cleaned %d connection(s)\n",
                   COMPONENT_NAME, emergency_cleaned);
    }

    /* Process pending cleanups (two-tier strategy) */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        struct connection_metadata *meta = &connection_table[i];

        if (!meta->active || !meta->metadata_close_pending) {
            continue;
        }

        uint32_t grace_elapsed = now - meta->close_timestamp;
        uint32_t tx_idle = now - meta->last_tx_timestamp;

        /* Tier 1: Fast-track cleanup after 1 second TX idle (normal case)
         * This handles 99% of connections:
         *   - TX completes quickly (50-100ms typical)
         *   - Wait 1 second after last TX activity
         *   - Metadata stays in pool for ~1 second total
         */
        if (tx_idle > 1000) {
            #if DEBUG_METADATA
            DEBUG("%s: [CLEAN] Fast-track cleanup: session_id=%u (tx_idle=%u ms)\n",
                  COMPONENT_NAME, meta->session_id, (unsigned int)tx_idle);
            #endif
            connection_cleanup_atomic(meta);
            cleaned++;
            continue;
        }

        /* Tier 2: Grace period cleanup after 5 seconds max (safety net)
         * This handles edge cases:
         *   - TX timestamp not updated (bug)
         *   - TX keeps running >1 second (impossible for Modbus)
         *   - Provides 103× safety margin (5000ms / 48ms race window)
         */
        if (grace_elapsed > 5000) {
            DEBUG_WARN("%s: [WARN] Grace period cleanup: session_id=%u (grace_elapsed=%u ms, tx_idle=%u ms)\n",
                       COMPONENT_NAME, meta->session_id,
                       (unsigned int)grace_elapsed, (unsigned int)tx_idle);
            connection_cleanup_atomic(meta);
            cleaned++;
        }
    }

    /* Log cleanup summary if anything was cleaned */
    if (cleaned > 0) {
        #if DEBUG_METADATA
        DEBUG("%s: [CLEAN] check_pending_cleanups: %d cleaned, %d pending, %d/%d active (%.1f%%)\n",
              COMPONENT_NAME, cleaned, pending_count - cleaned,
              active_count - cleaned, MAX_CONNECTIONS,
              ((float)(active_count - cleaned) / MAX_CONNECTIONS) * 100);
        #endif
    }
}

/* v2.218: Cleanup CLOSE_WAIT and LAST_ACK connections to prevent PBUF pool exhaustion
 * ══════════════════════════════════════════════════════════════════════════
 * Purpose: Automatically close connections stuck in closing states
 *
 * CLOSE_WAIT state:
 *   - Remote side (SCADA) sent FIN (requested close)
 *   - lwIP acknowledged the FIN and moved connection to CLOSE_WAIT
 *   - lwIP is waiting for application to call tcp_close()
 *   - Each connection holds ~27 PBUFs on average
 *
 * LAST_ACK state:
 *   - Application called tcp_close(), lwIP sent FIN
 *   - Waiting for ACK from remote side
 *   - If remote doesn't respond, connection stuck forever
 *   - Still holds PBUFs until final ACK received
 *
 * Root Cause of PBUF Leak:
 *   - 29 CLOSE_WAIT connections × 27 PBUFs = ~800/800 pool exhausted (v2.216)
 *   - v2.217 fixed CLOSE_WAIT but connections moved to LAST_ACK and got stuck
 *   - LAST_ACK connections never freed because SCADA doesn't ACK our FIN
 *   - Pool still exhausts: 6514 freed vs continuous allocations
 *
 * Solution (v2.218):
 *   - Use tcp_abort() instead of tcp_close() for CLOSE_WAIT
 *     - Immediate cleanup, no waiting for FIN handshake
 *     - Safe from main loop (NOT from callbacks!)
 *   - Also cleanup LAST_ACK connections (in case some remain)
 *     - Abort connections stuck in LAST_ACK state
 *     - Frees PBUFs immediately
 *
 * When to Call:
 *   - Every 5 seconds from main loop
 *   - Frequent enough to prevent pool exhaustion
 *
 * Safety:
 *   - tcp_abort() is SAFE from main loop context
 *   - tcp_abort() is UNSAFE from callbacks (would cause use-after-free)
 *   - We call it ONLY from main loop, never from callbacks
 *
 * Called from: Main loop (periodic cleanup)
 * ══════════════════════════════════════════════════════════════════════════
 */
static void cleanup_close_wait_connections(void)
{
    int close_wait_count = 0;
    int last_ack_count = 0;
    int aborted_count = 0;

    /* Scan lwIP's active PCB list for connections in closing states
     * tcp_active_pcbs is the linked list of all active TCP connections */
    struct tcp_pcb *pcb = tcp_active_pcbs;

    while (pcb != NULL) {
        struct tcp_pcb *next = pcb->next;  /* Save next pointer before potential abort */

        if (pcb->state == CLOSE_WAIT) {
            close_wait_count++;

            DEBUG_INFO("%s: [CLOSE_WAIT] Found connection in CLOSE_WAIT state: pcb=%p, "
                      "local=%u.%u.%u.%u:%u, remote=%u.%u.%u.%u:%u\n",
                      COMPONENT_NAME, (void*)pcb,
                      ip4_addr1(&pcb->local_ip), ip4_addr2(&pcb->local_ip),
                      ip4_addr3(&pcb->local_ip), ip4_addr4(&pcb->local_ip),
                      pcb->local_port,
                      ip4_addr1(&pcb->remote_ip), ip4_addr2(&pcb->remote_ip),
                      ip4_addr3(&pcb->remote_ip), ip4_addr4(&pcb->remote_ip),
                      pcb->remote_port);

            /* Use tcp_abort() instead of tcp_close()
             * - Immediately frees all PBUFs (no waiting for FIN handshake)
             * - SAFE from main loop (UNSAFE from callbacks!)
             * - Avoids LAST_ACK stuck state (v2.217 problem)
             */
            tcp_abort(pcb);
            DEBUG_INFO("%s:    → tcp_abort() called - PBUFs freed immediately\n",
                      COMPONENT_NAME);
            aborted_count++;
        }
        else if (pcb->state == LAST_ACK) {
            last_ack_count++;

            DEBUG_INFO("%s: [LAST_ACK] Found connection stuck in LAST_ACK state: pcb=%p, "
                      "local=%u.%u.%u.%u:%u, remote=%u.%u.%u.%u:%u\n",
                      COMPONENT_NAME, (void*)pcb,
                      ip4_addr1(&pcb->local_ip), ip4_addr2(&pcb->local_ip),
                      ip4_addr3(&pcb->local_ip), ip4_addr4(&pcb->local_ip),
                      pcb->local_port,
                      ip4_addr1(&pcb->remote_ip), ip4_addr2(&pcb->remote_ip),
                      ip4_addr3(&pcb->remote_ip), ip4_addr4(&pcb->remote_ip),
                      pcb->remote_port);

            /* Abort stuck LAST_ACK connections
             * Remote side is not responding to our FIN, so abort immediately */
            tcp_abort(pcb);
            DEBUG_INFO("%s:    → tcp_abort() called - freeing stuck connection\n",
                      COMPONENT_NAME);
            aborted_count++;
        }

        pcb = next;  /* Move to next connection */
    }

    /* Log summary if we found any connections to clean */
    if (close_wait_count > 0 || last_ack_count > 0) {
        DEBUG_INFO("%s: [CLEANUP] Summary: CLOSE_WAIT=%d, LAST_ACK=%d, aborted=%d\n",
                  COMPONENT_NAME, close_wait_count, last_ack_count, aborted_count);
    }
}

/*
 * Custom input function for protocol-break architecture WITH metadata preservation
 *
 * CRITICAL: Packets arrive with dest IP = 192.168.95.2 (PLC) but interface IP = 192.168.96.2
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
        DEBUG_WARN("%s: [WARN] Packet too small for ethernet header: p->len=%u, pbuf=%p, p->ref=%d\n",
               COMPONENT_NAME, p->len, (void*)p, p->ref);
        /* v2.222: REVERTED FIX #1 - Caller (line 2981-2986) frees pbuf when we return ERR_ARG
         * Double-free was breaking connections!
         */
        return ERR_ARG;
    }

    ethhdr = (struct eth_hdr *)p->payload;
    type = ntohs(ethhdr->type);

    /* Handle ARP packets normally - pass to lwIP's ARP handler */
    if (type == ETHTYPE_ARP) {
        pbuf_arp_count++;  /* v2.203: Track ARP packets */
        /* Remove Ethernet header and pass to etharp_input for ARP processing */
        if (pbuf_remove_header(p, sizeof(struct eth_hdr)) == 0) {
            /* v2.222: REVERTED FIX #2 - etharp_input() DOES free pbuf (line 741 in etharp.c)
             * Double-free was breaking connections!
             */
            etharp_input(p, inp);
            return ERR_OK;
        }
        /* pbuf_remove_header failed - we must free since we won't pass to lwIP */
        PBUF_TRACK_FREE(p);
        pbuf_free(p);
        pbuf_freed_count++;
        pbuf_error_count++;
        return ERR_ARG;
    }

    /* Handle IPv6 - pass to ethernet_input */
    if (type == ETHTYPE_IPV6) {
        /* v2.222: REVERTED FIX #3 - ethernet_input() DOES free pbuf on all paths
         * Double-free was breaking connections!
         */
        return ethernet_input(p, inp);
    }

    /* Handle IPv4 with IP rewriting for protocol-break */
    if (type == ETHTYPE_IP) {
        /* Remove Ethernet header first */
        if (pbuf_remove_header(p, sizeof(struct eth_hdr)) != 0) {
            /* v2.222: REVERTED FIX #4 - Caller frees pbuf when we return ERR_ARG
             * Double-free was breaking connections!
             */
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
                pbuf_tcp_count++;  /* v2.203: Track TCP packets */
                struct tcp_hdr *tcphdr = (struct tcp_hdr *)((uint8_t *)iphdr + (IPH_HL(iphdr) * 4));
                src_port = ntohs(tcphdr->src);
                dest_port = ntohs(tcphdr->dest);

                /* v2.222: TCP leak handling removed per user request (breaks connection)
                 * TCP packets to non-listening ports will be handled by lwIP
                 * Note: This may cause pbuf leaks for TCP to wrong ports, but connection
                 *       functionality takes precedence
                 */
            } else if (IPH_PROTO(iphdr) == IP_PROTO_UDP) {
                pbuf_udp_count++;  /* v2.203: Track UDP packets */
            } else {
                pbuf_other_count++;  /* v2.203: Track other IP protocols */
            }

            /* If packet is not destined for our interface IP, rewrite it */
            /* v2.95: CRITICAL FIX - Create metadata for ALL TCP connections!
             * ═══════════════════════════════════════════════════════════════════
             * BUG: Original code only created metadata if pkt_dest_ip != interface_ip
             * Problem: When SCADA connects directly to interface IP (192.168.96.2:502),
             * no metadata is created → responses can't find SCADA connection!
             *
             * Fix: Create metadata for ALL TCP connections, regardless of dest IP
             * This ensures we can route responses back to SCADA even for direct connections
             */
            if (IPH_PROTO(iphdr) == IP_PROTO_TCP && src_port != 0 && dest_port != 0) {
                /* Check if we already have metadata for this connection */
                struct connection_metadata *meta = connection_lookup_by_tuple(
                    pkt_src_ip, pkt_dest_ip, src_port, dest_port);

                if (!meta) {
                    /* New connection - store metadata */
                    #if DEBUG_METADATA
                    DEBUG("%s: 📝 RX: Storing NEW metadata: src=%u.%u.%u.%u:%u dest=%u.%u.%u.%u:%u\n",
                           COMPONENT_NAME,
                           (pkt_src_ip >> 24) & 0xFF, (pkt_src_ip >> 16) & 0xFF,
                           (pkt_src_ip >> 8) & 0xFF, pkt_src_ip & 0xFF, src_port,
                           (pkt_dest_ip >> 24) & 0xFF, (pkt_dest_ip >> 16) & 0xFF,
                           (pkt_dest_ip >> 8) & 0xFF, pkt_dest_ip & 0xFF, dest_port);
                    #endif
                    connection_add(pkt_src_ip, pkt_dest_ip, src_port, dest_port);
                } else if (!meta->active) {
                    /* v2.196: CRITICAL FIX - Don't increment connection_count on resurrection!
                     * ═══════════════════════════════════════════════════════════════════════════
                     * Root Cause (v2.195 analysis):
                     * - Resurrection reuses existing slot, not a "new" connection
                     * - tcp_accept() won't be called again (lwIP already processed original SYN)
                     * - Result: resurrected connection stuck with pcb=NULL, no callbacks
                     * - No cleanup path exists → LEAK!
                     *
                     * Evidence from v2.195 logs:
                     * - 772 [COUNT++] operations (299 connection_add + 473 RESURRECT)
                     * - 270 [COUNT--] operations (only from connection_add path!)
                     * - 502 connections leaked (exactly matches 772 - 270!)
                     *
                     * Fix: DON'T increment connection_count on resurrection
                     * - Resurrection reuses slot (connection already counted by connection_add)
                     * - Only connection_add() should increment count
                     * - Cleanup only happens for connection_add() connections
                     * - This prevents leak: only count connections with cleanup path
                     * ═══════════════════════════════════════════════════════════════════════════
                     */
                    DEBUG("%s: [RESURRECT] Reactivating slot for port %u→%u (old session=%u)\n",
                           COMPONENT_NAME, src_port, dest_port, meta->session_id);

                    /* Assign NEW session ID (old one might still be in cleanup queue) */
                    meta->session_id = next_session_id++;
                    meta->active = true;
                    meta->pcb = NULL;  /* Will be set when TCP accept happens */
                    meta->cleanup_in_progress = false;  /* Reset cleanup flag */

                    /* Reset lifecycle flags */
                    meta->awaiting_response = false;
                    meta->response_received = false;
                    meta->close_pending = false;
                    meta->closing = false;

                    /* Clean up any pending data */
                    if (meta->pending_outbound_data != NULL) {
                        free(meta->pending_outbound_data);
                        meta->pending_outbound_data = NULL;
                    }
                    meta->pending_outbound_len = 0;
                    meta->has_pending_outbound = false;

                    /* v2.196: DO NOT increment connection_count on resurrection!
                     * Resurrection reuses existing slot - connection already counted.
                     * Only connection_add() creates new connections and has cleanup path.
                     */
                    DEBUG("%s: [RESURRECT] session=%u port=%u→%u (count unchanged: %u, slot reused)\n",
                           COMPONENT_NAME, meta->session_id, src_port, dest_port,
                           connection_count);

                    update_shared_connection_state();
                } else {
                    #if DEBUG_ENABLED_DEBUG
                    DEBUG("%s: [FIND] RX: Found EXISTING metadata [slot %d] for %u.%u.%u.%u:%u → %u.%u.%u.%u:%u\n",
                           COMPONENT_NAME, (int)(meta - connection_table),
                           (pkt_src_ip >> 24) & 0xFF, (pkt_src_ip >> 16) & 0xFF,
                           (pkt_src_ip >> 8) & 0xFF, pkt_src_ip & 0xFF, src_port,
                           (pkt_dest_ip >> 24) & 0xFF, (pkt_dest_ip >> 16) & 0xFF,
                           (pkt_dest_ip >> 8) & 0xFF, pkt_dest_ip & 0xFF, dest_port);
                    #endif
                }
            }

            /* Rewrite destination IP to interface IP if needed */
            if (pkt_dest_ip != interface_ip) {
                #if DEBUG_ENABLED_DEBUG
                DEBUG("%s: [RETRY] RX: Rewriting dest IP: %u.%u.%u.%u → %u.%u.%u.%u (TCP %u → %u)\n",
                       COMPONENT_NAME,
                       (pkt_dest_ip >> 24) & 0xFF, (pkt_dest_ip >> 16) & 0xFF,
                       (pkt_dest_ip >> 8) & 0xFF, pkt_dest_ip & 0xFF,
                       (interface_ip >> 24) & 0xFF, (interface_ip >> 16) & 0xFF,
                       (interface_ip >> 8) & 0xFF, interface_ip & 0xFF,
                       src_port, dest_port);
                #endif

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
    pbuf_other_count++;  /* v2.203: Track unknown protocols */
    BREADCRUMB(9004);  /* pbuf_free at line 1321 (unknown protocol) */
    PBUF_TRACK_FREE(p);  /* v2.222: Track pbuf free */
    pbuf_free(p);
    pbuf_freed_count++;  /* v2.203: Track free */
    pbuf_error_count++;  /* v2.203: We freed unknown protocol */
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

    /* v2.138: CRITICAL FIX - Reentrancy guard to prevent deadlock
     * Problem: process_rx_packets() called from BOTH main loop AND IRQ handler
     * - Main loop calls at line 3848 (after B8004)
     * - IRQ handler calls at line 3239 (when VirtIO interrupt fires)
     * Without guard: IRQ can interrupt main loop's call → reentrant execution → deadlock!
     *
     * Evidence from v2.137:
     * - B8002-B8005 stopped (main loop stuck)
     * - B8006-B8009 continued (IRQ handler kept calling)
     * - Main loop never reached B8005 → deadlock
     *
     * Solution: Static flag prevents reentrant execution
     * - If already processing, return immediately (IRQ handler backs off)
     * - Main loop completes uninterrupted
     */
    static volatile bool in_rx_processing = false;

    if (in_rx_processing) {
        /* Already processing - return immediately to avoid reentrancy */
        return;
    }

    in_rx_processing = true;

    check_count++;

    /* v2.137: Track function entry */
    BREADCRUMB(8006);  /* process_rx_packets entry */

    /* v2.131: Diagnostic - check if RX processing is called */
    if (check_count % 1000 == 1) {
        BREADCRUMB(8010);  /* process_rx_packets called (every 1000th time) */
    }

    #if DEBUG_ENABLED_DEBUG
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
            /* v2.137: Track early return (no packets waiting) */
            BREADCRUMB(8007);  /* No packets, early return */
            /* No more packets - exit IRQ handler and let timer handle refill */
            in_rx_processing = false;  /* v2.138: Clear reentrancy guard */
            return;
        }

        /* v2.131: Diagnostic - packet found in RX queue */
        BREADCRUMB(8020);  /* RX packet detected in queue */

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
                in_rx_processing = false;  /* v2.138: Clear reentrancy guard */
                return;
            }

            /* True desync - should never happen with proper memory barriers */
            DEBUG_WARN("%s: [WARN] TRUE DESYNC: pending=%u exceeds ring_size=%u\n",
                   COMPONENT_NAME, pending_packets, vq->num);
            DEBUG("%s:   last_used_idx=%u, current_used_idx=%u\n",
                   COMPONENT_NAME, last_used_idx, current_used_idx);
            last_used_idx = current_used_idx;
            /* Don't refill here - let timer handle it to avoid IRQ storm */
            in_rx_processing = false;  /* v2.138: Clear reentrancy guard */
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
            DEBUG_WARN("%s: [WARN]  INVALID packet length: %u bytes (expected %u-%u)\n",
                   COMPONENT_NAME, len, VIRTIO_NET_HDR_SIZE, 1514 + VIRTIO_NET_HDR_SIZE);
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

        /* v2.204: Print pbuf statistics every 100 packets (MOVED INSIDE LOOP)
         * v2.203 bug: Stats were checked AFTER loop, missing packets 100, 200, etc.
         * because loop processes up to 8 packets at once (e.g., 96-103), and
         * by the time we check, packets_received=103 (103%100=3, not 0).
         *
         * Fix: Check immediately after each packet increment
         */
        if (packets_received % 100 == 0) {
            DEBUG_INFO("%s: [PBUF-STATS] Pkt#%u Pool:%u/%u Alloc=%u Free=%u ToLwIP=%u Leak=%d\n",
                   COMPONENT_NAME,
                   packets_received,
                   lwip_stats.memp[MEMP_PBUF_POOL]->used,
                   PBUF_POOL_SIZE,
                   pbuf_allocated_count,
                   pbuf_freed_count,
                   pbuf_leaked_to_lwip,
                   (int)pbuf_allocated_count - (int)pbuf_freed_count);
            DEBUG_INFO("%s: [PBUF-TYPE] ARP=%u TCP=%u UDP=%u Other=%u Err=%u\n",
                   COMPONENT_NAME,
                   pbuf_arp_count,
                   pbuf_tcp_count,
                   pbuf_udp_count,
                   pbuf_other_count,
                   pbuf_error_count);

            /* v2.205: Out-of-order segment diagnostics */
            ooseq_stats_t ooseq;
            get_ooseq_stats(&ooseq);
            DEBUG_INFO("%s: [OOSEQ] PCBs=%u/%u WithOoseq=%u Segments=%u Pbufs=%u\n",
                   COMPONENT_NAME,
                   ooseq.total_active_pcbs,
                   connection_count,
                   ooseq.pcbs_with_ooseq,
                   ooseq.total_ooseq_segments,
                   ooseq.total_ooseq_pbufs);

            /* v2.205: PCB state breakdown */
            pcb_state_stats_t pcb_states;
            get_pcb_state_stats(&pcb_states);
            DEBUG_INFO("%s: [PCB-STATE] ESTAB=%u CLOSE_WAIT=%u TIME_WAIT=%u FIN_WAIT1=%u FIN_WAIT2=%u\n",
                   COMPONENT_NAME,
                   pcb_states.pcb_established,
                   pcb_states.pcb_close_wait,
                   pcb_states.pcb_time_wait,
                   pcb_states.pcb_fin_wait_1,
                   pcb_states.pcb_fin_wait_2);

            /* v2.205: Connection metadata vs PCB matching */
            connection_match_stats_t conn_match;
            get_connection_match_stats(&conn_match);
            DEBUG_INFO("%s: [CONN-MATCH] WithPCB=%u WithoutPCB=%u Inactive=%u OrphanPCBs=%u\n",
                   COMPONENT_NAME,
                   conn_match.metadata_active_with_pcb,
                   conn_match.metadata_active_without_pcb,
                   conn_match.metadata_inactive,
                   conn_match.pcb_without_metadata);

            /* v2.206: Orphan PCB buffer diagnostics */
            if (conn_match.pcb_without_metadata > 0) {
                orphan_pcb_diag_t orphans[10];
                int orphan_count = diagnose_orphan_pcbs(orphans, 10);

                for (int i = 0; i < orphan_count; i++) {
                    DEBUG_INFO("%s: [ORPHAN-PCB] PCB=%p State=%u RcvWnd=%u SndBuf=%u\n",
                           COMPONENT_NAME,
                           orphans[i].pcb_addr,
                           orphans[i].state,
                           orphans[i].rcv_wnd,
                           orphans[i].snd_buf);
                    DEBUG("%s:   Refused=%u Unacked=%u/%u Unsent=%u/%u Ooseq=%u\n",
                           COMPONENT_NAME,
                           orphans[i].refused_data_pbufs,
                           orphans[i].unacked_segments,
                           orphans[i].unacked_pbufs,
                           orphans[i].unsent_segments,
                           orphans[i].unsent_pbufs,
                           orphans[i].ooseq_segments);
                }
            }
        }

        /* NOTE: TCP server initialization moved to post_init()
         * The tcp_server_initialized flag is set there.
         * This deferred initialization code is no longer needed.
         */



        /* Only show detailed packet processing if VERBOSE debug enabled */

        /* Allocate pbuf and copy packet data (skipping header) */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, packet_len, PBUF_POOL);
        if (p != NULL) {
            /* v2.215: Track PBUF_POOL allocation with interface identification */
            pbuf_allocated_count++;  /* v2.203: Track allocation */
            PBUF_TRACK_ALLOC(p, "Net0");  /* v2.222: Deep pbuf tracking */
            extern struct stats_ lwip_stats;
            uint32_t pbuf_before_take = lwip_stats.memp[MEMP_PBUF_POOL]->used;

            pbuf_take(p, packet_data, packet_len);

            uint32_t pbuf_after_take = lwip_stats.memp[MEMP_PBUF_POOL]->used;
            DEBUG_ERROR("[Net0][PBUF_POOL][RX-ALLOC] pbuf=%p, len=%u | PBUF: %u/800 (after take: %u/800)\n",
                       (void*)p, packet_len, pbuf_before_take, pbuf_after_take);

            #if DEBUG_ENABLED_DEBUG
            DEBUG("   [OK] pbuf allocated, passing to lwIP input handler\n");
            #endif

            /* v2.137: Track before lwIP input call */
            BREADCRUMB(8008);  /* Before lwIP input() */

            /* Feed packet to lwIP */
            err_t lwip_result = netif_data.input(p, &netif_data);

            /* v2.137: Track after lwIP input call */
            BREADCRUMB(8009);  /* After lwIP input() */

            /* v2.215: Track lwIP input acceptance/rejection */
            uint32_t pbuf_after_input = lwip_stats.memp[MEMP_PBUF_POOL]->used;
            if (lwip_result == ERR_OK) {
                DEBUG_ERROR("[Net0][PBUF_POOL][RX-ACCEPT] lwIP accepted pbuf=%p | PBUF: %u/800\n",
                           (void*)p, pbuf_after_input);
            } else {
                DEBUG_ERROR("[Net0][PBUF_POOL][RX-REJECT] lwIP rejected (err=%d), must free pbuf=%p | PBUF: %u/800\n",
                           lwip_result, (void*)p, pbuf_after_input);
            }

            #if DEBUG_ENABLED_DEBUG
            if (lwip_result == ERR_OK) {
                DEBUG("   [OK] lwIP accepted packet (will route to TCP/UDP/etc.)\n");
            } else {
                DEBUG("   ✗ lwIP rejected packet (err=%d)\n", lwip_result);
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
                                #if DEBUG_ENABLED_DEBUG
                                DEBUG("%s: [FIND] SYN packet detected: Dest IP = %u.%u.%u.%u:%u (Interface IP = 192.168.95.2)\n",
                                       COMPONENT_NAME,
                                       (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF,
                                       ntohs(tcp->dest));
                                DEBUG("%s:    → If dest IP matches interface IP, lwIP should accept. Otherwise it rejects.\n", COMPONENT_NAME);
                                #endif
                            }
                        }
                    }
                }
            }

            if (lwip_result != ERR_OK) {
                BREADCRUMB(9005);  /* pbuf_free at line 1791 (process_rx_packets lwip error) */
                PBUF_TRACK_FREE(p);  /* v2.222: Track pbuf free */
                pbuf_free(p);
                pbuf_freed_count++;      /* v2.203: Track free */
                pbuf_error_count++;      /* v2.203: lwIP rejected, we freed */
            } else {
                pbuf_leaked_to_lwip++;   /* v2.203: lwIP accepted, it owns pbuf now */
            }
        } else {
            /* CRITICAL: pbuf allocation failed - this means lwIP is out of memory */
            DEBUG_WARN("%s: [WARN]  WARNING: Failed to allocate pbuf for packet #%u - dropping (lwIP out of memory)\n",
                   COMPONENT_NAME, packets_received);

            /* v2.202: DIAGNOSTIC - Print detailed memory statistics on allocation failure */
            DEBUG("%s: [MEM-DIAG] PBUF_POOL: used=%u/%u, avail=%u, peak=%u\n",
                   COMPONENT_NAME,
                   lwip_stats.memp[MEMP_PBUF_POOL]->used,
                   PBUF_POOL_SIZE,
                   lwip_stats.memp[MEMP_PBUF_POOL]->avail,
                   lwip_stats.memp[MEMP_PBUF_POOL]->max);
            DEBUG("%s: [MEM-DIAG] TCP_PCB: used=%u/%u, avail=%u, peak=%u\n",
                   COMPONENT_NAME,
                   lwip_stats.memp[MEMP_TCP_PCB]->used,
                   MEMP_NUM_TCP_PCB,
                   lwip_stats.memp[MEMP_TCP_PCB]->avail,
                   lwip_stats.memp[MEMP_TCP_PCB]->max);
            DEBUG("%s: [MEM-DIAG] TCP_SEG: used=%u/%u, avail=%u, peak=%u\n",
                   COMPONENT_NAME,
                   lwip_stats.memp[MEMP_TCP_SEG]->used,
                   MEMP_NUM_TCP_SEG,
                   lwip_stats.memp[MEMP_TCP_SEG]->avail,
                   lwip_stats.memp[MEMP_TCP_SEG]->max);
            DEBUG("%s: [MEM-DIAG] MEM heap: used=%u, max=%u, err=%u\n",
                   COMPONENT_NAME,
                   lwip_stats.mem.used,
                   lwip_stats.mem.max,
                   lwip_stats.mem.err);
            DEBUG("%s: [MEM-DIAG] Requested packet size: %u bytes\n",
                   COMPONENT_NAME, packet_len);
            DEBUG("%s: [MEM-DIAG] Active connections: %u\n",
                   COMPONENT_NAME, connection_count);

            /* v2.203: Pbuf lifecycle tracking */
            DEBUG("%s: [PBUF-LEAK] Allocated=%u, Freed=%u, ToLwIP=%u, Leaked=%d\n",
                   COMPONENT_NAME,
                   pbuf_allocated_count,
                   pbuf_freed_count,
                   pbuf_leaked_to_lwip,
                   (int)pbuf_allocated_count - (int)pbuf_freed_count);
            DEBUG("%s: [PBUF-TYPE] ARP=%u, TCP=%u, UDP=%u, Other=%u, Errors=%u\n",
                   COMPONENT_NAME,
                   pbuf_arp_count,
                   pbuf_tcp_count,
                   pbuf_udp_count,
                   pbuf_other_count,
                   pbuf_error_count);

            /* v2.205: Out-of-order segment diagnostics on failure */
            ooseq_stats_t ooseq;
            get_ooseq_stats(&ooseq);
            DEBUG("%s: [OOSEQ] ActivePCBs=%u OurConns=%u PCBsWithOoseq=%u TotalSegments=%u TotalPbufs=%u\n",
                   COMPONENT_NAME,
                   ooseq.total_active_pcbs,
                   connection_count,
                   ooseq.pcbs_with_ooseq,
                   ooseq.total_ooseq_segments,
                   ooseq.total_ooseq_pbufs);

            /* v2.205: PCB state breakdown on failure */
            pcb_state_stats_t pcb_states;
            get_pcb_state_stats(&pcb_states);
            DEBUG_INFO("%s: [PCB-STATE] ESTAB=%u CLOSE_WAIT=%u TIME_WAIT=%u FIN_WAIT1=%u FIN_WAIT2=%u SYN=%u LAST_ACK=%u CLOSING=%u\n",
                   COMPONENT_NAME,
                   pcb_states.pcb_established,
                   pcb_states.pcb_close_wait,
                   pcb_states.pcb_time_wait,
                   pcb_states.pcb_fin_wait_1,
                   pcb_states.pcb_fin_wait_2,
                   pcb_states.pcb_syn_sent + pcb_states.pcb_syn_rcvd,
                   pcb_states.pcb_last_ack,
                   pcb_states.pcb_closing);

            /* v2.205: Connection metadata vs PCB matching on failure */
            connection_match_stats_t conn_match;
            get_connection_match_stats(&conn_match);
            DEBUG_INFO("%s: [CONN-MATCH] WithPCB=%u WithoutPCB=%u Inactive=%u OrphanPCBs=%u\n",
                   COMPONENT_NAME,
                   conn_match.metadata_active_with_pcb,
                   conn_match.metadata_active_without_pcb,
                   conn_match.metadata_inactive,
                   conn_match.pcb_without_metadata);

            /* v2.206: Orphan PCB buffer diagnostics on failure */
            if (conn_match.pcb_without_metadata > 0) {
                DEBUG_INFO("%s: [ORPHAN-DIAG] Found %u orphan PCB(s) - diagnosing buffer state:\n",
                       COMPONENT_NAME, conn_match.pcb_without_metadata);

                orphan_pcb_diag_t orphans[10];
                int orphan_count = diagnose_orphan_pcbs(orphans, 10);

                for (int i = 0; i < orphan_count; i++) {
                    DEBUG_INFO("%s: [ORPHAN-PCB-%d] PCB=%p State=%u RcvWnd=%u SndBuf=%u\n",
                           COMPONENT_NAME, i,
                           orphans[i].pcb_addr,
                           orphans[i].state,
                           orphans[i].rcv_wnd,
                           orphans[i].snd_buf);
                    DEBUG("%s:   Buffers: Refused=%u Unacked(seg/pbuf)=%u/%u Unsent(seg/pbuf)=%u/%u Ooseq=%u\n",
                           COMPONENT_NAME,
                           orphans[i].refused_data_pbufs,
                           orphans[i].unacked_segments,
                           orphans[i].unacked_pbufs,
                           orphans[i].unsent_segments,
                           orphans[i].unsent_pbufs,
                           orphans[i].ooseq_segments);

                    uint32_t total_pbufs = orphans[i].refused_data_pbufs +
                                          orphans[i].unacked_pbufs +
                                          orphans[i].unsent_pbufs;
                    DEBUG("%s:   TOTAL PBUFS IN THIS ORPHAN: ~%u\n",
                           COMPONENT_NAME, total_pbufs);
                }
            }
        }

        /* Mark buffer as free (buf_idx already defined above) */
        rx_buffer_used[buf_idx] = false;

        /* Move to next packet */
        last_used_idx++;
    }

    /* v2.204: Pbuf statistics moved INSIDE packet loop (see line 2315) */

    refill_rx_queue();

    /* v2.137: Track function exit (after processing all packets) */
    BREADCRUMB(8011);  /* process_rx_packets exit */

    /* v2.138: Clear reentrancy guard before returning */
    in_rx_processing = false;
}

/*
 * TCP Error callback - handles connection errors and cleanup
 * v2.146: Now receives metadata via arg to send close notifications
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

    /* v2.146: CRITICAL FIX - Send close notification to Net1 from error callback!
     * ═══════════════════════════════════════════════════════════════════════════
     * ROOT CAUSE OF NET1 PCB POOL EXHAUSTION:
     *
     * Problem (v2.143-v2.145):
     * - tcp_echo_err() did NOTHING, just printed "will be cleaned by recv(p=NULL)"
     * - recv(p=NULL) is NEVER called by lwIP after tcp_abort()
     * - NO close notifications sent to Net1
     * - Net1's PCB pool stays full → "Failed to create TCP PCB" after ~100 connections
     *
     * Evidence from logs:
     * - 0 close notifications sent
     * - Net0 active_connections goes to 0 (Net0 cleans up)
     * - But Net1 PCB pool exhausted (Net1 never cleans up)
     *
     * Fix: tcp_echo_err() MUST send close notification
     * - tcp_arg now passes metadata (line 2699)
     * - Error callback can access connection info
     * - Send close notification to Net1 (same as recv callback)
     * - Call connection_cleanup_atomic to clean up Net0
     * - Net1 receives notification and cleans up its PCB ✓
     * ═══════════════════════════════════════════════════════════════════════════
     */

    struct connection_metadata *meta = (struct connection_metadata *)arg;

    DEBUG_WARN("%s: [WARN]  TCP connection error - err=%d (%s)\n", COMPONENT_NAME, err, err_name);

    if (meta == NULL) {
        DEBUG("%s:    → No metadata (old connection?) - cannot send close notification\n",
               COMPONENT_NAME);
        return;
    }

    /* v2.149: CRITICAL - Check if this is a STALE callback for a reused metadata slot!
     * ═══════════════════════════════════════════════════════════════════════════
     * Problem: Port reuse causes metadata slot reuse
     * 1. Old connection closes → cleanup happens → meta->active = false, cleanup_in_progress = true
     * 2. New connection opens with SAME port → connection_add() reuses slot
     * 3. connection_add() resets cleanup_in_progress = false (line 1026)
     * 4. Old connection's ERROR CALLBACK finally fires (delayed by lwIP)
     * 5. Callback passes same metadata pointer → looks like new connection
     * 6. Cleanup runs AGAIN → active_connections-- for connection that already decremented!
     *
     * Solution: If cleanup_in_progress is FALSE but active is FALSE, this is STALE callback
     * - meta->active = false means connection was already cleaned up
     * - Old callback should be IGNORED
     * ═══════════════════════════════════════════════════════════════════════════
     */
    if (!meta->active) {
        DEBUG("%s: Stale error callback for already-cleaned connection (ignoring)\n",
               COMPONENT_NAME);
        return;
    }

    /* v2.153: Handle awaiting_response connections */
    if (meta->awaiting_response) {
        DEBUG("%s: Error while awaiting PLC response (session %u) - cleaning up immediately\n",
               COMPONENT_NAME, meta->session_id);
        DEBUG("%s:    → Error occurred, PLC response will not arrive\n", COMPONENT_NAME);
        /* Clear awaiting_response and proceed to cleanup */
        meta->awaiting_response = false;
    }

    /* v2.192: Error callback now only handles actual errors
     * ═══════════════════════════════════════════════════════════════════════════
     * Simplified from v2.180-v2.191:
     * - Old: Error callback handled deferred cleanup (meta->closing=true path)
     * - New: Poll callback cleans up immediately, error callback just for errors
     *
     * This callback fires when:
     * - SCADA sends RST (connection aborted)
     * - Connection times out
     * - lwIP internal error
     *
     * NOT called anymore for:
     * - Normal close (poll callback handles it now)
     * ═══════════════════════════════════════════════════════════════════════════
     */
    DEBUG("%s:    → SCADA connection error (session %u, err=%d) - cleaning up\n",
           COMPONENT_NAME, meta->session_id, err);

    /* STEP 1: Check for guard flag (prevent double-cleanup) */
    if (meta->cleanup_in_progress) {
        BREADCRUMB(9320);  /* Error callback blocked by guard */
        DEBUG("%s: GUARD: Cleanup already in progress (prevented double-cleanup)\n",
               COMPONENT_NAME);
        return;
    }

    /* STEP 2: Update shared state BEFORE sending notification (v2.144 fix) */
    update_shared_connection_state();

    /* STEP 3: Enqueue close notification to Net1 (v2.153)
     * ═══════════════════════════════════════════════════════════════════════
     * Uses control queue instead of overwriting request_msg.
     * Prevents race where close notification overwrites pending request.
     *
     * v2.192: Simplified - poll callback always sends notification before cleanup,
     * so this path only handles unexpected error cases
     * ═══════════════════════════════════════════════════════════════════════
     */
    if (inbound_dp != NULL && !meta->close_notified) {
        InboundDataport *dp = (InboundDataport *)inbound_dp;

        /* Enqueue close notification with error code
         * v2.175: Pass ERR_RST vs ERR_CLSD to enable symmetrical close behavior
         * - If SCADA sent RST (err == ERR_RST): Net1 should send RST to PLC (tcp_abort)
         * - If SCADA sent FIN (err == ERR_CLSD): Net1 should send FIN to PLC (tcp_close)
         * This prevents Net1 PCB pool exhaustion in ICS environments where
         * SCADA sends FIN+RST and PLC network stops responding.
         */
        bool success = control_queue_enqueue(
            &dp->close_queue,
            meta->session_id,
            (int8_t)err,  /* Pass ERR_RST (-14) or ERR_CLSD (-15) */
            0   /* flags */
        );

        if (success) {
            meta->close_notified = true;  /* Set dedup flag */

            /* v2.188-sentinel: Mark as close-only notification
             * Error callback means connection terminated, no request data to send
             * Set sentinel for defensive programming and consistency
             */
            dp->request_msg.payload_length = 0;  /* Sentinel: close-only, no payload */
            dp->request_msg.metadata.session_id = meta->session_id;
            __sync_synchronize();  /* Memory barrier - ensure sentinel visible before signal */

            inbound_ready_emit();         /* Signal Net1 */

            DEBUG("%s: Enqueued close notification to Net1 "
                   "(session %u, SCADA %u.%u.%u.%u:%u closed, err=%s)\n",
                   COMPONENT_NAME, meta->session_id,
                   (meta->original_src_ip >> 24) & 0xFF,
                   (meta->original_src_ip >> 16) & 0xFF,
                   (meta->original_src_ip >> 8) & 0xFF,
                   meta->original_src_ip & 0xFF,
                   meta->src_port,
                   err_name);
        } else {
            DEBUG("%s: [ERROR] Failed to enqueue close notification (queue full? session %u)\n",
                   COMPONENT_NAME, meta->session_id);
        }
    } else if (meta->close_notified) {
        DEBUG("%s: [DEDUP] Close already notified for session %u\n",
               COMPONENT_NAME, meta->session_id);
    }

    /* STEP 4: Enqueue cleanup (will be processed by main loop)
     * v2.193: Queue-based cleanup - no more direct cleanup calls from callbacks */
    enqueue_cleanup(meta->session_id);

    /* NOTE: PCB is already freed by lwIP - DO NOT access it! */
}

/*
 * TCP Echo callbacks
 */

/*
 * v2.111: TCP sent callback - Send pending outbound data from lwIP context
 * ══════════════════════════════════════════════════════════════════════════
 * This callback is called by lwIP when data is ACKed by remote peer.
 * We use it to check if there's pending outbound data queued by
 * outbound_ready_handle() and send it from safe lwIP callback context.
 *
 * WHY: Prevents race condition between tcp_write() and lwIP timers
 * ══════════════════════════════════════════════════════════════════════════
 */
static err_t tcp_echo_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    /* v2.163: REMOVED pending outbound data mechanism
     * ═══════════════════════════════════════════════════════════════════════════
     * Old design (v2.111-v2.162): tcp_sent callback sent pending data
     * - Bug: Callback never fired if SCADA closed before PLC responded
     * - Result: Response never sent, connection hung
     *
     * New design (v2.163): outbound_ready_handle sends immediately
     * - No pending data mechanism needed
     * - tcp_sent callback now just returns ERR_OK (lwIP requires it registered)
     * ═══════════════════════════════════════════════════════════════════════════
     */
    return ERR_OK;
}

static err_t tcp_echo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (p == NULL) {
        /* v2.115: CRITICAL FIX - Connection closed by remote peer
         * ═══════════════════════════════════════════════════════════════════════
         * CORRECT lwIP protocol for connection close:
         *
         * lwIP Documentation (tcp.h:71-82):
         *   "Only return ERR_ABRT if you have called tcp_abort from within the
         *    callback function!"
         *
         * lwIP Documentation (tcp.c:628-633):
         *   "When calling this from one of the TCP callbacks, make sure you always
         *    return ERR_ABRT (and never return ERR_ABRT otherwise or you will risk
         *    accessing deallocated memory or memory leaks!)"
         *
         * Previous Bug (v2.81-v2.114):
         * - We returned ERR_ABRT WITHOUT calling tcp_abort()
         * - lwIP assumed we aborted, jumped to cleanup
         * - But PCB was still in tcp_active_pcbs list!
         * - Next packet arrived, lwIP tried to cache PCB
         * - Found PCB in inconsistent state → assertion failed
         *   "tcp_input: pcb->next != pcb (before cache)"
         * - With default assert handler: abort() called → Net0 DEAD
         *
         * Correct Pattern (from lwIP examples):
         * 1. Save PCB data BEFORE closing (PCB becomes invalid after tcp_close/abort)
         * 2. Call tcp_close(pcb) to close gracefully
         * 3. If tcp_close() fails, call tcp_abort(pcb) and return ERR_ABRT
         * 4. If tcp_close() succeeds, return ERR_OK (NOT ERR_ABRT!)
         *
         * References:
         * - apps/http/altcp_proxyconnect.c:223-228
         * - apps/mqtt/mqtt.c:537-552
         * ═══════════════════════════════════════════════════════════════════════
         */

        /* v2.115: STEP 1 - Find metadata FIRST (contains all connection info)
         * CRITICAL: NEVER access PCB fields directly! (See CRITICAL_LESSON_PCB_ACCESS.md)
         * - Accessing PCB fields can crash if lwIP frees PCB during access
         * - Metadata already has all connection info (stored during accept)
         * - Use metadata instead of PCB fields for safety
         */
        struct connection_metadata *meta = NULL;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connection_table[i].active && connection_table[i].pcb == pcb) {
                meta = &connection_table[i];
                break;
            }
        }

        /* If no metadata found, can't send close notification (should not happen) */
        if (meta == NULL) {
            DEBUG_WARN("%s: [WARN]  Connection closed but no metadata found (PCB=%p)\n",
                   COMPONENT_NAME, (void*)pcb);
            /* Still need to close the PCB properly */
            err_t close_err = tcp_close(pcb);
            if (close_err != ERR_OK) {
                tcp_abort(pcb);
                return ERR_ABRT;
            }
            return ERR_OK;
        }

        /* v2.143: Guard flag check - prevent recursive cleanup
         * ═══════════════════════════════════════════════════════════════════════
         * If tcp_close() triggers this callback recursively, guard flag will be set
         * Return early to avoid double-cleanup
         * ═══════════════════════════════════════════════════════════════════════
         */
        if (meta->cleanup_in_progress) {
            BREADCRUMB(9310);  /* Recursive recv(p=NULL) blocked by guard */
            DEBUG("%s: GUARD: recv(p=NULL) called recursively - cleanup already in progress\n",
                   COMPONENT_NAME);
            return ERR_OK;
        }

        #if DEBUG_TRAFFIC
        DEBUG("%s: [INIT] TCP connection closed by SCADA\n", COMPONENT_NAME);
        DEBUG("%s:    Remote: %u.%u.%u.%u:%u\n", COMPONENT_NAME,
               (meta->original_src_ip >> 24) & 0xFF, (meta->original_src_ip >> 16) & 0xFF,
               (meta->original_src_ip >> 8) & 0xFF, meta->original_src_ip & 0xFF,
               meta->src_port);
        #endif

        /* v2.143: Design Change - Immediate cleanup instead of delayed cleanup
         * ═══════════════════════════════════════════════════════════════════════
         * OLD DESIGN (v2.90-v2.142): "Keep metadata alive"
         *   - Set pcb=NULL but keep active=true
         *   - Metadata cleaned up later by connection_cleanup_stale()
         *   - Goal: Detect responses arriving after SCADA closes
         *
         * Problem with old design:
         *   - Multiple cleanup paths led to double-decrement bugs
         *   - Counter management became complex and error-prone
         *   - "Ticking time bomb" in v2.142: counter underflowed to 0
         *
         * NEW DESIGN (v2.143): "Immediate atomic cleanup"
         *   - connection_cleanup_atomic() handles ALL cleanup
         *   - Guard flag prevents double-cleanup
         *   - Counters managed in single location
         *   - If response arrives after close: metadata not found → can't send (connection already closed anyway)
         *
         * Trade-off: Slight simplification (responses after close are dropped)
         * Benefit: Eliminates entire class of double-decrement bugs
         * ═══════════════════════════════════════════════════════════════════════
         */

        /* v2.144: STEP 2 - Update shared state BEFORE sending notification
         * ═══════════════════════════════════════════════════════════════════════
         * RACE CONDITION FIX: Net1 PCB pool exhaustion
         *
         * Problem in v2.143:
         *   1. Send close notification to Net1
         *   2. Call connection_cleanup_atomic()
         *   3. connection_cleanup_atomic() sets meta->active = false
         *   4. connection_cleanup_atomic() calls update_shared_connection_state()
         *   5. Connection removed from Net0's peer_state
         *   6. Net1 receives notification, checks Net0's peer_state
         *   7. Connection NOT FOUND! Net1 thinks it's stale, ignores it
         *   8. Net1 PCB never closed → pool exhausted!
         *
         * Evidence from v2.143 test:
         *   - 116 close notifications sent
         *   - 91 ignored as "stale" (78%!)
         *   - Result: Net1 PCB pool exhausted (100/100)
         *
         * Fix: Update shared state BEFORE sending notification
         *   1. Call update_shared_connection_state() explicitly (connection visible)
         *   2. Send close notification to Net1
         *   3. Net1 receives notification, checks Net0's peer_state
         *   4. Connection FOUND! Net1 processes notification ✓
         *   5. Net1 closes its PCB ✓
         *   6. Later: connection_cleanup_atomic() removes from shared state
         * ═══════════════════════════════════════════════════════════════════════
         */
        update_shared_connection_state();  /* Ensure connection visible in peer_state */

        /* v2.153: STEP 3 - Keep BOTH sides alive to await PLC response
         * ═══════════════════════════════════════════════════════════════════════
         * CRITICAL FIX: Don't send close notification yet!
         *
         * Problem in v2.151-initial:
         *   1. SCADA closes → Net0 sets awaiting_response=true (keeps connection alive)
         *   2. But Net0 SENDS close notification to Net1 anyway
         *   3. Net1 receives notification → calls tcp_abort() → PLC connection closed!
         *   4. PLC response arrives but connection already closed → DROPPED!
         *
         * Solution: Keep BOTH sides alive until response arrives
         *   1. SCADA closes → set awaiting_response=true
         *   2. DON'T send close notification to Net1 yet
         *   3. Net0 keeps SCADA connection in CLOSE_WAIT (can still send)
         *   4. Net1 keeps PLC connection alive (no notification received)
         *   5. PLC response arrives → Net0 sends to SCADA
         *   6. AFTER sending → Net0 sends close notification to Net1
         *   7. Net1 closes PLC connection
         *   8. Net0 closes SCADA connection
         *
         * Key Insight: Close notification should be DEFERRED until response is sent
         * - Don't notify Net1 until we're done with the connection
         * - This keeps PLC connection alive for the response
         * - After response sent, we send close notification and both sides close
         *
         * TCP half-close on Net0:
         * - SCADA sent FIN → connection in CLOSE_WAIT state
         * - We can still send data (the PLC response)
         * - After sending, we call tcp_close() to send our FIN
         * ═══════════════════════════════════════════════════════════════════════
         */

        /* v2.189: Check if response already received (fix race condition)
         * ═══════════════════════════════════════════════════════════════════════
         * Race condition: SCADA closes AFTER response arrives but BEFORE processing
         *
         * Timeline:
         *   T+0ms: SCADA sends request
         *   T+55ms: PLC response arrives → response_received=true
         *   T+100ms: SCADA sends FIN (closes) → tcp_echo_recv(p=NULL) fires
         *   T+105ms: Check: response_received? YES! → Skip awaiting mode
         *
         * If we blindly set awaiting_response=true, we'd keep connection alive
         * even though response already arrived. This causes pool exhaustion!
         * ═══════════════════════════════════════════════════════════════════════
         */
        if (meta->response_received) {
            /* Response already arrived and processed - proceed with normal close */
            DEBUG("%s: SCADA closed AFTER response received - proceeding with normal close (session %u)\n",
                   COMPONENT_NAME, meta->session_id);
            DEBUG("%s:    → Response was already sent to SCADA\n", COMPONENT_NAME);
            DEBUG("%s:    → No need to wait - closing both sides now\n", COMPONENT_NAME);

            /* Send close notification to Net1 */
            update_shared_connection_state();  /* Ensure connection visible in peer_state */

            InboundDataport *dp = (InboundDataport *)inbound_dp;
            if (dp != NULL && !meta->close_notified) {
                uint32_t head = dp->close_queue.head;
                uint32_t slot = head & CONTROL_QUEUE_MASK;
                volatile struct control_notification *notif = &dp->close_queue.notifications[slot];

                notif->session_id = meta->session_id;
                notif->err_code = 0;  /* Normal close */
                notif->seq_num = head;
                __sync_synchronize();

                dp->close_queue.head = head + 1;

                /* v2.188-sentinel: Mark as close-only notification */
                dp->request_msg.payload_length = 0;
                dp->request_msg.metadata.session_id = meta->session_id;
                __sync_synchronize();

                inbound_ready_emit();
                meta->close_notified = true;

                DEBUG("%s:   [OK] Close notification queued for Net1 (session %u)\n",
                       COMPONENT_NAME, meta->session_id);
            }

            /* v2.197: CRITICAL FIX - Close lwIP PCB before cleanup!
             * ═══════════════════════════════════════════════════════════════════════════
             * BUG in v2.196: recv(p=NULL) path never called tcp_close(pcb)
             * - Sent close notification to Net1 ✅
             * - Enqueued metadata cleanup ✅
             * - BUT: Never told lwIP to close PCB ❌
             *
             * Result: lwIP PCB leaked forever
             * - Our metadata: cleaned up (connection_count accurate)
             * - lwIP's PCB: still active, consuming memory
             * - Symptom: connection_count=23 but lwIP out of pbufs!
             *
             * Evidence from v2.196 test:
             * - Net0: 335 [COUNT++], 311 [COUNT--] (counter accurate!)
             * - But: lwIP out of memory at packet #2182
             * - Root cause: Hundreds of unclosed lwIP PCBs
             *
             * Fix: Call tcp_close(pcb) like poll callback does (line 3367)
             * - Graceful close with FIN handshake
             * - If tcp_close() fails, fallback to tcp_abort()
             * - lwIP frees PCB and memory properly
             * ═══════════════════════════════════════════════════════════════════════════
             */
            err_t close_err = tcp_close(pcb);
            if (close_err != ERR_OK) {
                DEBUG_WARN("%s:   [WARN] tcp_close failed (err=%d) - using tcp_abort\n",
                       COMPONENT_NAME, close_err);
                tcp_abort(pcb);
                /* Note: tcp_abort() doesn't return - PCB freed immediately */
            }

            /* v2.209 COMPLETE: CRITICAL FIX - Don't call enqueue_cleanup()!
             * ═══════════════════════════════════════════════════════════════════════════
             * BUG in v2.193-v2.208: Called enqueue_cleanup() when response_received=true
             *
             * Problem:
             *   - response_received=true means tcp_write() was called
             *   - But tcp_write() only QUEUES data in lwIP, doesn't transmit!
             *   - enqueue_cleanup() → process_cleanup_queue() → meta->active=FALSE
             *   - Then netif_output() tries to transmit → "TX: No metadata" ❌
             *
             * Timeline of the bug:
             *   T+20ms: PLC response arrives, tcp_write() queued, response_received=TRUE
             *   T+21ms: SCADA FIN arrives, this path executes
             *   T+21ms: enqueue_cleanup() queued ❌ BUG!
             *   T+22ms: process_cleanup_queue() → meta->active=FALSE ❌
             *   T+25ms: netif_output() → metadata NOT FOUND ❌
             *
             * Fix: Use delayed cleanup instead
             *   - Set metadata_close_pending = TRUE
             *   - Let check_pending_cleanups() handle it after TX idle >1s
             *   - process_cleanup_queue() will see close_pending and skip
             *
             * Result: Metadata persists until transmission complete ✅
             * ═══════════════════════════════════════════════════════════════════════════
             */
            meta->metadata_close_pending = true;
            meta->close_timestamp = sys_now();

            DEBUG("%s:   [v2.209] Metadata marked for delayed cleanup (session %u)\n",
                   COMPONENT_NAME, meta->session_id);
            DEBUG("%s:   → Will cleanup after TX idle >1s (prevents TX metadata errors)\n",
                   COMPONENT_NAME);

            return ERR_OK;
        }

        /* Response NOT received yet - enter awaiting_response mode */
        meta->awaiting_response = true;
        /* DON'T set cleanup_in_progress - error callbacks must still work */

        DEBUG("%s: SCADA closed - keeping BOTH SIDES alive to await PLC response (session %u)\n",
               COMPONENT_NAME, meta->session_id);
        DEBUG("%s:    → Net0 connection in CLOSE_WAIT state (half-closed, can still send)\n", COMPONENT_NAME);
        DEBUG("%s:    → Net1 connection stays OPEN (no close notification sent yet)\n", COMPONENT_NAME);
        DEBUG("%s:    → Will send response, THEN close notification, THEN cleanup\n", COMPONENT_NAME);

        #if DEBUG_TRAFFIC
        DEBUG("%s:    → Active connections: %u (still counted until response sent)\n",
               COMPONENT_NAME, active_connections);
        #endif

        /* DON'T call connection_cleanup_atomic() - keep metadata alive!
         * DON'T call tcp_close() - keep PCB alive in CLOSE_WAIT state!
         * lwIP will keep the connection open so we can send the response */

        /* v2.209: Set delayed cleanup flag (fix pbuf leak)
         * ═══════════════════════════════════════════════════════════════════════
         * Mark that SCADA closed but keep metadata alive for TX path
         * Cleanup will occur in check_pending_cleanups() after TX completes
         * ═══════════════════════════════════════════════════════════════════════
         */
        meta->metadata_close_pending = true;
        meta->close_timestamp = sys_now();

        BREADCRUMB(9200);  /* v2.153: awaiting_response mode */

        /* Return ERR_OK to keep connection in CLOSE_WAIT state
         * Connection stays open for sending response */
        return ERR_OK;
    }

    /* v2.157: lwIP BEST PRACTICE - Never call pbuf_free() from recv callback!
     * lwIP's tcp_input() always frees the pbuf after callback returns (tcp_in.c:600)
     * Previous bug: We freed it here, then lwIP tried to free it again → double-free crash! */
    if (err != ERR_OK) {
        BREADCRUMB(9001);  /* Error path - lwIP will free pbuf */
        return err;  /* ✅ lwIP frees pbuf automatically */
    }

    /* v2.131: CRITICAL FIX - Unconditional stale callback check
     * Problem: lwIP calls tcp_echo_recv TWICE with same pbuf → double-free
     * Solution: Early metadata lookup + pbuf ref count guard at end */

    struct connection_metadata *meta_early = connection_lookup_by_pcb(pcb);

    /* v2.191: CRITICAL FIX - Fallback lookup by port if PCB not linked yet
     * ═══════════════════════════════════════════════════════════════════════
     * Race Condition: Data arrives before tcp_echo_accept links PCB
     *
     * Scenario:
     * 1. SYN arrives → netif_input creates metadata (assigns session_id)
     * 2. SCADA sends data IMMEDIATELY (before tcp_echo_accept is called)
     * 3. tcp_echo_recv fires
     * 4. Looks up metadata by PCB → NULL (PCB not linked yet!)
     * 5. Fallback to session_id=0 → "TX: No metadata" errors
     * 6. Connection leak
     *
     * Fix: If PCB lookup fails, try lookup by port numbers
     * - netif_input already created metadata with correct session_id
     * - Just need to find it by (src_port, dest_port) instead of PCB
     * - This handles the window between netif_input and connection_link_pcb
     * ═══════════════════════════════════════════════════════════════════════
     */
    if (meta_early == NULL) {
        /* PCB not linked yet - try lookup by port numbers */
        meta_early = connection_lookup_by_tuple(
            ntohl(ip4_addr_get_u32(&pcb->remote_ip)),
            ntohl(ip4_addr_get_u32(&pcb->local_ip)),
            pcb->remote_port,
            pcb->local_port
        );
        #if DEBUG_METADATA
        if (meta_early != NULL) {
            DEBUG("%s: [FALLBACK] tcp_echo_recv: PCB lookup failed, found metadata by ports %u->%u (session %u)\n",
                   COMPONENT_NAME, pcb->remote_port, pcb->local_port, meta_early->session_id);
        }
        #endif
    }

    /* Check if this is a stale callback after connection close */
    if (meta_early != NULL && meta_early->pcb == NULL) {
        BREADCRUMB(9010);  /* v2.131: Stale callback after close */
        return ERR_OK;  /* ✅ v2.157: lwIP frees pbuf automatically */
    }

    bool metadata_valid_for_processing = (meta_early != NULL && meta_early->active);

    /* ═══ Forward TCP data to ICS_Inbound (INBOUND path) ═══ */


    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (inbound_dp == NULL) {
        DEBUG_ERROR("%s: [ERR] FATAL: inbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        DEBUG("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        BREADCRUMB(9002);  /* Dataport NULL - lwIP will free pbuf */
        return ERR_MEM;  /* ✅ v2.157: lwIP frees pbuf automatically */
    }

    #if DEBUG_ENABLED_DEBUG
    DEBUG("%s: [OK] Dataport check: inbound_dp=%p (valid)\n", COMPONENT_NAME, (void*)inbound_dp);
    #endif

    /* Step 1: Create ICS message with metadata */
    ICS_Message *ics_msg = (ICS_Message *)inbound_dp;

    /* Step 2: Populate FrameMetadata (Phase 1: basic info, Phase 2: full header parsing) */
    #if DEBUG_ENABLED_DEBUG
    DEBUG("%s: About to memset ics_msg->metadata at %p (size=%zu)\n",
           COMPONENT_NAME, (void*)&ics_msg->metadata, sizeof(FrameMetadata));
    #endif
    memset(&ics_msg->metadata, 0, sizeof(FrameMetadata));

    /* Basic metadata - will be enhanced with full frame parsing */
    ics_msg->metadata.ethertype = 0x0800;  /* IPv4 */
    ics_msg->metadata.ip_protocol = 6;     /* TCP */
    ics_msg->metadata.is_ip = 1;
    ics_msg->metadata.is_tcp = 1;

    /* Extract IP addresses - need ORIGINAL destination IP from connection tracking */
    ics_msg->metadata.src_ip = ntohl(ip4_addr_get_u32(&pcb->remote_ip));

    /* CRITICAL: Look up original destination IP from connection tracking table
     * pcb->local_ip is the REWRITTEN IP (192.168.96.2) used by lwIP
     * We need the ORIGINAL PLC IP (e.g., 192.168.95.2) for Net1 to connect to
     *
     * v2.131: Use meta_early from above (already looked up at line 2191) */
    struct connection_metadata *meta = meta_early;  /* v2.131: Reuse early lookup */
    if (meta != NULL && meta->active) {
        /* v2.125: CRITICAL FIX - Check if PCB is closed before processing data
         * ═══════════════════════════════════════════════════════════════════════
         * Problem: Race condition between tcp_close() and queued callbacks
         *
         * Sequence causing double-free:
         * 1. tcp_echo_recv(p=NULL) called (SCADA closed)
         * 2. meta->pcb set to NULL (line 2075)
         * 3. tcp_close(pcb) called and frees pbufs
         * 4. THEN tcp_echo_recv(p=DATA) fires with already-freed pbuf
         * 5. Code processes data and calls pbuf_free(p) at line 2338
         * 6. CRASH: pbuf->ref already 0 (already freed by tcp_close)
         *
         * Solution: Check if meta->pcb is NULL (connection closed)
         * If closed, the data is stale - free pbuf and return early
         * ═══════════════════════════════════════════════════════════════════════
         */
        if (meta->pcb == NULL) {
            DEBUG_WARN("%s: [WARN]  tcp_echo_recv: Connection already closed (meta->pcb=NULL), dropping %u bytes\n",
                   COMPONENT_NAME, p->len);
            /* Connection was closed - this data is stale */
            return ERR_OK;  /* ✅ v2.157: lwIP frees pbuf automatically */
        }

        /* Additional safety: check if callback pcb matches metadata pcb */
        if (meta->pcb != pcb) {
            DEBUG_WARN("%s: [WARN]  tcp_echo_recv: PCB mismatch (meta->pcb=%p, callback pcb=%p), dropping %u bytes\n",
                   COMPONENT_NAME, (void*)meta->pcb, (void*)pcb, p->len);
            return ERR_OK;  /* ✅ v2.157: lwIP frees pbuf automatically */
        }

        /* Use original destination IP and session ID from packet metadata */
        ics_msg->metadata.session_id = meta->session_id;  /* v2.150: Pass session ID to Net1 */
        ics_msg->metadata.dst_ip = meta->original_dest_ip;
        #if DEBUG_METADATA
        DEBUG("%s: [FIND] Lookup: Found metadata - using original dest IP %u.%u.%u.%u (session %u)\n",
               COMPONENT_NAME,
               (meta->original_dest_ip >> 24) & 0xFF,
               (meta->original_dest_ip >> 16) & 0xFF,
               (meta->original_dest_ip >> 8) & 0xFF,
               meta->original_dest_ip & 0xFF,
               meta->session_id);
        #endif
    } else {
        /* Fallback: use rewritten IP if lookup fails */
        ics_msg->metadata.session_id = 0;  /* v2.150: No session ID available */
        ics_msg->metadata.dst_ip = ntohl(ip4_addr_get_u32(&pcb->local_ip));
        #if DEBUG_METADATA
        DEBUG_WARN("%s: [WARN]  Lookup: No metadata found - using rewritten IP (WRONG!)\n", COMPONENT_NAME);
        #endif
    }

    ics_msg->metadata.src_port = pcb->remote_port;
    ics_msg->metadata.dst_port = pcb->local_port;
    ics_msg->metadata.payload_offset = 0;  /* TCP payload directly */
    ics_msg->metadata.payload_length = (p->len < MAX_PAYLOAD_SIZE) ? p->len : MAX_PAYLOAD_SIZE;

    /* Step 3: Copy TCP payload */
    ics_msg->payload_length = ics_msg->metadata.payload_length;
    memcpy(ics_msg->payload, p->payload, ics_msg->payload_length);

    #if DEBUG_TRAFFIC
    DEBUG("%s: INBOUND: Forwarding %u bytes to ICS_Inbound (proto=TCP, src_port=%u, dst_port=%u)\n",
           COMPONENT_NAME, ics_msg->payload_length,
           ics_msg->metadata.src_port, ics_msg->metadata.dst_port);
    #endif

    #if DEBUG_ENABLED_DEBUG
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
    #endif

    #if DEBUG_ENABLED_DEBUG
    DEBUG("   [OK] ICS message prepared in shared memory (inbound_dp)\n");
    DEBUG("   Action: Signaling ICS_Inbound component via inbound_ready_emit()\n");
    #endif

    /* v2.92: Mark that we're waiting for a response - don't cleanup metadata yet!
     * This prevents connection_cleanup_stale() from removing metadata while
     * the request is in flight and we're waiting for PLC response.
     * Note: meta was already looked up earlier (line 1938) */
    if (meta != NULL) {
        meta->awaiting_response = true;
    }

    /* Step 4: Signal ICS_Inbound that message is ready */
    inbound_ready_emit();

    #if DEBUG_ENABLED_DEBUG
    DEBUG("   [OK] Signal sent to ICS_Inbound - message handoff complete\n");
    DEBUG("   [MSG #%u now in ICS pipeline - waiting for processing]\n\n", msg_id);
    #endif

    /* Tell TCP we've processed the data */
    tcp_recved(pcb, p->len);

    /* v2.111: Check for pending outbound data and send from lwIP callback context
     * ══════════════════════════════════════════════════════════════════════════
     * After processing inbound request, check if there's a queued PLC response
     * that needs to be sent. Since we're in tcp_echo_recv (lwIP callback),
     * it's SAFE to call tcp_write here (no race with timers).
     * ══════════════════════════════════════════════════════════════════════════
     */
    if (meta != NULL && meta->has_pending_outbound && meta->pending_outbound_data != NULL) {
        DEBUG("%s: [CALLBACK] tcp_echo_recv: Found pending outbound (%u bytes), sending NOW\n",
               COMPONENT_NAME, meta->pending_outbound_len);

        /* Safe to call tcp_write from lwIP callback! */
        err_t write_err = tcp_write(pcb, meta->pending_outbound_data,
                                    meta->pending_outbound_len, TCP_WRITE_FLAG_COPY);

        if (write_err == ERR_OK) {
            DEBUG("%s: [OK] tcp_echo_recv: Sent pending outbound %u bytes\n",
                   COMPONENT_NAME, meta->pending_outbound_len);

            /* Clean up */
            free(meta->pending_outbound_data);
            meta->pending_outbound_data = NULL;
            meta->pending_outbound_len = 0;
            meta->has_pending_outbound = false;
            meta->awaiting_response = false;

            /* v2.113: CRITICAL - Check PCB is still valid before tcp_output()
             * Callbacks can fire AFTER connection is closed. If meta->pcb is NULL
             * or doesn't match callback pcb, the connection was closed and PCB is freed.
             * Calling tcp_output() on freed PCB → NULL pointer crash! */
            if (meta->pcb == pcb && meta->pcb != NULL) {
                tcp_output(pcb);
            } else {
                DEBUG_WARN("%s: [WARN]  tcp_echo_recv: PCB stale (meta->pcb=%p, callback pcb=%p) - skip tcp_output\n",
                       COMPONENT_NAME, meta->pcb, pcb);
            }

            /* v2.158: Set close_pending flag (lwIP best practice - deferred close)
             * ══════════════════════════════════════════════════════════════════════════
             * Response has been sent to SCADA. Now we need to close the connection.
             *
             * CRITICAL: Cannot call tcp_close() from recv callback (Rule 5 violation!)
             *
             * Solution: Set close_pending flag, let poll callback handle it
             *   1. Set flag here (safe - just setting boolean)
             *   2. Return ERR_OK from this callback (normal flow)
             *   3. Poll callback fires later (~2 seconds with interval=4)
             *   4. Poll callback detects flag → sends close notification → returns ERR_ABRT
             *   5. lwIP calls tcp_abort() internally → connection closed safely
             *
             * This follows lwIP Rule 6 Solution A: close_pending flag pattern
             * ══════════════════════════════════════════════════════════════════════════
             */
            meta->close_pending = true;
            meta->close_timestamp = sys_now();  /* v2.181: Record timestamp for latency measurement */

            DEBUG("%s: Response sent - close_pending=true (poll callback will close)\n",
                   COMPONENT_NAME);
        } else {
            DEBUG_WARN("%s: [WARN]  tcp_echo_recv: tcp_write failed (%d), will retry later\n",
                   COMPONENT_NAME, write_err);
        }
    }

    /* v2.157: lwIP BEST PRACTICE - NEVER call pbuf_free() from recv callback!
     *
     * CRITICAL FIX: This was the root cause of pbuf double-free crashes!
     *
     * What was wrong (v2.131-v2.156):
     * --------------------------------
     * 1. We called pbuf_free(p) here (manually freeing pbuf)
     * 2. Callback returned ERR_OK to lwIP
     * 3. lwIP's tcp_input() ALSO called pbuf_free(inseg.p) at line 600
     * 4. Result: DOUBLE-FREE → Assertion "p != NULL" failed at line 732 in pbuf.c
     *
     * Why ref count check didn't help:
     * --------------------------------
     * - Checking p->ref > 0 before pbuf_free() does NOT prevent double-free
     * - lwIP ALWAYS frees the pbuf in tcp_input() regardless of ref count
     * - Application code should NEVER call pbuf_free() on callback pbufs
     *
     * lwIP ownership model (from tcp_in.c:596-600):
     * ----------------------------------------------
     * if (inseg.p != NULL) {
     *     pbuf_free(inseg.p);  ← lwIP owns this, NOT application!
     *     inseg.p = NULL;
     * }
     *
     * Correct protocol:
     * -----------------
     * - Application processes pbuf data
     * - Application returns status code (ERR_OK, ERR_ABRT, etc.)
     * - lwIP handles ALL memory management (allocation AND deallocation)
     *
     * Fix (v2.157):
     * -------------
     * Remove ALL pbuf_free() calls from recv callback. Let lwIP handle it.
     */
    BREADCRUMB(9003);  /* Recv callback complete - lwIP will free pbuf */

    return ERR_OK;  /* ✅ lwIP frees pbuf automatically in tcp_input() */
}

/* v2.158: TCP poll callback for deferred connection cleanup
 * ═══════════════════════════════════════════════════════════════════════════
 * Purpose: Handle close_pending flag set by recv callback after sending response
 *
 * Why we need this:
 * - Cannot call tcp_close() from recv callback (lwIP Rule 5 violation)
 * - Poll callback runs in safe lwIP context
 * - Fires periodically (~2 seconds with interval=4 in tcp_echo_accept)
 *
 * Pattern (lwIP Rule 6, Solution A):
 * 1. Recv callback sends response → sets close_pending = true
 * 2. This poll callback detects flag
 * 3. Sends close notification to Net1 (so Net1 closes PLC connection)
 * 4. Returns ERR_ABRT → lwIP handles tcp_abort() internally
 * ═══════════════════════════════════════════════════════════════════════════
 */
static err_t tcp_echo_poll(void *arg, struct tcp_pcb *pcb)
{
    struct connection_metadata *meta = (struct connection_metadata *)arg;

    if (meta == NULL) {
        /* No metadata - shouldn't happen but safe to continue */
        return ERR_OK;
    }

    /* v2.166: Flush pending outbound data (deferred from event handler)
     * ═══════════════════════════════════════════════════════════════════════════
     * This handles data queued by outbound_ready_handle() (CAmkES event handler).
     *
     * Why deferred:
     * - outbound_ready_handle is interrupt-like (can fire during lwIP processing)
     * - Calling tcp_output() from event handler causes reentrancy → CRASH
     * - Solution: Event handler sets flag, poll callback flushes safely
     *
     * Safety:
     * - Poll callback runs in lwIP timer context (no reentrancy)
     * - tcp_write() already queued data, we just need to flush
     * - This follows lwIP NO_SYS=1 threading model
     * ═══════════════════════════════════════════════════════════════════════════
     */
    if (meta->has_pending_outbound) {
        #if DEBUG_TRAFFIC
        DEBUG("%s: Poll callback flushing pending outbound data (session %u)\n",
               COMPONENT_NAME, meta->session_id);
        #endif

        /* Flush lwIP output buffer - SAFE in poll callback context */
        tcp_output(pcb);

        /* Clear flag */
        meta->has_pending_outbound = false;

        #if DEBUG_TRAFFIC
        DEBUG("%s:   [OK] Outbound data flushed by poll callback\n", COMPONENT_NAME);
        #endif
    }

    /* v2.180: Deferred metadata cleanup (fix metadata/lwIP packet race)
     * ═══════════════════════════════════════════════════════════════════════════
     * Check if close was requested
     *
     * KEY CHANGE: We NO LONGER cleanup metadata here!
     * - Old behavior (v2.179): connection_cleanup_atomic() + return ERR_ABRT
     * - Problem: lwIP still sends FIN/RST after cleanup → no metadata for TX
     * - Result: 1,170 metadata failures, packets sent with wrong source IP
     *
     * New behavior (v2.180): tcp_close() + mark closing=true
     * - Keep metadata alive while lwIP completes close handshake
     * - Metadata available for ALL lwIP packets (FIN, ACK, retransmissions)
     * - Error callback will cleanup when lwIP frees PCB
     * ═══════════════════════════════════════════════════════════════════════════
     */
    if (meta->close_pending) {
        DEBUG("%s: Poll callback detected close_pending (session %u)\n",
               COMPONENT_NAME, meta->session_id);

        /* Send close notification to Net1 BEFORE closing
         * This tells Net1 to close its PLC connection too */
        InboundDataport *in_dp = (InboundDataport *)inbound_dp;
        if (in_dp != NULL && !meta->close_notified) {
            uint32_t head = in_dp->close_queue.head;
            uint32_t slot = head & CONTROL_QUEUE_MASK;
            volatile struct control_notification *notif = &in_dp->close_queue.notifications[slot];

            /* Fill notification */
            notif->session_id = meta->session_id;
            notif->err_code = 0;  /* Normal close (not error) */
            notif->seq_num = head;
            __sync_synchronize();  /* Memory barrier - ensure write visible to Net1 */

            /* Commit to queue */
            in_dp->close_queue.head = head + 1;

            /* v2.188-sentinel: Mark as close-only notification
             * Set payload_length = 0 to indicate this is NOT a request
             * ICS_Inbound will forward this, Net1 will see sentinel and skip request processing
             */
            in_dp->request_msg.payload_length = 0;  /* Sentinel: close-only, no payload */
            in_dp->request_msg.metadata.session_id = meta->session_id;
            __sync_synchronize();  /* Memory barrier - ensure sentinel visible before signal */

            inbound_ready_emit();  /* Signal Net1 to process queue */
            meta->close_notified = true;  /* v2.219: Mark notification sent to prevent duplicates */

            DEBUG("%s:   [OK] Close notification queued for Net1 (session %u)\n",
                   COMPONENT_NAME, meta->session_id);
        } else {
            DEBUG_WARN("%s:   [WARN] inbound_dp NULL - cannot notify Net1\n", COMPONENT_NAME);
        }

        /* v2.180: Initiate close but DON'T cleanup metadata yet!
         * ═══════════════════════════════════════════════════════════════════════════
         * Old (v2.179): connection_cleanup_atomic() → metadata gone → TX fails
         * New (v2.180): tcp_close() → metadata stays → TX succeeds → error callback cleans up
         * ═══════════════════════════════════════════════════════════════════════════
         */
        err_t close_err = tcp_close(pcb);
        if (close_err != ERR_OK) {
            DEBUG_WARN("%s:   [WARN] tcp_close failed with err=%d - using tcp_abort as fallback\n",
                   COMPONENT_NAME, close_err);

            /* Fallback: NULL callbacks first */
            tcp_arg(pcb, NULL);
            tcp_recv(pcb, NULL);
            tcp_sent(pcb, NULL);
            tcp_err(pcb, NULL);
            tcp_poll(pcb, NULL, 0);

            /* Force abort */
            tcp_abort(pcb);

            /* v2.193: Enqueue cleanup - main loop will handle counter decrement */
            enqueue_cleanup(meta->session_id);

            DEBUG("%s:   [OK] Connection aborted - cleanup enqueued (session %u)\n",
                   COMPONENT_NAME, meta->session_id);

            return ERR_ABRT;
        }

        /* v2.192: SIMPLIFIED - Cleanup immediately instead of waiting for error callback
         * ═══════════════════════════════════════════════════════════════════════════
         * Old design (v2.180-v2.191): Set closing=true, wait for error callback to cleanup
         * Problem: Error callback only fires 13.6% of the time (44/323 connections)
         * Result: 279 leaked connections (never cleaned up)
         *
         * Root cause analysis:
         * - Poll callback fires: 323 times ✅
         * - tcp_close() succeeds: ~323 times ✅
         * - Error callback fires: 44 times ❌ (only 13.6%!)
         * - Missing cleanups: 279 connections leak
         *
         * Why error callback doesn't fire:
         * - lwIP keeps PCB in TIME_WAIT/CLOSE_WAIT
         * - PCB stuck in retransmission
         * - PCB never freed → error callback never called
         *
         * New design (v2.192): Cleanup immediately after tcp_close()
         * - Close notification already sent to Net1 ✅
         * - No more PLC responses coming ✅
         * - FIN/ACK packets use PCB's IP (no metadata needed) ✅
         * - Safe to cleanup NOW instead of waiting!
         *
         * Benefits:
         * - 100% cleanup rate (poll callback always fires)
         * - No connection leaks
         * - Simpler code (no waiting for callbacks)
         * ═══════════════════════════════════════════════════════════════════════════
         */
        meta->close_pending = false;
        meta->pcb = NULL;  /* PCB will be freed by lwIP */

        /* Calculate total latency from close_pending to cleanup */
        uint32_t pending_duration = sys_now() - meta->close_timestamp;

        /* v2.193: Enqueue cleanup (main loop will process) */
        enqueue_cleanup(meta->session_id);

        DEBUG("%s:   [OK] Close initiated and cleanup enqueued (session %u, pending for %ums)\n",
               COMPONENT_NAME, meta->session_id, pending_duration);

        /* Return ERR_OK - lwIP will complete close handshake */
        return ERR_OK;
    }

    /* Normal poll - no action needed */
    return ERR_OK;
}

static err_t tcp_echo_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    #if DEBUG_TRAFFIC
    DEBUG("\n%s: ========================================\n", COMPONENT_NAME);
    DEBUG("%s: [TARGET] TCP ACCEPT CALLBACK TRIGGERED!\n", COMPONENT_NAME);
    DEBUG("%s:    arg=%p, newpcb=%p, err=%d\n", COMPONENT_NAME, arg, newpcb, err);
    #endif

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
            DEBUG("%s:    → Connection table state:\n", COMPONENT_NAME);
            connection_print_stats();
        }
        DEBUG("%s: ========================================\n\n", COMPONENT_NAME);
        return err != ERR_OK ? err : ERR_VAL;
    }

    /* v2.231: REJECT non-Modbus TCP connections immediately
     * Problem: Port 62977 and other non-Modbus ports cause pbuf leaks
     * Solution: Check ports and abort connection before creating metadata
     * - Prevents pbufs from accumulating in unwanted connections
     * - lwIP will properly clean up when we call tcp_abort()
     */
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

    /* v2.100: CRITICAL - Hard connection limit to prevent orphaned connections
     *
     * Problem (earlier versions):
     * - Net0 accepted unlimited SCADA connections (256 PCBs available)
     * - Net1 tried to create corresponding PLC connections
     * - PLC has limited connection capacity
     * - Result: Orphaned Net0 connections with no PLC connection!
     *
     * Solution: Reject SCADA connections if we're at capacity
     * - Both Net0 and Net1 set MEMP_NUM_TCP_PCB (v2.182: reverted to 100 for leak testing)
     * - Reject at limit-5 (leave 5-connection buffer for safety)
     * - Ensures 1:1 mapping between SCADA and PLC connections
     * - Connection symmetry (v2.117) allows safely scaling
     */
    #define MAX_SAFE_CONNECTIONS 95  /* v2.182: PCB limit is 100, stay 5 under */

    if (active_connections >= MAX_SAFE_CONNECTIONS) {
        BREADCRUMB(9210);  /* v2.140: Connection limit reached, rejecting */
        DEBUG_ERROR("%s: [ERR] CONNECTION LIMIT REACHED (%u/%u) - REJECTING SCADA connection\n",
               COMPONENT_NAME, active_connections, MAX_SAFE_CONNECTIONS);
        DEBUG("%s:    → This prevents orphaned connections when capacity limit reached\n",
               COMPONENT_NAME);
        DEBUG("%s:    → SCADA IP: %u.%u.%u.%u:%u\n",
               COMPONENT_NAME,
               ip4_addr1(&newpcb->remote_ip), ip4_addr2(&newpcb->remote_ip),
               ip4_addr3(&newpcb->remote_ip), ip4_addr4(&newpcb->remote_ip),
               newpcb->remote_port);

        tcp_abort(newpcb);  /* Send RST to SCADA */
        BREADCRUMB(9211);  /* v2.140: Rejected connection aborted */
        return ERR_ABRT;
    }

    BREADCRUMB(9212);  /* v2.140: Connection accepted */
    active_connections++;
    total_connections_created++;

    #if DEBUG_TRAFFIC
    DEBUG_INFO("%s: [OK] TCP connection ACCEPTED from %u.%u.%u.%u:%u\n",
           COMPONENT_NAME,
           ip4_addr1(&newpcb->remote_ip), ip4_addr2(&newpcb->remote_ip),
           ip4_addr3(&newpcb->remote_ip), ip4_addr4(&newpcb->remote_ip), newpcb->remote_port);
    DEBUG("%s:    → Local port: %u\n", COMPONENT_NAME, newpcb->local_port);
    DEBUG("%s:    → PCB address: %p\n", COMPONENT_NAME, newpcb);
    DEBUG("%s:    → PCB state: %d\n", COMPONENT_NAME, newpcb->state);
    DEBUG("%s:    → Active connections: %u | Total created: %u | Total closed: %u\n",
           COMPONENT_NAME, active_connections, total_connections_created, total_connections_closed);
    DEBUG("%s: ========================================\n\n", COMPONENT_NAME);
    #endif

    tcp_setprio(newpcb, TCP_PRIO_MIN);

    /* Link PCB to connection metadata for original IP restoration
     * This associates the PCB with the metadata entry stored during RX processing
     * MUST happen BEFORE tcp_arg so we can look up the metadata */
    connection_link_pcb(newpcb, newpcb->remote_port, newpcb->local_port);

    /* v2.146: Pass metadata to tcp_arg so error callback can send close notifications
     * Look up metadata after connection_link_pcb has linked the PCB */
    struct connection_metadata *meta = connection_lookup_by_pcb(newpcb);

    /* Register callbacks with metadata as argument */
    tcp_arg(newpcb, meta);
    tcp_recv(newpcb, tcp_echo_recv);
    tcp_err(newpcb, tcp_echo_err);  /* Register error callback for connection cleanup */
    tcp_sent(newpcb, tcp_echo_sent);  /* v2.111: Register sent callback for pending outbound data */

    /* v2.158: Register poll callback for deferred connection cleanup
     * ══════════════════════════════════════════════════════════════════════════
     * Purpose: Handle close_pending flag set by recv callback after sending response
     *
     * Interval = 4 means poll fires every 4 × 500ms = 2 seconds
     * - Frequent enough to close connections promptly after response sent
     * - Infrequent enough to minimize overhead
     *
     * This callback detects meta->close_pending flag and safely closes connection
     * following lwIP Rule 6 Solution A (deferred close pattern)
     * ══════════════════════════════════════════════════════════════════════════
     */
    tcp_poll(newpcb, tcp_echo_poll, 4);

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
 * OUTBOUND PATH: ICS_Outbound → External Network (TCP Client)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* TCP client connection state for OUTBOUND forwarding */
/* v2.106: DEAD CODE REMOVAL
 * The tcp_outbound_client_state and associated callbacks below are NEVER USED in Net0.
 * Net0 is a TCP SERVER that receives connections from SCADA.
 * Only Net1 uses outbound TCP client connections.
 * Keeping the struct definition and callbacks for reference, but removed the unused global variable.
 */

struct tcp_outbound_client_state {
    struct tcp_pcb *pcb;
    uint8_t *payload_data;
    uint16_t payload_len;
    uint16_t bytes_sent;
    bool active;
};

/* v2.106: REMOVED - This global was never used
 * static struct tcp_outbound_client_state outbound_tcp_client = {0}; */

/*
 * TCP client callbacks for OUTBOUND path (DEAD CODE - never called)
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

    /* v2.102: CRITICAL FIX - Check to_send > 0 before calling tcp_write()
     * Same issue as Net1 - missing check in OUTBOUND path
     *
     * Bug: Connection established but remote window not yet advertised
     * - tcp_sndbuf(pcb) returns 0, to_send = 0
     * - tcp_write(pcb, data, 0) creates NULL pbuf
     * - Later access to pbuf->len causes crash at 0x383e0
     *
     * Fix: Only call tcp_write() if to_send > 0
     */
    if (to_send == 0) {
        /* Send buffer full - defer transmission until sent callback */
        DEBUG_WARN("%s: [WARN]  OUTBOUND: Send buffer full (sndbuf=%u), deferring transmission of %u bytes\n",
               COMPONENT_NAME, tcp_sndbuf(pcb), state->payload_len);
        DEBUG("%s:    → Will retry in tcp_sent callback when buffer available\n", COMPONENT_NAME);

        /* Keep state active - sent callback will retry when buffer space available */
        state->bytes_sent = 0;
        return ERR_OK;
    }

    err = tcp_write(pcb, state->payload_data, to_send, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        DEBUG("%s: OUTBOUND: tcp_write failed: %d\n", COMPONENT_NAME, err);
        tcp_close(pcb);
        state->active = false;
        state->pcb = NULL;
        return err;
    }

    state->bytes_sent = to_send;

    /* Trigger transmission */
    tcp_output(pcb);

    DEBUG("%s: OUTBOUND: Sent initial %u bytes\n", COMPONENT_NAME, to_send);

    return ERR_OK;
}

/*
 * OUTBOUND notification handler - called when ICS_Outbound has PLC response
 * Looks up existing TCP connection and sends response back to SCADA
 */
void outbound_ready_handle(void)
{
    /* v2.104: Lightweight breadcrumb debugging
     * BREADCRUMB 3000: Entry - outbound_ready_handle() called by CAmkES
     * BREADCRUMB 3050: Handler processing complete
     * If these breadcrumbs appear, CAmkES event delivery is working */
    BREADCRUMB(3000);  /* Entry: ICS_Outbound notification received */


    #if DEBUG_TRAFFIC
    DEBUG("%s: ╔═══════════════════════════════════════════════════════════╗\n", COMPONENT_NAME);
    DEBUG("%s: ║  OUTBOUND: Received PLC response from ICS_Outbound       ║\n", COMPONENT_NAME);
    #endif

    BREADCRUMB(3001);  /* Checking dataport */

    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (outbound_dp == NULL) {
        BREADCRUMB(3002);  /* NULL dataport */
        DEBUG_ERROR("%s: [ERR] FATAL: outbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        DEBUG("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        DEBUG("%s: ╚═══════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
        return;
    }

    #if DEBUG_TRAFFIC
    DEBUG("%s: [OK] Dataport check: outbound_dp=%p (valid)\n", COMPONENT_NAME, (void*)outbound_dp);
    #endif

    BREADCRUMB(3003);  /* Reading ICS message */

    /* v2.153: Process error notification queue from Net1
     * ═══════════════════════════════════════════════════════════════════════
     * PLC errors (RST, timeout, etc.) are queued by Net1 in error_queue.
     * Process all queued errors and close corresponding SCADA connections.
     *
     * This replaces the old single-message error notification (v2.152).
     * Queue prevents:
     * - Race condition where error overwrites response data
     * - RST flood attacks (deduplication at producer side)
     * - Message loss during bursts (128-slot queue)
     * ═══════════════════════════════════════════════════════════════════════
     */
    OutboundDataport *dp = (OutboundDataport *)outbound_dp;
    static uint32_t error_queue_tail = 0;  /* Consumer state (local, never shared) */

    uint32_t error_queue_head = dp->error_queue.head;

    /* v2.159: CRITICAL FIX - Memory barrier for cache coherency
     * ═══════════════════════════════════════════════════════════════════════
     * Problem: Same as close_queue bug in v2.158 (Net1 side)
     *
     * Root Cause: Cache coherency issue with dataport reads
     * - Net1 writes error_queue.head with memory barrier (in control_queue_enqueue)
     * - Net0 reads error_queue.head WITHOUT memory barrier
     * - Net0 CPU cache may have stale value (reads old head value)
     * - Loop condition may be false when it should be true → missed notifications
     * - Result: Error notifications not processed, connections not closed on PLC errors
     *
     * Solution: Add memory barrier AFTER reading from shared dataport
     * - Forces CPU to invalidate cache line
     * - Ensures we read the actual value written by Net1
     * - Loop condition now correct → processes all error notifications
     *
     * Why external barrier at line 4390 is insufficient:
     * - Barrier is before function call (in main loop)
     * - Read happens inside function (compiler may reorder)
     * - Barrier must be immediately after the read for correct ordering
     * - Function boundary creates race window
     *
     * This is symmetric to Net1's write barrier in control_queue_enqueue():
     *   Net1: write notification → __sync_synchronize() → update head → emit signal
     *   Net0: wait signal → read head → __sync_synchronize() → process notification
     * ═══════════════════════════════════════════════════════════════════════
     */
    __sync_synchronize();  /* Force cache invalidation - read fresh value from Net1 */

    /* Check for consumer falling too far behind (queue wraparound) */
    if (error_queue_head - error_queue_tail > CONTROL_QUEUE_SIZE) {
        DEBUG_WARN("%s: [WARN] Error queue overflow! Missed %u notifications (too slow)\n",
               COMPONENT_NAME, error_queue_head - error_queue_tail - CONTROL_QUEUE_SIZE);
        error_queue_tail = error_queue_head - CONTROL_QUEUE_SIZE;  /* Skip to oldest */
    }

    /* Process all queued error notifications */
    while (error_queue_tail < error_queue_head) {
        uint32_t slot = error_queue_tail & CONTROL_QUEUE_MASK;
        volatile struct control_notification *notif = &dp->error_queue.notifications[slot];

        /* Verify sequence (detect wraparound overwrites) */
        if (notif->seq_num == error_queue_tail && notif->session_id != 0) {
            DEBUG("%s: Processing error notification: session %u, err=%d\n",
                   COMPONENT_NAME, notif->session_id, notif->err_code);

            /* Lookup connection by session_id (handles awaiting_response case) */
            struct connection_metadata *meta = connection_lookup_by_session_id(notif->session_id);

            if (meta != NULL && meta->active && meta->pcb != NULL) {
                struct tcp_pcb *pcb = meta->pcb;

                DEBUG("%s:   → Closing SCADA connection (session %u, PCB=%p)\n",
                       COMPONENT_NAME, notif->session_id, (void*)pcb);

                /* Mark as not awaiting response */
                meta->awaiting_response = false;

                /* v2.178: FIXED - Only mirror PLC's RST, not Net1's internal cleanup
                 * ═══════════════════════════════════════════════════════════════════════
                 * CRITICAL BUG FIX (v2.177 → v2.178):
                 *
                 * v2.177 BUG: Treated ERR_ABRT as "PLC sent RST" → Excessive RST to SCADA
                 *
                 * Error code semantics:
                 * - ERR_RST (-14): **PLC sent RST packet** → Mirror with RST to SCADA ✅
                 * - ERR_CLSD (-15): PLC sent FIN packet → Mirror with FIN to SCADA ✅
                 * - ERR_ABRT (-13): **Net1 called tcp_abort()** (internal cleanup, NOT from PLC!)
                 *                   → Use graceful close to SCADA ✅
                 *
                 * Example scenario that exposed the bug:
                 * 1. SCADA sends FIN → Net0 sends ERR_CLSD to Net1
                 * 2. Net1 tries tcp_close() → fails (memory) → fallback tcp_abort()
                 * 3. tcp_abort() triggers error callback → Net1 sends ERR_ABRT to Net0
                 * 4. v2.177: Net0 incorrectly thinks "PLC sent RST" → Sends RST to SCADA ❌
                 * 5. v2.178: Net0 recognizes ERR_ABRT is internal → Graceful close to SCADA ✅
                 *
                 * Result: Only send RST to SCADA when PLC **actually** sends RST
                 * ═══════════════════════════════════════════════════════════════════════
                 */

                BREADCRUMB(9220);  /* Closing SCADA connection from error queue */

                if (notif->err_code == ERR_RST) {
                    /* PLC sent RST packet → Mirror behavior by sending RST to SCADA */
                    DEBUG("%s:   → PLC sent RST, sending RST to SCADA (tcp_abort)\n",
                           COMPONENT_NAME);
                    BREADCRUMB(9221);  /* tcp_abort path */
                    tcp_abort(pcb);  /* Sends RST, frees PCB immediately */
                    BREADCRUMB(9222);  /* tcp_abort completed */
                } else {
                    /* PLC sent FIN (ERR_CLSD), Net1 cleanup (ERR_ABRT), or unknown → Graceful close */
                    const char *reason = (notif->err_code == ERR_CLSD) ? "PLC sent FIN" :
                                       (notif->err_code == ERR_ABRT) ? "Net1 internal cleanup" :
                                       "Unknown error";
                    DEBUG("%s:   → %s, sending FIN to SCADA (tcp_close)\n",
                           COMPONENT_NAME, reason);
                    BREADCRUMB(9223);  /* tcp_close path */
                    err_t close_err = tcp_close(pcb);
                    if (close_err != ERR_OK) {
                        BREADCRUMB(9224);  /* tcp_close failed */
                        DEBUG("%s:   → tcp_close() failed (err=%d), forcing tcp_abort()\n",
                               COMPONENT_NAME, close_err);
                        tcp_abort(pcb);  /* Fallback to abort */
                        BREADCRUMB(9225);  /* tcp_abort fallback completed */
                    } else {
                        BREADCRUMB(9226);  /* tcp_close succeeded */
                    }
                }

                /* v2.193: Enqueue cleanup */
                enqueue_cleanup(meta->session_id);
                BREADCRUMB(9227);  /* Cleanup enqueued */
            } else {
                DEBUG("%s:   → Connection already closed (session %u)\n",
                       COMPONENT_NAME, notif->session_id);
            }
        }

        error_queue_tail++;  /* Move to next notification */
    }

    /* Now process response data (if any) */
    ICS_Message *ics_msg = &dp->response_msg;

    #if DEBUG_TRAFFIC
    DEBUG("%s: ╚═══════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
    #endif

    /* Validate message */
    if (ics_msg->payload_length > MAX_PAYLOAD_SIZE) {
        BREADCRUMB(3004);  /* Invalid payload size */
        DEBUG("%s: OUTBOUND: Invalid payload length %u (max %u)\n",
               COMPONENT_NAME, ics_msg->payload_length, MAX_PAYLOAD_SIZE);
        #if DEBUG_ENABLED_DEBUG
        DEBUG("   ✗ [MSG #%u] DROPPED - invalid payload size\n\n", msg_id);
        #endif
        return;
    }

    BREADCRUMB(3005);  /* Payload size valid */

    /* v2.169: Handle connection pool exhaustion error from Net1
     * ═══════════════════════════════════════════════════════════════════════════
     * When Net1 cannot create TCP PCB (pool exhausted), it sends error with:
     * - payload_length = 0
     * - payload_offset = 0xFFFF (error marker)
     * - metadata contains SCADA connection 5-tuple
     *
     * Action: Send RST to SCADA immediately (industry best practice)
     * - SCADA gets ECONNREFUSED (0-100ms) instead of timeout (30-120s)
     * - Follows industry standard for connection limit handling
     *
     * See: research-docs/tcp-connection-exhaustion-industry-practices-research.md
     * ═══════════════════════════════════════════════════════════════════════════
     */
    if (ics_msg->payload_length == 0 && ics_msg->metadata.payload_offset == 0xFFFF) {
        DEBUG("%s: [CRITICAL] Net1 connection pool exhausted - rejecting SCADA connection\n", COMPONENT_NAME);
        DEBUG("%s:    → Error notification for session %u (%u.%u.%u.%u:%u → 502)\n",
               COMPONENT_NAME,
               ics_msg->metadata.session_id,
               (ics_msg->metadata.src_ip >> 24) & 0xFF,
               (ics_msg->metadata.src_ip >> 16) & 0xFF,
               (ics_msg->metadata.src_ip >> 8) & 0xFF,
               ics_msg->metadata.src_ip & 0xFF,
               ics_msg->metadata.src_port);

        /* v2.172 CRITICAL FIX: Lookup by session_id instead of IP/port
         * ═══════════════════════════════════════════════════════════════
         * Problem with v2.171 IP/port lookup:
         * - SCADA closes very quickly (normal Modbus: open, request, response, close)
         * - By time error notification arrives, SCADA metadata already deleted
         * - Lookup by IP/port fails → can't send close notification to Net1
         * - Net1's PCB leaks!
         *
         * Fix: Use session_id from error notification (Net1 includes it since v2.172)
         * - session_id is unique per connection
         * - Even if SCADA closed and metadata gone, we have session_id
         * - Can send close notification to Net1 using just session_id
         * - Net1 will close its PCB → no leak!
         * ═══════════════════════════════════════════════════════════════
         */
        struct connection_metadata *scada_meta = connection_lookup_by_session_id(
            ics_msg->metadata.session_id
        );

        if (scada_meta != NULL && scada_meta->active && scada_meta->pcb != NULL) {
            struct tcp_pcb *scada_pcb = scada_meta->pcb;

            DEBUG("%s:    → Found SCADA PCB=%p (session %u)\n",
                   COMPONENT_NAME, (void*)scada_pcb, scada_meta->session_id);
            DEBUG("%s:    → Sending RST to SCADA (immediate connection rejection)\n", COMPONENT_NAME);

            /* v2.170: CRITICAL FIX - Follow CRITICAL_LESSON Rule 2
             * ═══════════════════════════════════════════════════════════════
             * NEVER call tcp_abort() directly from event handler (main thread)!
             *
             * Event handlers run in main thread context. lwIP callbacks may be
             * executing in parallel. Direct tcp_abort() causes race condition:
             * - Main thread tries to free PCB
             * - lwIP callback may still be using PCB
             * - Result: PCB stuck in limbo, leaked from pool
             *
             * Correct protocol (CRITICAL_LESSON Rule 2):
             * 1. NULL all callbacks to prevent spurious firing
             * 2. Mark metadata inactive
             * 3. Try tcp_close() first (graceful, safer)
             * 4. Fallback to tcp_abort() only if tcp_close() fails
             * ═══════════════════════════════════════════════════════════════
             */

            /* Step 1: NULL all callbacks to prevent spurious firing during close */
            tcp_arg(scada_pcb, NULL);
            tcp_recv(scada_pcb, NULL);
            tcp_sent(scada_pcb, NULL);
            tcp_err(scada_pcb, NULL);
            tcp_poll(scada_pcb, NULL, 0);

            /* Step 2: Mark metadata inactive (prevent double-cleanup) */
            scada_meta->pcb = NULL;
            scada_meta->active = false;

            /* Step 3: Try graceful close first, fallback to abort if needed */
            err_t err = tcp_close(scada_pcb);
            if (err != ERR_OK) {
                /* tcp_close failed (e.g., unsent data), safe to abort now (callbacks NULL) */
                DEBUG("%s:    → tcp_close() failed (err=%d), forcing tcp_abort()\n",
                       COMPONENT_NAME, err);
                tcp_abort(scada_pcb);  /* Safe now - callbacks NULL, metadata inactive */
            }

            /* Step 4: Send close notification to Net1 (v2.171 CRITICAL FIX)
             * ═══════════════════════════════════════════════════════════════
             * BUG FOUND: When pool exhausts, we close SCADA connection but
             * NEVER notify Net1 to close PLC-side PCB → Net1 PCB LEAKS!
             *
             * Evidence from logs:
             * - 965 "TX: No metadata" warnings (PLC responses with no SCADA)
             * - 264 pool exhaustion events
             * - 108 active connections on Net1 (100 limit + 8 leaked)
             *
             * Fix: Send close notification to Net1 BEFORE cleanup
             * ═══════════════════════════════════════════════════════════════
             */
            if (inbound_dp != NULL && !scada_meta->close_notified) {
                InboundDataport *dp = (InboundDataport *)inbound_dp;

                /* Enqueue close notification (v2.175 FIX)
             * ═══════════════════════════════════════════════════════════════
             * CRITICAL: Use ERR_ABRT to signal forced close to Net1
             * - This is pool exhaustion: Net0 forcibly closing SCADA connection
             * - Net1 should use tcp_abort() to immediately free PLC-side PCB
             * - Sending ERR_ABRT (-13) tells Net1: "forced close, free immediately"
             * ═══════════════════════════════════════════════════════════════
             */
                bool success = control_queue_enqueue(
                    &dp->close_queue,
                    scada_meta->session_id,
                    ERR_ABRT,  /* Forced close due to pool exhaustion */
                    0   /* flags */
                );

                if (success) {
                    scada_meta->close_notified = true;  /* Set dedup flag */

                    /* v2.188-sentinel: Mark as close-only notification
                     * Pool exhaustion means rejecting NEW connection, no request data exists yet
                     * Set sentinel for defensive programming and consistency
                     */
                    dp->request_msg.payload_length = 0;  /* Sentinel: close-only, no payload */
                    dp->request_msg.metadata.session_id = scada_meta->session_id;
                    __sync_synchronize();  /* Memory barrier - ensure sentinel visible before signal */

                    inbound_ready_emit();               /* Signal Net1 */

                    DEBUG("%s:    → Sent close notification to Net1 (session %u, err=ERR_ABRT) - Net1 will tcp_abort() PLC PCB\n",
                           COMPONENT_NAME, scada_meta->session_id);
                } else {
                    DEBUG("%s:    → [ERROR] Failed to enqueue close notification (queue full? session %u)\n",
                           COMPONENT_NAME, scada_meta->session_id);
                }
            } else if (scada_meta->close_notified) {
                DEBUG("%s:    → [DEDUP] Close already notified for session %u\n",
                       COMPONENT_NAME, scada_meta->session_id);
            }

            /* Step 5: Enqueue cleanup */
            enqueue_cleanup(scada_meta->session_id);

            DEBUG("%s:    → RST sent, SCADA connection cleanup enqueued (pool exhaustion)\n", COMPONENT_NAME);
        } else {
            /* v2.187 CRITICAL FIX: Don't send close notification when Net1 never created PCB!
             * ═══════════════════════════════════════════════════════════════
             * Root Cause Analysis from v2.186 logs:
             * - 105,592 "Failed to create TCP PCB" errors (tcp_new() returned NULL)
             * - Infinite loop: session 721 repeated hundreds of times
             * - Net0 sending close notifications when Net1 never had a PCB to close!
             *
             * Two error cases with payload_offset=0xFFFF marker:
             * 1. tcp_new() == NULL: Pool exhausted, no PCB created → NOTHING TO CLOSE!
             * 2. tcp_connect() immediate fail: PCB created but Net1 already cleaned it up
             *    (see virtio_net1_driver.c:4356-4366 - NULLs callbacks, active=false,
             *     connection_count--, tcp_close/abort)
             *
             * v2.172 Logic Was WRONG:
             * - Comment claimed "Net1's PCB leaks!" but that's impossible:
             *   * Case 1: No PCB exists (tcp_new returned NULL)
             *   * Case 2: PCB already cleaned up by Net1 before error sent
             * - Sending close notification creates INFINITE LOOP:
             *   * SCADA retries with same port → same session ID
             *   * Net0: "connection not found" → sends close → Net1: "already closed"
             *   * GOTO step 1 (repeat forever)
             *
             * Fix: Check for error marker (payload_offset=0xFFFF) and skip close notification
             * ═══════════════════════════════════════════════════════════════
             */
            DEBUG("%s:    → SCADA connection not found (session %u) - already closed\n",
                   COMPONENT_NAME, ics_msg->metadata.session_id);

            /* Check if this is an error notification (0xFFFF marker) */
            if (ics_msg->metadata.payload_offset == 0xFFFF) {
                DEBUG("%s:    → Net1 error with 0xFFFF marker detected:\n", COMPONENT_NAME);
                DEBUG("%s:       - tcp_new() failed (pool exhausted), OR\n", COMPONENT_NAME);
                DEBUG("%s:       - tcp_connect() failed (Net1 already cleaned up)\n", COMPONENT_NAME);
                DEBUG("%s:    → No PCB exists in Net1 to close - skipping close notification\n",
                       COMPONENT_NAME);
                DEBUG("%s:    → (Prevents infinite loop from v2.172 bug)\n", COMPONENT_NAME);
            } else {
                /* Normal response but SCADA already closed - this shouldn't happen often */
                DEBUG("%s:    → Normal response but SCADA metadata not found\n", COMPONENT_NAME);
                DEBUG("%s:    → This might indicate a race condition or metadata cleanup issue\n",
                       COMPONENT_NAME);
            }
        }

        return;  /* Error handled, no response to forward */
    }

    /* Skip if no response data (only errors were queued) */
    if (ics_msg->payload_length == 0) {
        return;
    }

    #if DEBUG_TRAFFIC
    /* Print metadata - src/dst are SWAPPED because this is a response
     * Original request: SCADA (src) → PLC (dst)
     * Response: PLC (src) → SCADA (dst) */
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

    BREADCRUMB(3006);  /* Looking up connection metadata */

    /* Look up existing TCP connection by metadata
     * The metadata should have: src=PLC, dst=SCADA
     * We need to find the connection where: SCADA originally connected to us */
    struct connection_metadata *meta = connection_lookup_by_tuple(
        ics_msg->metadata.dst_ip,  /* Original SCADA IP */
        ics_msg->metadata.src_ip,  /* Original PLC IP (destination) */
        ics_msg->metadata.dst_port,  /* SCADA port */
        ics_msg->metadata.src_port   /* PLC port (502) */
    );

    /* v2.153: Try session_id lookup if port-based lookup fails
     * ═══════════════════════════════════════════════════════════════════════════
     * Problem: Port-based lookup fails when SCADA port is reused for new connection
     * Solution: Use session_id (unique, never reused) to find awaiting_response connections
     *
     * Scenario:
     * 1. SCADA:59046 connects → request sent → SCADA closes → awaiting_response=true
     * 2. SCADA:59046 connects again (port reused) → new connection → new metadata
     * 3. PLC responds to first request (session_id=123)
     * 4. Port lookup finds WRONG connection (new session_id=456)
     * 5. Session_id lookup finds CORRECT connection (session_id=123, awaiting_response=true)
     *
     * This is why session_id mapping is CRITICAL!
     * ═══════════════════════════════════════════════════════════════════════════
     */
    if (meta == NULL && ics_msg->metadata.session_id != 0) {
        BREADCRUMB(3006);  /* v2.153: Trying session_id lookup */

        meta = connection_lookup_by_session_id(ics_msg->metadata.session_id);

        if (meta != NULL) {
            DEBUG("%s: Found connection by session_id %u (port-based lookup failed)\n",
                   COMPONENT_NAME, ics_msg->metadata.session_id);

            if (meta->awaiting_response) {
                DEBUG("%s:    → Connection is awaiting_response - this is the queued PLC response!\n",
                       COMPONENT_NAME);
            } else {
                DEBUG("%s:    → Connection NOT awaiting_response - unexpected (port reuse?)\n",
                       COMPONENT_NAME);
            }
        } else {
            DEBUG("%s: session_id %u not found - connection already cleaned up\n",
                   COMPONENT_NAME, ics_msg->metadata.session_id);
        }
    }

    /* v2.89: CRITICAL FIX - Check meta != NULL BEFORE accessing meta->pcb!
     * Bug: "meta == NULL || meta->pcb == NULL" crashes if meta is NULL
     * Cause: Compiler evaluates meta->pcb before meta == NULL check
     * Result: Crash at address 0x4 (offset of pcb field in struct)
     * Fix: Separate NULL checks to guarantee safe evaluation order */

    if (meta == NULL) {
        BREADCRUMB(3007);  /* Connection not found */

        /* v2.117: CRITICAL FIX - Check Net1's connection state before dropping response
         * ═══════════════════════════════════════════════════════════════════════════
         * Problem: Asymmetric state between Net0 and Net1 causes system hang
         *
         * Scenario:
         * 1. SCADA sends request → Net0 → Net1 → PLC
         * 2. SCADA closes connection (FIN received by Net0)
         * 3. Net0 removes metadata (connection_remove called)
         * 4. Close notification sent to Net1
         * 5. Net1 IGNORES notification (self-cleaned tracking - correct!)
         * 6. PLC sends response → Net1 forwards to Net0
         * 7. Net0 looks up metadata → NOT FOUND (we removed it in step 3!)
         * 8. Net0 drops response → SYSTEM HANGS
         *
         * Root Cause: Net0 removed metadata but Net1 kept connection active
         *
         * Solution: Check Net1's connection state via peer_state dataport
         * - If Net1 still has the connection active, forward response anyway
         * - Use connection info from ICS message (already validated by ICS_Outbound)
         * - This solves the asymmetric state problem
         * ═══════════════════════════════════════════════════════════════════════════
         */

        DEBUG_WARN("%s: [WARN]  OUTBOUND: No local metadata for %u.%u.%u.%u:%u → %u.%u.%u.%u:%u\n",
               COMPONENT_NAME,
               (ics_msg->metadata.dst_ip >> 24) & 0xFF, (ics_msg->metadata.dst_ip >> 16) & 0xFF,
               (ics_msg->metadata.dst_ip >> 8) & 0xFF, ics_msg->metadata.dst_ip & 0xFF,
               ics_msg->metadata.dst_port,
               (ics_msg->metadata.src_ip >> 24) & 0xFF, (ics_msg->metadata.src_ip >> 16) & 0xFF,
               (ics_msg->metadata.src_ip >> 8) & 0xFF, ics_msg->metadata.src_ip & 0xFF,
               ics_msg->metadata.src_port);

        /* Check if Net1 still has this connection active */
        bool net1_has_connection = false;
        if (peer_state != NULL) {
            __sync_synchronize();  /* Memory barrier - ensure we read latest Net1 state */

            DEBUG("%s:    → Checking Net1's connection state (count=%u, last_update=%u)...\n",
                   COMPONENT_NAME, peer_state->count, peer_state->last_update);

            for (int i = 0; i < MAX_SHARED_CONNECTIONS; i++) {
                const struct connection_view *view = &peer_state->connections[i];
                if (view->active &&
                    view->src_ip == ics_msg->metadata.dst_ip &&   /* SCADA IP */
                    view->dst_ip == ics_msg->metadata.src_ip &&   /* PLC IP */
                    view->src_port == ics_msg->metadata.dst_port && /* SCADA port */
                    view->dst_port == ics_msg->metadata.src_port) { /* PLC port */

                    net1_has_connection = true;
                    DEBUG("%s:    ✓ Net1 STILL HAS connection (slot %d, age=%u ms)\n",
                           COMPONENT_NAME, i, sys_now() - view->timestamp);
                    DEBUG("%s:      → Forwarding response despite missing local metadata\n",
                           COMPONENT_NAME);
                    DEBUG("%s:      → This solves asymmetric state problem!\n", COMPONENT_NAME);
                    break;
                }
            }

            if (!net1_has_connection) {
                DEBUG("%s:    ✗ Net1 doesn't have connection either - response truly orphaned\n",
                       COMPONENT_NAME);
            }
        } else {
            DEBUG("%s:    ✗ peer_state not available - cannot check Net1\n", COMPONENT_NAME);
        }

        if (!net1_has_connection) {
            DEBUG("%s:    Connection closed by both Net0 and Net1 - dropping response\n",
                   COMPONENT_NAME);
            return;
        }

        /* Net1 has it! But we have a problem: SCADA already closed the connection on our side.
         * We removed metadata because SCADA sent FIN. There's no PCB to send the response to!
         *
         * This reveals the TRUE root cause: Net1 kept the connection alive but Net0 already
         * closed it. The response has nowhere to go because SCADA closed.
         *
         * This is actually EXPECTED BEHAVIOR - if SCADA closes before PLC responds, the
         * response should be dropped. The hang was caused by Net1 waiting indefinitely.
         *
         * The real fix is for Net1 to detect this asymmetry and close its side too.
         */
        DEBUG("%s: [ASYMMETRY DETECTED] Net1 has connection but Net0 doesn't (SCADA closed)\n",
               COMPONENT_NAME);
        DEBUG("%s:    → Cannot forward response - no PCB available (SCADA already closed)\n",
               COMPONENT_NAME);
        DEBUG("%s:    → Net1 should close its connection too (will happen via timeout)\n",
               COMPONENT_NAME);
        return;
    }

    /* Sanity check - meta should not be NULL at this point */
    if (meta == NULL) {
        DEBUG("%s: [BUG] meta is NULL but we didn't return - logic error!\n", COMPONENT_NAME);
        return;
    }

    if (meta->pcb == NULL) {
        BREADCRUMB(3014);  /* NULL PCB */
        DEBUG_WARN("%s: [WARN]  OUTBOUND: SCADA already closed connection - cannot send response for %u.%u.%u.%u:%u\n",
               COMPONENT_NAME,
               (ics_msg->metadata.dst_ip >> 24) & 0xFF, (ics_msg->metadata.dst_ip >> 16) & 0xFF,
               (ics_msg->metadata.dst_ip >> 8) & 0xFF, ics_msg->metadata.dst_ip & 0xFF,
               ics_msg->metadata.dst_port);
        DEBUG("%s:    Response from PLC arrived too late (connection closed by SCADA)\n", COMPONENT_NAME);

        /* v2.145: Just clear the awaiting_response flag - stale cleanup will handle it */
        meta->awaiting_response = false;
        return;
    }

    BREADCRUMB(3008);  /* Connection found */

    /* v2.62: CRITICAL FIX - Check PCB state FIRST before dereferencing other fields
     *
     * Problem: Net1 closes idle connections by calling tcp_abort(), which frees PCB.
     * But Net0 still has metadata with dangling PCB pointer.
     * Crash: meta->pcb points to freed memory, accessing meta->pcb->snd_nxt = page fault
     *
     * Solution: Check PCB state field FIRST. If state is CLOSED/invalid, PCB was freed.
     * State field is at offset 0, so it's safe to check even if PCB is partially freed.
     *
     * This catches dangling pointers before they cause crashes.
     */

    /* VALIDATION LAYER 1: NULL PCB Check (MUST BE FIRST!)
     * Net1 may have freed the PCB - check pointer validity before ANY dereference */
    if (meta->pcb == NULL) {
        /* PCB already freed by Net1 - drop response but keep metadata visible */
        DEBUG_WARN("%s: [WARN]  OUTBOUND: PCB is NULL - dropping response for %u.%u.%u.%u:%u\n",
               COMPONENT_NAME,
               (ics_msg->metadata.dst_ip >> 24) & 0xFF, (ics_msg->metadata.dst_ip >> 16) & 0xFF,
               (ics_msg->metadata.dst_ip >> 8) & 0xFF, ics_msg->metadata.dst_ip & 0xFF,
               ics_msg->metadata.dst_port);
        BREADCRUMB(3014);  /* Stale PCB detected */

        /* v2.120: DO NOT mark active=false here! (same fix as line 2936)
         * Let connection_cleanup_stale() handle metadata removal */

        /* REMOVED (v2.120): meta->active = false; */

        return;  /* Silent drop, metadata stays visible */
    }

    /* v2.85: REMOVED VALIDATION LAYER 2 - accessing pcb->state causes crashes!
     * DO NOT access pcb->state - it's a use-after-free bug if PCB is freed
     * Rely on tcp_write() to return error if connection is not established */

    /* v2.85: REMOVED VALIDATION LAYER 3 - accessing pcb->snd_nxt is use-after-free!
     * DO NOT access any PCB fields - only check if pcb != NULL */

    /* v2.85: REMOVED VALIDATION LAYER 4 - accessing pcb->local_port is use-after-free!
     * DO NOT access any PCB fields before tcp_write() */

    /* v2.85: ONLY validate PCB is non-NULL
     * tcp_write() will handle all other validation internally and return error if needed */

    BREADCRUMB(3009);  /* Attempting tcp_write */

    /* Final NULL check before tcp_write - this is the ONLY safe check we can do */
    if (meta->pcb == NULL) {
        DEBUG_WARN("%s: [WARN]  OUTBOUND: PCB became NULL before tcp_write!\n", COMPONENT_NAME);
        BREADCRUMB(3014);
        meta->active = false;
        return;
    }

    /* v2.95: REMOVED PCB state check - THIS WAS THE CRASH!
     * ═══════════════════════════════════════════════════════════════════
     * BUG: Accessing meta->pcb->state (offset ~0x10) causes page fault
     * if lwIP freed the PCB between NULL check and state access!
     *
     * Race condition:
     * 1. Check meta->pcb != NULL [OK]
     * 2. lwIP timer fires, frees PCB
     * 3. Access meta->pcb->state → CRASH at offset 0x10!
     *
     * Fix: Don't access ANY PCB fields! Let tcp_write() handle validation.
     * tcp_write() will return error if PCB is in wrong state.
     *
     * This matches v2.85 lesson: "DO NOT access any PCB fields"
     */

    /* v2.163: CRITICAL FIX - Send response IMMEDIATELY instead of queuing
     * ═══════════════════════════════════════════════════════════════════════════
     * Problem (v2.111-v2.162): Queuing mechanism broken for flow control
     *
     * Old design (v2.111-v2.162):
     *   - Store response in pending_outbound_data buffer
     *   - Wait for recv callback to send it
     *   - BUG: If SCADA closes before PLC responds → NO MORE RECV CALLBACKS!
     *   - Response sits in buffer forever
     *   - close_pending never set to true (only set after sending)
     *   - Close notification never sent to Net1
     *   - Net1 keeps PLC connection alive forever → SYSTEM HANG!
     *
     * Why old design assumed threading issues:
     *   - v2.111 comment claimed "race with lwIP timers"
     *   - v2.112 comment claimed "NULL pointer crash from main loop"
     *   - BOTH ASSUMPTIONS WRONG for seL4/CAmkES!
     *
     * seL4/CAmkES Architecture Reality:
     *   - Single-threaded component execution (no preemption in userspace)
     *   - lwIP timers run in SAME thread as CAmkES event handlers
     *   - outbound_ready_handle() runs in main event loop thread
     *   - lwIP callbacks ALSO run in main event loop thread
     *   - NO RACE CONDITIONS POSSIBLE! (single-threaded, non-preemptive)
     *
     * New design (v2.163): Bridge pattern
     *   - We ARE a bridge forwarding packets between networks
     *   - Response arrives → send immediately (no waiting)
     *   - Correct mental model: packet reconstruction, not deferred queuing
     *   - Same thread safety as lwIP callbacks (main event loop)
     *
     * Benefits:
     *   ✅ Zero delay (immediate response)
     *   ✅ No polling overhead
     *   ✅ Fixes flow control bug (send even if SCADA closed)
     *   ✅ Eliminates entire pending_outbound_data mechanism
     *   ✅ Simple, direct code
     *
     * Side effects: NONE for seL4/CAmkES (validated in design review)
     * ═══════════════════════════════════════════════════════════════════════════
     */

    /* Send response immediately! We're a bridge, not a queue. */
    err_t write_err = tcp_write(meta->pcb, ics_msg->payload, ics_msg->payload_length, TCP_WRITE_FLAG_COPY);

    if (write_err != ERR_OK) {
        BREADCRUMB(3010);  /* tcp_write failed */
        DEBUG_WARN("%s: [WARN]  OUTBOUND: tcp_write() failed (err=%d) - SCADA may have closed\n",
               COMPONENT_NAME, write_err);

        /* Connection not writable - clean up */
        meta->awaiting_response = false;

        /* If SCADA closed, poll callback or error callback will handle cleanup */
        return;
    }

    BREADCRUMB(3011);  /* tcp_write succeeded */

    /* v2.209: Update TX timestamp for delayed metadata cleanup
     * ═══════════════════════════════════════════════════════════════════════════
     * Purpose: Track last successful TX activity for fast-track cleanup
     *
     * This timestamp enables two-tier cleanup strategy:
     *   - Fast-track: Cleanup after 1 second TX idle (normal case)
     *   - Grace period: Cleanup after 5 seconds max (safety net)
     *
     * Why update here:
     *   - tcp_write() succeeded → data queued for transmission
     *   - This is the last TX activity for this connection
     *   - check_pending_cleanups() uses this to calculate tx_idle time
     *
     * Result: Metadata persists until TX completes, preventing "TX: No metadata"
     * ═══════════════════════════════════════════════════════════════════════════
     */
    meta->last_tx_timestamp = sys_now();

    /* v2.189: CRITICAL FIX - Mark response received BEFORE checking awaiting_response
     * ═══════════════════════════════════════════════════════════════════════════
     * Problem: Race condition between response arrival and SCADA close
     *
     * Scenario:
     *   1. SCADA sends request
     *   2. PLC response arrives, queued for processing
     *   3. SCADA sends FIN (closes connection)
     *   4. tcp_echo_recv(p=NULL) sets awaiting_response=true
     *   5. outbound_ready_handle() runs NOW
     *   6. tcp_write() succeeds (connection still open)
     *   7. BUG: awaiting_response still true, connection kept alive!
     *
     * Fix: Set response_received=true immediately after tcp_write succeeds
     *   - tcp_echo_recv checks response_received before setting awaiting_response
     *   - If response_received=true: skip awaiting mode, proceed with normal close
     * ═══════════════════════════════════════════════════════════════════════════
     */
    meta->response_received = true;  /* Response arrived and successfully written to TCP */

    /* v2.166: CRITICAL FIX - Defer tcp_output() to poll callback (fix reentrancy)
     * ═══════════════════════════════════════════════════════════════════════════
     * BUG: Calling tcp_output() from CAmkES event handler causes reentrancy!
     *
     * Problem:
     * - outbound_ready_handle() is a CAmkES event handler (interrupt-like)
     * - Can fire while lwIP is already processing (e.g., tcp_slowtmr)
     * - Calling tcp_output() from event handler violates lwIP NO_SYS=1 model
     * - Result: Recursive tcp_output() calls → DEPTH=3/DEPTH=4 → CRASH
     *
     * Evidence (v2.165 caller tracking):
     * - DEPTH=1: tcp_output() called by tcp_slowtmr (main loop)
     * - DEPTH=3: tcp_output() called by outbound_ready_handle (0xd4f0) ← EVENT
     * - DEPTH=4: tcp_output() called by tcp_input (recursive) → HALT
     *
     * Root Cause: Same as v2.139 RX reentrancy (fixed by making RX IRQ-only)
     * - v2.139 fixed process_rx_packets() reentrancy
     * - But outbound_ready_handle() reentrancy was never fixed!
     *
     * Solution: Defer tcp_output() to poll callback (lwIP best practice)
     * - tcp_write() already queued data in lwIP buffers (TCP_WRITE_FLAG_COPY)
     * - Set has_pending_outbound flag
     * - Poll callback (tcp_echo_poll) calls tcp_output() safely
     * - Poll runs in lwIP timer context → no reentrancy!
     *
     * Benefits:
     * - Eliminates reentrancy (only lwIP calls tcp_output)
     * - Follows lwIP NO_SYS=1 threading model
     * - Consistent with v2.111 deferred output design
     * ═══════════════════════════════════════════════════════════════════════════
     */
    meta->has_pending_outbound = true;  /* Signal poll callback to flush */

    #if DEBUG_TRAFFIC
    DEBUG("%s: [OK] OUTBOUND: Queued %u bytes (poll callback will flush)\n",
           COMPONENT_NAME, ics_msg->payload_length);
    #endif

    #if DEBUG_ENABLED_DEBUG
    DEBUG("   [OK] [MSG #%u] Response sent immediately to SCADA\n\n", msg_id);
    #endif

    /* v2.179: CRITICAL FIX - Only close if SCADA already sent FIN
     * ═══════════════════════════════════════════════════════════════════════════
     * BUG in v2.163-v2.178: Unconditional close_pending = true broke connection reuse!
     *
     * Problem:
     *   - Line 3985 (old): ALWAYS set close_pending = true after sending response
     *   - But Modbus/TCP supports connection reuse (multiple request/response cycles)
     *   - This caused premature close after EVERY response → Net1 PCB pool exhaustion!
     *
     * Evidence from v2.178 logs:
     *   - awaiting_response set (SCADA closed first):    27 connections
     *   - Poll callbacks fired (close_pending):         182 connections
     *   - Difference (response before SCADA close):     155 connections ← LEAKED!
     *   - Result: Net1 pool exhausted at 150/150
     *
     * Root Cause:
     *   - Comment said "if SCADA already closed" but code had NO if statement!
     *   - Always closed both sides, even when SCADA connection still open
     *
     * The Fix:
     *   - Check awaiting_response flag BEFORE clearing it
     *   - Only set close_pending if SCADA actually sent FIN (p=NULL in recv)
     *
     * Two Scenarios:
     *
     * A) SCADA closes BEFORE response (half-close pattern):
     *    1. SCADA sends request
     *    2. SCADA sends FIN (wants response then close)
     *       └─→ recv(p=NULL) sets awaiting_response=true
     *    3. PLC responds
     *    4. outbound_ready_handle sends response
     *       └─→ scada_closed = true (SCADA sent FIN)
     *       └─→ close_pending = true ✓ CORRECT - close both sides
     *
     * B) PLC responds, SCADA stays open (connection reuse):
     *    1. SCADA sends request
     *    2. PLC responds fast
     *    3. outbound_ready_handle sends response
     *       └─→ scada_closed = false (no FIN received)
     *       └─→ close_pending = false ✓ CORRECT - keep both sides open!
     *    4. SCADA sends ANOTHER request on same connection
     *       └─→ Connection reuse works! ✓
     *
     * lwIP Protocol Guarantee:
     *   - awaiting_response ONLY set when recv callback sees p=NULL
     *   - p=NULL means SCADA sent FIN (lwIP already acknowledged it)
     *   - RST goes to error callback (different path, doesn't set awaiting_response)
     *   - No race condition (same event loop thread)
     *
     * See: /tmp/scada_close_detection.md for full lwIP recv/error callback analysis
     * ═══════════════════════════════════════════════════════════════════════════
     */

    /* Check if SCADA sent FIN BEFORE clearing the flag */
    bool scada_closed = meta->awaiting_response;

    /* Clear awaiting_response flag (response delivered) */
    meta->awaiting_response = false;

    /* Only close if SCADA actually sent FIN */
    if (scada_closed) {
        /* SCADA closed first (half-close) - close both sides after response sent */
        meta->close_pending = true;
        meta->close_timestamp = sys_now();  /* v2.181: Record timestamp for latency measurement */
        BREADCRUMB(3012);  /* Response sent, close pending */

        #if DEBUG_TRAFFIC
        DEBUG("%s: Response sent to half-closed connection - will close both sides (session %u)\n",
               COMPONENT_NAME, meta->session_id);
        #endif
    } else {
        /* SCADA still open - keep connection alive for reuse */
        BREADCRUMB(3014);  /* Response sent, connection stays open for reuse */

        #if DEBUG_TRAFFIC
        DEBUG("%s: Response sent - connection stays open for reuse (session %u)\n",
               COMPONENT_NAME, meta->session_id);
        #endif
    }

    BREADCRUMB(3013);  /* Exit: outbound_ready_handle complete */
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
    #if DEBUG_ENABLED_DEBUG
    DEBUG("%s: ⚡ IRQ #%u: status=0x%x\n", COMPONENT_NAME, irq_count, irq_status);
    #endif

    if (irq_status & VIRTIO_MMIO_IRQ_VQUEUE) {
        #if DEBUG_ENABLED_DEBUG
        DEBUG("%s:   → VQUEUE interrupt - setting rx_packets_pending flag\n", COMPONENT_NAME);
        #endif
        /* v2.167: CRITICAL FIX - Don't call process_rx_packets() in IRQ!
         * Just set flag - main loop will process packets.
         * This prevents reentrancy with sys_check_timeouts() */
        rx_packets_pending = true;
        VREG_WRITE(VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_MMIO_IRQ_VQUEUE);
    }

    if (irq_status & VIRTIO_MMIO_IRQ_CONFIG) {
        DEBUG("%s:   → CONFIG interrupt\n", COMPONENT_NAME);
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

    /* ═══════════════════════════════════════════════════════════════ */
    /* COMPREHENSIVE VIRTIO SLOT SCANNER                                */
    /* Scan all 32 VirtIO MMIO slots to find which ones have devices   */
    /* ═══════════════════════════════════════════════════════════════ */
    DEBUG("\n╔═══════════════════════════════════════════════════════════════╗\n");
    DEBUG("║  [DISABLED] SCANNING MAPPED VIRTIO MMIO SLOTS FOR ACTIVE DEVICES         ║\n");
    DEBUG("║  Base: 0x0a000000, Each slot: 0x200 bytes apart               ║\n");
    DEBUG("║  Scanning slots 24-31 only (one 4KB page mapped)                ║\n");
    DEBUG("╚═══════════════════════════════════════════════════════════════╝\n\n");

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
//             DEBUG("Slot %2d @ 0x%08lx (offset +0x%03x): Magic=0x%08x Version=%u DeviceID=%u Vendor=0x%08x",
//                    slot, 0x0a000000 + offset, offset,
//                    slot_magic, slot_version, slot_device_id, slot_vendor_id);
// 
//             /* Identify device type */
//             if (slot_device_id == 1) {
//                 DEBUG(" [NETWORK]\n");
//             } else if (slot_device_id == 0) {
//                 DEBUG(" [NO DEVICE]\n");
//             } else {
//                 DEBUG(" [UNKNOWN TYPE]\n");
//             }
//         }
//     }

    DEBUG("\n");

    /* CRITICAL: Check if CAmkES dataport is properly mapped */
    if (virtio_mmio_regs == NULL) {
        DEBUG_ERROR("%s: [ERR] FATAL: virtio_mmio_regs dataport is NULL!\n", COMPONENT_NAME);
        DEBUG("%s:    CAmkES failed to map hardware component net0_hw\n", COMPONENT_NAME);
        DEBUG("%s:    Check ics_dual_nic.camkes configuration\n", COMPONENT_NAME);
        return -1;
    }

    DEBUG("%s: virtio_mmio_regs dataport mapped at %p\n", COMPONENT_NAME, (void *)virtio_mmio_regs);

    /* Access VirtIO device at SLOT 31 (offset 0xe00 from page base 0xa003000) */
    /* QEMU assigns FIRST -device virtio-net-device to slot 31 - matches vm_ethernet_echo */
    virtio_regs_base = (volatile uint32_t *)((uintptr_t)virtio_mmio_regs + 0xe00);

    DEBUG("%s: virtio_regs_base (slot 31) = %p (base + 0xe00)\n",
           COMPONENT_NAME, (void *)virtio_regs_base);

    /* Verify we have the network device using pointer arithmetic */
    uint32_t magic = VREG_READ(VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version = VREG_READ(VIRTIO_MMIO_VERSION);
    uint32_t device_id = VREG_READ(VIRTIO_MMIO_DEVICE_ID);

    DEBUG("%s: VirtIO @ slot 31 (+0xe00): Magic=0x%x, Version=%u, DeviceID=%u\n",
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
        DEBUG("\n");
        DEBUG("VirtIO legacy mode (Version 1) DOES NOT WORK on seL4 ARM32 hypervisor!\n");
        DEBUG("\n");
        DEBUG("ROOT CAUSE:\n");
        DEBUG("  - Legacy VirtIO has MMIO write failures in Stage 2 page tables\n");
        DEBUG("  - QueueReady/QueueSel registers become unresponsive\n");
        DEBUG("  - Device initialization will FAIL\n");
        DEBUG("\n");
        DEBUG("REQUIRED FIX:\n");
        DEBUG("  Add this QEMU flag to enable modern VirtIO protocol:\n");
        DEBUG("\n");
        DEBUG("  ./simulate --extra-qemu-args=\"-global virtio-mmio.force-legacy=false \\\n");
        DEBUG("    -netdev user,id=net0,hostfwd=tcp::6000-:6000 \\\n");
        DEBUG("    -device virtio-net-device,netdev=net0 \\\n");
        DEBUG("    -netdev user,id=net1,hostfwd=tcp::7000-:7000 \\\n");
        DEBUG("    -device virtio-net-device,netdev=net1\"\n");
        DEBUG("\n");
        DEBUG("WHAT THIS DOES:\n");
        DEBUG_INFO("  [OK] Enables VirtIO 1.0+ modern protocol (Version 2)\n");
        DEBUG_INFO("  [OK] Fixes MMIO write issues\n");
        DEBUG_INFO("  [OK] Allocates devices to slots 6-7 (not 30-31)\n");
        DEBUG_INFO("  [OK] Makes QueueReady registers writable\n");
        DEBUG("\n");
        DEBUG("DOCUMENTATION:\n");
        DEBUG("  See: research-docs/VIRTIO-FORCE-LEGACY-REQUIREMENT.md\n");
        DEBUG("\n");
        DEBUG("╔════════════════════════════════════════════════════════════════╗\n");
        DEBUG("║  System halted - cannot continue with legacy VirtIO           ║\n");
        DEBUG("╚════════════════════════════════════════════════════════════════╝\n");
        DEBUG("\n");
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

    DEBUG("\n%s: RX Queue Configuration BEFORE setting ready:\n", COMPONENT_NAME);
    DEBUG("  QueueNum written: %u\n", rx_virtq.num);
    DEBUG("  Desc  paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", desc_paddr, (uint32_t)desc_paddr, (uint32_t)(desc_paddr >> 32));
    DEBUG("  Avail paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", avail_paddr, (uint32_t)avail_paddr, (uint32_t)(avail_paddr >> 32));
    DEBUG("  Used  paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", used_paddr, (uint32_t)used_paddr, (uint32_t)(used_paddr >> 32));

    DEBUG("\n%s: Reading back RX queue registers BEFORE ready:\n", COMPONENT_NAME);
    DEBUG("  QueueNum:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_NUM), rx_virtq.num);
    DEBUG("  DescLow:      0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_DESC_LOW), (uint32_t)desc_paddr);
    DEBUG("  DescHigh:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_DESC_HIGH), (uint32_t)(desc_paddr >> 32));
    DEBUG("  AvailLow:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_AVAIL_LOW), (uint32_t)avail_paddr);
    DEBUG("  AvailHigh:    0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_AVAIL_HIGH), (uint32_t)(avail_paddr >> 32));
    DEBUG("  UsedLow:      0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_USED_LOW), (uint32_t)used_paddr);
    DEBUG("  UsedHigh:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_USED_HIGH), (uint32_t)(used_paddr >> 32));
    DEBUG("  QueueReady:   0x%08x (expect 0 before write)\n", VREG_READ(VIRTIO_MMIO_QUEUE_READY));

    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);
    DMB();

    uint32_t rx_ready_after = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    DEBUG("\n%s: After writing QUEUE_READY=1:\n", COMPONENT_NAME);
    DEBUG("  QueueReady readback: 0x%08x (expect 1 if QEMU accepted config)\n", rx_ready_after);
    if (rx_ready_after == 0) {
        DEBUG_ERROR("  [ERR] QEMU REJECTED RX queue - configuration invalid!\n");
    } else {
        DEBUG_INFO("  [OK] QEMU ACCEPTED RX queue\n");
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

    DEBUG("\n%s: TX Queue Configuration BEFORE setting ready:\n", COMPONENT_NAME);
    DEBUG("  QueueNum written: %u\n", tx_virtq.num);
    DEBUG("  Desc  paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", tx_desc_paddr, (uint32_t)tx_desc_paddr, (uint32_t)(tx_desc_paddr >> 32));
    DEBUG("  Avail paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", tx_avail_paddr, (uint32_t)tx_avail_paddr, (uint32_t)(tx_avail_paddr >> 32));
    DEBUG("  Used  paddr: 0x%lx (LOW=0x%08x, HIGH=0x%08x)\n", tx_used_paddr, (uint32_t)tx_used_paddr, (uint32_t)(tx_used_paddr >> 32));

    DEBUG("\n%s: Reading back TX queue registers BEFORE ready:\n", COMPONENT_NAME);
    DEBUG("  QueueNum:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_NUM), tx_virtq.num);
    DEBUG("  DescLow:      0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_DESC_LOW), (uint32_t)tx_desc_paddr);
    DEBUG("  DescHigh:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_DESC_HIGH), (uint32_t)(tx_desc_paddr >> 32));
    DEBUG("  AvailLow:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_AVAIL_LOW), (uint32_t)tx_avail_paddr);
    DEBUG("  AvailHigh:    0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_AVAIL_HIGH), (uint32_t)(tx_avail_paddr >> 32));
    DEBUG("  UsedLow:      0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_USED_LOW), (uint32_t)tx_used_paddr);
    DEBUG("  UsedHigh:     0x%08x (expect 0x%08x)\n", VREG_READ(VIRTIO_MMIO_QUEUE_USED_HIGH), (uint32_t)(tx_used_paddr >> 32));
    DEBUG("  QueueReady:   0x%08x (expect 0 before write)\n", VREG_READ(VIRTIO_MMIO_QUEUE_READY));

    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 1);
    DMB();

    uint32_t tx_ready_after = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    DEBUG("\n%s: After writing QUEUE_READY=1:\n", COMPONENT_NAME);
    DEBUG("  QueueReady readback: 0x%08x (expect 1 if QEMU accepted config)\n", tx_ready_after);
    if (tx_ready_after == 0) {
        DEBUG_ERROR("  [ERR] QEMU REJECTED TX queue - configuration invalid!\n");
    } else {
        DEBUG_INFO("  [OK] QEMU ACCEPTED TX queue\n");
    }

    /* Device ready - activate the device */
    VREG_WRITE(VIRTIO_MMIO_STATUS, VREG_READ(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);
    DEBUG_INFO("%s: [OK] VirtIO device initialized and activated\n", COMPONENT_NAME);
    /* ═══ CRITICAL: Test if MMIO writes work ═══ */
    DEBUG("\n%s: Testing MMIO write capability...\n", COMPONENT_NAME);

    /* Select queue 0 (RX queue) */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_SEL, 0);

    /* Read current QueueReady state */
    uint32_t original_ready = VREG_READ(VIRTIO_MMIO_QUEUE_READY);
    DEBUG("%s:   Queue 0 original ready state = 0x%x\n", COMPONENT_NAME, original_ready);

    /* Test write by toggling QueueReady */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 0x0);
    uint32_t read_back_0 = VREG_READ(VIRTIO_MMIO_QUEUE_READY);

    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, 0x1);
    uint32_t read_back_1 = VREG_READ(VIRTIO_MMIO_QUEUE_READY);

    DEBUG("%s:   Write 0x0, read back: 0x%x (expect 0x0)\n", COMPONENT_NAME, read_back_0);
    DEBUG("%s:   Write 0x1, read back: 0x%x (expect 0x1)\n", COMPONENT_NAME, read_back_1);

    /* Restore original state */
    VREG_WRITE(VIRTIO_MMIO_QUEUE_READY, original_ready);

    if (read_back_0 != 0x0 || read_back_1 != 0x1) {
        DEBUG_ERROR("\n%s: [ERR][ERR][ERR] FATAL ERROR: MMIO WRITES DO NOT WORK! [ERR][ERR][ERR]\n", COMPONENT_NAME);
        DEBUG("%s: Device memory attributes are incorrect.\n", COMPONENT_NAME);
        DEBUG("%s: This will cause infinite IRQ loops and duplicate packets.\n", COMPONENT_NAME);
        DEBUG("%s: Cannot continue - terminating initialization.\n\n", COMPONENT_NAME);
        return -1;
    }

    DEBUG_INFO("%s:   [OK] MMIO writes work correctly!\n\n", COMPONENT_NAME);

    return 0;
}

/*
 * Component initialization
 */
void post_init(void)
{
    DEBUG("%s: Component started\n", COMPONENT_NAME);
    DEBUG("%s: NET0 v2.219 (2025-11-01) - Fix duplicate close notifications from poll callback\n", COMPONENT_NAME);
    DEBUG("%s: [FIX] MODE: PRODUCTION with fast cleanup (every 100 iterations)\n", COMPONENT_NAME);
    DEBUG_INFO("%s: [OK] FIX 1: Immediate cleanup on tcp_echo_err (v2.145 behavior)\n", COMPONENT_NAME);
    DEBUG_INFO("%s: [OK] FIX 2: Send close notification to Net1 when SCADA closes\n", COMPONENT_NAME);
    DEBUG_INFO("%s: [OK] FIX 3: Handle PLC error notification - close SCADA immediately\n", COMPONENT_NAME);
    DEBUG_INFO("%s: [OK] FIX 4: REMOVED PCB state check - prevents crash at offset 0x10!\n", COMPONENT_NAME);
    DEBUG_INFO("%s: [OK] FIX 5: Connection table debugging for troubleshooting\n");
    DEBUG_INFO("%s: [OK] FIX 6: lwIP manages connection limits (MEMP_NUM_TCP_PCB=100)\n");
    DEBUG_INFO("%s: [OK] FIX 7: tcp_abort() CLOSE_WAIT+LAST_ACK (every 5s) - immediate PBUF free\n\n", COMPONENT_NAME);

    /* Initialize connection tracking table */
    memset(connection_table, 0, sizeof(connection_table));
    connection_count = 0;
    DEBUG_INFO("%s: [OK] Connection tracking table initialized (%d slots)\n", COMPONENT_NAME, MAX_CONNECTIONS);

    /* v2.117: Initialize connection state sharing dataports */
    own_state = (volatile struct connection_state_table *)net0_conn_state;
    peer_state = (volatile struct connection_state_table *)net1_conn_state;
    if (own_state) {
        memset((void *)own_state, 0, sizeof(struct connection_state_table));
        DEBUG_INFO("%s: [OK] Own connection state dataport mapped (size=%zu bytes)\n",
               COMPONENT_NAME, sizeof(struct connection_state_table));
    }
    if (peer_state) {
        DEBUG_INFO("%s: [OK] Peer connection state dataport mapped (read-only access to Net1)\n", COMPONENT_NAME);
    }

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
     * nic0 IS the external gateway (192.168.96.2) that pfSense routes through
     * No gateway needed - we ARE the gateway!
     * TCP server listens on 192.168.96.2:502
     */
    struct ip4_addr ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 192, 168, 96, 2);    /* Static IP: 192.168.96.2 */
    IP4_ADDR(&netmask, 255, 255, 255, 0);  /* Netmask: 255.255.255.0 */
    IP4_ADDR(&gw, 192, 168, 96, 1);        /* Gateway: pfSense (to reach 192.168.90.x network) */

    DEBUG("%s: Configuring network interface:\n", COMPONENT_NAME);
    DEBUG("%s:   IP:      192.168.96.2 (security gateway on 192.168.96.0/24)\n", COMPONENT_NAME);
    DEBUG("%s:   Netmask: 255.255.255.0\n", COMPONENT_NAME);
    DEBUG("%s:   Gateway: 192.168.96.1 (pfSense - routes to SCADA network)\n", COMPONENT_NAME);
    DEBUG("%s:   TCP server: 192.168.96.2:%d\n", COMPONENT_NAME, TCP_ECHO_PORT);

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
    DEBUG("%s: Role:           External gateway (transparent security gateway)\n", COMPONENT_NAME);
    DEBUG("%s: ═══════════════════════════════════════════════════════════\n", COMPONENT_NAME);
    DEBUG("\n");

    /* Validation check */
    uint8_t if_ip1 = ip4_addr1(netif_ip4_addr(&netif_data));
    uint8_t if_ip2 = ip4_addr2(netif_ip4_addr(&netif_data));
    uint8_t if_ip3 = ip4_addr3(netif_ip4_addr(&netif_data));
    uint8_t if_ip4 = ip4_addr4(netif_ip4_addr(&netif_data));

    if (if_ip1 == 192 && if_ip2 == 168 && if_ip3 == 96 && if_ip4 == 2) {
        DEBUG_INFO("%s: [OK] CONFIGURATION VALID: External gateway IP = 192.168.96.2\n", COMPONENT_NAME);
        DEBUG_INFO("%s: [OK] pfSense routes 192.168.95.0/24 traffic through this gateway\n", COMPONENT_NAME);
        DEBUG_INFO("%s: [OK] Bridge br0 forwards all traffic to/from ens224\n", COMPONENT_NAME);
    } else {
        DEBUG_WARN("%s: [WARN]  WARNING: Interface IP (%u.%u.%u.%u) does NOT match expected (192.168.96.2)\n",
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
        DEBUG_ERROR("║  [ERR] FATAL: VirtIO_Net0_Driver initialization FAILED     ║\n");
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

    /* v2.222: Initialize pbuf tracking database */
    pbuf_tracking_init();
    DEBUG_INFO("%s: [OK] PBUF tracking initialized (max %u entries)\n", COMPONENT_NAME, MAX_PBUF_TRACKING_ENTRIES);

    /* Main event loop - process lwIP timers, RX packets, and ICS notifications */
    /* Note: TCP server is now initialized in RX path after first packet */
    static uint32_t cleanup_counter = 0;
    static uint32_t heartbeat_counter = 0;
    static uint32_t last_close_wait_cleanup = 0;  /* v2.217: Track CLOSE_WAIT cleanup time */
    while (1) {
        /* v2.104: Lightweight heartbeat - removed table dump (stack overflow risk) */
        if (++heartbeat_counter >= 50000) {
            DEBUG("%s: [HB]  HB:%u conns:%u\n", COMPONENT_NAME, heartbeat_counter, connection_count);
            heartbeat_counter = 0;
        }

        /* v2.222: Periodic pbuf leak diagnostics (every 10 seconds) */
        pbuf_tracking_periodic_diagnostics("Net0", 10000);

        /* Check for OUTBOUND notifications from ICS_Outbound (non-blocking) */
        if (outbound_ready_poll()) {
            /* CRITICAL: Ensure we see latest dataport writes from ICS_Outbound */
            __sync_synchronize();
            BREADCRUMB(8000);  /* Before outbound_ready_handle */
            outbound_ready_handle();
            BREADCRUMB(8001);  /* After outbound_ready_handle */
        }

        /* Process lwIP timers and RX packets */
        /* v2.144: Reduced breadcrumb flooding - only print every 10000 iterations */
        static uint32_t loop_counter = 0;
        if (++loop_counter >= 10000) {
            BREADCRUMB(8002);  /* Periodic main loop heartbeat */
            loop_counter = 0;
        }
        sys_check_timeouts();

        /* v2.193: Process cleanup queue in main loop (guaranteed execution)
         * ═══════════════════════════════════════════════════════════════════════
         * This runs EVERY iteration (unlike poll callbacks which depend on lwIP)
         * - Guaranteed execution: No dependency on TCP state or timers
         * - Natural deduplication: active flag prevents double-cleanup
         * - Lock-free SPSC queue: Callbacks enqueue, main loop dequeues
         * - Single enforcement point: Only place counters are decremented
         * ═══════════════════════════════════════════════════════════════════════
         */
        process_cleanup_queue();

        /* v2.167: CORRECT FIX - Process RX packets in main loop (flag-based)
         * ═══════════════════════════════════════════════════════════════════════
         * Previous approach (v2.139): IRQ called process_rx_packets() directly
         * Problem: IRQ can interrupt sys_check_timeouts() → reentrancy!
         * - Main loop: sys_check_timeouts() → tcp_slowtmr() → tcp_output()
         * - IRQ fires: process_rx_packets() → tcp_input() → tcp_output()
         * - Result: DEPTH=4/5 recursion → CRASH
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

        /* v2.71: Periodic connection cleanup (every 100 iterations for fast Modbus cycles)
         * Previous: 10000 iterations was too slow, causing table exhaustion
         * With ~1 second Modbus cycles, cleanup needs to run frequently */
        if (++cleanup_counter >= 100) {
            cleanup_counter = 0;
            connection_cleanup_stale();

            /* v2.209: Check and cleanup connections with metadata_close_pending
             * ═══════════════════════════════════════════════════════════════════════════
             * Purpose: Process delayed metadata cleanups (fix pbuf leak race condition)
             *
             * This implements two-tier cleanup:
             *   1. Fast-track: Cleanup after 1 second TX idle (normal case)
             *   2. Grace period: Cleanup after 5 seconds max (safety net)
             *
             * Why here:
             *   - Runs periodically with connection_cleanup_stale() (every 100 iterations)
             *   - Ensures pending cleanups are processed promptly
             *   - Prevents connection pool exhaustion
             *
             * Result: Metadata persists until TX completes, preventing "TX: No metadata"
             * ═══════════════════════════════════════════════════════════════════════════
             */
            check_pending_cleanups();
        }

        /* v2.217: Periodic CLOSE_WAIT cleanup (every 5 seconds)
         * ═══════════════════════════════════════════════════════════════════════════
         * Purpose: Prevent PBUF pool exhaustion from CLOSE_WAIT connections
         *
         * Root Cause:
         *   - When SCADA closes connection, lwIP moves it to CLOSE_WAIT
         *   - Each CLOSE_WAIT connection holds ~27 PBUFs on average
         *   - 29 CLOSE_WAIT connections × 27 PBUFs = 800/800 pool exhausted
         *
         * Solution:
         *   - Scan lwIP's tcp_active_pcbs list every 5 seconds
         *   - Call tcp_close() on connections in CLOSE_WAIT state
         *   - lwIP frees all held PBUFs when connection closes
         *
         * Why every 5 seconds:
         *   - Not too frequent (avoid overhead scanning PCB list)
         *   - Frequent enough to prevent pool exhaustion
         *   - Typical SCADA behavior: bursts of connections every few seconds
         *
         * Safety: tcp_close() is safe to call from main loop context
         * ═══════════════════════════════════════════════════════════════════════════
         */
        uint32_t now = sys_now();
        if (now - last_close_wait_cleanup >= 5000) {  /* 5 seconds */
            cleanup_close_wait_connections();
            last_close_wait_cleanup = now;
        }

        seL4_Yield();

        /* v2.73: Add small delay after yield to allow Net1 to complete operations
         * This introduces timing delay similar to printf I/O */
        for (volatile int i = 0; i < 100; i++) {
            /* Busy wait to introduce delay */
        }
    }

    return 0;
}
