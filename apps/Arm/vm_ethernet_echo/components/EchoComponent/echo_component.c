/*
 * EchoComponent - Stage 1: Simple TCP Payload Echo via Dataport
 *
 * This component demonstrates zero-copy packet transfer between CAmkES components.
 * It receives TCP payload data from EthernetDriver via a shared memory dataport,
 * adds an "ECHO: " prefix, and sends it back via another dataport.
 *
 * Architecture:
 *   EthernetDriver → [rx_packet_buffer] → EchoComponent → [tx_packet_buffer] → EthernetDriver
 *
 * Copyright 2025, PhD Research Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>

#define COMPONENT_NAME "EchoComponent"

/*
 * REMOVED pre_init() and post_init() hooks to match working components
 * (vm_cross_connector and test_two_components don't have these)
 *
 * These hooks may interfere with component startup timing.
 */

/*
 * Main component loop
 *
 * Waits for TCP payload from EthernetDriver, processes it, and sends response.
 */
int run(void)
{
    /* CRITICAL: Print immediately to prove component started */
    printf("\n\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  ECHOCOMPONENT STARTED - run() function entered!        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("%s: Starting echo service (Stage 1)\n", COMPONENT_NAME);
    printf("%s: Waiting for network packets via dataport...\n", COMPONENT_NAME);
    fflush(stdout);  /* Force output before blocking on wait */

    unsigned int packet_count = 0;

    while (1) {
        /* Step 1: Wait for notification from EthernetDriver */
        rx_packet_ready_wait();
        packet_count++;

        printf("\n%s: ═══ Packet #%u received ═══\n", COMPONENT_NAME, packet_count);

        /* Step 2: Read TCP payload from rx_packet_buffer dataport */
        /* Note: Driver writes payload as null-terminated string */
        char *input_data = (char *)rx_packet_buffer;
        size_t input_len = strlen(input_data);

        printf("%s: RX Data (%zu bytes): \"%s\"\n", COMPONENT_NAME, input_len, input_data);

        /* Step 3: Process data - Add "ECHO: " prefix for demonstration */
        /* This is where we could add sanitization, protocol parsing, etc. */
        memset(tx_packet_buffer, 0, 2048);  /* Clear TX buffer */
        snprintf((char *)tx_packet_buffer, 2048, "ECHO: %s", input_data);

        size_t output_len = strlen((char *)tx_packet_buffer);
        printf("%s: TX Data (%zu bytes): \"%s\"\n", COMPONENT_NAME, output_len,
               (char *)tx_packet_buffer);

        /* Step 4: Signal EthernetDriver that echo response is ready */
        tx_packet_done_emit();

        printf("%s: Response sent to driver\n", COMPONENT_NAME);
    }

    return 0;
}
