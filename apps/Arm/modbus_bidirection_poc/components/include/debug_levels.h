/**
 * debug_levels.h - Industry Standard Debug Level System
 *
 * 5-level hierarchical debug system (RFC 5424 / syslog style)
 *
 * Levels (each level includes all above):
 *   0 (NONE)  - No output (production/timing-sensitive)
 *   1 (ERROR) - Critical failures only [ERR]
 *   2 (WARN)  - Errors + Warnings [ERR] [WARN]
 *   3 (INFO)  - + Operational info (stats, lifecycle, connections)
 *   4 (DEBUG) - + Detailed diagnostics (packet dumps, hex, protocol parsing)
 *
 * Usage:
 *   1. Define debug level in your .c file BEFORE including this header:
 *      #define DEBUG_LEVEL DEBUG_LEVEL_ERROR   // Choose level
 *
 *   2. Include this header:
 *      #include "debug_levels.h"
 *
 *   3. Use the macros in your code:
 *      DEBUG_ERROR("Fatal error: %s\n", msg);
 *      DEBUG_WARN("Warning: %s\n", msg);
 *      DEBUG_INFO("Connection established\n");
 *      DEBUG("Packet details: len=%u\n", len);
 *
 * Recommended defaults:
 *   - Production:           DEBUG_LEVEL_ERROR (only critical failures)
 *   - Production Monitoring: DEBUG_LEVEL_WARN (catch potential issues)
 *   - Development/Testing:  DEBUG_LEVEL_INFO (see operational flow)
 *   - Deep Debugging:       DEBUG_LEVEL_DEBUG (full packet traces)
 *
 * Revision: v2.207
 * Date: 2025-10-30
 * Standard: RFC 5424 (syslog), log4j, Python logging compatible
 */

#ifndef DEBUG_LEVELS_H
#define DEBUG_LEVELS_H

#include <stdio.h>
#include <stdint.h>

/* =============================================================================
 * DEBUG LEVEL DEFINITIONS (Industry Standard)
 * =============================================================================
 */

#define DEBUG_LEVEL_NONE   0  /* No output at all */
#define DEBUG_LEVEL_ERROR  1  /* Critical failures only */
#define DEBUG_LEVEL_WARN   2  /* Errors + Warnings */
#define DEBUG_LEVEL_INFO   3  /* + Operational information */
#define DEBUG_LEVEL_DEBUG  4  /* + Detailed diagnostics */

/* =============================================================================
 * DEBUG LEVEL VALIDATION
 * =============================================================================
 * Ensure DEBUG_LEVEL is defined before including this header
 */

#ifndef DEBUG_LEVEL
    #error "Must define DEBUG_LEVEL before including debug_levels.h"
    #error "Example: #define DEBUG_LEVEL DEBUG_LEVEL_INFO"
#endif

#if DEBUG_LEVEL < DEBUG_LEVEL_NONE || DEBUG_LEVEL > DEBUG_LEVEL_DEBUG
    #error "Invalid DEBUG_LEVEL: must be 0-4 (NONE/ERROR/WARN/INFO/DEBUG)"
#endif

/* =============================================================================
 * LEVEL 1: ERROR - Critical Failures
 * =============================================================================
 * Use for: Fatal errors, NULL pointers, allocation failures, system halt
 * Examples: "[ERR] FATAL: outbound_dp is NULL"
 *           "[ERR] Invalid buffer index"
 */

#if DEBUG_LEVEL >= DEBUG_LEVEL_ERROR
    #define DEBUG_ERROR(...)   printf(__VA_ARGS__)
#else
    #define DEBUG_ERROR(...)   do {} while(0)
#endif

/* =============================================================================
 * LEVEL 2: WARN - Warnings
 * =============================================================================
 * Use for: Resource exhaustion, invalid data, recoverable errors
 * Examples: "[WARN] Failed to allocate pbuf - dropping packet"
 *           "[WARN] INVALID packet length"
 *           "[WARN] Connection table full"
 */

#if DEBUG_LEVEL >= DEBUG_LEVEL_WARN
    #define DEBUG_WARN(...)    printf(__VA_ARGS__)
#else
    #define DEBUG_WARN(...)    do {} while(0)
#endif

/* =============================================================================
 * LEVEL 3: INFO - Operational Information
 * =============================================================================
 * Use for: Connection lifecycle, periodic statistics, operational status
 * Examples: "[OK] Connection established"
 *           "[PBUF-STATS] Pool: 245/800"
 *           "TCP server listening on port 502"
 */

