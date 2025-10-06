/*
 * Common definitions for ICS One-Way Pipeline
 *
 * Shared structures, constants, and utility macros for all components
 * in the ICS pipeline: ExtNicDrv -> ExtFrontend -> ParserNorm -> PolicyEmit -> IntNicDrv
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ICS_ONEWAY_COMMON_H
#define ICS_ONEWAY_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Protocol constants */
#define MODBUS_TCP_TAG      0x0001
#define DNP3_TAG            0x0002
#define ETHERNET_IP_TAG     0x0003
#define GENERIC_TAG         0x0004

/* Buffer and message size limits */
#define MAX_PAYLOAD_SIZE    60000   /* Maximum payload size in bytes */
#define MIN_PAYLOAD_SIZE    1       /* Minimum payload size */
#define DATAPORT_SIZE       65536   /* Size of shared dataport buffer */

/* Ring buffer configuration */
#define RING_BUFFER_SIZE    32768   /* Must be power of 2 for fast modulo */
#define RING_BUFFER_MASK    (RING_BUFFER_SIZE - 1)

/* Audit log configuration */
#define AUDIT_LOG_SIZE      1024    /* Maximum audit entries */
#define AUDIT_MSG_SIZE      128     /* Maximum audit message size */

/* Statistical counters */
#define STATS_WINDOW_SIZE   10      /* Moving average window */

/*
 * TLV-style message header (8 bytes total)
 * Designed for network byte order (big-endian) but using host order for simplicity in phase 1
 */
typedef struct {
    uint16_t tag;       /* Protocol identifier (MODBUS_TCP, DNP3, etc.) */
    uint16_t len;       /* Payload length in bytes (excluding this header) */
    uint32_t flags;     /* Reserved for future use (auth, integrity, etc.) */
} __attribute__((packed)) MsgHeader;

/* Ensure header size is exactly 8 bytes */
_Static_assert(sizeof(MsgHeader) == 8, "MsgHeader must be exactly 8 bytes");

/*
 * Audit log entry for tracking dropped/rejected messages
 */
typedef struct {
    uint64_t timestamp;         /* Component-local timestamp */
    MsgHeader header;           /* Copy of message header that was rejected */
    uint32_t reason_code;       /* Reason for rejection */
    char reason_msg[64];        /* Human-readable reason */
} AuditEntry;

/* Audit reason codes */
#define AUDIT_BOUNDS_CHECK_FAILED   0x0001  /* Header length vs available data mismatch */
#define AUDIT_PAYLOAD_TOO_LARGE     0x0002  /* Payload exceeds MAX_PAYLOAD_SIZE */
#define AUDIT_PAYLOAD_TOO_SMALL     0x0003  /* Payload smaller than MIN_PAYLOAD_SIZE */
#define AUDIT_EVERPARSE_FAILED      0x0004  /* EverParse validation failed */
#define AUDIT_POLICY_DENIED         0x0005  /* Policy component denied message */
#define AUDIT_MALFORMED_HEADER      0x0006  /* Invalid header structure */

/*
 * Component statistics structure
 */
typedef struct {
    uint64_t messages_received;     /* Total messages received */
    uint64_t messages_forwarded;    /* Total messages successfully forwarded */
    uint64_t messages_dropped;      /* Total messages dropped */
    uint64_t bytes_processed;       /* Total bytes processed */
    uint64_t last_activity_time;    /* Last message timestamp */
    uint32_t error_count;           /* Count of processing errors */
} ComponentStats;

/*
 * Utility macros for common operations
 */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Safe string operations */
#define SAFE_STRNCPY(dest, src, size) do { \
    strncpy((dest), (src), (size) - 1); \
    (dest)[(size) - 1] = '\0'; \
} while(0)

/* Validation macros */
#define IS_VALID_TAG(tag) ((tag) >= 0x0001 && (tag) <= 0xFFFF)
#define IS_VALID_PAYLOAD_SIZE(len) ((len) >= MIN_PAYLOAD_SIZE && (len) <= MAX_PAYLOAD_SIZE)

/*
 * Basic bounds checking function prototype
 * This function validates that a message header is consistent with available buffer space
 */
static inline bool basic_bounds_check(const MsgHeader* header, size_t available_bytes) {
    if (!header) {
        return false;
    }

    /* Check that we have at least the header */
    if (available_bytes < sizeof(MsgHeader)) {
        return false;
    }

    /* Check payload size limits */
    if (!IS_VALID_PAYLOAD_SIZE(header->len)) {
        return false;
    }

    /* Check that claimed payload fits in available bytes */
    if (sizeof(MsgHeader) + header->len > available_bytes) {
        return false;
    }

    /* Check for overflow in addition */
    if (header->len > SIZE_MAX - sizeof(MsgHeader)) {
        return false;
    }

    return true;
}

/*
 * EverParse validation function prototype
 * This is a stub function that will be replaced with real EverParse integration
 */
static inline bool everparse_validate(const uint8_t* payload, size_t length) {
    /* Phase 1: Always return true (no-op validation) */
    /* TODO: Replace with actual EverParse validator */
    (void)payload;  /* Suppress unused parameter warning */
    (void)length;
    return true;
}

/*
 * Simple logging macros for debug output
 * Uses printf-style formatting but can be redirected to serial/syslog
 */
#define LOG_DEBUG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  printf("[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) printf("[ERROR] " fmt "\n", ##__VA_ARGS__)

/*
 * Timestamp utility (simple incrementing counter for now)
 */
extern uint64_t global_timestamp_counter;

static inline uint64_t get_timestamp(void) {
    return ++global_timestamp_counter;
}

#endif /* ICS_ONEWAY_COMMON_H */