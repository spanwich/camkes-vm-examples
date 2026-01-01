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
 *
 * v2.240 (2025-11-02): CENTRALIZED PBUF CLEANUP PATTERN
 *   - Implemented single-exit-point pattern in tcp_echo_recv()
 *   - All code paths goto cleanup label with centralized pbuf_free()
 *   - Fixes both pbuf leaks (early returns) AND PCB corruption (double-free)
 *   - lwIP contract: Application must free pbuf when returning ERR_OK
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
#include "lwip/priv/tcp_priv.h"  /* v2.205: Access tcp_active_pcbs for ooseq diagnostics */
#include "netif/ethernet.h"

/* ICS common definitions */
#include "common.h"
#include "version.h"  /* v2.241: Unified version management */

/* v2.117: Connection state sharing */
#include "connection_state.h"

#define COMPONENT_NAME "VirtIO_Net0_Driver"
#define TCP_SERVER_PORT 502  /* INBOUND: Modbus port - pretends to be PLC */

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

    bool response_received;        /* True if PLC response arrived (even if not sent yet) */

    bool close_pending;            /* True if connection should close (poll handles) */

    bool closing;                  /* True if close initiated, waiting for PCB free */
    /* NOTE: close_timestamp moved to v2.209 section below (reused for metadata_close_pending) */

    uint8_t *pending_outbound_data;  /* Queued outbound data awaiting send */
    uint16_t pending_outbound_len;   /* Length of queued data */
    bool has_pending_outbound;       /* True if data needs to be sent */

    bool cleanup_in_progress;        /* Guard: prevents double-cleanup */

    bool close_notified;             /* True if close notification already queued (Net0 → Net1) */

    bool metadata_close_pending;     /* True if SCADA closed but metadata persists for TX */
    uint32_t close_timestamp;        /* When metadata_close_pending was set (for grace period) */
    uint32_t last_tx_timestamp;      /* Last TX path activity (for fast-track cleanup) */
};

static struct connection_metadata connection_table[MAX_CONNECTIONS];
static int connection_count = 0;

static uint32_t active_connections = 0;
static uint32_t total_connections_created = 0;
static uint32_t total_connections_closed = 0;

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

static uint32_t next_session_id = 1;

static volatile struct connection_state_table *own_state = NULL;   /* Our state (exposed to Net1) */
static volatile struct connection_state_table *peer_state = NULL;  /* Net1's state (read-only) */

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

