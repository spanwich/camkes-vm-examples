/*
 * Modbus Response Policy F* - Symmetric Bidirectional Validation
 *
 * This module provides response validation using EverParse/F* verified parsers.
 * Implements Zero Trust ICS architecture - validates responses from internal devices.
 *
 * SECURITY OBJECTIVES:
 *   1. Data Exfiltration Prevention - limit response data size
 *   2. Address Confusion Prevention - validate echoed addresses
 *   3. Covert Channel Mitigation - strict structure validation
 *   4. Exception Code Validation - prevent exception abuse
 *
 * VERIFICATION ARCHITECTURE:
 *   - EverParse (F*): Verifies response structure AND policy constraints
 *   - Single verification framework: End-to-end mathematically proven
 *   - Symmetric with request validation (ICS_Inbound)
 *
 * Author: Ford (Saranachon) Iammongkol
 * Date: 2026-01-26
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef MODBUS_RESPONSE_POLICY_FSTAR_H
#define MODBUS_RESPONSE_POLICY_FSTAR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 * RESPONSE POLICY CONFIGURATION (Runtime Modifiable)
 * =============================================================================
 */

/* Maximum bytes allowed in data responses (exfiltration prevention) */
extern uint16_t g_response_max_bytes;

/* Address ranges for echo response validation (same as request policy) */
extern uint16_t g_response_holding_reg_start;
extern uint16_t g_response_holding_reg_end;
extern uint16_t g_response_input_reg_start;
extern uint16_t g_response_input_reg_end;
extern uint16_t g_response_coil_start;
extern uint16_t g_response_coil_end;
extern uint16_t g_response_discrete_input_start;
extern uint16_t g_response_discrete_input_end;

/*
 * =============================================================================
 * VALIDATION RESULT CODES
 * =============================================================================
 */
#define FSTAR_RESPONSE_OK                0  /* Validation passed */
#define FSTAR_RESPONSE_SYNTAX_ERROR      1  /* Response structure invalid */
#define FSTAR_RESPONSE_POLICY_REJECT     2  /* Policy violation (size/address) */
#define FSTAR_RESPONSE_INVALID_FC        3  /* Unknown function code */
#define FSTAR_RESPONSE_TOO_SHORT         4  /* Response too short */
#define FSTAR_RESPONSE_INVALID_EXCEPTION 5  /* Invalid exception code */

/*
 * =============================================================================
 * UNIFIED RESPONSE VALIDATION FUNCTION
 * =============================================================================
 *
 * Validates Modbus TCP response using F* verified parsers.
 * Automatically routes to appropriate validator based on function code.
 *
 * Parameters:
 *   payload     - Pointer to MBAP + PDU data (starting at Transaction ID)
 *   payload_len - Length of payload in bytes
 *
 * Returns:
 *   FSTAR_RESPONSE_OK on success, error code otherwise
 *
 * Response Categories:
 *   - Data responses (FC 0x01-0x04, 0x17): Validates ByteCount <= MaxResponseBytes
 *   - Echo responses (FC 0x05, 0x06, 0x0F, 0x10): Validates address in policy range
 *   - Exception responses (FC | 0x80): Validates exception code is valid
 */
uint8_t modbus_validate_response_with_policy(const uint8_t *payload, uint16_t payload_len);

/*
 * =============================================================================
 * POLICY INITIALIZATION FUNCTIONS
 * =============================================================================
 */

/*
 * Initialize response policy with permissive defaults
 * - MaxResponseBytes = 250 (protocol max)
 * - All address ranges 0-65535
 */
void fstar_response_policy_init_permissive(void);

/*
 * Initialize response policy for CVE-2022-0367 test case
 * - MaxResponseBytes = 250
 * - Holding/Input registers: 100-109
 * - Coils/Discrete: 0-65535
 */
void fstar_response_policy_init_cve_test(void);

/*
 * Initialize response policy with custom values
 */
void fstar_response_policy_init_custom(
    uint16_t max_response_bytes,
    uint16_t holding_start, uint16_t holding_end,
    uint16_t input_start, uint16_t input_end,
    uint16_t coil_start, uint16_t coil_end,
    uint16_t discrete_start, uint16_t discrete_end
);

/*
 * Synchronize response policy with request policy
 * Call this after initializing request policy in ICS_Inbound
 * to ensure symmetric validation uses the same address ranges.
 */
void fstar_response_policy_sync_with_request(
    uint16_t holding_start, uint16_t holding_end,
    uint16_t input_start, uint16_t input_end,
    uint16_t coil_start, uint16_t coil_end,
    uint16_t discrete_start, uint16_t discrete_end
);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_RESPONSE_POLICY_FSTAR_H */
