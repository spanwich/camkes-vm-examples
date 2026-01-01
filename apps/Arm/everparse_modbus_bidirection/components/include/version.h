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
#define ICS_VERSION_MINOR   253
#define ICS_VERSION_STRING  "2.253"
#define ICS_VERSION_DATE    "2026-01-02"

/*
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
