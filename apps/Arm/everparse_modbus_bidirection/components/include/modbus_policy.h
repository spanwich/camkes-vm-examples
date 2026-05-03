/*
 * Modbus Address Policy Enforcement Layer
 *
 * This layer sits AFTER EverParse validation and enforces runtime-configurable
 * address policies. EverParse validates protocol correctness; this layer
 * validates semantic correctness (address ranges).
 *
 * CVE-2022-0367 Mitigation:
 * The vulnerability exists because libmodbus only validates read_address
 * but not write_address in FC 0x17. This policy layer validates BOTH.
 *
 * Usage:
 *   1. First validate with EverParse (protocol correctness)
 *   2. Then validate with policy layer (address ranges)
 *
 * Author: Ford (Saranachon) Iammongkol
 * Date: 2026-01-06
 */

#ifndef MODBUS_POLICY_H
#define MODBUS_POLICY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Address Policy Configuration
 *
 * Configure the valid address ranges for each memory type.
 * Addresses outside these ranges will be REJECTED.
 */
typedef struct {
    /* Holding Registers (FC 0x03, 0x06, 0x10, 0x17) */
    uint16_t holding_reg_start;     /* First valid address (inclusive) */
    uint16_t holding_reg_end;       /* Last valid address (inclusive) */

    /* Input Registers (FC 0x04) - read-only */
    uint16_t input_reg_start;
    uint16_t input_reg_end;

    /* Coils (FC 0x01, 0x05, 0x0F) */
    uint16_t coil_start;
    uint16_t coil_end;

    /* Discrete Inputs (FC 0x02) - read-only */
    uint16_t discrete_input_start;
    uint16_t discrete_input_end;
} modbus_policy_t;

/*
 * Policy Validation Result
 */
typedef enum {
    POLICY_OK = 0,
    POLICY_ERR_ADDRESS_TOO_LOW,      /* Address < allowed start */
    POLICY_ERR_ADDRESS_TOO_HIGH,     /* Address > allowed end */
    POLICY_ERR_RANGE_OVERFLOW,       /* Address + quantity exceeds allowed end */
    POLICY_ERR_UNKNOWN_FC,           /* Function code not recognized */
    POLICY_ERR_INVALID_POLICY,       /* Policy configuration invalid */
} policy_result_t;

/*
 * Policy Validation Error Details
 */
typedef struct {
    policy_result_t result;
    uint8_t function_code;
    const char *field_name;          /* "read_address", "write_address", etc. */
    uint16_t actual_address;
    uint16_t actual_quantity;
    uint16_t allowed_start;
    uint16_t allowed_end;
} policy_error_t;

/*
 * Initialize policy with default values (allow all addresses 0-65535)
 */
static inline void modbus_policy_init_permissive(modbus_policy_t *policy) {
    policy->holding_reg_start = 0;
    policy->holding_reg_end = 65535;
    policy->input_reg_start = 0;
    policy->input_reg_end = 65535;
    policy->coil_start = 0;
    policy->coil_end = 65535;
    policy->discrete_input_start = 0;
    policy->discrete_input_end = 65535;
}

/*
 * Initialize policy for CVE-2022-0367 test case
 * START_REGISTERS = 100, NB_REGISTERS = 10
 */
static inline void modbus_policy_init_cve_test(modbus_policy_t *policy) {
    /* Holding registers: 100-109 */
    policy->holding_reg_start = 100;
    policy->holding_reg_end = 109;

    /* Input registers: same range */
    policy->input_reg_start = 100;
    policy->input_reg_end = 109;

    /* Coils: allow all (not used in CVE test) */
    policy->coil_start = 0;
    policy->coil_end = 65535;

    /* Discrete inputs: allow all */
    policy->discrete_input_start = 0;
    policy->discrete_input_end = 65535;
}

/*
 * Validate a single address range
 * Returns true if valid, false if out of policy
 */
