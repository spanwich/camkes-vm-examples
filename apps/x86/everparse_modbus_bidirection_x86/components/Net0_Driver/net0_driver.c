/*
 * Net0_Driver - External Network (Bidirectional) - x86 Port
 *
 * This component manages the external network interface with bidirectional
 * data flow and protocol break architecture.
 *
 * Architecture:
 * - Intel e1000 device driver for packet RX/TX (via ethdrivers library)
 * - lwIP TCP/IP stack for network protocol handling
 * - DHCP client to obtain IP address from QEMU
 * - TCP server on port 502 for INBOUND connections (Modbus)
 * - TCP client for OUTBOUND connections
 * - Frame header parsing to extract FrameMetadata
 *
 * Data Flow:
 *   INBOUND:  External TCP:502 => lwIP => extract metadata+payload => ICS_Inbound
 *   OUTBOUND: ICS_Outbound => create TCP packet => lwIP => External
 *
 * x86 Port - Uses Intel 82574/e1000 ethernet driver instead of ARM VirtIO
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* Debug configuration */
#define DEBUG_LEVEL DEBUG_LEVEL_INFO
#include "debug_levels.h"

#include <camkes.h>
#include <camkes/dma.h>
#include <camkes/io.h>
#include <camkes/irq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include <utils/util.h>
#include <platsupport/io.h>
#include <platsupport/irq.h>
#include <ethdrivers/raw.h>
#include <ethdrivers/intel.h>

/* lwIP headers */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/stats.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"
#include "lwip/prot/ip.h"
#include "netif/ethernet.h"

/* ICS common definitions */
#include "common.h"
#include "version.h"

/* Connection state sharing */
#include "connection_state.h"

#define COMPONENT_NAME "Net0_Driver_x86"
#define TCP_SERVER_PORT 502  /* Modbus TCP port */

#define MAX_CONNECTIONS 150

/* x86 memory barriers */
#define DMB() __asm__ volatile("mfence" ::: "memory")
#define DSB() __asm__ volatile("mfence" ::: "memory")
#define ISB() __asm__ volatile("" ::: "memory")

/* Connection metadata (same structure as ARM) */
struct connection_metadata {
    struct tcp_pcb *pcb;
    uint32_t session_id;
    uint32_t original_src_ip;
    uint32_t original_dest_ip;
    uint16_t src_port;
    uint16_t dest_port;
    bool active;
    uint32_t tcp_seq_num;
    uint32_t timestamp;
    bool awaiting_response;
    bool response_received;
    bool close_pending;
    bool closing;
    uint8_t *pending_outbound_data;
    uint16_t pending_outbound_len;
    bool has_pending_outbound;
    bool cleanup_in_progress;
    bool close_notified;
    bool metadata_close_pending;
    uint32_t close_timestamp;
    uint32_t last_tx_timestamp;
};

static struct connection_metadata connection_table[MAX_CONNECTIONS];
static int connection_count = 0;
static uint32_t active_connections = 0;
static uint32_t total_connections_created = 0;
static uint32_t total_connections_closed = 0;
static uint32_t next_session_id = 1;

/* Connection state sharing */
static volatile struct connection_state_table *own_state = NULL;
static volatile struct connection_state_table *peer_state = NULL;

/* Cleanup queue (same as ARM) */
#define CLEANUP_QUEUE_SIZE 512
#define CLEANUP_QUEUE_MASK (CLEANUP_QUEUE_SIZE - 1)

struct cleanup_request {
    uint32_t session_id;
    uint32_t timestamp;
};

struct cleanup_queue {
    volatile uint32_t head;
    volatile uint32_t tail;
    struct cleanup_request requests[CLEANUP_QUEUE_SIZE];
};

static struct cleanup_queue cleanup_queue = {
    .head = 0,
    .tail = 0
};

/* Network configuration */
#define OUTBOUND_FORWARD_IP "192.168.95.2"
#define OUTBOUND_FORWARD_PORT 502

