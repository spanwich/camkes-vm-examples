/*
 * Parser/Normalizer Implementation
 *
 * Critical validation component that performs bounds checking and protocol validation.
 * In phase 1, implements strict bounds checking with EverParse hook for future expansion.
 * Maintains audit log of all rejected messages for security analysis.
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

/* Audit log for rejected messages */
static AuditEntry audit_log[AUDIT_LOG_SIZE];
static uint32_t audit_head = 0;
static uint32_t audit_count = 0;

/*
 * Add entry to audit log (circular buffer, drops oldest)
 */
static void audit_log_add(const MsgHeader* header, uint32_t reason_code, const char* reason_msg) {
    AuditEntry* entry = &audit_log[audit_head];

    entry->timestamp = get_timestamp();
    entry->header = *header;
    entry->reason_code = reason_code;
    SAFE_STRNCPY(entry->reason_msg, reason_msg, sizeof(entry->reason_msg));

    audit_head = (audit_head + 1) % AUDIT_LOG_SIZE;
    if (audit_count < AUDIT_LOG_SIZE) {
        audit_count++;
    }

    printf("ParserNorm: AUDIT - Rejected message (tag=0x%04X, len=%u): %s\n",
           header->tag, header->len, reason_msg);
}

/*
 * Print recent audit log entries for debugging
 */
static void print_audit_summary(void) {
    if (audit_count == 0) {
        printf("ParserNorm: Audit log is empty\n");
        return;
    }

    printf("ParserNorm: Audit log summary (%u entries):\n", audit_count);

    uint32_t reasons[8] = {0};  /* Count by reason code */
    uint32_t start = (audit_count < AUDIT_LOG_SIZE) ? 0 : audit_head;
    uint32_t entries_to_show = MIN(audit_count, AUDIT_LOG_SIZE);

    for (uint32_t i = 0; i < entries_to_show; i++) {
        uint32_t idx = (start + i) % AUDIT_LOG_SIZE;
        const AuditEntry* entry = &audit_log[idx];

        if (entry->reason_code < sizeof(reasons)/sizeof(reasons[0])) {
            reasons[entry->reason_code]++;
        }
    }

    printf("  - Bounds check failed: %u\n", reasons[AUDIT_BOUNDS_CHECK_FAILED]);
    printf("  - Payload too large: %u\n", reasons[AUDIT_PAYLOAD_TOO_LARGE]);
    printf("  - Payload too small: %u\n", reasons[AUDIT_PAYLOAD_TOO_SMALL]);
    printf("  - EverParse failed: %u\n", reasons[AUDIT_EVERPARSE_FAILED]);
    printf("  - Malformed header: %u\n", reasons[AUDIT_MALFORMED_HEADER]);
}

/*
 * Comprehensive validation of message header and payload
 */
