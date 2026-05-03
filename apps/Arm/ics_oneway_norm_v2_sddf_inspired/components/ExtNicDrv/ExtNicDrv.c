/*
 * External NIC Driver Implementation
 *
 * Traffic generator that creates synthetic ICS protocol messages
 * Periodically generates both valid and malformed messages for testing
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
static RingBuffer* output_buffer;
static ComponentStats stats;
static uint32_t message_counter = 0;
static uint32_t malformed_counter = 0;

/* Test payload patterns */
static const char* test_payloads[] = {
    "MODBUS_READ_HOLDING_REGISTERS",
    "DNP3_DATA_REQUEST",
    "ETHERNET_IP_CLASS_REQUEST",
    "GENERIC_SENSOR_DATA_12345",
    "MODBUS_WRITE_SINGLE_COIL_ON",
    "DNP3_CONFIRM_DATA_SET",
};

#define NUM_TEST_PAYLOADS (sizeof(test_payloads) / sizeof(test_payloads[0]))

/* Global timestamp counter definition */
uint64_t global_timestamp_counter = 0;

/*
 * Generate a synthetic message with given parameters
 */
static bool generate_message(uint16_t tag, const char* payload_str, bool make_malformed) {
    MsgHeader header;
    const uint8_t* payload_data = (const uint8_t*)payload_str;
    size_t payload_len = strlen(payload_str);

    /* Build header */
    header.tag = tag;
    header.len = (uint16_t)payload_len;
    header.flags = 0;

    /* Occasionally generate malformed messages for testing */
    if (make_malformed) {
        malformed_counter++;

        switch (malformed_counter % 4) {
        case 0:
            /* Length mismatch - claim more data than we have */
            header.len = (uint16_t)(payload_len + 100);
            printf("ExtNicDrv: Generated malformed message (len=%u, actual=%zu)\n",
                   header.len, payload_len);
            break;

        case 1:
            /* Zero length payload with non-empty data */
            header.len = 0;
            printf("ExtNicDrv: Generated malformed message (zero len with data)\n");
            break;

        case 2:
            /* Oversized payload */
            header.len = MAX_PAYLOAD_SIZE + 1;
            printf("ExtNicDrv: Generated malformed message (oversized len=%u)\n", header.len);
            break;

        case 3:
            /* Invalid tag */
            header.tag = 0x0000;
            printf("ExtNicDrv: Generated malformed message (invalid tag)\n");
            break;
        }
    }

    /* Attempt to write to ring buffer */
    bool success = rb_write(output_buffer, &header, payload_data);

    if (success) {
        stats.messages_forwarded++;
        stats.bytes_processed += sizeof(MsgHeader) + payload_len;

        /* Signal the next component */
        out_ntfy_emit();

        printf("ExtNicDrv: Generated message #%u (tag=0x%04X, len=%u) %s\n",
               message_counter, header.tag, header.len,
               make_malformed ? "[MALFORMED]" : "[VALID]");
    } else {
        stats.error_count++;
        printf("ExtNicDrv: Failed to write message #%u to buffer (full?)\n", message_counter);
    }

    return success;
}

/*
 * Generate traffic in main loop instead of timer callback
 */
void generate_periodic_traffic(void) {
    message_counter++;
    stats.messages_received++;  /* Count generation attempts */
    stats.last_activity_time = get_timestamp();

    /* Select payload pattern */
    const char* payload = test_payloads[message_counter % NUM_TEST_PAYLOADS];

    /* Select protocol tag based on payload */
    uint16_t tag = MODBUS_TCP_TAG;
    if (strstr(payload, "DNP3")) {
        tag = DNP3_TAG;
    } else if (strstr(payload, "ETHERNET_IP")) {
        tag = ETHERNET_IP_TAG;
    } else if (strstr(payload, "GENERIC")) {
        tag = GENERIC_TAG;
    }

    /* Generate malformed message approximately 10% of the time */
    bool make_malformed = (message_counter % 10 == 0);

    generate_message(tag, payload, make_malformed);

    /* Print stats every 50 messages */
    if (message_counter % 50 == 0) {
        printf("ExtNicDrv: Stats - Generated: %llu, Forwarded: %llu, Errors: %u, Bytes: %llu\n",
               stats.messages_received, stats.messages_forwarded,
               stats.error_count, stats.bytes_processed);
    }
}

/*
 * Component initialization
 */
void pre_init(void) {
    /* Initialize stats */
    memset(&stats, 0, sizeof(stats));

    printf("ExtNicDrv: Initializing traffic generator...\n");
}

int run(void) {
    printf("ExtNicDrv: Starting up...\n");

    /* Initialize output ring buffer */
    output_buffer = rb_init(out_dp, sizeof(Buf));
    if (!output_buffer) {
        printf("ExtNicDrv: ERROR - Failed to initialize output buffer\n");
        return -1;
    }

    if (!rb_is_valid(output_buffer)) {
        printf("ExtNicDrv: ERROR - Output buffer validation failed\n");
        return -1;
    }

    printf("ExtNicDrv: Output buffer initialized (size=%u bytes)\n", output_buffer->size);

    printf("ExtNicDrv: Starting traffic generation loop\n");
    printf("ExtNicDrv: Traffic pattern includes ~10%% malformed messages for testing\n");

    /* Main event loop - generate traffic continuously */
    while (1) {
        /* Generate traffic */
        generate_periodic_traffic();

        /* Simple delay mechanism (busy wait for demo) */
        for (volatile int i = 0; i < 1000000; i++) {
            /* Busy wait - replace with proper timing mechanism later */
        }
    }

    return 0;
}