/* Packet buffer configuration */
#define PACKET_BUFFER_SIZE 2048
#define RX_BUFS 256
#define TX_BUFS 128
#define BUF_SIZE 2048

/* Global state */
static struct eth_driver *eth_driver = NULL;
static struct netif netif_data;
static bool tcp_server_initialized = false;
static volatile bool initialization_successful = false;

/* Packet buffers */
typedef struct eth_buf {
    void *buf;
    uintptr_t phys;
} eth_buf_t;

static int num_rx_bufs = 0;
static eth_buf_t rx_bufs[RX_BUFS];
static eth_buf_t *rx_buf_pool[RX_BUFS];

/* Statistics */
static uint32_t packets_received = 0;
static uint32_t packets_sent = 0;
static uint32_t dhcp_bound = 0;

/* lwIP time tracking */
static volatile uint32_t lwip_time_ms = 0;

uint32_t sys_now(void)
{
    lwip_time_ms++;
    return lwip_time_ms;
}

/* Forward declarations */
static err_t netif_output(struct netif *netif, struct pbuf *p);
static err_t custom_netif_init(struct netif *netif);
static void netif_status_callback(struct netif *netif);
static void setup_tcp_echo_server(void);
static void process_rx_packets(void);

/* Ethernet header structure */
struct ethhdr {
    uint8_t h_dest[6];
    uint8_t h_source[6];
    uint16_t h_proto;
} __attribute__((packed));

/* IP header structure */
struct iphdr {
    uint8_t ihl:4;
    uint8_t version:4;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
} __attribute__((packed));

/* TCP header structure */
struct tcphdr {
    uint16_t source;
    uint16_t dest;
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t res1:4;
    uint16_t doff:4;
    uint16_t fin:1;
    uint16_t syn:1;
    uint16_t rst:1;
    uint16_t psh:1;
    uint16_t ack:1;
    uint16_t urg:1;
    uint16_t res2:2;
    uint16_t window;
    uint16_t check;
    uint16_t urg_ptr;
} __attribute__((packed));

/*
 * ethdrivers callbacks
 */
static void eth_tx_complete(void *iface, void *cookie)
{
    /* TX buffer released back to pool */
    packets_sent++;
}

static uintptr_t eth_allocate_rx_buf(void *iface, size_t buf_size, void **cookie)
{
    if (buf_size > BUF_SIZE || num_rx_bufs == 0) {
        return 0;
    }
    num_rx_bufs--;
    *cookie = rx_buf_pool[num_rx_bufs];
    return rx_buf_pool[num_rx_bufs]->phys;
}

static void eth_rx_complete(void *iface, unsigned int num_bufs, void **cookies, unsigned int *lens)
{
    if (num_bufs != 1) {
        /* Return buffers to pool */
        for (int i = 0; i < num_bufs; i++) {
            eth_buf_t *buf = cookies[i];
            rx_buf_pool[num_rx_bufs++] = buf;
        }
        return;
    }

    eth_buf_t *buf = cookies[0];
    unsigned int len = lens[0];

    if (len < sizeof(struct ethhdr)) {
        rx_buf_pool[num_rx_bufs++] = buf;
        return;
    }

    packets_received++;

    /* Create pbuf and pass to lwIP */
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p != NULL) {
        memcpy(p->payload, buf->buf, len);

        /* Feed to lwIP ethernet input */
        if (netif_data.input(p, &netif_data) != ERR_OK) {
            pbuf_free(p);
        }
    }

    /* Return buffer to pool */
    rx_buf_pool[num_rx_bufs++] = buf;
}

static struct raw_iface_callbacks ethdriver_callbacks = {
    .tx_complete = eth_tx_complete,
    .rx_complete = eth_rx_complete,
    .allocate_rx_buf = eth_allocate_rx_buf
};

/*
 * Connection tracking helpers
 */