#define OUTBOUND_FORWARD_IP "192.168.95.2"        /* Forward to Net1 (private network) */
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
                            uint32_t current_src = ntohl(ip->saddr);  /* Currently 192.168.96.2 */
                            uint32_t new_src = meta->original_dest_ip;  /* Should be 192.168.95.2 */

                            DEBUG("%s: [ICMP-TX] Restoring source IP: %u.%u.%u.%u → %u.%u.%u.%u (id=%u, seq=%u)\n",
                                   COMPONENT_NAME,
                                   (current_src >> 24) & 0xFF, (current_src >> 16) & 0xFF,
                                   (current_src >> 8) & 0xFF, current_src & 0xFF,
                                   (new_src >> 24) & 0xFF, (new_src >> 16) & 0xFF,
                                   (new_src >> 8) & 0xFF, new_src & 0xFF,
                                   icmp_id, icmp_seq);

                            /* Restore source IP to original destination (pretend to be PLC) */
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
            connection_table[i].closing = false;

            connection_table[i].cleanup_in_progress = false;

            connection_table[i].close_notified = false;

            connection_table[i].metadata_close_pending = false;
            connection_table[i].close_timestamp = 0;
            connection_table[i].last_tx_timestamp = sys_now();

            connection_count++;

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

        if (connection_table[i].original_src_ip == src_ip &&
            connection_table[i].src_port == sport &&
            connection_table[i].dest_port == dport) {

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

            meta->close_notified = false;
            meta->cleanup_in_progress = false;

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
        /* connection_count decremented */
    }

    if (active_connections > 0) {
        active_connections--;
    } else {
        /* WARN: active_connections already 0 - prevented underflow */
        DEBUG_WARN("%s: [WARN] active_connections already 0 (prevented underflow)!\n",
               COMPONENT_NAME);
    }

    /* Mark metadata as inactive */
    meta->active = false;
    meta->pcb = NULL;
    meta->has_pending_outbound = false;
    meta->pending_outbound_len = 0;
    meta->awaiting_response = false;
    meta->response_received = false;  /* v2.189: Reset response tracking */

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

        /* v2.92: ONLY cleanup if PCB is NULL AND we're not awaiting a response */
        if (pcb == NULL && !connection_table[i].awaiting_response) {
            #if DEBUG_METADATA
            DEBUG("%s: [CLEAN] Cleanup stale connection [%d]: PCB is NULL and no response pending\n", COMPONENT_NAME, i);
            #endif
            connection_table[i].active = false;

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

        if (tx_idle > 1000) {
            #if DEBUG_METADATA
            DEBUG_WARN("%s: [WARN] Fast-track cleanup: session_id=%u (tx_idle=%u ms)\n",
                  COMPONENT_NAME, meta->session_id, (unsigned int)tx_idle);
            #endif
            connection_cleanup_atomic(meta);
            cleaned++;
            continue;
        }

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

    /* Check if this is a request for PLC IP (192.168.95.2) */
    #define PLC_IP 0xC0A85F02  /* 192.168.95.2 in hex */
    if (target_ip != PLC_IP) {
        return false;  /* Not for our proxied IP */
    }

    DEBUG("%s: [ARP-PROXY] Request for 192.168.95.2 (PLC) - replying with our MAC\n",
           COMPONENT_NAME);

    /* Build ARP reply in the same pbuf (reuse the request packet) */
    arphdr->opcode = htons(2);  /* ARP_REPLY */

    /* Swap sender and target fields */
    /* Target becomes the original sender */
    memcpy(&arphdr->dhwaddr, &arphdr->shwaddr, sizeof(struct eth_addr));
    arphdr->dipaddr.addrw[0] = arphdr->sipaddr.addrw[0];
    arphdr->dipaddr.addrw[1] = arphdr->sipaddr.addrw[1];

    /* Sender becomes us (with PLC's IP) */
    memcpy(&arphdr->shwaddr, &inp->hwaddr, sizeof(struct eth_addr));
    arphdr->sipaddr.addrw[0] = htons((PLC_IP >> 16) & 0xFFFF);
    arphdr->sipaddr.addrw[1] = htons(PLC_IP & 0xFFFF);

    /* Fix Ethernet header */
    memcpy(&ethhdr->dest, &ethhdr->src, sizeof(struct eth_addr));  /* Reply to sender */
    memcpy(&ethhdr->src, &inp->hwaddr, sizeof(struct eth_addr));   /* From us */

    /* Send the ARP reply directly via low-level output */
    inp->linkoutput(inp, p);

    DEBUG("%s: [ARP-PROXY] Sent ARP reply: 192.168.95.2 is at %02x:%02x:%02x:%02x:%02x:%02x\n",
           COMPONENT_NAME,
           inp->hwaddr[0], inp->hwaddr[1], inp->hwaddr[2],
           inp->hwaddr[3], inp->hwaddr[4], inp->hwaddr[5]);

    return true;  /* We handled it */
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

    /* Handle ARP packets - check for proxy first, then pass to lwIP */
    if (type == ETHTYPE_ARP) {
        pbuf_arp_count++;  /* v2.203: Track ARP packets */

        /* v2.242: Check if this is an ARP request for our proxied IP (192.168.95.2) */
        if (arp_proxy_check_and_reply(p, inp)) {
            /* We handled it - ARP reply sent */
            pbuf_free(p);  /* Free the request packet */
            return ERR_OK;
        }

        /* Not for proxied IP - pass to lwIP's ARP handler */
        /* Remove Ethernet header and pass to etharp_input for ARP processing */
        if (pbuf_remove_header(p, sizeof(struct eth_hdr)) == 0) {
            /* v2.222: REVERTED FIX #2 - etharp_input() DOES free pbuf (line 741 in etharp.c)
             * Double-free was breaking connections!
             */
            etharp_input(p, inp);
            return ERR_OK;
        }
        /* pbuf_remove_header failed - we must free since we won't pass to lwIP */
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

            } else if (IPH_PROTO(iphdr) == IP_PROTO_UDP) {
                pbuf_udp_count++;  /* v2.203: Track UDP packets */
            } else if (IPH_PROTO(iphdr) == IP_PROTO_ICMP && p->len >= 20 + 8) {
                /* v2.242: ICMP packet - check if it's an echo request */
                struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)((uint8_t *)iphdr + (IPH_HL(iphdr) * 4));

                if (icmp->type == ICMP_ECHO) {  /* Echo request (ping) */
                    uint16_t icmp_id = ntohs(icmp->id);
                    uint16_t icmp_seq = ntohs(icmp->seqno);

                    /* Store metadata so TX path can restore source IP */
                    icmp_metadata_store(pkt_dest_ip, icmp_id, icmp_seq);
                }
                pbuf_other_count++;  /* v2.203: Track ICMP packets */
            } else {
                pbuf_other_count++;  /* v2.203: Track other IP protocols */
            }

            if (IPH_PROTO(iphdr) == IP_PROTO_TCP && src_port != 0 && dest_port != 0) {
                /* Check if we already have metadata for this connection */
                struct connection_metadata *meta = connection_lookup_by_tuple(
                    pkt_src_ip, pkt_dest_ip, src_port, dest_port);

                if (!meta) {
                    /* New connection - store metadata */
                    connection_add(pkt_src_ip, pkt_dest_ip, src_port, dest_port);
                } else if (!meta->active) {
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
            /* v2.215: Track PBUF_POOL allocation with interface identification
             * v2.257: Changed to DEBUG level (too verbose at INFO) */
            pbuf_allocated_count++;  /* v2.203: Track allocation */

            pbuf_take(p, packet_data, packet_len);

            #if DEBUG_ENABLED_DEBUG
            DEBUG("   [OK] pbuf allocated, passing to lwIP input handler\n");
            #endif

            /* v2.137: Track before lwIP input call */
            BREADCRUMB(8008);  /* Before lwIP input() */

            /* Feed packet to lwIP */
            err_t lwip_result = netif_data.input(p, &netif_data);

            /* v2.137: Track after lwIP input call */
            BREADCRUMB(8009);  /* After lwIP input() */

            /* v2.215: Track lwIP input acceptance/rejection
             * v2.257: RX-ACCEPT at DEBUG level (too verbose), RX-REJECT stays at WARN */
            if (lwip_result != ERR_OK) {
                extern struct stats_ lwip_stats;
                uint32_t pbuf_after_input = lwip_stats.memp[MEMP_PBUF_POOL]->used;
                DEBUG_WARN("[Net0][PBUF_POOL][RX-REJECT] lwIP rejected (err=%d), pbuf=%p | PBUF: %u/800\n",
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
                                /* v2.257: ALWAYS log SYN packets at INFO level to diagnose connection issues */
                                uint32_t saddr = ntohl(ip->saddr);
                                DEBUG_INFO("%s: [SYN] TCP SYN from %u.%u.%u.%u:%u → %u.%u.%u.%u:%u\n",
                                       COMPONENT_NAME,
                                       (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF, (saddr >> 8) & 0xFF, saddr & 0xFF,
                                       ntohs(tcp->source),
                                       (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF, (daddr >> 8) & 0xFF, daddr & 0xFF,
                                       ntohs(tcp->dest));
                            }
                        }
                    }
                }
            }

            if (lwip_result != ERR_OK) {
                BREADCRUMB(9005);  /* pbuf_free at line 1791 (process_rx_packets lwip error) */
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

    struct connection_metadata *meta = (struct connection_metadata *)arg;

    DEBUG_WARN("%s: [WARN]  TCP connection error - err=%d (%s)\n", COMPONENT_NAME, err, err_name);

    if (meta == NULL) {
        DEBUG("%s:    → No metadata (old connection?) - cannot send close notification\n",
               COMPONENT_NAME);
        return;
    }

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

    DEBUG("%s:    → SCADA connection error (session %u, err=%d) - cleaning up\n",
           COMPONENT_NAME, meta->session_id, err);

    if (meta->cleanup_in_progress) {
        DEBUG("%s: GUARD: Cleanup already in progress (prevented double-cleanup)\n",
               COMPONENT_NAME);
        return;
    }

    update_shared_connection_state();

    if (inbound_dp != NULL && !meta->close_notified) {
        InboundDataport *dp = (InboundDataport *)inbound_dp;

        bool success = control_queue_enqueue(
            &dp->close_queue,
            meta->session_id,
            (int8_t)err,  /* Pass ERR_RST (-14) or ERR_CLSD (-15) */
            0   /* flags */
        );

        if (success) {
            meta->close_notified = true;  /* Set dedup flag */

            dp->request_msg.payload_length = 0;  /* Sentinel: close-only, no payload */
            dp->request_msg.metadata.payload_length = 0;  /* v2.254: Must match payload_length! */
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

    enqueue_cleanup(meta->session_id);
}

/*
 * TCP Echo callbacks
 */

static err_t tcp_echo_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    return ERR_OK;
}

static err_t tcp_echo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (p == NULL) {
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

        if (meta->cleanup_in_progress) {
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

        update_shared_connection_state();  /* Ensure connection visible in peer_state */

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
                dp->request_msg.metadata.payload_length = 0;  /* v2.254: Must match payload_length! */
                dp->request_msg.metadata.session_id = meta->session_id;
                __sync_synchronize();

                inbound_ready_emit();
                meta->close_notified = true;

                DEBUG("%s:   [OK] Close notification queued for Net1 (session %u)\n",
                       COMPONENT_NAME, meta->session_id);
            }

            err_t close_err = tcp_close(pcb);
            if (close_err != ERR_OK) {
                DEBUG_WARN("%s:   [WARN] tcp_close failed (err=%d) - using tcp_abort\n",
                       COMPONENT_NAME, close_err);
                tcp_abort(pcb);
                /* Note: tcp_abort() doesn't return - PCB freed immediately */
            }

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

        meta->metadata_close_pending = true;
        meta->close_timestamp = sys_now();

        return ERR_OK;
    }

    err_t result = ERR_OK;  /* Default return value */

    /* Early error check - pass through to lwIP */
    if (err != ERR_OK) {
        BREADCRUMB(9001);  /* Error path */
        result = err;
        goto cleanup;  /* lwIP will handle pbuf (stores in refused_data) */
    }

    struct connection_metadata *meta_early = connection_lookup_by_pcb(pcb);

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
        result = ERR_OK;  /* v2.240: Will free pbuf at cleanup */
        goto cleanup;
    }

    bool metadata_valid_for_processing = (meta_early != NULL && meta_early->active);


    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (inbound_dp == NULL) {
        DEBUG_ERROR("%s: [ERR] FATAL: inbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        result = ERR_MEM;  /* v2.240: lwIP will store in refused_data, don't free */
        goto cleanup;
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

    struct connection_metadata *meta = meta_early;  /* v2.131: Reuse early lookup */
    if (meta != NULL && meta->active) {
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

    /* v2.250: Minimal packet flow logging */
    DEBUG_INFO("[N0→ICS] session=%u, %u bytes, port %u→%u\n",
               ics_msg->metadata.session_id, ics_msg->payload_length,
               ics_msg->metadata.src_port, ics_msg->metadata.dst_port);

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

    if (meta != NULL) {
        meta->awaiting_response = true;
    }

    /* Step 4: Signal ICS_Inbound that message is ready */
    inbound_ready_emit();

    #if DEBUG_ENABLED_DEBUG
    DEBUG("   [OK] Signal sent to ICS_Inbound - message handoff complete\n");
    #endif

    /* Tell TCP we've processed the data */
    tcp_recved(pcb, p->len);

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

            if (meta->pcb == pcb && meta->pcb != NULL) {
                tcp_output(pcb);
            } else {
                DEBUG_WARN("%s: [WARN]  tcp_echo_recv: PCB stale (meta->pcb=%p, callback pcb=%p) - skip tcp_output\n",
                       COMPONENT_NAME, meta->pcb, pcb);
            }

            meta->close_pending = true;
            meta->close_timestamp = sys_now();  /* v2.181: Record timestamp for latency measurement */

            DEBUG("%s: Response sent - close_pending=true (poll callback will close)\n",
                   COMPONENT_NAME);
        } else {
            DEBUG_WARN("%s: [WARN]  tcp_echo_recv: tcp_write failed (%d), will retry later\n",
                   COMPONENT_NAME, write_err);
        }
    }

    /* Normal path - data processed successfully */
    BREADCRUMB(9003);  /* Recv callback complete */
    result = ERR_OK;
    /* Fall through to cleanup */

cleanup:
    if (result == ERR_OK && p != NULL) {
        pbuf_free(p);
    }

    return result;
}

static err_t tcp_echo_poll(void *arg, struct tcp_pcb *pcb)
{
    struct connection_metadata *meta = (struct connection_metadata *)arg;

    if (meta == NULL) {
        /* No metadata - shouldn't happen but safe to continue */
        return ERR_OK;
    }

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

            in_dp->request_msg.payload_length = 0;  /* Sentinel: close-only, no payload */
            in_dp->request_msg.metadata.payload_length = 0;  /* v2.254: Must match payload_length! */
            in_dp->request_msg.metadata.session_id = meta->session_id;
            __sync_synchronize();  /* Memory barrier - ensure sentinel visible before signal */

            inbound_ready_emit();  /* Signal Net1 to process queue */
            meta->close_notified = true;  /* v2.219: Mark notification sent to prevent duplicates */

            DEBUG("%s:   [OK] Close notification queued for Net1 (session %u)\n",
                   COMPONENT_NAME, meta->session_id);
        } else {
            DEBUG_WARN("%s:   [WARN] inbound_dp NULL - cannot notify Net1\n", COMPONENT_NAME);
        }

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

        meta->close_pending = false;
        /* v2.252: Mark for deferred cleanup - don't enqueue yet!
         * lwIP will call tcp_echo_recv(p=NULL) when FIN handshake completes.
         * That callback will handle the actual cleanup.
         * If we enqueue now, main loop sets active=false before recv callback. */
        meta->metadata_close_pending = true;
        meta->close_timestamp = sys_now();

        DEBUG("%s:   [OK] Close initiated, waiting for tcp_echo_recv(p=NULL) callback (session %u)\n",
               COMPONENT_NAME, meta->session_id);

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

    /* v2.231: DEBUG - Check if port 62977 ever reaches accept callback */
    uint16_t local_port = newpcb->local_port;
    uint16_t remote_port = newpcb->remote_port;

    if (local_port == 62977 || remote_port == 62977) {
        DEBUG("%s: [DEBUG-62977] tcp_echo_accept CALLED for port 62977! remote=%u.%u.%u.%u:%u, local:%u\n",
               COMPONENT_NAME,
               ip4_addr1(&newpcb->remote_ip), ip4_addr2(&newpcb->remote_ip),
               ip4_addr3(&newpcb->remote_ip), ip4_addr4(&newpcb->remote_ip),
               remote_port, local_port);
    }

    if (local_port != TCP_SERVER_PORT && remote_port != TCP_SERVER_PORT) {
        DEBUG("%s: [REJECT-TCP] Non-Modbus connection from %u.%u.%u.%u:%u -> local:%u (aborting)\n",
               COMPONENT_NAME,
               ip4_addr1(&newpcb->remote_ip), ip4_addr2(&newpcb->remote_ip),
               ip4_addr3(&newpcb->remote_ip), ip4_addr4(&newpcb->remote_ip),
               remote_port, local_port);
        tcp_abort(newpcb);  /* Tell lwIP to abort and clean up */
        return ERR_ABRT;
    }

    #define MAX_SAFE_CONNECTIONS 95  /* v2.182: PCB limit is 100, stay 5 under */

    if (active_connections >= MAX_SAFE_CONNECTIONS) {
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
        return ERR_ABRT;
    }

    active_connections++;
    total_connections_created++;

    /* v2.257: ALWAYS log TCP accept at INFO level - crucial for debugging */
    DEBUG_INFO("%s: [ACCEPT] TCP from %u.%u.%u.%u:%u → port %u (active=%u)\n",
           COMPONENT_NAME,
           ip4_addr1(&newpcb->remote_ip), ip4_addr2(&newpcb->remote_ip),
           ip4_addr3(&newpcb->remote_ip), ip4_addr4(&newpcb->remote_ip),
           newpcb->remote_port, newpcb->local_port, active_connections);

    #if DEBUG_TRAFFIC
    DEBUG("%s:    → PCB address: %p, state: %d\n", COMPONENT_NAME, newpcb, newpcb->state);
    DEBUG("%s:    → Total created: %u | Total closed: %u\n",
           COMPONENT_NAME, total_connections_created, total_connections_closed);
    #endif

    tcp_setprio(newpcb, TCP_PRIO_MIN);

    /* Link PCB to connection metadata for original IP restoration
     * This associates the PCB with the metadata entry stored during RX processing
     * MUST happen BEFORE tcp_arg so we can look up the metadata */
    connection_link_pcb(newpcb, newpcb->remote_port, newpcb->local_port);

    struct connection_metadata *meta = connection_lookup_by_pcb(newpcb);

    /* Register callbacks with metadata as argument */
    tcp_arg(newpcb, meta);
    tcp_recv(newpcb, tcp_echo_recv);
    tcp_err(newpcb, tcp_echo_err);  /* Register error callback for connection cleanup */
    tcp_sent(newpcb, tcp_echo_sent);  /* v2.111: Register sent callback for pending outbound data */

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
    /* v2.250: Minimal packet flow logging - box drawing removed */

    BREADCRUMB(3001);  /* Checking dataport */

    /* CRITICAL: Check if dataport is properly mapped by CAmkES */
    if (outbound_dp == NULL) {
        BREADCRUMB(3002);  /* NULL dataport */
        DEBUG_ERROR("%s: [ERR] FATAL: outbound_dp is NULL! CAmkES dataport not mapped\n", COMPONENT_NAME);
        DEBUG("%s:    This indicates seL4 capability/memory allocation failure\n", COMPONENT_NAME);
        DEBUG("%s: ╚═══════════════════════════════════════════════════════════╝\n", COMPONENT_NAME);
        return;
    }


    OutboundDataport *dp = (OutboundDataport *)outbound_dp;
    static uint32_t error_queue_tail = 0;  /* Consumer state (local, never shared) */

    uint32_t error_queue_head = dp->error_queue.head;

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

                if (notif->err_code == ERR_RST) {
                    /* PLC sent RST packet → Mirror behavior by sending RST to SCADA */
                    DEBUG("%s:   → PLC sent RST, sending RST to SCADA (tcp_abort)\n",
                           COMPONENT_NAME);
                    tcp_abort(pcb);  /* Sends RST, frees PCB immediately */
                } else {
                    /* PLC sent FIN (ERR_CLSD), Net1 cleanup (ERR_ABRT), or unknown → Graceful close */
                    const char *reason = (notif->err_code == ERR_CLSD) ? "PLC sent FIN" :
                                       (notif->err_code == ERR_ABRT) ? "Net1 internal cleanup" :
                                       "Unknown error";
                    DEBUG("%s:   → %s, sending FIN to SCADA (tcp_close)\n",
                           COMPONENT_NAME, reason);
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
        return;
    }

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

        struct connection_metadata *scada_meta = connection_lookup_by_session_id(
            ics_msg->metadata.session_id
        );

        if (scada_meta != NULL && scada_meta->active && scada_meta->pcb != NULL) {
            struct tcp_pcb *scada_pcb = scada_meta->pcb;

            DEBUG("%s:    → Found SCADA PCB=%p (session %u)\n",
                   COMPONENT_NAME, (void*)scada_pcb, scada_meta->session_id);
            DEBUG("%s:    → Sending RST to SCADA (immediate connection rejection)\n", COMPONENT_NAME);

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
             */
            if (inbound_dp != NULL && !scada_meta->close_notified) {
                InboundDataport *dp = (InboundDataport *)inbound_dp;

                bool success = control_queue_enqueue(
                    &dp->close_queue,
                    scada_meta->session_id,
                    ERR_ABRT,  /* Forced close due to pool exhaustion */
                    0   /* flags */
                );

                if (success) {
                    scada_meta->close_notified = true;  /* Set dedup flag */

                    dp->request_msg.payload_length = 0;  /* Sentinel: close-only, no payload */
                    dp->request_msg.metadata.payload_length = 0;  /* v2.254: Must match payload_length! */
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

    /* v2.250: Minimal packet flow logging - received from ICS_Outbound */
    DEBUG_INFO("[N0←ICS] session=%u, %u bytes from PLC\n",
               ics_msg->metadata.session_id, ics_msg->payload_length);

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

    if (meta == NULL) {
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

    /* VALIDATION LAYER 1: NULL PCB Check (MUST BE FIRST!)
     * Net1 may have freed the PCB - check pointer validity before ANY dereference */
    if (meta->pcb == NULL) {
        /* PCB already freed by Net1 - drop response but keep metadata visible */
        DEBUG_WARN("%s: [WARN]  OUTBOUND: PCB is NULL - dropping response for %u.%u.%u.%u:%u\n",
               COMPONENT_NAME,
               (ics_msg->metadata.dst_ip >> 24) & 0xFF, (ics_msg->metadata.dst_ip >> 16) & 0xFF,
               (ics_msg->metadata.dst_ip >> 8) & 0xFF, ics_msg->metadata.dst_ip & 0xFF,
               ics_msg->metadata.dst_port);

        return;  /* Silent drop, metadata stays visible */
    }

    /* Final NULL check before tcp_write - this is the ONLY safe check we can do */
    if (meta->pcb == NULL) {
        DEBUG_WARN("%s: [WARN]  OUTBOUND: PCB became NULL before tcp_write!\n", COMPONENT_NAME);
        BREADCRUMB(3014);
        meta->active = false;
        return;
    }

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

    meta->last_tx_timestamp = sys_now();

    meta->response_received = true;  /* Response arrived and successfully written to TCP */

    meta->has_pending_outbound = true;  /* Signal poll callback to flush */

    /* v2.250: Minimal packet flow logging - sent to SCADA */
    DEBUG_INFO("[N0-TX] session=%u, %u bytes → SCADA\n",
               meta->session_id, ics_msg->payload_length);

    /* Clear awaiting_response flag (response delivered) */
    meta->awaiting_response = false;

    /* v2.258: FIX - Check metadata_close_pending (set when SCADA FIN received)
     * NOT awaiting_response (which just means "waiting for PLC response")
     * Bug: awaiting_response is always true after sending request, so connection
     * was incorrectly closing after every response! */
    if (meta->metadata_close_pending) {
        /* SCADA sent FIN (half-close) - close both sides after response sent */
        meta->close_pending = true;
        BREADCRUMB(3012);  /* Response sent, close pending */

        DEBUG_INFO("[N0-TX] session=%u response sent to half-closed connection - closing\n",
               meta->session_id);
    } else {
        /* SCADA still open - keep connection alive for reuse */
        BREADCRUMB(3014);  /* Response sent, connection stays open for reuse */

        DEBUG("[N0-TX] session=%u connection stays open for reuse\n", meta->session_id);
    }

    BREADCRUMB(3013);  /* Exit: outbound_ready_handle complete */
}

/*
 * VirtIO IRQ Handler
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
        
        return -1;
    }

    if (device_id != 1) {
        DEBUG("%s: ERROR: DeviceID=%u (expected 1 for network)\n", COMPONENT_NAME, device_id);
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
    DEBUG_INFO("%s: %s (%s) started\n", COMPONENT_NAME, NET0_VERSION, ICS_VERSION_DATE);

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

        /* Check for OUTBOUND notifications from ICS_Outbound (non-blocking) */
        if (outbound_ready_poll()) {
            /* CRITICAL: Ensure we see latest dataport writes from ICS_Outbound */
            __sync_synchronize();
            outbound_ready_handle();
        }

        /* Process lwIP timers and RX packets */
        /* v2.144: Reduced breadcrumb flooding - only print every 10000 iterations */
        static uint32_t loop_counter = 0;
        if (++loop_counter >= 10000) {
            loop_counter = 0;
        }
        sys_check_timeouts();

        process_cleanup_queue();

        if (rx_packets_pending) {
            rx_packets_pending = false;
            process_rx_packets();
        }

        /* Refill RX buffers OUTSIDE IRQ context to avoid IRQ storm
         * This happens in main loop after processing completes */
        refill_rx_queue();

        if (++cleanup_counter >= 100) {
            cleanup_counter = 0;
            connection_cleanup_stale();
            check_pending_cleanups();
        }

        uint32_t now = sys_now();
        if (now - last_close_wait_cleanup >= 5000) {  /* 5 seconds */
            cleanup_close_wait_connections();
            last_close_wait_cleanup = now;
        }

        seL4_Yield();
    }

    return 0;
}
