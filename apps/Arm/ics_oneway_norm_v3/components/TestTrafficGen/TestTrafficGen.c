/*
 * Copyright 2024, PhD Research Project
 * Test Traffic Generator - Minimal ICS pipeline test component
 *
 * Generates simple test messages to verify ICS pipeline operation
 * while Linux VM runs independently
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <camkes.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Simple message format for testing */
typedef struct {
    uint16_t tag;
    uint16_t len;
    uint32_t flags;
    uint8_t payload[256];
} __attribute__((packed)) test_msg_t;

static uint64_t msg_count = 0;

int run(void) {
    printf("TestTrafficGen: Starting (ICS pipeline test component)\n");
    printf("TestTrafficGen: This component tests ICS pipeline while Linux VM runs independently\n");

    test_msg_t msg;

    while (1) {
        /* Generate a simple test message every 5 seconds */
        seL4_Yield();

        /* Sleep for ~5 seconds (approximate) */
        for (volatile int i = 0; i < 50000000; i++);

        msg_count++;

        /* Create test message */
        msg.tag = 0x0001;  // MODBUS_TCP tag
        msg.len = 32;
        msg.flags = 0;
        snprintf((char*)msg.payload, sizeof(msg.payload),
                 "TEST_MSG_%lu_FROM_TRAFFIC_GEN", msg_count);

        /* Write to output dataport */
        memcpy((void*)out_dp, &msg, sizeof(msg));

        /* Notify ExtFrontend */
        out_ntfy_emit_underlying();

        printf("TestTrafficGen: Sent test message #%lu to ICS pipeline\n", msg_count);
    }

    return 0;
}
