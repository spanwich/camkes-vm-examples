/*
 * ICS_Outbound - Internal to External Validation Component
 *
 * Validates traffic from internal network (VirtIO_Net1_Driver) before
 * forwarding to external network (VirtIO_Net0_Driver).
 *
 * Phase 1: Pass-through with comprehensive logging and metadata inspection
 * Phase 2: Add policy rules, EverParse validation, rate limiting
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <camkes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "common.h"

/* Debug output control - set to 0 for silent mode (breadcrumbs only) */
#define DEBUG_SILENT 1

#if DEBUG_SILENT
    #define DEBUG_PRINTF(...) do {} while(0)
#else
    #define DEBUG_PRINTF printf
#endif

/* Global timestamp counter definition */
uint64_t global_timestamp_counter = 0;

/* Component statistics */
static ComponentStats stats;

/* Protocol-specific counters */
static uint64_t tcp_messages = 0;
static uint64_t udp_messages = 0;
static uint64_t arp_messages = 0;
static uint64_t other_messages = 0;

/*
 * Print frame metadata for debugging
 */
static void print_frame_metadata(const FrameMetadata *meta) {
    DEBUG_PRINTF("ICS_Outbound: Frame Metadata:\n");
    DEBUG_PRINTF("  EtherType: 0x%04X\n", meta->ethertype);
    DEBUG_PRINTF("  Src MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           meta->src_mac[0], meta->src_mac[1], meta->src_mac[2],
           meta->src_mac[3], meta->src_mac[4], meta->src_mac[5]);
    DEBUG_PRINTF("  Dst MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           meta->dst_mac[0], meta->dst_mac[1], meta->dst_mac[2],
           meta->dst_mac[3], meta->dst_mac[4], meta->dst_mac[5]);

    if (meta->is_ip) {
        DEBUG_PRINTF("  IP Protocol: %u (", meta->ip_protocol);
        if (meta->is_tcp) DEBUG_PRINTF("TCP");
        else if (meta->is_udp) DEBUG_PRINTF("UDP");
        else DEBUG_PRINTF("Other");
        DEBUG_PRINTF(")\n");
        DEBUG_PRINTF("  Src IP: %u.%u.%u.%u\n",
               (meta->src_ip >> 24) & 0xFF, (meta->src_ip >> 16) & 0xFF,
               (meta->src_ip >> 8) & 0xFF, meta->src_ip & 0xFF);
        DEBUG_PRINTF("  Dst IP: %u.%u.%u.%u\n",
               (meta->dst_ip >> 24) & 0xFF, (meta->dst_ip >> 16) & 0xFF,
               (meta->dst_ip >> 8) & 0xFF, meta->dst_ip & 0xFF);

        if (meta->is_tcp || meta->is_udp) {
            DEBUG_PRINTF("  Src Port: %u\n", meta->src_port);
            DEBUG_PRINTF("  Dst Port: %u\n", meta->dst_port);
        }
    } else if (meta->is_arp) {
        DEBUG_PRINTF("  ARP packet\n");
    }

    DEBUG_PRINTF("  Payload: offset=%u, length=%u\n",
           meta->payload_offset, meta->payload_length);
}

/*
 * Validate ICS message (Phase 1: pass-through with logging)
 */
static bool validate_message(const ICS_Message *msg) {
    const FrameMetadata *meta = &msg->metadata;

    /* Basic validation */
    if (msg->payload_length > MAX_PAYLOAD_SIZE) {
        DEBUG_PRINTF("ICS_Outbound: REJECT - Payload too large (%u > %u)\n",
               msg->payload_length, MAX_PAYLOAD_SIZE);
        return false;
    }

    if (msg->payload_length != meta->payload_length) {
        DEBUG_PRINTF("ICS_Outbound: REJECT - Payload length mismatch (msg=%u, meta=%u)\n",
               msg->payload_length, meta->payload_length);
        return false;
    }

    /* EverParse validation hook (Phase 1: no-op) */
    if (msg->payload_length > 0) {
        if (!everparse_validate(msg->payload, msg->payload_length)) {
            DEBUG_PRINTF("ICS_Outbound: REJECT - EverParse validation failed\n");
            return false;
        }
    }

    /* Update protocol counters */
    if (meta->is_tcp) tcp_messages++;
    else if (meta->is_udp) udp_messages++;
    else if (meta->is_arp) arp_messages++;
    else other_messages++;

    /* Phase 1: Allow all valid messages */
    DEBUG_PRINTF("ICS_Outbound: ALLOW - Message passed validation\n");
    return true;
}

