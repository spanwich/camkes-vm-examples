/*
 * NetworkEcho Component - Network Packet Echo Service
 *
 * This component demonstrates receiving network packets from NetworkFilter
 * via dataports, processing them, and echoing them back.
 *
 * Architecture:
 *   NetworkFilter → (dataport) → NetworkEcho → (dataport) → NetworkFilter → VM
 *
 * This proves the bridge component pattern where:
 * - NetworkFilter handles VirtQueue communication with VM
 * - NetworkEcho processes packets (inspection, filtering, modification)
 * - Communication via shared memory dataports + events
 */

#include <camkes.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Network packet buffer structure */
struct network_packet {
    uint32_t length;           /* Packet length in bytes */
    uint32_t sequence;         /* Packet sequence number */
    uint8_t  data[4088];       /* Packet data (4096 - 8 bytes header) */
};

static uint32_t packets_processed = 0;
static uint32_t packets_echoed = 0;

/**
 * process_packet - Process received network packet
 *
 * This function demonstrates packet processing logic.
 * In a real system, this could implement:
 * - Deep packet inspection
 * - Protocol analysis
 * - Security filtering
 * - Intrusion detection
 * - Packet modification (NAT, header rewriting, etc.)
 */
static void process_packet(struct network_packet *pkt)
{
    packets_processed++;

    printf("NetworkEcho: ═══════════════════════════════════════════\n");
    printf("NetworkEcho: Packet #%u (seq=%u, len=%u bytes)\n",
           packets_processed, pkt->sequence, pkt->length);

    /* Example: Display first 64 bytes as hex dump */
    if (pkt->length > 0) {
        printf("NetworkEcho: Hex dump (first %u bytes):\n",
               pkt->length < 64 ? pkt->length : 64);

        for (uint32_t i = 0; i < pkt->length && i < 64; i++) {
            if (i % 16 == 0) {
                printf("  %04x: ", i);
            }
            printf("%02x ", pkt->data[i]);
            if ((i + 1) % 16 == 0) {
                printf("\n");
            }
        }
        if (pkt->length % 16 != 0) {
            printf("\n");
        }
    }

    /* Example processing: Add a marker to the packet */
    if (pkt->length < sizeof(pkt->data) - 16) {
        const char *marker = "[PROCESSED]";
        memcpy(&pkt->data[pkt->length], marker, strlen(marker));
        pkt->length += strlen(marker);
        printf("NetworkEcho: Added processing marker\n");
    }

    printf("NetworkEcho: Packet processed successfully\n");
}

/**
 * echo_packet - Echo packet back to sender
 *
 * Copies processed packet to output dataport and signals completion.
 */
static void echo_packet(struct network_packet *pkt)
{
    /* Copy packet to output dataport */
    struct network_packet *out = (struct network_packet *)packet_out;
    memcpy(out, pkt, sizeof(struct network_packet));

    packets_echoed++;

    printf("NetworkEcho: Echoed packet #%u back to NetworkFilter\n",
           packets_echoed);

    /* Notify NetworkFilter that packet is ready */
    done_emit();
}

/**
 * ready_callback - Called when NetworkFilter sends a packet
 *
 * This is the main event handler that receives packets from NetworkFilter.
 */
void ready_callback(void)
{
    printf("NetworkEcho: ┌─────────────────────────────────────────┐\n");
    printf("NetworkEcho: │ Packet received from NetworkFilter      │\n");
    printf("NetworkEcho: └─────────────────────────────────────────┘\n");

    /* Read packet from input dataport */
    struct network_packet *pkt = (struct network_packet *)packet_in;

    /* Validate packet */
    if (pkt->length == 0 || pkt->length > sizeof(pkt->data)) {
        printf("NetworkEcho: WARNING - Invalid packet length: %u\n", pkt->length);
        return;
    }

    /* Process the packet */
    process_packet(pkt);

    /* Echo it back */
    echo_packet(pkt);

    printf("NetworkEcho: ═══════════════════════════════════════════\n\n");
}

/**
 * run - Component entry point
 */
int run(void)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║       NetworkEcho Component - Packet Processor       ║\n");
    printf("║   Dataport-based Network Packet Echo Service         ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("\n");

    printf("NetworkEcho: Initializing packet echo service...\n");
    printf("NetworkEcho: Dataport sizes:\n");
    printf("NetworkEcho:   packet_in:  %zu bytes\n", sizeof(struct network_packet));
    printf("NetworkEcho:   packet_out: %zu bytes\n", sizeof(struct network_packet));
    printf("\n");

    /* Initialize output buffer */
    memset(packet_out, 0, 4096);

    printf("NetworkEcho: ══════════════════════════════════════════\n");
    printf("NetworkEcho: Service READY\n");
    printf("NetworkEcho: Waiting for packets from NetworkFilter...\n");
    printf("NetworkEcho: ══════════════════════════════════════════\n");
    printf("\n");

    /* Main event loop handled by CAmkES */
    return 0;
}
