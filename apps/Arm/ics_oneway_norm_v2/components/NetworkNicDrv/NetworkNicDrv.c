/*
 * Network NIC Driver Implementation - V2 (Dataport Version)
 *
 * Receives ICS protocol messages from VM via dataports
 * Converts VM messages to seL4 pipeline format and forwards to ExtFrontend
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <camkes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils/util.h>
#include "common.h"
#include "ringbuf.h"

/* Global state for this component */
static RingBuffer* output_buffer;
static ComponentStats stats;
static uint32_t message_counter = 0;

/* Global timestamp counter definition */
uint64_t global_timestamp_counter = 0;

/*
 * Parse message received from VM and convert to MsgHeader format
 * Expected VM message format: JSON-like or simple string
 * For now, we'll support simple string messages and map them to protocol tags
 */
static bool parse_vm_message(const char* vm_data, size_t vm_len, MsgHeader* header, uint8_t* payload_out) {
    printf("NetworkNicDrv: Parsing VM message: %.*s\n", (int)vm_len, vm_data);

    /* Simple protocol detection based on message content */
    uint16_t protocol_tag = GENERIC_TAG;  /* Default */

    if (strstr(vm_data, "MODBUS") || strstr(vm_data, "modbus")) {
        protocol_tag = MODBUS_TCP_TAG;
    } else if (strstr(vm_data, "DNP3") || strstr(vm_data, "dnp3")) {
        protocol_tag = DNP3_TAG;
    } else if (strstr(vm_data, "ETHERNET_IP") || strstr(vm_data, "ethernet_ip")) {
        protocol_tag = ETHERNET_IP_TAG;
    }

    /* Limit payload length to prevent buffer overflow */
    size_t safe_len = (vm_len > MAX_PAYLOAD_SIZE) ? MAX_PAYLOAD_SIZE : vm_len;

    /* Build header */
    header->tag = protocol_tag;
    header->len = (uint16_t)safe_len;
    header->flags = 0;

    /* Copy payload */
    memcpy(payload_out, vm_data, safe_len);

    printf("NetworkNicDrv: Converted to protocol tag=0x%04X, len=%u\n",
           header->tag, header->len);

    return true;
}

/*
 * Handle received data from VM
 */
static void handle_recv_data(const char* data, size_t len) {
    MsgHeader header;
    uint8_t payload_buffer[MAX_PAYLOAD_SIZE];

    printf("NetworkNicDrv: Received %zu bytes from VM: %.*s\n", len, (int)len, data);

    /* Parse VM message into our protocol format */
    if (!parse_vm_message(data, len, &header, payload_buffer)) {
        printf("NetworkNicDrv: Failed to parse VM message\n");
        stats.error_count++;
        return;
    }

    /* Write message to output ring buffer (same interface as ExtNicDrv) */
    if (rb_write(output_buffer, &header, payload_buffer)) {
        stats.messages_forwarded++;
        stats.bytes_processed += sizeof(MsgHeader) + header.len;
        message_counter++;

        /* Signal ExtFrontend that new data is available (SAME as ExtNicDrv) */
        out_ntfy_emit();

        printf("NetworkNicDrv: Forwarded message #%u (tag=0x%04X, len=%u) to pipeline\n",
               message_counter, header.tag, header.len);
    } else {
        stats.error_count++;
        printf("NetworkNicDrv: Failed to write message #%u to ring buffer (full?)\n", message_counter);
    }

    /* Print stats every 50 messages */
    if (message_counter % 50 == 0) {
        printf("NetworkNicDrv: Stats - Received: %u, Forwarded: %llu, Errors: %u, Bytes: %llu\n",
               message_counter, stats.messages_forwarded, stats.error_count, stats.bytes_processed);
    }
}

/*
 * Handle notification from VM (dataport-based communication)
 */
void vm_ready_callback(void) {
    printf("NetworkNicDrv: Received notification from VM\n");

    /* Read data from VM dataport (VM writes here) */
    if (strlen((char*)vm_input) > 0) {
        printf("NetworkNicDrv: Processing VM data: %s", (char*)vm_input);

        /* Process the received data */
        handle_recv_data((char*)vm_input, strlen((char*)vm_input));

        /* Clear input buffer for next message */
        memset(vm_input, '\0', 4096);
    } else {
        printf("NetworkNicDrv: No data received from VM\n");
    }

    /* Notify VM that processing is complete */
    vm_done_emit();
}

/*
 * Component initialization
 */
void pre_init(void) {
    memset(&stats, 0, sizeof(stats));
    printf("NetworkNicDrv: Initializing network-based traffic receiver...\n");
}

int run(void) {
    printf("NetworkNicDrv: Starting up...\n");

    /* Initialize output ring buffer (same as ExtNicDrv) */
    output_buffer = rb_init(out_dp, sizeof(Buf));
    if (!output_buffer) {
        printf("NetworkNicDrv: ERROR - Failed to initialize output buffer\n");
        return -1;
    }

    if (!rb_is_valid(output_buffer)) {
        printf("NetworkNicDrv: ERROR - Output buffer validation failed\n");
        return -1;
    }

    printf("NetworkNicDrv: Output buffer initialized (size=%u bytes)\n", output_buffer->size);

    /* Initialize dataports for VM communication */
    memset(vm_input, '\0', 4096);
    memset(vm_output, '\0', 4096);
    strcpy((char*)vm_output, "NetworkNicDrv ready for messages\n");

    printf("NetworkNicDrv: Dataports initialized successfully\n");
    printf("NetworkNicDrv: Ready to receive messages from VM via notifications\n");
    printf("NetworkNicDrv: Pipeline output interface compatible with ExtFrontend\n");

    /* Wait for notifications from VM (event-driven, like vm_echo_connector) */
    while (1) {
        /* Wait for vm_ready notifications - handled by vm_ready_callback() */
        vm_ready_wait();
    }

    return 0;
}