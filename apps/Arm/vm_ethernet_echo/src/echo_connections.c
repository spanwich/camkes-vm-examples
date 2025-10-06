/*
 * echo_connections.c - Initialize EchoComponent connections
 *
 * This file initializes dataport connections between EthernetDriver and EchoComponent.
 *
 * Copyright 2025, PhD Research Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <camkes.h>

/*
 * Initialize echo component connections
 *
 * In regular CAmkES (non-VM) components:
 * - Components with 'control;' attribute automatically get their run() called
 * - Dataport connections are set up by CAmkES glue code
 * - Notifications work automatically through generated interfaces
 */
void init_echo_connections(void)
{
    /* Dataports are already set up by CAmkES - just verify they exist */
    if (!rx_packet_buffer || !tx_packet_buffer) {
        printf("EthernetDriver: ERROR: Dataport connections not initialized\n");
        return;
    }
    /* Connections verified - EchoComponent should start automatically */
}
