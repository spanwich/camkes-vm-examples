/*
 * Connection State Sharing Between Net0 and Net1
 *
 * This header defines the read-only connection state that each network driver
 * exposes to the other via CAmkES dataports. This allows both components to
 * see each other's current connection state without violating isolation
 * (read-only access only).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef CONNECTION_STATE_H
#define CONNECTION_STATE_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum connections tracked per component */
#define MAX_SHARED_CONNECTIONS 256

/*
 * Minimal connection view for sharing between components
 *
 * v2.150: Added session_id for reliable SCADA ↔ PLC connection mapping
 *
 * This structure contains only the essential information needed for:
 * - Identifying connections (5-tuple)
 * - Detecting stale notifications (timestamp)
 * - Checking if connection is active (active flag)
 * - Linking SCADA and PLC connections (session_id)
 *
 * Size: 24 bytes per entry × 256 = 6 KB total per component
 */
struct connection_view {
    uint32_t session_id;  /* v2.150: Unique ID for SCADA connection (0 = unassigned) */
    uint32_t src_ip;      /* Source IP (SCADA) */
    uint32_t dst_ip;      /* Destination IP (PLC) */
    uint16_t src_port;    /* Source port (SCADA ephemeral) */
    uint16_t dst_port;    /* Destination port (502) */
    uint32_t timestamp;   /* Creation timestamp (for staleness detection) */
    bool active;          /* Is this connection currently active? */
    uint8_t _padding[3];  /* Explicit padding for alignment */
} __attribute__((packed));

/*
 * Connection state table exposed by each component
 *
 * This entire structure is mapped as a read-only dataport to the other component.
 *
 * Usage:
 * - Net0 exposes net0_connection_state (Net1 can read it)
 * - Net1 exposes net1_connection_state (Net0 can read it)
 */
struct connection_state_table {
    struct connection_view connections[MAX_SHARED_CONNECTIONS];
    uint32_t count;       /* Number of active connections */
    uint32_t last_update; /* Timestamp of last update (for debugging) */
} __attribute__((packed));

#endif /* CONNECTION_STATE_H */