static inline bool policy_check_range(
    uint16_t address,
    uint16_t quantity,
    uint16_t allowed_start,
    uint16_t allowed_end,
    policy_error_t *error
) {
    /* Check start address */
    if (address < allowed_start) {
        if (error) {
            error->result = POLICY_ERR_ADDRESS_TOO_LOW;
            error->actual_address = address;
            error->actual_quantity = quantity;
            error->allowed_start = allowed_start;
            error->allowed_end = allowed_end;
        }
        return false;
    }

    if (address > allowed_end) {
        if (error) {
            error->result = POLICY_ERR_ADDRESS_TOO_HIGH;
            error->actual_address = address;
            error->actual_quantity = quantity;
            error->allowed_start = allowed_start;
            error->allowed_end = allowed_end;
        }
        return false;
    }

    /* Check that address + quantity - 1 <= allowed_end
     * Rewritten as: quantity - 1 <= allowed_end - address (safe since address <= allowed_end)
     */
    if (quantity > 0 && (quantity - 1) > (allowed_end - address)) {
        if (error) {
            error->result = POLICY_ERR_RANGE_OVERFLOW;
            error->actual_address = address;
            error->actual_quantity = quantity;
            error->allowed_start = allowed_start;
            error->allowed_end = allowed_end;
        }
        return false;
    }

    return true;
}

/*
 * Extract fields from Modbus TCP packet
 * Assumes packet has already passed EverParse validation
 */

/* MBAP header offsets */
#define MBAP_TRANSACTION_ID  0
#define MBAP_PROTOCOL_ID     2
#define MBAP_LENGTH          4
#define MBAP_UNIT_ID         6
#define MBAP_FUNCTION_CODE   7
#define MBAP_DATA_START      8

/* Read 16-bit big-endian value */
static inline uint16_t policy_read_be16(const uint8_t *buf) {
    return ((uint16_t)buf[0] << 8) | buf[1];
}

/*
 * Validate Modbus request against address policy
 *
 * This function should be called AFTER EverParse validation passes.
 * EverParse ensures protocol correctness; this ensures policy compliance.
 *
 * Parameters:
 *   packet     - Modbus TCP packet (starting from MBAP header)
 *   packet_len - Total packet length
 *   policy     - Address policy configuration
 *   error      - [out] Error details if validation fails (can be NULL)
 *
 * Returns:
 *   true if packet complies with policy, false otherwise
 */
