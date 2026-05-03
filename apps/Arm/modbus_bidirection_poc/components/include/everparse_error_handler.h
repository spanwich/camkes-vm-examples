/*
 * EverParse Error Handler for ICS Bidirectional Gateway
 *
 * Implements error reporting callback required by EverParse wrapper.
 * Integrates with gateway's debug_levels.h logging system.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef EVERPARSE_ERROR_HANDLER_H
#define EVERPARSE_ERROR_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Error handler callback for EverParse validation failures
 * Called by ModbusTCP_SimpleWrapper when validation fails
 *
 * Parameters:
 *   StructName - Name of the Modbus structure being validated (e.g., "_MODBUS_READ_REQUEST")
 *   FieldName  - Field that failed validation (e.g., "Quantity", "ProtocolId")
 *   Reason     - Human-readable failure reason (e.g., "constraint failed")
 */
void ModbusTCP_SimpleEverParseError(const char *StructName,
                                     const char *FieldName,
                                     const char *Reason);

#ifdef __cplusplus
}
#endif

#endif /* EVERPARSE_ERROR_HANDLER_H */
