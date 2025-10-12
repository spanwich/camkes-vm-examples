/*
 * Common definitions for ICS Bidirectional Cross-Domain Firewall
 *
 * Shared structures for bidirectional protocol break architecture:
 * VirtIO_Net0_Driver ⟷ ICS_Inbound ⟷ VirtIO_Net1_Driver
 *                     ⟷ ICS_Outbound ⟷
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ICS_BIDIRECTIONAL_COMMON_H
#define ICS_BIDIRECTIONAL_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

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
 * Frame Metadata Structure (Passed Between Components)
 *
 * Contains all protocol/frame information extracted by VirtIO drivers.
 * ICS validation components use this for protocol-aware validation.
 */
typedef struct {
    // Ethernet frame info
    uint8_t  dst_mac[6];        /* Destination MAC address */
    uint8_t  src_mac[6];        /* Source MAC address */
    uint16_t ethertype;         /* 0x0800=IPv4, 0x0806=ARP, 0x88B8=GOOSE, etc. */
    uint16_t vlan_id;           /* VLAN ID (0 if no VLAN) */
    uint8_t  vlan_priority;     /* VLAN priority (0-7) */

    // IP layer info (if applicable)
    uint8_t  ip_protocol;       /* 6=TCP, 17=UDP, 0=not IP */
    uint32_t src_ip;            /* Source IP address */
    uint32_t dst_ip;            /* Destination IP address */

    // Transport layer info (if TCP/UDP)
    uint16_t src_port;          /* Source port */
    uint16_t dst_port;          /* Destination port */

    // Payload info
    uint16_t payload_offset;    /* Offset in original frame */
    uint16_t payload_length;    /* Actual payload length */

    // Protocol flags (for quick identification)
    uint8_t  is_ip      : 1;    /* 1 if IP packet */
    uint8_t  is_tcp     : 1;    /* 1 if TCP */
    uint8_t  is_udp     : 1;    /* 1 if UDP */
    uint8_t  is_arp     : 1;    /* 1 if ARP */
    uint8_t  reserved   : 4;    /* Reserved for future protocols */

} __attribute__((packed)) FrameMetadata;

/*
 * ICS Message Structure (Passed via Dataports)
 *
 * Contains metadata + payload extracted by VirtIO driver.
 * ICS components validate payload using metadata context.
 */
typedef struct {
    FrameMetadata metadata;                 /* Frame/protocol information */
    uint16_t      payload_length;           /* Length of payload */
    uint8_t       payload[MAX_PAYLOAD_SIZE]; /* Actual payload data */
} __attribute__((packed)) ICS_Message;

/*
 * Audit log entry for tracking dropped/rejected messages
 */
typedef struct {
    uint64_t timestamp;         /* Component-local timestamp */
    FrameMetadata metadata;     /* Copy of metadata that was rejected */
    uint32_t reason_code;       /* Reason for rejection */
    char reason_msg[64];        /* Human-readable reason */
} AuditEntry;

/* Audit reason codes */
#define AUDIT_BOUNDS_CHECK_FAILED   0x0001  /* Payload length vs available data mismatch */
#define AUDIT_PAYLOAD_TOO_LARGE     0x0002  /* Payload exceeds MAX_PAYLOAD_SIZE */
#define AUDIT_PAYLOAD_TOO_SMALL     0x0003  /* Payload smaller than MIN_PAYLOAD_SIZE */
#define AUDIT_EVERPARSE_FAILED      0x0004  /* EverParse validation failed */
#define AUDIT_POLICY_DENIED         0x0005  /* Policy component denied message */
#define AUDIT_MALFORMED_METADATA    0x0006  /* Invalid metadata structure */

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
#define IS_VALID_ETHERTYPE(et) ((et) == 0x0800 || (et) == 0x0806 || (et) == 0x86DD || (et) == 0x88B8)
#define IS_VALID_PAYLOAD_SIZE(len) ((len) >= MIN_PAYLOAD_SIZE && (len) <= MAX_PAYLOAD_SIZE)

/*
 * Basic bounds checking for ICS messages
 * Validates that payload length is consistent with available buffer space
 */
static inline bool basic_bounds_check(const ICS_Message* msg, size_t available_bytes) {
    if (!msg) {
        return false;
    }

    /* Check that we have at least the ICS_Message header (metadata + length field) */
    size_t min_size = sizeof(FrameMetadata) + sizeof(uint16_t);
    if (available_bytes < min_size) {
        return false;
    }

    /* Check payload size limits */
    if (msg->payload_length > 0 && !IS_VALID_PAYLOAD_SIZE(msg->payload_length)) {
        return false;
    }

    /* Check that claimed payload fits in available bytes */
    if (min_size + msg->payload_length > available_bytes) {
        return false;
    }

    /* Check for overflow in addition */
    if (msg->payload_length > SIZE_MAX - min_size) {
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
 * Minimal breadcrumb tracing for debugging race conditions
 * These are ultra-minimal - just marks execution points
 * Enable/disable by setting BREADCRUMB_TRACE to 1/0
 */
#ifndef BREADCRUMB_TRACE
#define BREADCRUMB_TRACE 1
#endif

#if BREADCRUMB_TRACE
    #define BREADCRUMB(id) printf("B%d\n", (id))
#else
    #define BREADCRUMB(id) do {} while(0)
#endif

/*
 * Timestamp utility (simple incrementing counter for now)
 */
extern uint64_t global_timestamp_counter;

static inline uint64_t get_timestamp(void) {
    return ++global_timestamp_counter;
}

#endif /* ICS_BIDIRECTIONAL_COMMON_H */