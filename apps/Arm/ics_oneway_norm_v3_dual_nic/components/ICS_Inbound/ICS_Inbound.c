/*
 * ICS_Inbound - External to Internal Validation Component
 *
 * Validates traffic from external network (VirtIO_Net0_Driver) before
 * forwarding to internal network (VirtIO_Net1_Driver).
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
    printf("ICS_Inbound: Frame Metadata:\n");
    printf("  EtherType: 0x%04X\n", meta->ethertype);
    printf("  Src MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           meta->src_mac[0], meta->src_mac[1], meta->src_mac[2],
           meta->src_mac[3], meta->src_mac[4], meta->src_mac[5]);
    printf("  Dst MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           meta->dst_mac[0], meta->dst_mac[1], meta->dst_mac[2],
           meta->dst_mac[3], meta->dst_mac[4], meta->dst_mac[5]);

    if (meta->is_ip) {
        printf("  IP Protocol: %u (", meta->ip_protocol);
        if (meta->is_tcp) printf("TCP");
        else if (meta->is_udp) printf("UDP");
        else printf("Other");
        printf(")\n");
        printf("  Src IP: %u.%u.%u.%u\n",
               (meta->src_ip >> 24) & 0xFF, (meta->src_ip >> 16) & 0xFF,
               (meta->src_ip >> 8) & 0xFF, meta->src_ip & 0xFF);
        printf("  Dst IP: %u.%u.%u.%u\n",
               (meta->dst_ip >> 24) & 0xFF, (meta->dst_ip >> 16) & 0xFF,
               (meta->dst_ip >> 8) & 0xFF, meta->dst_ip & 0xFF);

        if (meta->is_tcp || meta->is_udp) {
            printf("  Src Port: %u\n", meta->src_port);
            printf("  Dst Port: %u\n", meta->dst_port);
        }
    } else if (meta->is_arp) {
        printf("  ARP packet\n");
    }

    printf("  Payload: offset=%u, length=%u\n",
           meta->payload_offset, meta->payload_length);
}

/*
 * Validate ICS message (Phase 1: pass-through with logging)
 */
static bool validate_message(const ICS_Message *msg) {
    const FrameMetadata *meta = &msg->metadata;

    /* Basic validation */
    if (msg->payload_length > MAX_PAYLOAD_SIZE) {
        printf("ICS_Inbound: REJECT - Payload too large (%u > %u)\n",
               msg->payload_length, MAX_PAYLOAD_SIZE);
        return false;
    }

    if (msg->payload_length != meta->payload_length) {
        printf("ICS_Inbound: REJECT - Payload length mismatch (msg=%u, meta=%u)\n",
               msg->payload_length, meta->payload_length);
        return false;
    }

    /* EverParse validation hook (Phase 1: no-op) */
    if (msg->payload_length > 0) {
        if (!everparse_validate(msg->payload, msg->payload_length)) {
            printf("ICS_Inbound: REJECT - EverParse validation failed\n");
            return false;
        }
    }

    /* Update protocol counters */
    if (meta->is_tcp) tcp_messages++;
    else if (meta->is_udp) udp_messages++;
    else if (meta->is_arp) arp_messages++;
    else other_messages++;

    /* Phase 1: Allow all valid messages */
    printf("ICS_Inbound: ALLOW - Message passed validation\n");
    return true;
}

/*
 * Process one message from input dataport
 */
static bool process_message(void) {
    ICS_Message *in_msg = (ICS_Message *)in_dp;

    /* Basic bounds check */
    if (!basic_bounds_check(in_msg, sizeof(Buf))) {
        printf("ICS_Inbound: ERROR - Bounds check failed\n");
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
    ICS_Message *out_msg = (ICS_Message *)out_dp;
    memcpy(out_msg, in_msg, sizeof(FrameMetadata) + sizeof(uint16_t) + in_msg->payload_length);

    /* Signal VirtIO_Net1_Driver */
    out_ntfy_emit();

    stats.messages_forwarded++;
    stats.bytes_processed += sizeof(FrameMetadata) + sizeof(uint16_t) + in_msg->payload_length;

    printf("ICS_Inbound: Forwarded message to internal network\n");
    return true;
}

/*
 * Notification handler - called when VirtIO_Net0_Driver has data
 */
void in_ntfy_handle(void) {
    stats.last_activity_time = get_timestamp();

    if (process_message()) {
        /* Print periodic stats */
        static uint32_t stats_counter = 0;
        if (++stats_counter % 10 == 0) {
            printf("\n=== ICS_Inbound Statistics ===\n");
            printf("Received: %llu, Forwarded: %llu, Dropped: %llu\n",
                   stats.messages_received, stats.messages_forwarded, stats.messages_dropped);
            printf("TCP: %llu, UDP: %llu, ARP: %llu, Other: %llu\n",
                   tcp_messages, udp_messages, arp_messages, other_messages);
            printf("==============================\n\n");
        }
    }
}

/*
 * Component initialization
 */
void pre_init(void) {
    memset(&stats, 0, sizeof(stats));
    tcp_messages = udp_messages = arp_messages = other_messages = 0;
    printf("ICS_Inbound: Initializing external→internal validation...\n");
}

int run(void) {
    printf("ICS_Inbound: Ready to validate external→internal traffic\n");
    printf("ICS_Inbound: Phase 1 - Pass-through with comprehensive logging\n");

    /* Main event loop */
    while (1) {
        in_ntfy_wait();
        in_ntfy_handle();
    }

    return 0;
}
