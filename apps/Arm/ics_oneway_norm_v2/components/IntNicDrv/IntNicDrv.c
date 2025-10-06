/*
 * Internal NIC Driver Implementation
 *
 * Message sink and monitoring component for the ICS one-way pipeline.
 * Receives all approved messages and maintains comprehensive statistics.
 * Simulates forwarding to internal network infrastructure.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <camkes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "common.h"
#include "ringbuf.h"

/* Global state for this component */
static RingBuffer* input_buffer;
static ComponentStats stats;
static uint8_t payload_buffer[MAX_PAYLOAD_SIZE];

/* Global timestamp counter definition */
uint64_t global_timestamp_counter = 0;

/* Protocol-specific statistics */
typedef struct {
    uint64_t modbus_messages;
    uint64_t dnp3_messages;
    uint64_t ethernet_ip_messages;
    uint64_t generic_messages;
    uint64_t unknown_messages;
    uint64_t total_payload_bytes;
} ProtocolStats;

static ProtocolStats protocol_stats;

/* Message rate tracking for performance monitoring */
#define RATE_WINDOW_SIZE 60  /* Track rates over 60 seconds */
static uint32_t message_rates[RATE_WINDOW_SIZE];
static uint32_t rate_window_index = 0;
static uint64_t last_rate_reset = 0;

/*
 * Update protocol-specific statistics
 */
static void update_protocol_stats(uint16_t tag, uint16_t payload_len) {
    protocol_stats.total_payload_bytes += payload_len;

    switch (tag) {
    case MODBUS_TCP_TAG:
        protocol_stats.modbus_messages++;
        break;
    case DNP3_TAG:
        protocol_stats.dnp3_messages++;
        break;
    case ETHERNET_IP_TAG:
        protocol_stats.ethernet_ip_messages++;
        break;
    case GENERIC_TAG:
        protocol_stats.generic_messages++;
        break;
    default:
        protocol_stats.unknown_messages++;
        break;
    }
}

/*
 * Update message rate tracking
 */
static void update_message_rates(void) {
    uint64_t now = get_timestamp();

    /* Reset rate window every second (approximate) */
    if (now - last_rate_reset > 1000) {  /* Assuming timestamp is in ms */
        rate_window_index = (rate_window_index + 1) % RATE_WINDOW_SIZE;
        message_rates[rate_window_index] = 0;
        last_rate_reset = now;
    }

    message_rates[rate_window_index]++;
}

/*
 * Calculate average message rate over the tracking window
 */
static uint32_t calculate_average_rate(void) {
    uint32_t total = 0;
    for (int i = 0; i < RATE_WINDOW_SIZE; i++) {
        total += message_rates[i];
    }
    return total / RATE_WINDOW_SIZE;
}

/*
 * Simulate forwarding message to internal network
 * In a real implementation, this would interface with network hardware
 */
static bool forward_to_internal_network(const MsgHeader* header, const uint8_t* payload) {
    /* Phase 1: Just count and log the message */
    printf("IntNicDrv: Forwarding to internal network (tag=0x%04X, len=%u)\n",
           header->tag, header->len);

    /* TODO Phase 2: Implement actual network interface
     * Examples:
     * - Write to hardware NIC driver
     * - Send via TCP socket to internal system
     * - Queue in network stack buffer
     * - Apply final encapsulation (Ethernet, IP, etc.)
     */

    /* Simulate network transmission delay and success/failure */
    static uint32_t transmission_counter = 0;
    transmission_counter++;

    /* Simulate occasional transmission failures (1 in 1000) */
    if (transmission_counter % 1000 == 0) {
        printf("IntNicDrv: Simulated network transmission failure\n");
        return false;
    }

    return true;
}

/*
 * Process one message from input buffer
 */
static bool process_message(void) {
    MsgHeader header;

    /* Peek at the next message header */
    int total_size = rb_peek_header(input_buffer, &header);
    if (total_size <= 0) {
        return false;  /* No complete message available */
    }

    stats.messages_received++;

    /* Read payload if present */
    size_t payload_len = 0;
    const uint8_t* payload_ptr = NULL;

    if (header.len > 0) {
        payload_len = rb_peek_payload(input_buffer, payload_buffer, sizeof(payload_buffer));
        payload_ptr = payload_buffer;

        if (payload_len != header.len) {
            printf("IntNicDrv: ERROR - Payload size mismatch (expected=%u, got=%zu)\n",
                   header.len, payload_len);
            rb_drop(input_buffer);
            stats.error_count++;
            return true;
        }
    }

    /* Forward message to internal network */
    if (forward_to_internal_network(&header, payload_ptr)) {
        stats.messages_forwarded++;
        stats.bytes_processed += sizeof(MsgHeader) + payload_len;

        /* Update protocol-specific statistics */
        update_protocol_stats(header.tag, header.len);

        /* Update message rate tracking */
        update_message_rates();

        printf("IntNicDrv: Successfully processed message (tag=0x%04X, len=%u)\n",
               header.tag, header.len);
    } else {
        /* Network transmission failed */
        stats.messages_dropped++;
        stats.error_count++;
        printf("IntNicDrv: Failed to forward message to internal network\n");
    }

    /* Consume message from buffer regardless of transmission result */
    rb_consume(input_buffer);
    return true;
}

