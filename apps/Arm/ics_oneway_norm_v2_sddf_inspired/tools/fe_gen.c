/*
 * Frame Generator Test Tool
 *
 * Optional standalone tool for testing the ICS pipeline with custom messages.
 * Can be used to generate specific test vectors or replay captured ICS traffic.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "../components/include/common.h"

/*
 * Generate test messages for pipeline validation
 */
int main(int argc, char* argv[]) {
    printf("ICS Frame Generator - Test Tool for ics_oneway_norm pipeline\n");

    if (argc < 2) {
        printf("Usage: %s <test_type>\n", argv[0]);
        printf("Test types:\n");
        printf("  valid    - Generate valid messages for all protocols\n");
        printf("  invalid  - Generate malformed messages for robustness testing\n");
        printf("  stress   - Generate high-volume traffic for performance testing\n");
        return 1;
    }

    const char* test_type = argv[1];

    if (strcmp(test_type, "valid") == 0) {
        printf("Generating valid test messages...\n");

        /* MODBUS TCP message */
        MsgHeader modbus_header = {
            .tag = MODBUS_TCP_TAG,
            .len = 12,
            .flags = 0
        };
        uint8_t modbus_payload[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x01, 0x03, 0x00, 0x00, 0x00, 0x02};

        printf("MODBUS TCP: tag=0x%04X, len=%u\n", modbus_header.tag, modbus_header.len);
        printf("  Payload: ");
        for (int i = 0; i < modbus_header.len; i++) {
            printf("%02X ", modbus_payload[i]);
        }
        printf("\n");

        /* DNP3 message */
        MsgHeader dnp3_header = {
            .tag = DNP3_TAG,
            .len = 8,
            .flags = 0
        };
        uint8_t dnp3_payload[] = {0x05, 0x64, 0x05, 0xC0, 0x01, 0x00, 0x00, 0x04};

        printf("DNP3: tag=0x%04X, len=%u\n", dnp3_header.tag, dnp3_header.len);
        printf("  Payload: ");
        for (int i = 0; i < dnp3_header.len; i++) {
            printf("%02X ", dnp3_payload[i]);
        }
        printf("\n");

        /* EtherNet/IP message */
        MsgHeader eip_header = {
            .tag = ETHERNET_IP_TAG,
            .len = 6,
            .flags = 0
        };
        uint8_t eip_payload[] = {0x6F, 0x00, 0x02, 0x00, 0x00, 0x00};

        printf("EtherNet/IP: tag=0x%04X, len=%u\n", eip_header.tag, eip_header.len);
        printf("  Payload: ");
        for (int i = 0; i < eip_header.len; i++) {
            printf("%02X ", eip_payload[i]);
        }
        printf("\n");

    } else if (strcmp(test_type, "invalid") == 0) {
        printf("Generating invalid test messages...\n");

        /* Length mismatch - header claims more data than available */
        MsgHeader bad_header1 = {
            .tag = MODBUS_TCP_TAG,
            .len = 100,  /* Claims 100 bytes */
            .flags = 0
        };
        uint8_t short_payload[] = {0x01, 0x02, 0x03};  /* Only 3 bytes */

        printf("Length Mismatch: tag=0x%04X, claimed_len=%u, actual_len=%zu\n",
               bad_header1.tag, bad_header1.len, sizeof(short_payload));

        /* Invalid tag */
        MsgHeader bad_header2 = {
            .tag = 0x0000,  /* Invalid tag */
            .len = 4,
            .flags = 0
        };
        uint8_t normal_payload[] = {0xAA, 0xBB, 0xCC, 0xDD};

        printf("Invalid Tag: tag=0x%04X, len=%u\n", bad_header2.tag, bad_header2.len);

        /* Oversized payload */
        MsgHeader bad_header3 = {
            .tag = GENERIC_TAG,
            .len = MAX_PAYLOAD_SIZE + 1,  /* Too large */
            .flags = 0
        };

        printf("Oversized: tag=0x%04X, len=%u (max=%u)\n",
               bad_header3.tag, bad_header3.len, MAX_PAYLOAD_SIZE);

    } else if (strcmp(test_type, "stress") == 0) {
        printf("Generating stress test pattern...\n");

        srand((unsigned int)time(NULL));

        for (int i = 0; i < 1000; i++) {
            uint16_t tags[] = {MODBUS_TCP_TAG, DNP3_TAG, ETHERNET_IP_TAG, GENERIC_TAG};
            uint16_t tag = tags[rand() % 4];
            uint16_t len = (uint16_t)(1 + rand() % 1000);

            printf("Message %d: tag=0x%04X, len=%u\n", i, tag, len);

            /* Simulate processing delay */
            if (i % 100 == 0) {
                printf("  ... %d messages generated\n", i);
            }
        }

    } else {
        printf("Unknown test type: %s\n", test_type);
        return 1;
    }

    printf("Test generation complete.\n");
    return 0;
}