#if DEBUG_LEVEL >= DEBUG_LEVEL_INFO
    #define DEBUG_INFO(...)    printf(__VA_ARGS__)
#else
    #define DEBUG_INFO(...)    do {} while(0)
#endif

/* =============================================================================
 * LEVEL 4: DEBUG - Detailed Diagnostics
 * =============================================================================
 * Use for: Packet dumps, protocol parsing, hex output, detailed traces
 * Examples: "Ethernet: 00:11:22:33:44:55 → ..."
 *           "IP: 192.168.1.1 → 192.168.1.2"
 *           Hex dumps, box drawing, packet analysis
 */

#if DEBUG_LEVEL >= DEBUG_LEVEL_DEBUG
    #define DEBUG(...)         printf(__VA_ARGS__)
    #define DEBUG_VERBOSE(...) printf(__VA_ARGS__)  /* Alias for readability */
#else
    #define DEBUG(...)         do {} while(0)
    #define DEBUG_VERBOSE(...) do {} while(0)
#endif

/* =============================================================================
 * CONDITIONAL COMPILATION HELPERS
 * =============================================================================
 * Use these to conditionally compile code blocks
 *
 * Example:
 *   #if DEBUG_ENABLED_DEBUG
 *       hex_dump_packet(data, len);
 *   #endif
 */

#define DEBUG_ENABLED_ERROR  (DEBUG_LEVEL >= DEBUG_LEVEL_ERROR)
#define DEBUG_ENABLED_WARN   (DEBUG_LEVEL >= DEBUG_LEVEL_WARN)
#define DEBUG_ENABLED_INFO   (DEBUG_LEVEL >= DEBUG_LEVEL_INFO)
#define DEBUG_ENABLED_DEBUG  (DEBUG_LEVEL >= DEBUG_LEVEL_DEBUG)

/* =============================================================================
 * LEGACY COMPATIBILITY MACROS
 * =============================================================================
 * Map old debug flags to new system for backwards compatibility
 * Old code using these flags will still compile
 */

#if DEBUG_LEVEL >= DEBUG_LEVEL_ERROR
    #define DEBUG_CRITICAL      1
#else
    #define DEBUG_CRITICAL      0
#endif

#if DEBUG_LEVEL >= DEBUG_LEVEL_INFO
    #define DEBUG_TRAFFIC       1
    #define DEBUG_METADATA      1
    #define DEBUG_INIT          1
#else
    #define DEBUG_TRAFFIC       0
    #define DEBUG_METADATA      0
    #define DEBUG_INIT          0
#endif

#if DEBUG_LEVEL >= DEBUG_LEVEL_DEBUG
    #define DEBUG_PACKET_DETAIL 1
#else
    #define DEBUG_PACKET_DETAIL 0
#endif

/* =============================================================================
 * BREADCRUMB SYSTEM (v2.208 - Centralized from common.h)
 * =============================================================================
 * Simple execution tracing for debugging race conditions and timing issues.
 * Prints minimal markers (e.g., "B8001") to track execution flow.
 *
 * Control:
 *   BREADCRUMB_TRACE=0 (default) - Disabled (no output)
 *   BREADCRUMB_TRACE=1            - Enabled at DEBUG level only
 *
 * Behavior:
 *   - When BREADCRUMB_TRACE=0: BREADCRUMB() compiles to no-op
 *   - When BREADCRUMB_TRACE=1: BREADCRUMB() prints ONLY if DEBUG_LEVEL >= DEBUG
 *
 * Usage:
 *   BREADCRUMB(8001);  // Prints "B8001" at DEBUG level (if TRACE=1)
 *
 * Note: Previous buffer-based implementation removed (never instantiated)
 */

#ifndef BREADCRUMB_TRACE
    #define BREADCRUMB_TRACE 0  /* Disabled by default - use only for race debugging */
#endif

#ifndef BREADCRUMB
    #if BREADCRUMB_TRACE
        /* Enabled: Print breadcrumb at DEBUG level only */
        #if DEBUG_LEVEL >= DEBUG_LEVEL_DEBUG
            #define BREADCRUMB(id) printf("B%d\n", (id))
        #else
            #define BREADCRUMB(id) do {} while(0)  /* Not at DEBUG level - no-op */
        #endif
    #else
        /* Disabled: Always no-op */
        #define BREADCRUMB(id) do {} while(0)
    #endif
#endif

#endif /* DEBUG_LEVELS_H */
