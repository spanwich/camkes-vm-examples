/*
 * ICS_Inbound - External to Internal Validation Component
 *
 * Validates traffic from external network (VirtIO_Net0_Driver) before
 * forwarding to internal network (VirtIO_Net1_Driver).
 *
 * Architecture:
 * - Receives metadata + payload from NET0
 * - Validates frame integrity and protocol
 * - Forwards close_queue notifications from NET0 to NET1
 * - Performs EverParse validation (future: add policy rules)
 *
 * Stable since v2.240 (2025-11-02)
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define DEBUG_LEVEL DEBUG_LEVEL_INFO  /* v2.255: Reduced - sentinel fix verified */
#include "debug_levels.h"

#include <camkes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "common.h"
#include "version.h"  /* v2.241: Unified version management */

/* Global timestamp counter definition */
uint64_t global_timestamp_counter = 0;

/*
 * Policy Enforcement Configuration (v2.270)
 *
 * These globals control the address policy validation layer.
 * Set g_policy_enabled = true to enforce address range checks.
 *
 * CVE-2022-0367 MITIGATION:
 * When enabled, the policy layer validates that ALL Modbus addresses
 * (including write_address in FC 0x17) are within the configured range.
 */
modbus_policy_t g_modbus_policy;
bool g_policy_enabled = true;  /* Enable policy enforcement by default */

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
    DEBUG("ICS_Inbound: Frame Metadata:\n");
    DEBUG("  EtherType: 0x%04X\n", meta->ethertype);
    DEBUG("  Src MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           meta->src_mac[0], meta->src_mac[1], meta->src_mac[2],
           meta->src_mac[3], meta->src_mac[4], meta->src_mac[5]);
    DEBUG("  Dst MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           meta->dst_mac[0], meta->dst_mac[1], meta->dst_mac[2],
           meta->dst_mac[3], meta->dst_mac[4], meta->dst_mac[5]);

    if (meta->is_ip) {
        DEBUG("  IP Protocol: %u (", meta->ip_protocol);
        if (meta->is_tcp) DEBUG("TCP");
        else if (meta->is_udp) DEBUG("UDP");
        else DEBUG("Other");
        DEBUG(")\n");
        DEBUG("  Src IP: %u.%u.%u.%u\n",
               (meta->src_ip >> 24) & 0xFF, (meta->src_ip >> 16) & 0xFF,
               (meta->src_ip >> 8) & 0xFF, meta->src_ip & 0xFF);
        DEBUG("  Dst IP: %u.%u.%u.%u\n",
               (meta->dst_ip >> 24) & 0xFF, (meta->dst_ip >> 16) & 0xFF,
               (meta->dst_ip >> 8) & 0xFF, meta->dst_ip & 0xFF);

        if (meta->is_tcp || meta->is_udp) {
            DEBUG("  Src Port: %u\n", meta->src_port);
            DEBUG("  Dst Port: %u\n", meta->dst_port);
        }
    } else if (meta->is_arp) {
        DEBUG("  ARP packet\n");
    }

    DEBUG("  Payload: offset=%u, length=%u\n",
           meta->payload_offset, meta->payload_length);
}

/*
 * Validate ICS message
 */
static bool validate_message(const ICS_Message *msg) {
    const FrameMetadata *meta = &msg->metadata;

    /* Basic validation */
    if (msg->payload_length > MAX_PAYLOAD_SIZE) {
        DEBUG_ERROR("ICS_Inbound: REJECT - Payload too large (%u > %u)\n",
                    msg->payload_length, MAX_PAYLOAD_SIZE);
        return false;
    }

    if (msg->payload_length != meta->payload_length) {
        DEBUG_ERROR("ICS_Inbound: REJECT - Payload length mismatch (msg=%u, meta=%u)\n",
                    msg->payload_length, meta->payload_length);
        return false;
    }

    /* EverParse validation hook */
    if (msg->payload_length > 0) {
        if (!everparse_validate(msg->payload, msg->payload_length)) {
            DEBUG_ERROR("ICS_Inbound: REJECT - EverParse validation failed\n");
            return false;
        }
    }

    /* Update protocol counters */
    if (meta->is_tcp) tcp_messages++;
    else if (meta->is_udp) udp_messages++;
    else if (meta->is_arp) arp_messages++;
    else other_messages++;

    return true;
}

