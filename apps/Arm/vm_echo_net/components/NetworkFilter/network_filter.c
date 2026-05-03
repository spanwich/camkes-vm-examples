/*
 * NetworkFilter Component - Bridge between VM VirtQueues and NetworkEcho Dataports
 *
 * This component demonstrates the bridge pattern:
 * - Receives packets from VM via VirtQueue (zero-copy network I/O)
 * - Forwards packets to NetworkEcho via dataport (shared memory)
 * - Receives processed packets from NetworkEcho via dataport
 * - Sends packets back to VM via VirtQueue
 *
 * Architecture: VM ↔ VirtQueue ↔ NetworkFilter ↔ Dataport ↔ NetworkEcho
 */

#include <autoconf.h>
#include <camkes.h>
#include <stdio.h>
#include <string.h>
#include <virtqueue.h>
#include <camkes/virtqueue.h>
#include <utils/util.h>

#include <net/ethernet.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>

/* ========================================================================
 * CONFIGURATION
 * ======================================================================== */

#define FILTER_COMPONENT_NAME "NetworkFilter"
#define MAX_PACKET_SIZE 1514  /* Standard Ethernet MTU + header */

/* Network packet structure for dataport communication */
struct network_packet {
    uint32_t length;           /* Packet length in bytes */
    uint32_t sequence;         /* Packet sequence number */
    uint8_t  data[4088];       /* Packet data (4096 - 8 bytes header) */
};

/* Statistics tracking */
struct filter_stats {
    uint64_t packets_from_vm;
    uint64_t packets_to_vm;
    uint64_t packets_to_echo;
    uint64_t packets_from_echo;
    uint64_t bytes_from_vm;
    uint64_t bytes_to_vm;
};

static struct filter_stats stats = {0};
static uint32_t packet_sequence = 0;

/* ========================================================================
 * VIRTQUEUE MANAGEMENT
 * ======================================================================== */

virtqueue_device_t vm_recv_virtqueue;  /* Receive packets FROM VM */
virtqueue_driver_t vm_send_virtqueue;  /* Send packets TO VM */

/* ========================================================================
 * DATAPORT COMMUNICATION WITH NETWORKECHO
 * ======================================================================== */

/**
 * forward_packet_to_echo - Send packet to NetworkEcho for processing
 *
 * Copies packet to dataport and signals NetworkEcho via event.
 */
static void forward_packet_to_echo(char *packet_data, size_t packet_size)
{
    struct network_packet *pkt = (struct network_packet *)packet_to_echo;

    /* Validate packet size */
    if (packet_size > sizeof(pkt->data)) {
        printf("%s: ERROR - Packet too large for dataport (%zu > %zu)\n",
               FILTER_COMPONENT_NAME, packet_size, sizeof(pkt->data));
        return;
    }

    /* Prepare packet structure */
    pkt->length = packet_size;
    pkt->sequence = ++packet_sequence;
    memcpy(pkt->data, packet_data, packet_size);

    stats.packets_to_echo++;

    printf("%s: → Forwarding packet #%u (%u bytes) to NetworkEcho\n",
           FILTER_COMPONENT_NAME, pkt->sequence, pkt->length);

    /* Signal NetworkEcho that packet is ready */
    echo_ready_emit();
}

/**
 * echo_done_callback - Called when NetworkEcho finishes processing
 *
 * Receives processed packet from NetworkEcho and forwards to VM.
 */
void echo_done_callback(void)
{
    struct network_packet *pkt = (struct network_packet *)packet_from_echo;

    printf("%s: ← Received processed packet #%u (%u bytes) from NetworkEcho\n",
           FILTER_COMPONENT_NAME, pkt->sequence, pkt->length);

    stats.packets_from_echo++;

    /* Validate packet */
    if (pkt->length == 0 || pkt->length > sizeof(pkt->data)) {
        printf("%s: ERROR - Invalid packet length from NetworkEcho: %u\n",
               FILTER_COMPONENT_NAME, pkt->length);
        return;
    }

    /* Send processed packet back to VM */
    void *buf = NULL;
    int err = camkes_virtqueue_buffer_alloc(&vm_send_virtqueue, &buf, pkt->length);
    if (err) {
        printf("%s: ERROR - Failed to allocate send buffer for VM\n", FILTER_COMPONENT_NAME);
        return;
    }

    memcpy(buf, pkt->data, pkt->length);

    if (camkes_virtqueue_driver_send_buffer(&vm_send_virtqueue, buf, pkt->length) != 0) {
        printf("%s: ERROR - Failed to send buffer to VM\n", FILTER_COMPONENT_NAME);
        camkes_virtqueue_buffer_free(&vm_send_virtqueue, buf);
        return;
    }

    vm_send_virtqueue.notify();

    stats.packets_to_vm++;
    stats.bytes_to_vm += pkt->length;

    printf("%s: → Sent processed packet back to VM (%u bytes)\n",
           FILTER_COMPONENT_NAME, pkt->length);
}