static bool validate_message(const MsgHeader* header, const uint8_t* payload, size_t available_bytes) {
    /* Basic bounds checking first */
    if (!basic_bounds_check(header, available_bytes)) {
        audit_log_add(header, AUDIT_BOUNDS_CHECK_FAILED,
                     "Header length exceeds available bytes");
        return false;
    }

    /* Check payload size limits */
    if (header->len > MAX_PAYLOAD_SIZE) {
        audit_log_add(header, AUDIT_PAYLOAD_TOO_LARGE,
                     "Payload exceeds maximum allowed size");
        return false;
    }

    if (header->len < MIN_PAYLOAD_SIZE && header->len != 0) {  /* Allow zero-length */
        audit_log_add(header, AUDIT_PAYLOAD_TOO_SMALL,
                     "Payload smaller than minimum allowed size");
        return false;
    }

    /* Validate tag range */
    if (!IS_VALID_TAG(header->tag)) {
        audit_log_add(header, AUDIT_MALFORMED_HEADER,
                     "Invalid protocol tag");
        return false;
    }

    /* EverParse validation hook - Phase 1: always passes */
    if (header->len > 0 && payload) {
        if (!everparse_validate(payload, header->len)) {
            audit_log_add(header, AUDIT_EVERPARSE_FAILED,
                         "EverParse validation failed");
            return false;
        }
    }

    /* Additional consistency checks */
    if (header->len > 0 && !payload) {
        audit_log_add(header, AUDIT_MALFORMED_HEADER,
                     "Non-zero length with null payload");
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

    printf("ParserNorm: Validating message (tag=0x%04X, len=%u)\n", header.tag, header.len);

    /* Read payload if present */
    size_t payload_len = 0;
    const uint8_t* payload_ptr = NULL;

    if (header.len > 0) {
        payload_len = rb_peek_payload(input_buffer, payload_buffer, sizeof(payload_buffer));
        payload_ptr = payload_buffer;

        if (payload_len != header.len) {
            /* Payload size mismatch is a critical error */
            audit_log_add(&header, AUDIT_BOUNDS_CHECK_FAILED,
                         "Payload size mismatch during read");
            rb_drop(input_buffer);
            stats.messages_dropped++;
            return true;
        }
    }

    /* Perform comprehensive validation */
    if (!validate_message(&header, payload_ptr, total_size)) {
        /* Message failed validation - drop it */
        rb_drop(input_buffer);
        stats.messages_dropped++;
        return true;
    }

    /* Message is valid - forward it unchanged (phase 1: no normalization) */
    bool write_success = rb_write(output_buffer, &header, payload_ptr);

    if (write_success) {
        /* Successfully forwarded */
        rb_consume(input_buffer);
        stats.messages_forwarded++;
        stats.bytes_processed += sizeof(MsgHeader) + payload_len;

        /* Signal the next component */
        out_ntfy_emit();

        printf("ParserNorm: Forwarded validated message (tag=0x%04X, len=%u)\n",
               header.tag, header.len);
    } else {
        /* Output buffer full - leave message in input buffer for retry */
        printf("ParserNorm: Output buffer full, retrying later\n");
        stats.error_count++;
        return false;
    }

    return true;
}

/*
 * Notification handler - called when ExtFrontend has new data
 */
void in_ntfy_handle(void) {
    stats.last_activity_time = get_timestamp();

    /* Process all available messages */
    int processed = 0;
    while (process_message() && processed < 10) {  /* Limit to prevent starvation */
        processed++;
    }

    if (processed > 0) {
        printf("ParserNorm: Processed %d messages\n", processed);
    }

    /* Print periodic stats and audit summary */
    static uint32_t stats_counter = 0;
    if (++stats_counter % 50 == 0) {
        printf("ParserNorm: Stats - Received: %llu, Forwarded: %llu, Dropped: %llu, Errors: %u\n",
               stats.messages_received, stats.messages_forwarded,
               stats.messages_dropped, stats.error_count);

        if (stats.messages_dropped > 0) {
            print_audit_summary();
        }
    }
}

/*
 * Component initialization
 */
void pre_init(void) {
    /* Initialize stats and audit log */
    memset(&stats, 0, sizeof(stats));
    memset(audit_log, 0, sizeof(audit_log));
    audit_head = 0;
    audit_count = 0;

    printf("ParserNorm: Initializing validation engine...\n");
}

int run(void) {
    printf("ParserNorm: Starting up...\n");

    /* Initialize input ring buffer */
    input_buffer = rb_init(in_dp, sizeof(Buf));
    if (!input_buffer) {
        printf("ParserNorm: ERROR - Failed to initialize input buffer\n");
        return -1;
    }

    /* Initialize output ring buffer */
    output_buffer = rb_init(out_dp, sizeof(Buf));
    if (!output_buffer) {
        printf("ParserNorm: ERROR - Failed to initialize output buffer\n");
        return -1;
    }

    if (!rb_is_valid(input_buffer) || !rb_is_valid(output_buffer)) {
        printf("ParserNorm: ERROR - Buffer validation failed\n");
        return -1;
    }

    printf("ParserNorm: Buffers initialized (input=%u, output=%u bytes)\n",
           input_buffer->size, output_buffer->size);

    printf("ParserNorm: Ready to validate messages from ExtFrontend\n");
    printf("ParserNorm: Validation: bounds checking + EverParse hooks (phase 1: no-op)\n");
    printf("ParserNorm: Audit log capacity: %u entries\n", AUDIT_LOG_SIZE);

    /* Main event loop */
    while (1) {
        /* Wait for input notification */
        in_ntfy_wait();

        /* Handle the notification */
        in_ntfy_handle();
    }

    return 0;
}