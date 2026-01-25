/*
 * Modbus Response Policy F* - Implementation
 *
 * Routes Modbus response validation to appropriate EverParse/F* verified parser.
 * Implements symmetric bidirectional validation for Zero Trust ICS architecture.
 *
 * VERIFICATION:
 *   The underlying parsers are extracted from F* specifications that have
 *   been type-checked by F*. Response structure and policy constraints
 *   are mathematically proven correct.
 *
 * Author: Ford (Saranachon) Iammongkol
 * Date: 2026-01-26
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "modbus_response_policy_fstar.h"
#include "ModbusTCP_v5_ResponsePolicyWrapper.h"

/*
 * =============================================================================
 * GLOBAL RESPONSE POLICY CONFIGURATION
 * =============================================================================
 */

/* Maximum bytes in data responses (exfiltration prevention) */
uint16_t g_response_max_bytes = 250;  /* Protocol maximum */

/* Address ranges for echo validation (default: CVE test case) */
uint16_t g_response_holding_reg_start = 100;
uint16_t g_response_holding_reg_end = 109;
uint16_t g_response_input_reg_start = 100;
uint16_t g_response_input_reg_end = 109;
uint16_t g_response_coil_start = 0;
uint16_t g_response_coil_end = 65535;
uint16_t g_response_discrete_input_start = 0;
uint16_t g_response_discrete_input_end = 65535;

/*
 * =============================================================================
 * RESPONSE CLASSIFICATION
 * =============================================================================
 */

/* Response function code categories */
#define FC_CLASS_DATA       0  /* Data responses: FC 0x01-0x04, 0x17 */
#define FC_CLASS_ECHO       1  /* Echo responses: FC 0x05, 0x06, 0x0F, 0x10 */
#define FC_CLASS_EXCEPTION  2  /* Exception responses: FC | 0x80 */
#define FC_CLASS_INVALID    3  /* Unknown FC */

static uint8_t classify_response_fc(uint8_t fc) {
    /* Data responses (read operations return data) */
    if (fc >= 0x01 && fc <= 0x04) {
        return FC_CLASS_DATA;
    }
    /* FC 0x17 response is also data response (returns read data) */
    if (fc == 0x17) {
        return FC_CLASS_DATA;
    }
    /* Echo responses (write operations echo request) */
    if (fc == 0x05 || fc == 0x06 || fc == 0x0F || fc == 0x10) {
        return FC_CLASS_ECHO;
    }
    /* Exception responses (FC | 0x80) */
    if (fc & 0x80) {
        /* Verify the base FC is valid */
        uint8_t base_fc = fc & 0x7F;
        if (base_fc == 0x01 || base_fc == 0x02 || base_fc == 0x03 ||
            base_fc == 0x04 || base_fc == 0x05 || base_fc == 0x06 ||
            base_fc == 0x0F || base_fc == 0x10 || base_fc == 0x17) {
            return FC_CLASS_EXCEPTION;
        }
    }
    return FC_CLASS_INVALID;
}

/*
 * =============================================================================
 * UNIFIED RESPONSE VALIDATION IMPLEMENTATION
 * =============================================================================
 */