/* ========================================================================
 * PACKET INSPECTION (Simplified)
 * ======================================================================== */

static void inspect_packet(char *packet, size_t packet_size)
{
    struct ethhdr *eth = (struct ethhdr *)packet;

    printf("%s: ETH | ", FILTER_COMPONENT_NAME);
    printf("src=%02x:%02x:%02x:%02x:%02x:%02x -> ",
           eth->h_source[0], eth->h_source[1], eth->h_source[2],
           eth->h_source[3], eth->h_source[4], eth->h_source[5]);
    printf("dst=%02x:%02x:%02x:%02x:%02x:%02x | ",
           eth->h_dest[0], eth->h_dest[1], eth->h_dest[2],
           eth->h_dest[3], eth->h_dest[4], eth->h_dest[5]);
    printf("type=0x%04x\n", ntohs(eth->h_proto));

    uint16_t eth_proto = ntohs(eth->h_proto);

    if (eth_proto == ETH_P_IP) {
        struct iphdr *ip = (struct iphdr *)(packet + sizeof(struct ethhdr));
        struct in_addr saddr = {ip->saddr};
        struct in_addr daddr = {ip->daddr};

        printf("%s: IP  | v%d proto=%d | %s -> %s\n",
               FILTER_COMPONENT_NAME, ip->version, ip->protocol,
               inet_ntoa(saddr), inet_ntoa(daddr));

        if (ip->protocol == IPPROTO_ICMP) {
            struct icmphdr *icmp = (struct icmphdr *)(
                packet + sizeof(struct ethhdr) + sizeof(struct iphdr)
            );
            printf("%s: ICMP| type=%d code=%d\n",
                   FILTER_COMPONENT_NAME, icmp->type, icmp->code);
        }
    }
}

/* ========================================================================
 * VIRTQUEUE CALLBACKS
 * ======================================================================== */

/**
 * handle_recv_from_vm_callback - VirtQueue device callback
 *
 * Called when VM sends packets. Forwards to NetworkEcho for processing.
 */
static void handle_recv_from_vm_callback(virtqueue_device_t *vq)
{
    void *buf = NULL;
    unsigned int buf_size = 0;
    vq_flags_t flag;
    virtqueue_ring_object_t handle;

    /* Get available buffer from VM */
    if (!virtqueue_get_available_buf(vq, &handle)) {
        printf("%s: ERROR - Failed to dequeue recv buffer from VM\n", FILTER_COMPONENT_NAME);
        return;
    }

    /* Gather buffer contents */
    while (camkes_virtqueue_device_gather_buffer(vq, &handle, &buf, &buf_size, &flag) >= 0) {
        stats.packets_from_vm++;
        stats.bytes_from_vm += buf_size;

        printf("%s: ══════════════════════════════════════════\n", FILTER_COMPONENT_NAME);
        printf("%s: ← Received %u bytes from VM\n", FILTER_COMPONENT_NAME, buf_size);

        /* Inspect packet */
        inspect_packet((char *)buf, buf_size);

        /* Forward to NetworkEcho for processing */
        forward_packet_to_echo((char *)buf, buf_size);
    }

    /* Return buffer to VM */
    if (!virtqueue_add_used_buf(&vm_recv_virtqueue, &handle, 0)) {
        printf("%s: ERROR - Unable to return used recv buffer to VM\n", FILTER_COMPONENT_NAME);
        return;
    }

    vm_recv_virtqueue.notify();
}

/**
 * handle_send_to_vm_callback - VirtQueue driver callback
 *
 * Called when VM acknowledges receiving packets.
 */