static struct connection_metadata *find_connection_by_pcb(struct tcp_pcb *pcb)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active && connection_table[i].pcb == pcb) {
            return &connection_table[i];
        }
    }
    return NULL;
}

static struct connection_metadata *find_free_connection_slot(void)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_table[i].active) {
            return &connection_table[i];
        }
    }
    return NULL;
}

static struct connection_metadata *create_connection(struct tcp_pcb *pcb,
                                                      uint32_t src_ip, uint16_t src_port,
                                                      uint32_t dest_ip, uint16_t dest_port)
{
    struct connection_metadata *meta = find_free_connection_slot();
    if (meta == NULL) {
        DEBUG_WARN("%s: Connection table full!\n", COMPONENT_NAME);
        return NULL;
    }

    memset(meta, 0, sizeof(*meta));
    meta->pcb = pcb;
    meta->session_id = next_session_id++;
    meta->original_src_ip = src_ip;
    meta->original_dest_ip = dest_ip;
    meta->src_port = src_port;
    meta->dest_port = dest_port;
    meta->active = true;
    meta->timestamp = sys_now();

    connection_count++;
    active_connections++;
    total_connections_created++;

    DEBUG_INFO("%s: Created connection session=%u src=%u.%u.%u.%u:%u\n",
               COMPONENT_NAME, meta->session_id,
               (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
               (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port);

    return meta;
}

static void cleanup_connection(struct connection_metadata *meta)
{
    if (meta == NULL || !meta->active) return;

    DEBUG_INFO("%s: Cleanup connection session=%u\n",
               COMPONENT_NAME, meta->session_id);

    if (meta->pending_outbound_data) {
        free(meta->pending_outbound_data);
    }

    meta->active = false;
    meta->pcb = NULL;
    connection_count--;
    active_connections--;
    total_connections_closed++;
}

/*
 * TCP server callbacks
 */
static err_t tcp_echo_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct connection_metadata *meta = (struct connection_metadata *)arg;

    if (p == NULL) {
        /* Connection closed by remote */
        DEBUG_INFO("%s: Remote closed connection session=%u\n",
                   COMPONENT_NAME, meta ? meta->session_id : 0);
        if (meta) {
            meta->close_pending = true;
        }
        return ERR_OK;
    }

    if (err != ERR_OK || meta == NULL || !meta->active) {
        pbuf_free(p);
        return ERR_OK;
    }

    /* Extract payload and forward to ICS_Inbound */
    InboundDataport *dp = (InboundDataport *)inbound_dp;
    ICS_Message *msg = &dp->request_msg;

    /* Build metadata */
    msg->metadata.session_id = meta->session_id;
    msg->metadata.src_ip = meta->original_src_ip;
    msg->metadata.dst_ip = meta->original_dest_ip;
    msg->metadata.src_port = meta->src_port;
    msg->metadata.dst_port = meta->dest_port;
    msg->metadata.is_ip = 1;
    msg->metadata.is_tcp = 1;
    msg->metadata.ip_protocol = 6;
    msg->metadata.payload_length = p->tot_len;

    /* Copy payload */
    if (p->tot_len <= MAX_PAYLOAD_SIZE) {
        pbuf_copy_partial(p, msg->payload, p->tot_len, 0);
        msg->payload_length = p->tot_len;

        /* Signal ICS_Inbound */
        __sync_synchronize();
        inbound_ready_emit();

        DEBUG_INFO("%s: [INBOUND] session=%u, %u bytes -> ICS_Inbound\n",
                   COMPONENT_NAME, meta->session_id, p->tot_len);
    } else {
        DEBUG_WARN("%s: Payload too large: %u bytes\n", COMPONENT_NAME, p->tot_len);
    }

    /* Acknowledge data */
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

static void tcp_echo_err(void *arg, err_t err)
{
    struct connection_metadata *meta = (struct connection_metadata *)arg;
    if (meta && meta->active) {
        DEBUG_INFO("%s: TCP error on session=%u, err=%d\n",
                   COMPONENT_NAME, meta->session_id, err);
        meta->pcb = NULL;  /* PCB is already freed by lwIP */
        cleanup_connection(meta);
    }
}

static err_t tcp_echo_poll(void *arg, struct tcp_pcb *tpcb)
{
    struct connection_metadata *meta = (struct connection_metadata *)arg;

    if (meta && meta->close_pending && !meta->awaiting_response) {
        tcp_close(tpcb);
        cleanup_connection(meta);
    }

    return ERR_OK;
}

static err_t tcp_echo_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    /* Get original IPs from TCP PCB */
    uint32_t src_ip = ntohl(newpcb->remote_ip.addr);
    uint32_t dest_ip = ntohl(newpcb->local_ip.addr);
    uint16_t src_port = newpcb->remote_port;
    uint16_t dest_port = newpcb->local_port;

    /* Create connection metadata */
    struct connection_metadata *meta = create_connection(newpcb,
                                                          src_ip, src_port,
                                                          dest_ip, dest_port);
    if (meta == NULL) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    /* Setup callbacks */
    tcp_arg(newpcb, meta);
    tcp_recv(newpcb, tcp_echo_recv);
    tcp_err(newpcb, tcp_echo_err);
    tcp_poll(newpcb, tcp_echo_poll, 4);

    DEBUG_INFO("%s: Accepted connection session=%u from %u.%u.%u.%u:%u\n",
               COMPONENT_NAME, meta->session_id,
               (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
               (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port);

    return ERR_OK;
}

static void setup_tcp_echo_server(void)
{
    struct tcp_pcb *pcb = tcp_new();
    if (pcb == NULL) {
        DEBUG_ERROR("%s: Failed to create TCP PCB\n", COMPONENT_NAME);
        return;
    }

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (err != ERR_OK) {
        DEBUG_ERROR("%s: Failed to bind TCP port %d: %d\n",
                    COMPONENT_NAME, TCP_SERVER_PORT, err);
        tcp_close(pcb);
        return;
    }

    pcb = tcp_listen(pcb);
    if (pcb == NULL) {
        DEBUG_ERROR("%s: Failed to listen on port %d\n",
                    COMPONENT_NAME, TCP_SERVER_PORT);
        return;
    }

    tcp_accept(pcb, tcp_echo_accept);
    tcp_server_initialized = true;

    DEBUG_INFO("%s: TCP server listening on port %d\n",
               COMPONENT_NAME, TCP_SERVER_PORT);
}

/*
 * Network interface output function (lwIP -> hardware)
 */
static err_t netif_output(struct netif *netif, struct pbuf *p)
{
    if (eth_driver == NULL) {
        return ERR_IF;
    }

    /* Allocate TX buffer */
    void *buf = NULL;
    uintptr_t phys = 0;

    /* Copy pbuf to contiguous buffer and transmit */
    buf = malloc(p->tot_len);
    if (buf == NULL) {
        return ERR_MEM;
    }

    pbuf_copy_partial(p, buf, p->tot_len, 0);

    /* Get physical address - for simulation this is the virtual address */
    phys = (uintptr_t)buf;

    /* Transmit via ethdrivers */
    int err = eth_driver->i_fn.raw_tx(eth_driver, 1, &phys,
                                       (unsigned int *)&p->tot_len, buf);

    if (err != ETHIF_TX_ENQUEUED && err != ETHIF_TX_COMPLETE) {
        free(buf);
        return ERR_IF;
    }

    if (err == ETHIF_TX_COMPLETE) {
        free(buf);
    }

    packets_sent++;
    return ERR_OK;
}

/*
 * Network interface init
 */
static err_t custom_netif_init(struct netif *netif)
{
    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->linkoutput = netif_output;
    netif->output = etharp_output;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

    /* Get MAC address from driver */
    if (eth_driver != NULL) {
        eth_driver->i_fn.get_mac(eth_driver, netif->hwaddr);
    } else {
        /* Default MAC for simulation */
        netif->hwaddr[0] = 0x52;
        netif->hwaddr[1] = 0x54;
        netif->hwaddr[2] = 0x00;
        netif->hwaddr[3] = 0x12;
        netif->hwaddr[4] = 0x34;
        netif->hwaddr[5] = 0x56;
    }
    netif->hwaddr_len = 6;

    DEBUG_INFO("%s: Interface MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               COMPONENT_NAME,
               netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
               netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);

    return ERR_OK;
}

static void netif_status_callback(struct netif *netif)
{
    if (netif_is_up(netif)) {
        DEBUG_INFO("%s: Interface up, IP: %s\n",
                   COMPONENT_NAME, ip4addr_ntoa(netif_ip4_addr(netif)));

        if (!tcp_server_initialized && !ip4_addr_isany(netif_ip4_addr(netif))) {
            setup_tcp_echo_server();
            dhcp_bound = 1;
        }
    }
}

/*
 * Handle outbound data from ICS_Outbound
 */
static void handle_outbound_notification(void)
{
    OutboundDataport *dp = (OutboundDataport *)outbound_dp;
    ICS_Message *msg = &dp->response_msg;

    /* Sentinel check - payload_length=0 means control-only */
    if (msg->payload_length == 0) {
        return;
    }

    /* Find connection by session ID */
    struct connection_metadata *meta = NULL;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_table[i].active &&
            connection_table[i].session_id == msg->metadata.session_id) {
            meta = &connection_table[i];
            break;
        }
    }

    if (meta == NULL || meta->pcb == NULL) {
        DEBUG_WARN("%s: No connection for outbound session=%u\n",
                   COMPONENT_NAME, msg->metadata.session_id);
        return;
    }

    /* Send response via TCP */
    err_t err = tcp_write(meta->pcb, msg->payload, msg->payload_length, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
        tcp_output(meta->pcb);
        DEBUG_INFO("%s: [OUTBOUND] session=%u, %u bytes sent\n",
                   COMPONENT_NAME, msg->metadata.session_id, msg->payload_length);
    } else {
        DEBUG_WARN("%s: TCP write failed for session=%u: %d\n",
                   COMPONENT_NAME, msg->metadata.session_id, err);
    }
}

/*
 * IRQ handler
 */
void eth_irq_handle(void)
{
    if (eth_driver != NULL) {
        eth_driver->i_fn.raw_poll(eth_driver);
    }

    /* Process lwIP timeouts */
    sys_check_timeouts();
}

/*
 * Outbound notification handler
 */
void outbound_ready_handle(void)
{
    handle_outbound_notification();
}

/*
 * Component initialization
 */
void pre_init(void)
{
    DEBUG_INFO("%s: Initializing x86 ethernet driver...\n", COMPONENT_NAME);

    /* Initialize connection table */
    memset(connection_table, 0, sizeof(connection_table));

    /* Initialize connection state sharing */
    own_state = (volatile struct connection_state_table *)net0_conn_state;
    peer_state = (volatile struct connection_state_table *)net1_conn_state;
    if (own_state) {
        memset((void *)own_state, 0, sizeof(*own_state));
    }
}

static int init_ethernet(ps_io_ops_t *io_ops)
{
    int error;

    /* Allocate ethernet driver structure */
    error = ps_calloc(&io_ops->malloc_ops, 1, sizeof(*eth_driver), (void **)&eth_driver);
    if (error) {
        DEBUG_ERROR("%s: Failed to allocate eth_driver\n", COMPONENT_NAME);
        return error;
    }

    /* Configure Intel 82574 driver */
    ethif_intel_config_t *eth_config = calloc(1, sizeof(ethif_intel_config_t) + sizeof(ps_irq_t));
    if (eth_config == NULL) {
        DEBUG_ERROR("%s: Failed to allocate eth_config\n", COMPONENT_NAME);
        return -1;
    }

    *eth_config = (ethif_intel_config_t) {
        .bar0 = (void *)eth_mmio,
        .prom_mode = 1,  /* Promiscuous mode for ICS gateway */
        .num_irqs = 1
    };

    eth_config->irq_info[0] = (ps_irq_t) {
        .type = PS_IOAPIC,
        .ioapic = { .ioapic = 0, .pin = 11, .level = 1, .polarity = 1, .vector = 11 }
    };

    /* Initialize Intel e1000 driver */
    error = ethif_e82574_init(eth_driver, *io_ops, eth_config);
    if (error) {
        DEBUG_ERROR("%s: Failed to init Intel e1000: %d\n", COMPONENT_NAME, error);
        return error;
    }

    /* Setup callbacks */
    eth_driver->cb_cookie = NULL;
    eth_driver->i_cb = ethdriver_callbacks;

    DEBUG_INFO("%s: Intel e1000 driver initialized\n", COMPONENT_NAME);
    return 0;
}

static int init_rx_buffers(ps_io_ops_t *io_ops)
{
    /* Preallocate RX buffers */
    for (int i = 0; i < RX_BUFS; i++) {
        void *buf = ps_dma_alloc(&io_ops->dma_manager, BUF_SIZE, 4, 1, PS_MEM_NORMAL);
        if (!buf) {
            buf = ps_dma_alloc(&io_ops->dma_manager, BUF_SIZE, 4, 0, PS_MEM_NORMAL);
        }
        if (!buf) {
            DEBUG_ERROR("%s: Failed to allocate RX buffer %d\n", COMPONENT_NAME, i);
            return -1;
        }
        memset(buf, 0, BUF_SIZE);
        uintptr_t phys = ps_dma_pin(&io_ops->dma_manager, buf, BUF_SIZE);

        rx_bufs[num_rx_bufs] = (eth_buf_t) { .buf = buf, .phys = phys };
        rx_buf_pool[num_rx_bufs] = &rx_bufs[num_rx_bufs];
        num_rx_bufs++;
    }

    DEBUG_INFO("%s: Allocated %d RX buffers\n", COMPONENT_NAME, num_rx_bufs);
    return 0;
}

static int post_init_setup(ps_io_ops_t *io_ops)
{
    int error;

    /* Initialize ethernet driver */
    error = init_ethernet(io_ops);
    if (error) {
        return error;
    }

    /* Initialize RX buffers */
    error = init_rx_buffers(io_ops);
    if (error) {
        return error;
    }

    /* Initialize lwIP */
    lwip_init();

    /* Setup network interface */
    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    if (netif_add(&netif_data, &ipaddr, &netmask, &gw, NULL,
                  custom_netif_init, ethernet_input) == NULL) {
        DEBUG_ERROR("%s: Failed to add network interface\n", COMPONENT_NAME);
        return -1;
    }

    netif_set_default(&netif_data);
    netif_set_status_callback(&netif_data, netif_status_callback);
    netif_set_up(&netif_data);

    /* Start DHCP */
    dhcp_start(&netif_data);

    initialization_successful = true;
    DEBUG_INFO("%s: Initialization complete, waiting for DHCP...\n", COMPONENT_NAME);

    return 0;
}

CAMKES_POST_INIT_MODULE_DEFINE(net0_driver_init, post_init_setup);

int run(void)
{
    DEBUG_INFO("%s: Main loop starting\n", COMPONENT_NAME);

    while (1) {
        /* Wait for interrupt */
        eth_irq_wait();

        /* Handle ethernet interrupt */
        if (eth_driver != NULL) {
            eth_driver->i_fn.raw_poll(eth_driver);
        }

        /* Process lwIP timeouts */
        sys_check_timeouts();

        /* Acknowledge interrupt */
        eth_irq_acknowledge();
    }

    return 0;
}