uint8_t modbus_validate_response_with_policy(const uint8_t *payload, uint16_t payload_len) {
    /* Minimum size check: MBAP header (7 bytes) + Function Code (1 byte) */
    if (payload_len < 8) {
        return FSTAR_RESPONSE_TOO_SHORT;
    }

    /* Extract function code from MBAP + PDU */
    uint8_t fc = payload[7];
    uint8_t fc_class = classify_response_fc(fc);

    BOOLEAN result;

    switch (fc_class) {
        case FC_CLASS_DATA:
            /* Route to data response validator based on FC */
            switch (fc) {
                case 0x01:  /* Read Coils Response */
                    result = ModbusTcpV5ResponsePolicyCheckModbusReadCoilsResponsePolicy(
                        (uint32_t)payload_len,
                        g_response_max_bytes,
                        (uint8_t *)payload,
                        (uint32_t)payload_len
                    );
                    break;

                case 0x02:  /* Read Discrete Inputs Response */
                    result = ModbusTcpV5ResponsePolicyCheckModbusReadDiscreteInputsResponsePolicy(
                        (uint32_t)payload_len,
                        g_response_max_bytes,
                        (uint8_t *)payload,
                        (uint32_t)payload_len
                    );
                    break;

                case 0x03:  /* Read Holding Registers Response */
                    result = ModbusTcpV5ResponsePolicyCheckModbusReadHoldingRegistersResponsePolicy(
                        (uint32_t)payload_len,
                        g_response_max_bytes,
                        (uint8_t *)payload,
                        (uint32_t)payload_len
                    );
                    break;

                case 0x04:  /* Read Input Registers Response */
                    result = ModbusTcpV5ResponsePolicyCheckModbusReadInputRegistersResponsePolicy(
                        (uint32_t)payload_len,
                        g_response_max_bytes,
                        (uint8_t *)payload,
                        (uint32_t)payload_len
                    );
                    break;

                case 0x17:  /* Read/Write Multiple Registers Response */
                    result = ModbusTcpV5ResponsePolicyCheckModbusReadWriteRegistersResponsePolicy(
                        (uint32_t)payload_len,
                        g_response_max_bytes,
                        (uint8_t *)payload,
                        (uint32_t)payload_len
                    );
                    break;

                default:
                    return FSTAR_RESPONSE_INVALID_FC;
            }
            break;

        case FC_CLASS_ECHO:
            /* Route to echo response validator based on FC */
            switch (fc) {
                case 0x05:  /* Write Single Coil Response (echo) */
                    result = ModbusTcpV5ResponsePolicyCheckModbusWriteSingleCoilResponsePolicy(
                        (uint32_t)payload_len,
                        g_response_coil_start,
                        g_response_coil_end,
                        (uint8_t *)payload,
                        (uint32_t)payload_len
                    );
                    break;

                case 0x06:  /* Write Single Register Response (echo) */
                    result = ModbusTcpV5ResponsePolicyCheckModbusWriteSingleRegisterResponsePolicy(
                        (uint32_t)payload_len,
                        g_response_holding_reg_start,
                        g_response_holding_reg_end,
                        (uint8_t *)payload,
                        (uint32_t)payload_len
                    );
                    break;

                case 0x0F:  /* Write Multiple Coils Response (echo) */
                    result = ModbusTcpV5ResponsePolicyCheckModbusWriteMultipleCoilsResponsePolicy(
                        (uint32_t)payload_len,
                        g_response_coil_start,
                        g_response_coil_end,
                        (uint8_t *)payload,
                        (uint32_t)payload_len
                    );
                    break;

                case 0x10:  /* Write Multiple Registers Response (echo) */
                    result = ModbusTcpV5ResponsePolicyCheckModbusWriteMultipleRegistersResponsePolicy(
                        (uint32_t)payload_len,
                        g_response_holding_reg_start,
                        g_response_holding_reg_end,
                        (uint8_t *)payload,
                        (uint32_t)payload_len
                    );
                    break;

                default:
                    return FSTAR_RESPONSE_INVALID_FC;
            }
            break;

        case FC_CLASS_EXCEPTION:
            /* Validate exception response */
            result = ModbusTcpV5ResponsePolicyCheckModbusExceptionResponsePolicy(
                (uint32_t)payload_len,
                (uint8_t *)payload,
                (uint32_t)payload_len
            );
            if (!result) {
                return FSTAR_RESPONSE_INVALID_EXCEPTION;
            }
            return FSTAR_RESPONSE_OK;

        case FC_CLASS_INVALID:
        default:
            return FSTAR_RESPONSE_INVALID_FC;
    }

    /* F* validation includes structure AND policy checks */
    return result ? FSTAR_RESPONSE_OK : FSTAR_RESPONSE_POLICY_REJECT;
}

/*
 * =============================================================================
 * POLICY INITIALIZATION FUNCTIONS
 * =============================================================================
 */

void fstar_response_policy_init_permissive(void) {
    g_response_max_bytes = 250;  /* Protocol maximum */
    g_response_holding_reg_start = 0;
    g_response_holding_reg_end = 65535;
    g_response_input_reg_start = 0;
    g_response_input_reg_end = 65535;
    g_response_coil_start = 0;
    g_response_coil_end = 65535;
    g_response_discrete_input_start = 0;
    g_response_discrete_input_end = 65535;
}

void fstar_response_policy_init_cve_test(void) {
    g_response_max_bytes = 250;  /* Protocol maximum */

    /* Holding registers: 100-109 (matches request policy) */
    g_response_holding_reg_start = 100;
    g_response_holding_reg_end = 109;

    /* Input registers: same range */
    g_response_input_reg_start = 100;
    g_response_input_reg_end = 109;

    /* Coils: allow all (not used in CVE test) */
    g_response_coil_start = 0;
    g_response_coil_end = 65535;

    /* Discrete inputs: allow all */
    g_response_discrete_input_start = 0;
    g_response_discrete_input_end = 65535;
}

void fstar_response_policy_init_custom(
    uint16_t max_response_bytes,
    uint16_t holding_start, uint16_t holding_end,
    uint16_t input_start, uint16_t input_end,
    uint16_t coil_start, uint16_t coil_end,
    uint16_t discrete_start, uint16_t discrete_end
) {
    g_response_max_bytes = max_response_bytes;
    g_response_holding_reg_start = holding_start;
    g_response_holding_reg_end = holding_end;
    g_response_input_reg_start = input_start;
    g_response_input_reg_end = input_end;
    g_response_coil_start = coil_start;
    g_response_coil_end = coil_end;
    g_response_discrete_input_start = discrete_start;
    g_response_discrete_input_end = discrete_end;
}

void fstar_response_policy_sync_with_request(
    uint16_t holding_start, uint16_t holding_end,
    uint16_t input_start, uint16_t input_end,
    uint16_t coil_start, uint16_t coil_end,
    uint16_t discrete_start, uint16_t discrete_end
) {
    /* Sync address ranges with request policy for symmetric validation */
    g_response_holding_reg_start = holding_start;
    g_response_holding_reg_end = holding_end;
    g_response_input_reg_start = input_start;
    g_response_input_reg_end = input_end;
    g_response_coil_start = coil_start;
    g_response_coil_end = coil_end;
    g_response_discrete_input_start = discrete_start;
    g_response_discrete_input_end = discrete_end;
}
