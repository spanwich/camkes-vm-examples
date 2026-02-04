/*
 * version.h - Unified Version Management for ICS Bidirectional Gateway
 *
 * Single source of truth for project versioning.
 * All components should include this header and use these macros.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ICS_VERSION_H
#define ICS_VERSION_H

/*
 * Version Format: MAJOR.MINOR
 * - MAJOR: Architecture changes (e.g., new components, protocol changes)
 * - MINOR: Incremental improvements, bug fixes
 *
 * Changelog maintained in project root or research-docs/
 */

#define ICS_VERSION_MAJOR   2
#define ICS_VERSION_MINOR   270
#define ICS_VERSION_STRING  "2.270"
#define ICS_VERSION_DATE    "2026-01-06"

/*
 * v2.270 Changes (2026-01-06):
 * - NEW: Policy enforcement layer for CVE-2022-0367 mitigation
 *   Added modbus_policy.h - runtime-configurable address range validation
 *   Architecture: Stage 1 (EverParse) → Stage 2 (Policy)
 *   CVE-2022-0367: libmodbus only validated read_address in FC 0x17,
 *   allowing write_address to target memory outside PLC mapping.
 *   Fix: Policy layer validates BOTH addresses in FC 0x17.
 * - ICS_Inbound: Policy enabled, configured for CVE test (registers 100-109)
 * - ICS_Outbound: Policy disabled (responses from trusted PLC don't need validation)
 *
 * v2.260 Changes (2026-01-02):
 * - FIX: Backend connection reuse - stop creating new PLC connection per request
 *   Root cause: Net1 created NEW backend TCP connection for EVERY Modbus request,
 *   even when SCADA sent multiple requests on ONE persistent connection.
 *   Old behavior: Request 1 → tcp_connect → Request 2 → tcp_abort + tcp_connect → ...
 *   This RST flood overwhelmed PLC's single-threaded accept loop.
 *   Fix: Check if existing connection is in ESTABLISHED state, if so reuse it
 *   by sending new request on existing PCB instead of creating new connection.
 *   New log: [N1-REUSE] shows when connection is being reused.
 *
 * v2.259 Changes (2026-01-02):
 * - Add [N1-CONNECT] log to show PLC IP:port being connected to
 *   Diagnose: ERR_ABRT on every request - is PLC reachable?
 *
 * v2.258 Changes (2026-01-02):
 * - FIX: Connection closing after every response (ModScan socket error)
 *   Root cause: Used awaiting_response to check if SCADA closed, but
 *   awaiting_response means "waiting for PLC response" (always true after request)
 *   Fix: Check metadata_close_pending instead (set when SCADA FIN received)
 *
 * v2.257 Changes (2026-01-02):
 * - Diagnose ModScan socket error: no inbound connection logs appearing
 * - Add unconditional [SYN] log at INFO level when TCP SYN received
 * - Add unconditional [ACCEPT] log at INFO level when tcp_echo_accept succeeds
 * - Remove verbose PBUF_POOL RX-ALLOC and RX-ACCEPT logs (changed to DEBUG)
 * - Keep PBUF_POOL RX-REJECT at WARN level (important for debugging)
 *
 * v2.256 Changes (2026-01-02):
 * - FIX: Remove active_connections decrement from Net1 cleanup queue
 *   Root cause: active_connections tracks INBOUND TCP server connections
 *   (incremented in tcp_echo_accept), but cleanup queue processes SESSION
 *   cleanup for OUTBOUND connections (Net1→PLC). These are NOT 1:1.
 *   Fix: Only decrement total_connections_closed in cleanup queue.
 *
 * v2.255 Changes (2026-01-02):
 * - Reduce debug level to INFO (sentinel fix verified working)
 * - Remaining issue: Net1 active_connections counter underflow
 *
 * v2.254 Changes (2026-01-02):
 * - FIX: Set metadata.payload_length=0 when sending sentinel messages
 *   Net0: 4 locations, Net1: 2 locations
 *   Root cause: msg.payload_length=0 but metadata.payload_length had stale value
 *   This caused "Payload length mismatch" false positives in ICS validation
 *
 * v2.253 Changes (2026-01-02):
 * - Enable DEBUG_LEVEL_DEBUG for all components to trace sentinel issue
 * - Net0, Net1, ICS_Inbound all set to verbose output
 *
 * v2.252 Changes (2026-01-02):
 * - Fix Flaw 3 (complete): Don't enqueue cleanup from tcp_echo_poll()
 *   Let tcp_echo_recv(p=NULL) handle cleanup when FIN handshake completes
 *   (Actually fixes "Connection closed but no metadata found" errors)
 *
 * v2.251 Changes (2026-01-02):
 * - Fix Flaw 1: Add missing session_id in Net1 response path
 *   (Net0 can now correlate responses with sessions)
 * - Fix Flaw 3 (partial): Don't set meta->pcb=NULL in tcp_echo_poll()
 *   (Was incomplete - cleanup queue still ran before recv callback)
 *
 * v2.250 Changes (2026-01-02):
 * - EverParse v3 parser with trailing byte attack detection
 * - InputLength validation: Rejects packets where actual size != declared size
 * - Prevents CVE-2019-14462 style attacks (MBAP Length under-declaration)
 * - Reverted sentinel false positive fix for debugging
 */

/*
 * Feature flags for this version
 * Enable/disable features at compile time
 */
#define ICS_FEATURE_EVERPARSE       1   /* EverParse Modbus TCP validation */
#define ICS_FEATURE_SESSION_TRACKING 1  /* Session ID for SCADA↔PLC mapping */
#define ICS_FEATURE_CONTROL_QUEUE   1   /* Lock-free close/error queues */

/*
 * Component version strings (for startup banners)
 */
#define ICS_INBOUND_VERSION   "ICS_Inbound v" ICS_VERSION_STRING
#define ICS_OUTBOUND_VERSION  "ICS_Outbound v" ICS_VERSION_STRING
#define NET0_VERSION          "VirtIO_Net0 v" ICS_VERSION_STRING
#define NET1_VERSION          "VirtIO_Net1 v" ICS_VERSION_STRING

#endif /* ICS_VERSION_H */