static inline bool modbus_policy_validate_request(
    const uint8_t *packet,
    uint16_t packet_len,
    const modbus_policy_t *policy,
    policy_error_t *error
) {
    if (packet_len < MBAP_DATA_START) {
        return false;  /* Too short - should have been caught by EverParse */
    }

    uint8_t fc = packet[MBAP_FUNCTION_CODE];

    if (error) {
        error->function_code = fc;
        error->result = POLICY_OK;
    }

    switch (fc) {
        /* ===================================================================
         * FC 0x01: Read Coils
         * FC 0x02: Read Discrete Inputs
         * Format: [FC][StartAddress:2][Quantity:2]
         * =================================================================== */
        case 0x01: {  /* Read Coils */
            uint16_t start_addr = policy_read_be16(&packet[MBAP_DATA_START]);
            uint16_t quantity = policy_read_be16(&packet[MBAP_DATA_START + 2]);
            if (error) error->field_name = "coil_address";
            return policy_check_range(start_addr, quantity,
                                      policy->coil_start, policy->coil_end, error);
        }

        case 0x02: {  /* Read Discrete Inputs */
            uint16_t start_addr = policy_read_be16(&packet[MBAP_DATA_START]);
            uint16_t quantity = policy_read_be16(&packet[MBAP_DATA_START + 2]);
            if (error) error->field_name = "discrete_input_address";
            return policy_check_range(start_addr, quantity,
                                      policy->discrete_input_start,
                                      policy->discrete_input_end, error);
        }

        /* ===================================================================
         * FC 0x03: Read Holding Registers
         * FC 0x04: Read Input Registers
         * Format: [FC][StartAddress:2][Quantity:2]
         * =================================================================== */
        case 0x03: {  /* Read Holding Registers */
            uint16_t start_addr = policy_read_be16(&packet[MBAP_DATA_START]);
            uint16_t quantity = policy_read_be16(&packet[MBAP_DATA_START + 2]);
            if (error) error->field_name = "holding_register_address";
            return policy_check_range(start_addr, quantity,
                                      policy->holding_reg_start,
                                      policy->holding_reg_end, error);
        }

        case 0x04: {  /* Read Input Registers */
            uint16_t start_addr = policy_read_be16(&packet[MBAP_DATA_START]);
            uint16_t quantity = policy_read_be16(&packet[MBAP_DATA_START + 2]);
            if (error) error->field_name = "input_register_address";
            return policy_check_range(start_addr, quantity,
                                      policy->input_reg_start,
                                      policy->input_reg_end, error);
        }

        /* ===================================================================
         * FC 0x05: Write Single Coil
         * Format: [FC][OutputAddress:2][OutputValue:2]
         * =================================================================== */
        case 0x05: {  /* Write Single Coil */
            uint16_t output_addr = policy_read_be16(&packet[MBAP_DATA_START]);
            if (error) error->field_name = "coil_address";
            return policy_check_range(output_addr, 1,
                                      policy->coil_start, policy->coil_end, error);
        }

        /* ===================================================================
         * FC 0x06: Write Single Register
         * Format: [FC][RegisterAddress:2][RegisterValue:2]
         * =================================================================== */
        case 0x06: {  /* Write Single Register */
            uint16_t reg_addr = policy_read_be16(&packet[MBAP_DATA_START]);
            if (error) error->field_name = "holding_register_address";
            return policy_check_range(reg_addr, 1,
                                      policy->holding_reg_start,
                                      policy->holding_reg_end, error);
        }

        /* ===================================================================
         * FC 0x0F (15): Write Multiple Coils
         * Format: [FC][StartAddress:2][Quantity:2][ByteCount:1][Values...]
         * =================================================================== */
        case 0x0F: {  /* Write Multiple Coils */
            uint16_t start_addr = policy_read_be16(&packet[MBAP_DATA_START]);
            uint16_t quantity = policy_read_be16(&packet[MBAP_DATA_START + 2]);
            if (error) error->field_name = "coil_address";
            return policy_check_range(start_addr, quantity,
                                      policy->coil_start, policy->coil_end, error);
        }

        /* ===================================================================
         * FC 0x10 (16): Write Multiple Registers
         * Format: [FC][StartAddress:2][Quantity:2][ByteCount:1][Values...]
         * =================================================================== */
        case 0x10: {  /* Write Multiple Registers */
            uint16_t start_addr = policy_read_be16(&packet[MBAP_DATA_START]);
            uint16_t quantity = policy_read_be16(&packet[MBAP_DATA_START + 2]);
            if (error) error->field_name = "holding_register_address";
            return policy_check_range(start_addr, quantity,
                                      policy->holding_reg_start,
                                      policy->holding_reg_end, error);
        }

        /* ===================================================================
         * FC 0x17 (23): Read/Write Multiple Registers
         * Format: [FC][ReadAddr:2][ReadQty:2][WriteAddr:2][WriteQty:2][ByteCount:1][Values...]
         *
         * CVE-2022-0367 MITIGATION:
         * This function code requires validating BOTH read AND write addresses.
         * The libmodbus vulnerability was that it only validated the read address.
         * =================================================================== */
        case 0x17: {  /* Read/Write Multiple Registers */
            uint16_t read_addr = policy_read_be16(&packet[MBAP_DATA_START]);
            uint16_t read_qty = policy_read_be16(&packet[MBAP_DATA_START + 2]);
            uint16_t write_addr = policy_read_be16(&packet[MBAP_DATA_START + 4]);
            uint16_t write_qty = policy_read_be16(&packet[MBAP_DATA_START + 6]);

            /* Validate READ address range */
            if (error) error->field_name = "read_address";
            if (!policy_check_range(read_addr, read_qty,
                                    policy->holding_reg_start,
                                    policy->holding_reg_end, error)) {
                return false;
            }

            /* Validate WRITE address range - CVE-2022-0367 FIX */
            if (error) error->field_name = "write_address";
            if (!policy_check_range(write_addr, write_qty,
                                    policy->holding_reg_start,
                                    policy->holding_reg_end, error)) {
                return false;
            }

            return true;
        }

        default:
            /* Unknown function code - should have been rejected by EverParse */
            if (error) {
                error->result = POLICY_ERR_UNKNOWN_FC;
                error->field_name = "function_code";
            }
            return false;
    }
}

/*
 * Get human-readable error message
 */
static inline const char* policy_error_message(policy_result_t result) {
    switch (result) {
        case POLICY_OK:                  return "OK";
        case POLICY_ERR_ADDRESS_TOO_LOW: return "Address below allowed range";
        case POLICY_ERR_ADDRESS_TOO_HIGH: return "Address above allowed range";
        case POLICY_ERR_RANGE_OVERFLOW:  return "Address range exceeds allowed end";
        case POLICY_ERR_UNKNOWN_FC:      return "Unknown function code";
        case POLICY_ERR_INVALID_POLICY:  return "Invalid policy configuration";
        default:                         return "Unknown error";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_POLICY_H */
