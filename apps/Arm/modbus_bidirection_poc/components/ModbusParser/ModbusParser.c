/*
 * ModbusParser - Isolated EverParse Validation Component
 *
 * Contains ONLY the formally verified EverParse parser code.
 * Runs in a separate seL4 address space from ICS_Inbound/ICS_Outbound.
 *
 * Serves both ICS_Inbound and ICS_Outbound via seL4RPCDataport:
 * - Each client has a dedicated 4KB shared buffer
 * - Client writes payload, calls RPC with length
 * - Server reads from per-client buffer, validates, returns verdict
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define DEBUG_LEVEL DEBUG_LEVEL_INFO
#include "debug_levels.h"

#include <camkes.h>
#include <string.h>
#include <stdint.h>
#include "ModbusTCP_v3_SimpleWrapper.h"

#define MODBUS_MAX_FRAME_SIZE 260
#define MODBUS_MIN_FRAME_SIZE 8

int parser_validate(int payload_length) {
    if (payload_length < MODBUS_MIN_FRAME_SIZE ||
        payload_length > MODBUS_MAX_FRAME_SIZE) {
        return -1;
    }

    seL4_Word badge = parser_get_sender_id();
    uint8_t *buf = (uint8_t *)parser_buf(badge);
    if (!buf) {
        return -1;
    }

    uint32_t input_length = (uint32_t)payload_length;
    BOOLEAN result = ModbusTcpV3SimpleCheckModbusTcpFrameV3(
        input_length, buf, input_length);

    return result ? 1 : 0;
}

int run(void) {
    DEBUG_INFO("ModbusParser: Isolated EverParse validator ready\n");
    /* CAmkES RPC server loop handles dispatch automatically */
    return 0;
}
