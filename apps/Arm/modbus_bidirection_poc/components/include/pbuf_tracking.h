/*
 * pbuf_tracking.h - Comprehensive lwIP PBUF Lifecycle Tracking
 *
 * v2.222: Deep inspection of pbuf allocation/free patterns to identify leaks
 *
 * Purpose:
 * - Track every pbuf allocation/free with packet content
 * - Identify what type of traffic is being leaked
 * - Correlate pbuf lifecycle with TCP connection state
 * - Detect leaked pbufs still in lwIP internal queues
 *
 * Usage:
 *   1. Include this header in both Net0 and Net1 drivers
 *   2. Call PBUF_TRACK_ALLOC() after every pbuf_alloc()
 *   3. Call PBUF_TRACK_FREE() before every pbuf_free()
 *   4. Call pbuf_tracking_periodic_diagnostics() from run() loop
 */

#ifndef PBUF_TRACKING_H
#define PBUF_TRACKING_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/ip4.h"

/* Forward declaration - sys_now() is defined in each driver */
extern uint32_t sys_now(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * PBUF Allocation Tracking Database
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define MAX_PBUF_TRACKING_ENTRIES 1600  /* Track up to 1600 concurrent pbufs */

/* Detailed pbuf tracking record */
struct pbuf_track_record {
    /* Identity */
    struct pbuf *pbuf_ptr;         /* Pointer to pbuf (NULL = free slot) */
    uint32_t alloc_timestamp;      /* When allocated (sys_now()) */
    uint32_t sequence_num;         /* Global allocation sequence number */

    /* Content inspection */
    uint8_t protocol;              /* IP protocol (TCP=6, UDP=17, ICMP=1) */
    uint32_t src_ip;               /* Source IP address */
    uint32_t dst_ip;               /* Destination IP address */
    uint16_t src_port;             /* Source port (TCP/UDP) */
    uint16_t dst_port;             /* Destination port (TCP/UDP) */
    uint16_t total_len;            /* Total packet length */
    uint16_t payload_len;          /* Payload length */

    /* TCP-specific tracking */
    uint32_t tcp_seq;              /* TCP sequence number */
    uint32_t tcp_ack;              /* TCP ACK number */
    uint8_t tcp_flags;             /* TCP flags (SYN, ACK, FIN, RST) */

    /* Allocation context */
    const char *alloc_location;    /* Where allocated (function name) */
    const char *component_name;    /* "Net0" or "Net1" */

    /* Lifecycle */
    bool freed;                    /* Has been freed? */
    uint32_t free_timestamp;       /* When freed (0 = not freed) */
    uint32_t lifetime_ms;          /* How long allocated (ms) */

    /* v2.237: Reference counting debug */
    uint16_t alloc_ref_count;      /* p->ref at allocation time */
    uint16_t current_ref_count;    /* p->ref when checked (for leak detection) */
};

/* Global tracking database */
static struct pbuf_track_record pbuf_tracking_db[MAX_PBUF_TRACKING_ENTRIES];
static uint32_t pbuf_tracking_next_slot = 0;
static uint32_t pbuf_tracking_sequence = 0;

/* Statistics */
static uint32_t pbuf_track_total_allocs = 0;
static uint32_t pbuf_track_total_frees = 0;
static uint32_t pbuf_track_db_overflows = 0;  /* Tracking DB full */

/* ═══════════════════════════════════════════════════════════════════════════
 * Packet Content Inspection Helpers
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* Extract IP header fields from pbuf
 *
 * CRITICAL: pbufs are allocated with PBUF_RAW which means p->payload points
 * to the FULL Ethernet frame (Ethernet header + IP packet).
 *
 * Ethernet header structure (14 bytes):
 *   - Destination MAC: 6 bytes
 *   - Source MAC: 6 bytes
 *   - EtherType: 2 bytes (0x0800 = IPv4, 0x0806 = ARP)
 *
 * We need to skip the 14-byte Ethernet header to reach the IP header.
 *
 * IMPORTANT: Check p->tot_len (total length of pbuf chain), not p->len
 * (length of first pbuf only). Packets can be chained across multiple pbufs.
 */
static inline void pbuf_inspect_ip_header(struct pbuf *p,
                                          uint8_t *protocol,
                                          uint32_t *src_ip,
                                          uint32_t *dst_ip,
                                          uint16_t *total_len,
                                          uint16_t *payload_len)
{
    /* Need at least 14 (Ethernet) + 20 (IP) = 34 bytes total */
    if (p == NULL || p->tot_len < 34) {
        *protocol = 0;
        *src_ip = 0;
        *dst_ip = 0;
        *total_len = 0;
        *payload_len = 0;
        return;
    }

    /* Check if first pbuf has enough data (headers must be in first pbuf) */
    if (p->len < 34) {
        /* Headers span multiple pbufs - can't reliably parse, return zeros */
        *protocol = 0;
        *src_ip = 0;
        *dst_ip = 0;
        *total_len = p->tot_len;  /* Still report total length */
        *payload_len = 0;
        return;
    }

    /* Skip 14-byte Ethernet header to reach IP header */
    #define ETHERNET_HEADER_SIZE 14
    struct ip_hdr *ip_header = (struct ip_hdr *)((uint8_t *)p->payload + ETHERNET_HEADER_SIZE);

    *protocol = IPH_PROTO(ip_header);
    *src_ip = lwip_ntohl(ip_header->src.addr);
    *dst_ip = lwip_ntohl(ip_header->dest.addr);
    *total_len = lwip_ntohs(IPH_LEN(ip_header));

    uint8_t ip_hlen = IPH_HL(ip_header) * 4;  /* IP header length in bytes */
    *payload_len = *total_len - ip_hlen;
}

/* Extract TCP header fields from pbuf
 *
 * Remember: pbuf payload includes Ethernet header (14 bytes) + IP header + TCP header
 * Also: Headers must be in first pbuf (p->len), can't parse across pbuf chains
 */
static inline void pbuf_inspect_tcp_header(struct pbuf *p,
                                           uint16_t *src_port,
                                           uint16_t *dst_port,
                                           uint32_t *seq,
                                           uint32_t *ack,
                                           uint8_t *flags)
{
    /* Need Ethernet(14) + IP(20) + TCP(20) = 54 bytes minimum (in tot_len) */
    if (p == NULL || p->tot_len < 54) {
        *src_port = 0;
        *dst_port = 0;
        *seq = 0;
        *ack = 0;
        *flags = 0;
        return;
    }

    /* Check if headers are in first pbuf (required for direct access) */
    if (p->len < 34) {  /* At least Ethernet + IP headers needed */
        *src_port = 0;
        *dst_port = 0;
        *seq = 0;
        *ack = 0;
        *flags = 0;
        return;
    }

    /* Skip Ethernet header to reach IP header */
    struct ip_hdr *ip_header = (struct ip_hdr *)((uint8_t *)p->payload + ETHERNET_HEADER_SIZE);
    uint8_t ip_hlen = IPH_HL(ip_header) * 4;

    /* Check if we have enough data in first pbuf for TCP header */
    if (p->len < ETHERNET_HEADER_SIZE + ip_hlen + 20) {
        *src_port = 0;
        *dst_port = 0;
        *seq = 0;
        *ack = 0;
        *flags = 0;
        return;
    }

    /* Skip Ethernet + IP headers to reach TCP header */
    struct tcp_hdr *tcp_header = (struct tcp_hdr *)((uint8_t *)p->payload + ETHERNET_HEADER_SIZE + ip_hlen);
    *src_port = lwip_ntohs(tcp_header->src);
    *dst_port = lwip_ntohs(tcp_header->dest);
    *seq = lwip_ntohl(tcp_header->seqno);
    *ack = lwip_ntohl(tcp_header->ackno);
    *flags = TCPH_FLAGS(tcp_header);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PBUF Tracking API
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* Initialize tracking database */
static inline void pbuf_tracking_init(void)
{
    memset(pbuf_tracking_db, 0, sizeof(pbuf_tracking_db));
    pbuf_tracking_next_slot = 0;
    pbuf_tracking_sequence = 0;
    pbuf_track_total_allocs = 0;
    pbuf_track_total_frees = 0;
    pbuf_track_db_overflows = 0;
}

/* Track pbuf allocation with full packet inspection */
static inline void pbuf_tracking_record_alloc(struct pbuf *p,
                                              const char *location,
                                              const char *component)
{
    if (p == NULL) return;

    pbuf_track_total_allocs++;

    /* Find free slot (circular buffer) */
    uint32_t start_slot = pbuf_tracking_next_slot;
    struct pbuf_track_record *rec = NULL;

    for (uint32_t i = 0; i < MAX_PBUF_TRACKING_ENTRIES; i++) {
        uint32_t slot = (start_slot + i) % MAX_PBUF_TRACKING_ENTRIES;
        if (pbuf_tracking_db[slot].pbuf_ptr == NULL) {
            rec = &pbuf_tracking_db[slot];
            pbuf_tracking_next_slot = (slot + 1) % MAX_PBUF_TRACKING_ENTRIES;
            break;
        }
    }

    if (rec == NULL) {
        /* Tracking DB full - overwrite oldest entry */
        pbuf_track_db_overflows++;
        rec = &pbuf_tracking_db[pbuf_tracking_next_slot];
        pbuf_tracking_next_slot = (pbuf_tracking_next_slot + 1) % MAX_PBUF_TRACKING_ENTRIES;
    }

    /* Record basic info */
    rec->pbuf_ptr = p;
    rec->alloc_timestamp = sys_now();
    rec->sequence_num = pbuf_tracking_sequence++;
    rec->alloc_location = location;
    rec->component_name = component;
    rec->freed = false;
    rec->free_timestamp = 0;
    rec->lifetime_ms = 0;

    /* v2.237: Capture reference count at allocation time */
    rec->alloc_ref_count = p->ref;
    rec->current_ref_count = p->ref;

    /* Inspect packet content */
    pbuf_inspect_ip_header(p, &rec->protocol, &rec->src_ip, &rec->dst_ip,
                          &rec->total_len, &rec->payload_len);

    if (rec->protocol == IP_PROTO_TCP) {
        pbuf_inspect_tcp_header(p, &rec->src_port, &rec->dst_port,
                               &rec->tcp_seq, &rec->tcp_ack, &rec->tcp_flags);
    } else if (rec->protocol == IP_PROTO_UDP) {
        pbuf_inspect_tcp_header(p, &rec->src_port, &rec->dst_port,
                               &rec->tcp_seq, &rec->tcp_ack, &rec->tcp_flags);
        /* For UDP, seq/ack/flags will be 0 */
    } else {
        rec->src_port = 0;
        rec->dst_port = 0;
        rec->tcp_seq = 0;
        rec->tcp_ack = 0;
        rec->tcp_flags = 0;
    }
}

/* Track pbuf free */
static inline void pbuf_tracking_record_free(struct pbuf *p, const char *location)
{
    if (p == NULL) return;

    pbuf_track_total_frees++;

    /* Find matching allocation record */
    for (uint32_t i = 0; i < MAX_PBUF_TRACKING_ENTRIES; i++) {
        struct pbuf_track_record *rec = &pbuf_tracking_db[i];
        if (rec->pbuf_ptr == p && !rec->freed) {
            rec->freed = true;
            rec->free_timestamp = sys_now();
            rec->lifetime_ms = rec->free_timestamp - rec->alloc_timestamp;
            rec->pbuf_ptr = NULL;  /* Mark slot as reusable */
            return;
        }
    }

    /* If we get here, pbuf was freed but not in tracking DB (overflow?) */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Diagnostic Reporting
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* Print TCP flags in human-readable format */
static inline void pbuf_tracking_print_tcp_flags(uint8_t flags)
{
    if (flags == 0) {
        printf("NONE");
        return;
    }

    bool first = true;
    if (flags & TCP_FIN) { printf("%sFIN", first ? "" : "|"); first = false; }
    if (flags & TCP_SYN) { printf("%sSYN", first ? "" : "|"); first = false; }
    if (flags & TCP_RST) { printf("%sRST", first ? "" : "|"); first = false; }
    if (flags & TCP_PSH) { printf("%sPSH", first ? "" : "|"); first = false; }
    if (flags & TCP_ACK) { printf("%sACK", first ? "" : "|"); first = false; }
    if (flags & TCP_URG) { printf("%sURG", first ? "" : "|"); first = false; }
}

/* Print IP address in dotted decimal */
static inline void pbuf_tracking_print_ip(uint32_t ip)
{
    printf("%d.%d.%d.%d",
           (int)((ip >> 24) & 0xFF),
           (int)((ip >> 16) & 0xFF),
           (int)((ip >> 8) & 0xFF),
           (int)(ip & 0xFF));
}

/* Print protocol name */
static inline const char* pbuf_tracking_protocol_name(uint8_t proto)
{
    switch (proto) {
        case IP_PROTO_ICMP: return "ICMP";
        case IP_PROTO_TCP:  return "TCP";
        case IP_PROTO_UDP:  return "UDP";
        default:            return "OTHER";
    }
}

/* Analyze leaked pbufs (never freed) */
static inline void pbuf_tracking_report_leaks(const char *component)
{
    uint32_t now = sys_now();
    uint32_t leak_count = 0;

    printf("\n[%s] ═══ PBUF LEAK ANALYSIS (t=%u) ═══\n", component, now);
    printf("[%s] Total allocations: %u, Total frees: %u, Outstanding: %d\n",
           component,
           pbuf_track_total_allocs,
           pbuf_track_total_frees,
           (int)(pbuf_track_total_allocs - pbuf_track_total_frees));
    printf("[%s] DB overflows: %u (tracking DB was full)\n", component, pbuf_track_db_overflows);

    printf("\n[%s] --- Leaked PBUFs (never freed) ---\n", component);
    for (uint32_t i = 0; i < MAX_PBUF_TRACKING_ENTRIES; i++) {
        struct pbuf_track_record *rec = &pbuf_tracking_db[i];
        if (rec->pbuf_ptr != NULL && !rec->freed) {
            leak_count++;
            uint32_t age_ms = now - rec->alloc_timestamp;

            /* v2.237: Read current ref count from pbuf (if pointer still valid) */
            rec->current_ref_count = rec->pbuf_ptr->ref;

            printf("[%s] LEAK #%u: seq=%u, age=%ums, loc=%s, p=%p, ref=%u (alloc=%u)",
                   component, leak_count, rec->sequence_num, age_ms,
                   rec->alloc_location, (void*)rec->pbuf_ptr,
                   rec->current_ref_count, rec->alloc_ref_count);

            printf(", proto=%s", pbuf_tracking_protocol_name(rec->protocol));

            if (rec->protocol == IP_PROTO_TCP || rec->protocol == IP_PROTO_UDP) {
                printf(", ");
                pbuf_tracking_print_ip(rec->src_ip);
                printf(":%u -> ", rec->src_port);
                pbuf_tracking_print_ip(rec->dst_ip);
                printf(":%u", rec->dst_port);

                if (rec->protocol == IP_PROTO_TCP) {
                    printf(", flags=");
                    pbuf_tracking_print_tcp_flags(rec->tcp_flags);
                    printf(", seq=%u, ack=%u", rec->tcp_seq, rec->tcp_ack);
                }
            }

            printf(", len=%u/%u\n", rec->payload_len, rec->total_len);
        }
    }

    if (leak_count == 0) {
        printf("[%s] ✅ NO LEAKS DETECTED!\n", component);
    } else {
        printf("[%s] ❌ TOTAL LEAKED PBUFS: %u\n", component, leak_count);
    }
    printf("[%s] ═══════════════════════════════════\n\n", component);
}

/* Report top traffic patterns in leaked pbufs */
static inline void pbuf_tracking_report_leak_patterns(const char *component)
{
    /* Count leaks by connection (src_ip:src_port -> dst_ip:dst_port) */
    struct leak_pattern {
        uint32_t src_ip;
        uint16_t src_port;
        uint32_t dst_ip;
        uint16_t dst_port;
        uint8_t protocol;
        uint32_t count;
    } patterns[32];

    uint32_t pattern_count = 0;

    for (uint32_t i = 0; i < MAX_PBUF_TRACKING_ENTRIES; i++) {
        struct pbuf_track_record *rec = &pbuf_tracking_db[i];
        if (rec->pbuf_ptr != NULL && !rec->freed) {
            /* Find or create pattern */
            bool found = false;
            for (uint32_t j = 0; j < pattern_count; j++) {
                if (patterns[j].src_ip == rec->src_ip &&
                    patterns[j].src_port == rec->src_port &&
                    patterns[j].dst_ip == rec->dst_ip &&
                    patterns[j].dst_port == rec->dst_port &&
                    patterns[j].protocol == rec->protocol) {
                    patterns[j].count++;
                    found = true;
                    break;
                }
            }

            if (!found && pattern_count < 32) {
                patterns[pattern_count].src_ip = rec->src_ip;
                patterns[pattern_count].src_port = rec->src_port;
                patterns[pattern_count].dst_ip = rec->dst_ip;
                patterns[pattern_count].dst_port = rec->dst_port;
                patterns[pattern_count].protocol = rec->protocol;
                patterns[pattern_count].count = 1;
                pattern_count++;
            }
        }
    }

    if (pattern_count == 0) {
        printf("[%s] No leak patterns (no leaks detected)\n", component);
        return;
    }

    /* Sort by count (simple bubble sort) */
    for (uint32_t i = 0; i < pattern_count - 1; i++) {
        for (uint32_t j = 0; j < pattern_count - i - 1; j++) {
            if (patterns[j].count < patterns[j+1].count) {
                struct leak_pattern temp = patterns[j];
                patterns[j] = patterns[j+1];
                patterns[j+1] = temp;
            }
        }
    }

    printf("\n[%s] --- Top Leak Patterns (by connection) ---\n", component);
    for (uint32_t i = 0; i < pattern_count && i < 10; i++) {
        printf("[%s] #%u: %s ", component, i+1, pbuf_tracking_protocol_name(patterns[i].protocol));
        pbuf_tracking_print_ip(patterns[i].src_ip);
        printf(":%u -> ", patterns[i].src_port);
        pbuf_tracking_print_ip(patterns[i].dst_ip);
        printf(":%u", patterns[i].dst_port);
        printf(" - %u leaked pbufs\n", patterns[i].count);
    }
    printf("\n");
}

/* Report lwIP internal pbuf pool usage */
static inline void pbuf_tracking_report_lwip_pools(const char *component)
{
    extern struct stats_ lwip_stats;

    printf("\n[%s] ═══ lwIP INTERNAL POOL USAGE ═══\n", component);

    /* PBUF_POOL - main packet buffer pool */
    if (lwip_stats.memp[MEMP_PBUF_POOL] != NULL) {
        uint32_t used = lwip_stats.memp[MEMP_PBUF_POOL]->used;
        uint32_t max = lwip_stats.memp[MEMP_PBUF_POOL]->max;
        uint32_t avail = lwip_stats.memp[MEMP_PBUF_POOL]->avail;
        uint32_t err = lwip_stats.memp[MEMP_PBUF_POOL]->err;
        float usage_pct = (max > 0) ? (float)used / max * 100.0f : 0.0f;

        printf("[%s] PBUF_POOL: %u/%u used (%.1f%%), avail=%u, alloc_errors=%u",
               component, used, max, usage_pct, avail, err);
        if (usage_pct > 80.0f) {
            printf(" ⚠️  HIGH USAGE!");
        }
        if (err > 0) {
            printf(" ❌ ALLOCATION FAILURES!");
        }
        printf("\n");
    }

    /* PBUF - pbuf header pool */
    if (lwip_stats.memp[MEMP_PBUF] != NULL) {
        uint32_t used = lwip_stats.memp[MEMP_PBUF]->used;
        uint32_t max = lwip_stats.memp[MEMP_PBUF]->max;
        printf("[%s] PBUF (headers): %u/%u used\n", component, used, max);
    }

    /* TCP PCB pools */
    if (lwip_stats.memp[MEMP_TCP_PCB] != NULL) {
        uint32_t used = lwip_stats.memp[MEMP_TCP_PCB]->used;
        uint32_t max = lwip_stats.memp[MEMP_TCP_PCB]->max;
        printf("[%s] TCP_PCB (active): %u/%u used\n", component, used, max);
    }

    if (lwip_stats.memp[MEMP_TCP_SEG] != NULL) {
        uint32_t used = lwip_stats.memp[MEMP_TCP_SEG]->used;
        uint32_t max = lwip_stats.memp[MEMP_TCP_SEG]->max;
        uint32_t err = lwip_stats.memp[MEMP_TCP_SEG]->err;
        printf("[%s] TCP_SEG (segments): %u/%u used, errors=%u", component, used, max, err);
        if (err > 0) {
            printf(" ❌ ALLOCATION FAILURES!");
        }
        printf("\n");
    }

    printf("[%s] ═════════════════════════════════\n\n", component);
}

/* Report TCP connection state (PCB lists) */
static inline void pbuf_tracking_report_tcp_state(const char *component)
{
    /* Count active TCP PCBs */
    extern struct tcp_pcb *tcp_active_pcbs;
    extern struct tcp_pcb *tcp_tw_pcbs;  /* TIME_WAIT state */

    uint32_t active_count = 0;
    uint32_t tw_count = 0;

    struct tcp_pcb *pcb = tcp_active_pcbs;
    while (pcb != NULL) {
        active_count++;
        pcb = pcb->next;
    }

    pcb = tcp_tw_pcbs;
    while (pcb != NULL) {
        tw_count++;
        pcb = pcb->next;
    }

    printf("[%s] TCP Connection State: active=%u, TIME_WAIT=%u\n",
           component, active_count, tw_count);
}

/* Periodic diagnostics (call from run() loop every ~10 seconds) */
static inline void pbuf_tracking_periodic_diagnostics(const char *component, uint32_t interval_ms)
{
    static uint32_t last_report_time = 0;
    uint32_t now = sys_now();

    if (now - last_report_time < interval_ms) {
        return;  /* Not time yet */
    }

    last_report_time = now;

    /* lwIP internal pool usage */
    pbuf_tracking_report_lwip_pools(component);

    /* TCP connection state */
    pbuf_tracking_report_tcp_state(component);

    /* Full leak analysis */
    pbuf_tracking_report_leaks(component);
    pbuf_tracking_report_leak_patterns(component);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Convenience Macros
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define PBUF_TRACK_ALLOC(p, component) \
    pbuf_tracking_record_alloc(p, __func__, component)

#define PBUF_TRACK_FREE(p) \
    pbuf_tracking_record_free(p, __func__)

#endif /* PBUF_TRACKING_H */