static void handle_send_to_vm_callback(virtqueue_driver_t *vq)
{
    void *buf = NULL;
    unsigned int buf_size = 0;
    uint32_t wr_len = 0;
    vq_flags_t flag;
    virtqueue_ring_object_t handle;

    /* Get used buffer from VM */
    if (!virtqueue_get_used_buf(vq, &handle, &wr_len)) {
        printf("%s: ERROR - Failed to dequeue send buffer\n", FILTER_COMPONENT_NAME);
        return;
    }

    /* Free buffers we allocated */
    while (camkes_virtqueue_driver_gather_buffer(vq, &handle, &buf, &buf_size, &flag) >= 0) {
        camkes_virtqueue_buffer_free(vq, buf);
    }
}

/* ========================================================================
 * EVENT LOOP
 * ======================================================================== */

/**
 * network_wait_callback - Main event loop callback
 *
 * Polls virtqueues for VM communication.
 */
void network_wait_callback(void)
{
    /* Check for packets from VM */
    if (VQ_DEV_POLL(&vm_recv_virtqueue)) {
        handle_recv_from_vm_callback(&vm_recv_virtqueue);
    }

    /* Check for completed sends to VM */
    if (VQ_DRV_POLL(&vm_send_virtqueue)) {
        handle_send_to_vm_callback(&vm_send_virtqueue);
    }
}

/* ========================================================================
 * INITIALIZATION
 * ======================================================================== */

static void print_statistics(void)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║        NetworkFilter Bridge Component - Statistics        ║\n");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║ VM Communication:                                          ║\n");
    printf("║   Packets from VM:      %10llu                         ║\n", stats.packets_from_vm);
    printf("║   Packets to VM:        %10llu                         ║\n", stats.packets_to_vm);
    printf("║   Bytes from VM:        %10llu                         ║\n", stats.bytes_from_vm);
    printf("║   Bytes to VM:          %10llu                         ║\n", stats.bytes_to_vm);
    printf("╟────────────────────────────────────────────────────────────╢\n");
    printf("║ NetworkEcho Communication:                                 ║\n");
    printf("║   Packets to Echo:      %10llu                         ║\n", stats.packets_to_echo);
    printf("║   Packets from Echo:    %10llu                         ║\n", stats.packets_from_echo);
    printf("╚════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

int run(void)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║        NetworkFilter Bridge Component                     ║\n");
    printf("║   VM ↔ VirtQueue ↔ Filter ↔ Dataport ↔ NetworkEcho       ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    printf("%s: Initializing VirtQueues for VM communication...\n", FILTER_COMPONENT_NAME);

    /* Initialize receive virtqueue (from VM) */
    int err = camkes_virtqueue_device_init(&vm_recv_virtqueue, 0);
    if (err) {
        printf("%s: FATAL - Failed to initialize recv virtqueue (err=%d)\n",
               FILTER_COMPONENT_NAME, err);
        return 1;
    }
    printf("%s: ✓ VM recv virtqueue initialized (ID=0)\n", FILTER_COMPONENT_NAME);

    /* Initialize send virtqueue (to VM) */
    err = camkes_virtqueue_driver_init(&vm_send_virtqueue, 1);
    if (err) {
        printf("%s: FATAL - Failed to initialize send virtqueue (err=%d)\n",
               FILTER_COMPONENT_NAME, err);
        return 1;
    }
    printf("%s: ✓ VM send virtqueue initialized (ID=1)\n", FILTER_COMPONENT_NAME);

    /* Initialize dataport communication */
    printf("\n%s: Initializing dataports for NetworkEcho communication...\n",
           FILTER_COMPONENT_NAME);

    /* Clear dataport buffers */
    memset(packet_to_echo, 0, 4096);
    memset(packet_from_echo, 0, 4096);

    printf("%s: ✓ Dataport packet_to_echo initialized (4096 bytes)\n", FILTER_COMPONENT_NAME);
    printf("%s: ✓ Dataport packet_from_echo initialized (4096 bytes)\n", FILTER_COMPONENT_NAME);

    printf("\n");
    printf("%s: ══════════════════════════════════════════════════════════\n", FILTER_COMPONENT_NAME);
    printf("%s: NetworkFilter Bridge is READY\n", FILTER_COMPONENT_NAME);
    printf("%s: - Receiving packets from VM via VirtQueue\n", FILTER_COMPONENT_NAME);
    printf("%s: - Forwarding to NetworkEcho via dataport\n", FILTER_COMPONENT_NAME);
    printf("%s: - Sending processed packets back to VM\n", FILTER_COMPONENT_NAME);
    printf("%s: ══════════════════════════════════════════════════════════\n", FILTER_COMPONENT_NAME);
    printf("\n");

    print_statistics();

    return 0;
}
