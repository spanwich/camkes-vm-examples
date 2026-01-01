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
#define ICS_VERSION_MINOR   241
#define ICS_VERSION_STRING  "2.241"
#define ICS_VERSION_DATE    "2025-11-15"

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
