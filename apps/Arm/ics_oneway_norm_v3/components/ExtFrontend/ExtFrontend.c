/*
 * External Frontend Implementation
 *
 * Processes incoming frames from ExtNicDrv and forwards standardized messages
 * to ParserNorm. In this phase, performs minimal frame parsing and validation.
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
static RingBuffer* output_buffer;
static ComponentStats stats;
static uint8_t payload_buffer[MAX_PAYLOAD_SIZE];

/* Global timestamp counter definition */
uint64_t global_timestamp_counter = 0;

/*
 * Process one message from input buffer and forward it to output
 */
static bool process_message(void) {
    MsgHeader header;

    /* Peek at the next message header */
    int total_size = rb_peek_header(input_buffer, &header);
    if (total_size <= 0) {
        return false;  /* No complete message available */
    }

    stats.messages_received++;

    printf("ExtFrontend: Processing message (tag=0x%04X, len=%u)\n", header.tag, header.len);

    /* Basic validation before forwarding */
    if (!IS_VALID_TAG(header.tag)) {
        printf("ExtFrontend: Dropping message with invalid tag 0x%04X\n", header.tag);
        rb_drop(input_buffer);
        stats.messages_dropped++;
        return true;  /* Continue processing */
    }

    /* Read payload if present */
    size_t payload_len = 0;
    if (header.len > 0) {
        if (header.len > MAX_PAYLOAD_SIZE) {
            printf("ExtFrontend: Dropping oversized message (len=%u)\n", header.len);
            rb_drop(input_buffer);
            stats.messages_dropped++;
            return true;
        }

        payload_len = rb_peek_payload(input_buffer, payload_buffer, sizeof(payload_buffer));
        if (payload_len != header.len) {
            printf("ExtFrontend: Payload size mismatch (expected=%u, got=%zu)\n",
                   header.len, payload_len);
            rb_drop(input_buffer);
            stats.messages_dropped++;
            return true;
        }
    }

    /* Forward message to output buffer */
    bool write_success = rb_write(output_buffer, &header,
                                  payload_len > 0 ? payload_buffer : NULL);

    if (write_success) {
        /* Successfully forwarded */
        rb_consume(input_buffer);
        stats.messages_forwarded++;
        stats.bytes_processed += sizeof(MsgHeader) + payload_len;

        /* Signal the next component */
        out_ntfy_emit();

        printf("ExtFrontend: Forwarded message (tag=0x%04X, len=%u)\n", header.tag, header.len);
    } else {
        /* Output buffer full - leave message in input buffer for retry */
        printf("ExtFrontend: Output buffer full, retrying later\n");
        stats.error_count++;
        return false;
    }

    return true;
}

/*
 * Notification handler - called when ExtNicDrv has new data
 */
void in_ntfy_handle(void) {
    stats.last_activity_time = get_timestamp();

    /* Process all available messages */
    int processed = 0;
    while (process_message() && processed < 10) {  /* Limit to prevent starvation */
        processed++;
    }

    if (processed > 0) {
        printf("ExtFrontend: Processed %d messages\n", processed);
    }

    /* Print periodic stats */
    static uint32_t stats_counter = 0;
    if (++stats_counter % 100 == 0) {
        printf("ExtFrontend: Stats - Received: %llu, Forwarded: %llu, Dropped: %llu, Errors: %u\n",
               stats.messages_received, stats.messages_forwarded,
               stats.messages_dropped, stats.error_count);
    }
}

/*
 * Component initialization
 */
void pre_init(void) {
    /* Initialize stats */
    memset(&stats, 0, sizeof(stats));

    printf("ExtFrontend: Initializing frame processor...\n");
}

int run(void) {
    printf("ExtFrontend: Starting up...\n");

    /* Initialize input ring buffer */
    input_buffer = rb_init(in_dp, sizeof(Buf));
    if (!input_buffer) {
        printf("ExtFrontend: ERROR - Failed to initialize input buffer\n");
        return -1;
    }

    /* Initialize output ring buffer */
    output_buffer = rb_init(out_dp, sizeof(Buf));
    if (!output_buffer) {
        printf("ExtFrontend: ERROR - Failed to initialize output buffer\n");
        return -1;
    }

    if (!rb_is_valid(input_buffer) || !rb_is_valid(output_buffer)) {
        printf("ExtFrontend: ERROR - Buffer validation failed\n");
        return -1;
    }

    printf("ExtFrontend: Buffers initialized (input=%u, output=%u bytes)\n",
           input_buffer->size, output_buffer->size);

    printf("ExtFrontend: Ready to process frames from ExtNicDrv\n");
    printf("ExtFrontend: Validation: tag range, payload size limits\n");

    /* Main event loop */
    while (1) {
        /* Wait for input notification */
        in_ntfy_wait();

        /* Handle the notification */
        in_ntfy_handle();
    }

    return 0;
}