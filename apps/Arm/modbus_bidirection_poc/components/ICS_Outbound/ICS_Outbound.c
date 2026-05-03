/*
 * ICS_Outbound - Internal to External Validation Component
 *
 * Validates traffic from internal network (VirtIO_Net1_Driver) before
 * forwarding to external network (VirtIO_Net0_Driver).
 *
 * v2.300: SYMMETRIC BIDIRECTIONAL VALIDATION
 * ==========================================
 * Implements Zero Trust ICS architecture - internal devices are NOT trusted.
 * Uses F*-verified response validators to prevent:
 *   - Data Exfiltration: Limits response data size
 *   - Address Confusion: Validates echoed addresses in write responses
 *   - Covert Channels: Strict structure validation
 *   - Exception Abuse: Validates exception codes
 *
 * Verification Architecture:
 *   - EverParse (F*): Response structure AND policy verification
 *   - Symmetric with ICS_Inbound: Both directions use F* validation
 *   - Response-specific validators: Different structure from requests
 *
 * Stable since v2.240 (2025-11-02)
 * Symmetric since v2.300 (2026-01-26)
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define DEBUG_LEVEL DEBUG_LEVEL_INFO
#include "debug_levels.h"

#include <camkes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "common.h"
#include "modbus_response_policy_fstar.h"  /* v2.300: F* response validation */

/* Global timestamp counter definition */
uint64_t global_timestamp_counter = 0;

/* Component statistics */
static ComponentStats stats;

/* Protocol-specific counters */
static uint64_t tcp_messages = 0;
static uint64_t udp_messages = 0;
static uint64_t arp_messages = 0;
static uint64_t other_messages = 0;

/* Response validation counters (v2.300) */
static uint64_t data_responses = 0;
static uint64_t echo_responses = 0;
static uint64_t exception_responses = 0;

/*
 * Print frame metadata for debugging
 */