/*
 * Process one message from input dataport
 */
static bool process_message(void) {
    InboundDataport *in_dataport = (InboundDataport *)in_dp;
    ICS_Message *in_msg = &in_dataport->request_msg;

    /* Basic bounds check */
    if (!basic_bounds_check(in_msg, sizeof(Buf))) {
        DEBUG_ERROR("ICS_Inbound: ERROR - Bounds check failed\n");
        stats.messages_dropped++;
        return false;
    }

    stats.messages_received++;

    /* v2.250: Minimal packet flow logging */
    DEBUG_INFO("[ICS-IN] session=%u, %u bytes\n",
               in_msg->metadata.session_id, in_msg->payload_length);

    /* Print metadata for debugging (only at DEBUG level) */
    #if DEBUG_ENABLED_DEBUG
    print_frame_metadata(&in_msg->metadata);
    #endif

    /* Validate message */
    if (!validate_message(in_msg)) {
        DEBUG_INFO("[ICS-IN] REJECT session=%u\n", in_msg->metadata.session_id);
        stats.messages_dropped++;
        return true;  /* Message consumed but rejected */
    }

    DEBUG_INFO("[ICS-IN] PASS session=%u → Net1\n", in_msg->metadata.session_id);

    /* Forward to output dataport */
    InboundDataport *out_dataport = (InboundDataport *)out_dp;
    ICS_Message *out_msg = &out_dataport->request_msg;
    memcpy(out_msg, in_msg, sizeof(FrameMetadata) + sizeof(uint16_t) + in_msg->payload_length);

    /*
     * Forward close_queue from Net0 to Net1
     * Net0 writes close notifications to in_dp->close_queue
     * Net1 reads close notifications from out_dp->close_queue
     * Memory barrier ensures Net1 sees updated close_queue.head
     */
    memcpy((void*)&out_dataport->close_queue,
           (void*)&in_dataport->close_queue,
           sizeof(struct control_queue));

    __sync_synchronize();

    /* Signal VirtIO_Net1_Driver */
    out_ntfy_emit();

    stats.messages_forwarded++;
    stats.bytes_processed += sizeof(FrameMetadata) + sizeof(uint16_t) + in_msg->payload_length;

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
            DEBUG("\n=== ICS_Inbound Statistics ===\n");
            DEBUG("Received: %llu, Forwarded: %llu, Dropped: %llu\n",
                   stats.messages_received, stats.messages_forwarded, stats.messages_dropped);
            DEBUG("TCP: %llu, UDP: %llu, ARP: %llu, Other: %llu\n",
                   tcp_messages, udp_messages, arp_messages, other_messages);
            DEBUG("==============================\n\n");
        }
    }
}

/*
 * Component initialization
 */
void pre_init(void) {
    memset(&stats, 0, sizeof(stats));
    tcp_messages = udp_messages = arp_messages = other_messages = 0;

    /*
     * Initialize Modbus address policy (v2.270)
     *
     * CVE-2022-0367 TEST CONFIGURATION:
     * - Holding registers: 100-109 (matches PLC START_REGISTERS=100, NB_REGISTERS=10)
     * - Attack with write_address=50 will be REJECTED
     *
     * For production, configure based on actual PLC memory mapping.
     */
    modbus_policy_init_cve_test(&g_modbus_policy);
    g_policy_enabled = true;

    DEBUG_INFO("%s (%s) - Stable external→internal validation\n", ICS_INBOUND_VERSION, ICS_VERSION_DATE);
    DEBUG_INFO("Policy: holding_reg=%u-%u, coil=%u-%u, enabled=%s\n",
               g_modbus_policy.holding_reg_start, g_modbus_policy.holding_reg_end,
               g_modbus_policy.coil_start, g_modbus_policy.coil_end,
               g_policy_enabled ? "YES" : "NO");
}

int run(void) {
    DEBUG_INFO("ICS_Inbound: Ready to validate external→internal traffic\n");

    /* Main event loop */
    while (1) {
        in_ntfy_wait();
        in_ntfy_handle();
    }

    return 0;
}