/*
 * Print comprehensive statistics
 */
static void print_detailed_stats(void) {
    uint32_t avg_rate = calculate_average_rate();

    printf("\n=== IntNicDrv Comprehensive Statistics ===\n");
    printf("Component Stats:\n");
    printf("  - Messages received: %llu\n", stats.messages_received);
    printf("  - Messages forwarded: %llu\n", stats.messages_forwarded);
    printf("  - Messages dropped: %llu\n", stats.messages_dropped);
    printf("  - Processing errors: %u\n", stats.error_count);
    printf("  - Bytes processed: %llu\n", stats.bytes_processed);

    printf("\nProtocol Distribution:\n");
    printf("  - MODBUS TCP: %llu\n", protocol_stats.modbus_messages);
    printf("  - DNP3: %llu\n", protocol_stats.dnp3_messages);
    printf("  - EtherNet/IP: %llu\n", protocol_stats.ethernet_ip_messages);
    printf("  - Generic: %llu\n", protocol_stats.generic_messages);
    printf("  - Unknown: %llu\n", protocol_stats.unknown_messages);
    printf("  - Total payload bytes: %llu\n", protocol_stats.total_payload_bytes);

    printf("\nPerformance Metrics:\n");
    printf("  - Average message rate: %u msg/sec\n", avg_rate);
    printf("  - Current window index: %u\n", rate_window_index);

    if (stats.messages_received > 0) {
        double success_rate = (double)stats.messages_forwarded / stats.messages_received * 100.0;
        printf("  - Success rate: %.1f%%\n", success_rate);

        if (stats.bytes_processed > 0) {
            double avg_msg_size = (double)stats.bytes_processed / stats.messages_received;
            printf("  - Average message size: %.1f bytes\n", avg_msg_size);
        }
    }
    printf("=========================================\n\n");
}

/*
 * Notification handler - called when PolicyEmit has new data
 */
void in_ntfy_handle(void) {
    stats.last_activity_time = get_timestamp();

    /* Process all available messages */
    int processed = 0;
    while (process_message() && processed < 20) {  /* Higher limit for sink component */
        processed++;
    }

    if (processed > 0) {
        printf("IntNicDrv: Processed %d messages\n", processed);
    }
}

/*
 * Print statistics periodically (called from main loop)
 */
void periodic_stats_print(void) {
    static uint32_t stats_counter = 0;
    stats_counter++;

    /* Print stats every 100 message processing cycles */
    if (stats_counter % 100 == 0) {
        print_detailed_stats();
    }
}

/*
 * Component initialization
 */
void pre_init(void) {
    /* Initialize stats and protocol tracking */
    memset(&stats, 0, sizeof(stats));
    memset(&protocol_stats, 0, sizeof(protocol_stats));
    memset(message_rates, 0, sizeof(message_rates));
    rate_window_index = 0;
    last_rate_reset = 0;

    printf("IntNicDrv: Initializing message sink and monitor...\n");
}

int run(void) {
    printf("IntNicDrv: Starting up...\n");

    /* Initialize input ring buffer */
    input_buffer = rb_init(in_dp, sizeof(Buf));
    if (!input_buffer) {
        printf("IntNicDrv: ERROR - Failed to initialize input buffer\n");
        return -1;
    }

    if (!rb_is_valid(input_buffer)) {
        printf("IntNicDrv: ERROR - Input buffer validation failed\n");
        return -1;
    }

    printf("IntNicDrv: Input buffer initialized (size=%u bytes)\n", input_buffer->size);

    printf("IntNicDrv: Ready to receive approved messages from PolicyEmit\n");
    printf("IntNicDrv: Message forwarding: SIMULATED (Phase 1)\n");

    /* Main event loop */
    while (1) {
        /* Wait for input notification */
        in_ntfy_wait();

        /* Handle the notification */
        in_ntfy_handle();

        /* Print periodic statistics */
        periodic_stats_print();
    }

    return 0;
}