/*
 * Process one message from input dataport
 */
static bool process_message(void) {
    /* v2.161: Cast to OutboundDataport* to access both response_msg AND error_queue */
    OutboundDataport *in_dataport = (OutboundDataport *)in_dp;
    ICS_Message *in_msg = &in_dataport->response_msg;

    /* Basic bounds check */
    if (!basic_bounds_check(in_msg, sizeof(Buf))) {
        DEBUG_PRINTF("ICS_Outbound: ERROR - Bounds check failed\n");
        stats.messages_dropped++;
        return false;
    }

    stats.messages_received++;

    /* Print metadata for debugging */
    print_frame_metadata(&in_msg->metadata);

    /* Validate message */
    if (!validate_message(in_msg)) {
        stats.messages_dropped++;
        return true;  /* Message consumed but rejected */
    }

    /* Forward to output dataport */
    OutboundDataport *out_dataport = (OutboundDataport *)out_dp;
    ICS_Message *out_msg = &out_dataport->response_msg;
    memcpy(out_msg, in_msg, sizeof(FrameMetadata) + sizeof(uint16_t) + in_msg->payload_length);

    /* v2.161: CRITICAL FIX - Forward error_queue from Net1 to Net0
     * ═══════════════════════════════════════════════════════════════════════
     * Bug: Net1 writes error notifications to ics_outbound.in_dp->error_queue
     *      Net0 reads error notifications from ics_outbound.out_dp->error_queue
     *      ICS_Outbound wasn't forwarding error_queue → Net0 never saw notifications!
     *
     * Fix: Copy entire error_queue structure from in_dp to out_dp
     *      This forwards all pending error notifications to Net0
     *
     * Memory barrier: Ensures Net0 sees updated error_queue.head
     * ═══════════════════════════════════════════════════════════════════════
     */
    memcpy((void*)&out_dataport->error_queue,
           (void*)&in_dataport->error_queue,
           sizeof(struct control_queue));

    /* CRITICAL: Force cache flush before notification to ensure Net0 sees latest data
     * v2.161: This barrier now covers BOTH response_msg AND error_queue */
    __sync_synchronize();

    /* Signal VirtIO_Net0_Driver */
    out_ntfy_emit();

    stats.messages_forwarded++;
    stats.bytes_processed += sizeof(FrameMetadata) + sizeof(uint16_t) + in_msg->payload_length;

    DEBUG_PRINTF("ICS_Outbound: Forwarded message to external network\n");
    return true;
}

/*
 * Notification handler - called when VirtIO_Net1_Driver has data
 */
void in_ntfy_handle(void) {
    stats.last_activity_time = get_timestamp();

    if (process_message()) {
        /* Print periodic stats */
        static uint32_t stats_counter = 0;
        if (++stats_counter % 10 == 0) {
            DEBUG_PRINTF("\n=== ICS_Outbound Statistics ===\n");
            DEBUG_PRINTF("Received: %llu, Forwarded: %llu, Dropped: %llu\n",
                   stats.messages_received, stats.messages_forwarded, stats.messages_dropped);
            DEBUG_PRINTF("TCP: %llu, UDP: %llu, ARP: %llu, Other: %llu\n",
                   tcp_messages, udp_messages, arp_messages, other_messages);
            DEBUG_PRINTF("===============================\n\n");
        }
    }
}

/*
 * Component initialization
 */
void pre_init(void) {
    memset(&stats, 0, sizeof(stats));
    tcp_messages = udp_messages = arp_messages = other_messages = 0;
    DEBUG_PRINTF("ICS_Outbound: Initializing internal→external validation...\n");
    DEBUG_PRINTF("ICS_Outbound: 🔖 SOFTWARE VERSION: v2.161 (2025-10-25)\n");
    DEBUG_PRINTF("ICS_Outbound: 🔧 Features: Metadata logging + EverParse validation hooks + error_queue forwarding\n");
    DEBUG_PRINTF("ICS_Outbound: 📊 Protocols: TCP, UDP, ARP detection\n");
    DEBUG_PRINTF("ICS_Outbound: ✅ CRITICAL FIX: Forwards error_queue from Net1 to Net0 (fixes error notification delivery)\n\n");
}

int run(void) {
    DEBUG_PRINTF("ICS_Outbound: Ready to validate internal→external traffic\n");
    DEBUG_PRINTF("ICS_Outbound: Phase 1 - Pass-through with comprehensive logging\n");

    /* Main event loop */
    while (1) {
        in_ntfy_wait();
        in_ntfy_handle();
    }

    return 0;
}
