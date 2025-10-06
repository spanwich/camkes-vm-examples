/*
 * Policy and Emitter Implementation
 *
 * Final security checkpoint before messages reach internal network.
 * Phase 1: Allow-all policy with comprehensive logging and hooks for future policies.
 * Includes framework for function code filtering, rate limiting, and value range checking.
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

/* Policy decision statistics */
static uint64_t policy_allows = 0;
static uint64_t policy_denies = 0;

/* TODO: Policy configuration tables (phase 2 implementation) */
typedef struct {
    uint16_t tag;           /* Protocol tag */
    uint8_t function_code;  /* Protocol-specific function code */
    bool allowed;           /* Allow/deny for this function */
    uint32_t rate_limit_ms; /* Minimum time between messages (0 = no limit) */
    uint64_t last_seen;     /* Last message timestamp */
} PolicyRule;

/* Placeholder policy table - Phase 1: empty, Phase 2: populate */
static PolicyRule policy_table[] = {
    /* TODO Phase 2: Add actual policy rules */
    /* Example:
     * {MODBUS_TCP_TAG, 0x03, true, 100, 0},   // Read holding registers - allow, 100ms limit
     * {MODBUS_TCP_TAG, 0x06, false, 0, 0},    // Write single register - deny
     * {DNP3_TAG, 0x01, true, 50, 0},          // Read request - allow, 50ms limit
     */
};

#define POLICY_TABLE_SIZE (sizeof(policy_table) / sizeof(policy_table[0]))

/*
 * Extract function code from payload based on protocol tag
 * Returns 0xFF if function code cannot be determined
 */
static uint8_t extract_function_code(uint16_t tag, const uint8_t* payload, size_t len) {
    if (!payload || len < 1) {
        return 0xFF;  /* Unknown */
    }

    switch (tag) {
    case MODBUS_TCP_TAG:
        /* MODBUS TCP: function code is typically at offset 7 in full frame,
         * but our payload might start after MBAP header */
        if (len >= 1) {
            return payload[0];  /* Assume payload starts with function code */
        }
        break;

    case DNP3_TAG:
        /* DNP3: function code in application header */
        if (len >= 2) {
            return payload[1];  /* Simplified - real DNP3 parsing is complex */
        }
        break;

    case ETHERNET_IP_TAG:
        /* EtherNet/IP: service code in Common Industrial Protocol header */
        if (len >= 1) {
            return payload[0];
        }
        break;

    default:
        /* Generic or unknown protocol */
        return 0xFF;
    }

    return 0xFF;
}

/*
 * Apply policy rules to message
 * Phase 1: Allow-all with comprehensive logging
 * Phase 2: Implement actual rule checking
 */
static bool apply_policy(const MsgHeader* header, const uint8_t* payload) {
    uint64_t now = get_timestamp();
    uint8_t function_code = extract_function_code(header->tag, payload, header->len);

    printf("PolicyEmit: Evaluating policy (tag=0x%04X, len=%u, func=0x%02X)\n",
           header->tag, header->len, function_code);

    /* TODO Phase 2: Implement actual policy checking */
    #ifdef PHASE_2_POLICY
    for (size_t i = 0; i < POLICY_TABLE_SIZE; i++) {
        PolicyRule* rule = &policy_table[i];

        if (rule->tag == header->tag && rule->function_code == function_code) {
            /* Check rate limit */
            if (rule->rate_limit_ms > 0) {
                uint64_t elapsed = now - rule->last_seen;
                if (elapsed < rule->rate_limit_ms) {
                    printf("PolicyEmit: DENY - Rate limit exceeded (elapsed=%llu, limit=%u)\n",
                           elapsed, rule->rate_limit_ms);
                    policy_denies++;
                    return false;
                }
                rule->last_seen = now;
            }

            /* Apply allow/deny decision */
            if (!rule->allowed) {
                printf("PolicyEmit: DENY - Function code blocked by policy\n");
                policy_denies++;
                return false;
            }

            printf("PolicyEmit: ALLOW - Policy rule matched and permits\n");
            policy_allows++;
            return true;
        }
    }

    /* No specific rule found - apply default policy */
    printf("PolicyEmit: DENY - No policy rule found (default deny)\n");
    policy_denies++;
    return false;
    #else
    /* Phase 1: Allow-all policy */
    printf("PolicyEmit: ALLOW - Phase 1 allow-all policy\n");
    policy_allows++;
    return true;
    #endif
}

/*
 * Additional payload validation for specific protocols
 * TODO Phase 2: Implement protocol-specific value range checking
 */