static void print_frame_metadata(const FrameMetadata *meta) {
    DEBUG("ICS_Outbound: Frame Metadata:\n");
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
 *
 * v2.300: SYMMETRIC F*-VERIFIED RESPONSE VALIDATION
 * =================================================
 * Uses modbus_validate_response_with_policy() which validates:
 *   - Data responses: ByteCount <= MaxResponseBytes (exfiltration prevention)
 *   - Echo responses: Echoed address within policy range
 *   - Exception responses: Valid exception code
 *
 * ZERO TRUST: Internal PLC is NOT assumed to be trustworthy.
 */
static bool validate_message(const ICS_Message *msg) {
    const FrameMetadata *meta = &msg->metadata;

    /* Basic validation */
    if (msg->payload_length > MAX_PAYLOAD_SIZE) {
        DEBUG_ERROR("ICS_Outbound: REJECT - Payload too large (%u > %u)\n",
                    msg->payload_length, MAX_PAYLOAD_SIZE);
        return false;
    }

    if (msg->payload_length != meta->payload_length) {
        DEBUG_ERROR("ICS_Outbound: REJECT - Payload length mismatch (msg=%u, meta=%u)\n",
                    msg->payload_length, meta->payload_length);
        return false;
    }

    /*
     * F*-VERIFIED RESPONSE VALIDATION (v2.300):
     *
     * Single call validates response structure AND policy using EverParse/F*.
     * Routes to appropriate validator based on function code:
     *   - FC 0x01-0x04, 0x17: Data response (MaxResponseBytes check)
     *   - FC 0x05, 0x06, 0x0F, 0x10: Echo response (address range check)
     *   - FC | 0x80: Exception response (exception code whitelist)
     *
     * SECURITY: Prevents data exfiltration, address confusion, and exception abuse.
     */
    if (msg->payload_length > 0) {
        uint8_t result = modbus_validate_response_with_policy(msg->payload, msg->payload_length);

        switch (result) {
            case FSTAR_RESPONSE_OK:
                /* Passed response validation */
                /* Track response type for statistics */
                if (msg->payload_length >= 8) {
                    uint8_t fc = msg->payload[7];
                    if (fc & 0x80) {
                        exception_responses++;
                    } else if (fc == 0x05 || fc == 0x06 || fc == 0x0F || fc == 0x10) {
                        echo_responses++;
                    } else {
                        data_responses++;
                    }
                }
                break;

            case FSTAR_RESPONSE_SYNTAX_ERROR:
                DEBUG_ERROR("ICS_Outbound: REJECT - F* response syntax validation failed\n");
                return false;

            case FSTAR_RESPONSE_POLICY_REJECT:
                DEBUG_ERROR("ICS_Outbound: REJECT - F* response policy violation\n");
                return false;

            case FSTAR_RESPONSE_INVALID_FC:
                DEBUG_ERROR("ICS_Outbound: REJECT - Invalid response FC\n");
                return false;

            case FSTAR_RESPONSE_TOO_SHORT:
                DEBUG_ERROR("ICS_Outbound: REJECT - Response too short for F*\n");
                return false;

            case FSTAR_RESPONSE_INVALID_EXCEPTION:
                DEBUG_ERROR("ICS_Outbound: REJECT - Invalid exception code\n");
                return false;

            default:
                DEBUG_ERROR("ICS_Outbound: REJECT - Unknown F* response result %u\n", result);
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
    OutboundDataport *in_dataport = (OutboundDataport *)in_dp;
    ICS_Message *in_msg = &in_dataport->response_msg;

    /* Basic bounds check */
    if (!basic_bounds_check(in_msg, sizeof(Buf))) {
        DEBUG_ERROR("ICS_Outbound: ERROR - Bounds check failed\n");
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

    /* Forward error_queue from Net1 to Net0 */
    memcpy((void*)&out_dataport->error_queue,
           (void*)&in_dataport->error_queue,
           sizeof(struct control_queue));

    /* Memory barrier to ensure data visibility before notification */
    __sync_synchronize();

    /* Signal VirtIO_Net0_Driver */
    out_ntfy_emit();

    stats.messages_forwarded++;
    stats.bytes_processed += sizeof(FrameMetadata) + sizeof(uint16_t) + in_msg->payload_length;

    DEBUG("ICS_Outbound: Forwarded message to external network\n");
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
            DEBUG("\n=== ICS_Outbound Statistics ===\n");
            DEBUG("Received: %llu, Forwarded: %llu, Dropped: %llu\n",
                   stats.messages_received, stats.messages_forwarded, stats.messages_dropped);
            DEBUG("TCP: %llu, UDP: %llu, ARP: %llu, Other: %llu\n",
                   tcp_messages, udp_messages, arp_messages, other_messages);
            DEBUG("Responses - Data: %llu, Echo: %llu, Exception: %llu\n",
                   data_responses, echo_responses, exception_responses);
            DEBUG("===============================\n\n");
        }
    }
}

/*
 * Component initialization
 */
void pre_init(void) {
    memset(&stats, 0, sizeof(stats));
    tcp_messages = udp_messages = arp_messages = other_messages = 0;
    data_responses = echo_responses = exception_responses = 0;

    /*
     * Initialize Modbus response policy (v2.300)
     *
     * SYMMETRIC VALIDATION:
     * Response policy should match request policy for consistency.
     * Uses same address ranges as ICS_Inbound for echo validation.
     *
     * CVE-2022-0367 TEST CONFIGURATION:
     * - MaxResponseBytes: 250 (protocol max)
     * - Holding registers: 100-109 (matches inbound policy)
     *
     * For production, synchronize with ICS_Inbound policy configuration.
     */
    fstar_response_policy_init_cve_test();

    DEBUG_INFO("ICS_Outbound: v2.300 (2026-01-26) - Symmetric F* response validation (Zero Trust)\n");
    DEBUG_INFO("Response Policy: max_bytes=%u, holding_reg=%u-%u (F* verified)\n",
               g_response_max_bytes,
               g_response_holding_reg_start, g_response_holding_reg_end);
}

int run(void) {
    DEBUG_INFO("ICS_Outbound: Ready to validate internal→external traffic (symmetric)\n");

    /* Main event loop */
    while (1) {
        in_ntfy_wait();
        in_ntfy_handle();
    }

    return 0;
}