static bool validate_payload_ranges(uint16_t tag, const uint8_t* payload, size_t len) {
    /* Phase 1: No range checking, always pass */
    (void)tag;
    (void)payload;
    (void)len;
    return true;

    /* TODO Phase 2: Implement range checking
    switch (tag) {
    case MODBUS_TCP_TAG:
        // Check register addresses, coil ranges, etc.
        return validate_modbus_ranges(payload, len);

    case DNP3_TAG:
        // Check point indices, variation codes, etc.
        return validate_dnp3_ranges(payload, len);

    default:
        return true;
    }
    */
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
            printf("PolicyEmit: ERROR - Payload size mismatch (expected=%u, got=%zu)\n",
                   header.len, payload_len);
            rb_drop(input_buffer);
            stats.error_count++;
            return true;
        }
    }

    /* Apply security policies */
    if (!apply_policy(&header, payload_ptr)) {
        printf("PolicyEmit: Dropping message due to policy violation\n");
        rb_drop(input_buffer);
        stats.messages_dropped++;
        return true;
    }

    /* Additional payload validation */
    if (!validate_payload_ranges(header.tag, payload_ptr, payload_len)) {
        printf("PolicyEmit: Dropping message due to payload range violation\n");
        rb_drop(input_buffer);
        stats.messages_dropped++;
        return true;
    }

    /* Policy allows message - forward it */
    bool write_success = rb_write(output_buffer, &header, payload_ptr);

    if (write_success) {
        /* Successfully forwarded */
        rb_consume(input_buffer);
        stats.messages_forwarded++;
        stats.bytes_processed += sizeof(MsgHeader) + payload_len;

        /* Signal the next component */
        out_ntfy_emit();

        printf("PolicyEmit: Forwarded approved message (tag=0x%04X, len=%u)\n",
               header.tag, header.len);
    } else {
        /* Output buffer full - leave message in input buffer for retry */
        printf("PolicyEmit: Output buffer full, retrying later\n");
        stats.error_count++;
        return false;
    }

    return true;
}

/*
 * Notification handler - called when ParserNorm has new data
 */
void in_ntfy_handle(void) {
    stats.last_activity_time = get_timestamp();

    /* Process all available messages */
    int processed = 0;
    while (process_message() && processed < 10) {  /* Limit to prevent starvation */
        processed++;
    }

    if (processed > 0) {
        printf("PolicyEmit: Processed %d messages\n", processed);
    }

    /* Print periodic stats */
    static uint32_t stats_counter = 0;
    if (++stats_counter % 25 == 0) {
        printf("PolicyEmit: Stats - Received: %llu, Forwarded: %llu, Dropped: %llu, Errors: %u\n",
               stats.messages_received, stats.messages_forwarded,
               stats.messages_dropped, stats.error_count);
        printf("PolicyEmit: Policy - Allows: %llu, Denies: %llu\n",
               policy_allows, policy_denies);
    }
}

/*
 * Component initialization
 */
void pre_init(void) {
    /* Initialize stats */
    memset(&stats, 0, sizeof(stats));
    policy_allows = 0;
    policy_denies = 0;

    printf("PolicyEmit: Initializing security policy engine...\n");
}

int run(void) {
    printf("PolicyEmit: Starting up...\n");

    /* Initialize input ring buffer */
    input_buffer = rb_init(in_dp, sizeof(Buf));
    if (!input_buffer) {
        printf("PolicyEmit: ERROR - Failed to initialize input buffer\n");
        return -1;
    }

    /* Initialize output ring buffer */
    output_buffer = rb_init(out_dp, sizeof(Buf));
    if (!output_buffer) {
        printf("PolicyEmit: ERROR - Failed to initialize output buffer\n");
        return -1;
    }

    if (!rb_is_valid(input_buffer) || !rb_is_valid(output_buffer)) {
        printf("PolicyEmit: ERROR - Buffer validation failed\n");
        return -1;
    }

    printf("PolicyEmit: Buffers initialized (input=%u, output=%u bytes)\n",
           input_buffer->size, output_buffer->size);

    printf("PolicyEmit: Ready to apply policies to messages from ParserNorm\n");
    printf("PolicyEmit: Current policy: ALLOW-ALL (Phase 1)\n");
    printf("PolicyEmit: Policy table size: %zu rules\n", POLICY_TABLE_SIZE);

    /* Main event loop */
    while (1) {
        /* Wait for input notification */
        in_ntfy_wait();

        /* Handle the notification */
        in_ntfy_handle();
    }

    return 0;
}