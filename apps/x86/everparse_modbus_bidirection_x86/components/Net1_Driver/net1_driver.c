/*
 * Net1_Driver - Internal Network (Bidirectional) - x86 Port
 *
 * This component manages the internal network interface with bidirectional
 * data flow and protocol break architecture.
 *
 * Architecture:
 * - Intel e1000 device driver for packet RX/TX (via ethdrivers library)
 * - lwIP TCP/IP stack for network protocol handling
 * - DHCP client to obtain IP address from QEMU
 * - TCP client to PLC port 502 for forwarding validated requests
 * - Receives PLC responses and forwards to ICS_Outbound
 *
 * Data Flow:
 *   INBOUND:  ICS_Inbound => TCP client to PLC:502 => PLC
 *   OUTBOUND: PLC response => lwIP => ICS_Outbound
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

#define COMPONENT_NAME "Net1_Driver_x86"
#define PLC_PORT 502  /* Modbus TCP port on PLC */

#define MAX_CONNECTIONS 150

/* x86 memory barriers */
#define DMB() __asm__ volatile("mfence" ::: "memory")
#define DSB() __asm__ volatile("mfence" ::: "memory")
#define ISB() __asm__ volatile("" ::: "memory")

/* PLC connection tracking */
struct plc_connection {
    struct tcp_pcb *pcb;
    uint32_t session_id;        /* Linked to Net0's session ID */
    uint32_t plc_ip;            /* PLC IP address */
    uint16_t plc_port;          /* PLC port (usually 502) */
    bool active;
    bool connected;             /* TCP connection established */
    bool connecting;            /* Connection in progress */
    uint8_t *pending_data;      /* Data waiting for connection */
    uint16_t pending_len;
    uint32_t timestamp;
};

static struct plc_connection plc_connections[MAX_CONNECTIONS];
static int connection_count = 0;
static uint32_t active_connections = 0;

/* Connection state sharing */
static volatile struct connection_state_table *own_state = NULL;
static volatile struct connection_state_table *peer_state = NULL;

/* Default PLC IP (configurable) */
static ip4_addr_t plc_ip_addr;
#define DEFAULT_PLC_IP "192.168.95.100"

/* Packet buffer configuration */
#define RX_BUFS 256
#define TX_BUFS 128
#define BUF_SIZE 2048

/* Global state */
static struct eth_driver *eth_driver = NULL;
static struct netif netif_data;
static volatile bool initialization_successful = false;
static bool dhcp_bound = false;

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

/*
 * ethdrivers callbacks
 */
static void eth_tx_complete(void *iface, void *cookie)
{
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
        for (int i = 0; i < num_bufs; i++) {
            eth_buf_t *buf = cookies[i];
            rx_buf_pool[num_rx_bufs++] = buf;
        }
        return;
    }

    eth_buf_t *buf = cookies[0];
    unsigned int len = lens[0];

    if (len < 14) {  /* Minimum ethernet header */
        rx_buf_pool[num_rx_bufs++] = buf;
        return;
    }

    packets_received++;

    /* Create pbuf and pass to lwIP */
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p != NULL) {
        memcpy(p->payload, buf->buf, len);
        if (netif_data.input(p, &netif_data) != ERR_OK) {
            pbuf_free(p);
        }
    }

    rx_buf_pool[num_rx_bufs++] = buf;
}

static struct raw_iface_callbacks ethdriver_callbacks = {
    .tx_complete = eth_tx_complete,
    .rx_complete = eth_rx_complete,
    .allocate_rx_buf = eth_allocate_rx_buf
};

/*
 * PLC connection helpers
 */
static struct plc_connection *find_plc_connection_by_session(uint32_t session_id)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (plc_connections[i].active && plc_connections[i].session_id == session_id) {
            return &plc_connections[i];
        }
    }
    return NULL;
}

static struct plc_connection *find_plc_connection_by_pcb(struct tcp_pcb *pcb)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (plc_connections[i].active && plc_connections[i].pcb == pcb) {
            return &plc_connections[i];
        }
    }
    return NULL;
}

static struct plc_connection *create_plc_connection(uint32_t session_id)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!plc_connections[i].active) {
            struct plc_connection *conn = &plc_connections[i];
            memset(conn, 0, sizeof(*conn));
            conn->session_id = session_id;
            conn->active = true;
            conn->timestamp = sys_now();
            connection_count++;
            active_connections++;
            return conn;
        }
    }
    return NULL;
}

static void cleanup_plc_connection(struct plc_connection *conn)
{
    if (conn == NULL || !conn->active) return;

    DEBUG_INFO("%s: Cleanup PLC connection session=%u\n",
               COMPONENT_NAME, conn->session_id);

    if (conn->pending_data) {
        free(conn->pending_data);
    }

    conn->active = false;
    conn->pcb = NULL;
    connection_count--;
    active_connections--;
}

/*
 * TCP client callbacks for PLC connection
 */
static err_t plc_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct plc_connection *conn = (struct plc_connection *)arg;

    if (p == NULL) {
        /* PLC closed connection */
        DEBUG_INFO("%s: PLC closed connection session=%u\n",
                   COMPONENT_NAME, conn ? conn->session_id : 0);
        if (conn) {
            cleanup_plc_connection(conn);
        }
        return ERR_OK;
    }

    if (err != ERR_OK || conn == NULL || !conn->active) {
        pbuf_free(p);
        return ERR_OK;
    }

    /* Forward PLC response to ICS_Outbound */
    OutboundDataport *dp = (OutboundDataport *)outbound_dp;
    ICS_Message *msg = &dp->response_msg;

    /* Build metadata */
    msg->metadata.session_id = conn->session_id;
    msg->metadata.src_ip = conn->plc_ip;
    msg->metadata.dst_ip = 0;  /* Will be filled by Net0 */
    msg->metadata.src_port = conn->plc_port;
    msg->metadata.dst_port = PLC_PORT;
    msg->metadata.is_ip = 1;
    msg->metadata.is_tcp = 1;
    msg->metadata.ip_protocol = 6;
    msg->metadata.payload_length = p->tot_len;

    /* Copy payload */
    if (p->tot_len <= MAX_PAYLOAD_SIZE) {
        pbuf_copy_partial(p, msg->payload, p->tot_len, 0);
        msg->payload_length = p->tot_len;

        /* Signal ICS_Outbound */
        __sync_synchronize();
        outbound_ready_emit();

        DEBUG_INFO("%s: [OUTBOUND] session=%u, %u bytes from PLC -> ICS_Outbound\n",
                   COMPONENT_NAME, conn->session_id, p->tot_len);
    } else {
        DEBUG_WARN("%s: PLC response too large: %u bytes\n", COMPONENT_NAME, p->tot_len);
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

static void plc_tcp_err(void *arg, err_t err)
{
    struct plc_connection *conn = (struct plc_connection *)arg;
    if (conn && conn->active) {
        DEBUG_INFO("%s: TCP error on session=%u, err=%d\n",
                   COMPONENT_NAME, conn->session_id, err);
        conn->pcb = NULL;
        cleanup_plc_connection(conn);
    }
}

static err_t plc_tcp_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    struct plc_connection *conn = (struct plc_connection *)arg;

    if (err != ERR_OK || conn == NULL) {
        return ERR_VAL;
    }

    conn->connected = true;
    conn->connecting = false;

    DEBUG_INFO("%s: Connected to PLC session=%u\n",
               COMPONENT_NAME, conn->session_id);

    /* Send any pending data */
    if (conn->pending_data && conn->pending_len > 0) {
        err_t werr = tcp_write(tpcb, conn->pending_data, conn->pending_len, TCP_WRITE_FLAG_COPY);
        if (werr == ERR_OK) {
            tcp_output(tpcb);
            DEBUG_INFO("%s: Sent pending data session=%u, %u bytes\n",
                       COMPONENT_NAME, conn->session_id, conn->pending_len);
        }
        free(conn->pending_data);
        conn->pending_data = NULL;
        conn->pending_len = 0;
    }

    return ERR_OK;
}

static err_t plc_tcp_poll(void *arg, struct tcp_pcb *tpcb)
{
    /* Periodic poll - can add timeout handling here */
    return ERR_OK;
}

/*
 * Connect to PLC and send request
 */
static int connect_to_plc(uint32_t session_id, const uint8_t *data, uint16_t len)
{
    struct plc_connection *conn = find_plc_connection_by_session(session_id);

    if (conn != NULL && conn->connected && conn->pcb != NULL) {
        /* Reuse existing connection */
        err_t err = tcp_write(conn->pcb, data, len, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK) {
            tcp_output(conn->pcb);
            DEBUG_INFO("%s: Sent to PLC (existing conn) session=%u, %u bytes\n",
                       COMPONENT_NAME, session_id, len);
            return 0;
        }
        DEBUG_WARN("%s: TCP write failed session=%u: %d\n",
                   COMPONENT_NAME, session_id, err);
        return -1;
    }

    /* Create new connection */
    conn = create_plc_connection(session_id);
    if (conn == NULL) {
        DEBUG_WARN("%s: Failed to create PLC connection session=%u\n",
                   COMPONENT_NAME, session_id);
        return -1;
    }

    /* Create TCP PCB */
    struct tcp_pcb *pcb = tcp_new();
    if (pcb == NULL) {
        DEBUG_ERROR("%s: Failed to create TCP PCB\n", COMPONENT_NAME);
        cleanup_plc_connection(conn);
        return -1;
    }

    conn->pcb = pcb;
    conn->plc_ip = plc_ip_addr.addr;
    conn->plc_port = PLC_PORT;

    /* Store pending data */
    conn->pending_data = malloc(len);
    if (conn->pending_data) {
        memcpy(conn->pending_data, data, len);
        conn->pending_len = len;
    }

    /* Setup callbacks */
    tcp_arg(pcb, conn);
    tcp_recv(pcb, plc_tcp_recv);
    tcp_err(pcb, plc_tcp_err);
    tcp_poll(pcb, plc_tcp_poll, 4);

    /* Connect to PLC */
    conn->connecting = true;
    err_t err = tcp_connect(pcb, &plc_ip_addr, PLC_PORT, plc_tcp_connected);
    if (err != ERR_OK) {
        DEBUG_ERROR("%s: Failed to connect to PLC: %d\n", COMPONENT_NAME, err);
        cleanup_plc_connection(conn);
        return -1;
    }

    DEBUG_INFO("%s: Connecting to PLC session=%u, ip=%s\n",
               COMPONENT_NAME, session_id, ip4addr_ntoa(&plc_ip_addr));

    return 0;
}

/*
 * Handle inbound data from ICS_Inbound (validated SCADA requests)
 */
static void handle_inbound_notification(void)
{
    InboundDataport *dp = (InboundDataport *)inbound_dp;
    ICS_Message *msg = &dp->request_msg;

    /* Sentinel check */
    if (msg->payload_length == 0) {
        /* Handle close queue */
        return;
    }

    DEBUG_INFO("%s: [INBOUND] session=%u, %u bytes from ICS_Inbound\n",
               COMPONENT_NAME, msg->metadata.session_id, msg->payload_length);

    /* Forward validated request to PLC */
    int err = connect_to_plc(msg->metadata.session_id, msg->payload, msg->payload_length);
    if (err != 0) {
        DEBUG_WARN("%s: Failed to forward to PLC session=%u\n",
                   COMPONENT_NAME, msg->metadata.session_id);
    }
}

/*
 * Network interface output function
 */
static err_t netif_output(struct netif *netif, struct pbuf *p)
{
    if (eth_driver == NULL) {
        return ERR_IF;
    }

    void *buf = malloc(p->tot_len);
    if (buf == NULL) {
        return ERR_MEM;
    }

    pbuf_copy_partial(p, buf, p->tot_len, 0);
    uintptr_t phys = (uintptr_t)buf;

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
    netif->name[1] = '1';
    netif->linkoutput = netif_output;
    netif->output = etharp_output;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

    if (eth_driver != NULL) {
        eth_driver->i_fn.get_mac(eth_driver, netif->hwaddr);
    } else {
        netif->hwaddr[0] = 0x52;
        netif->hwaddr[1] = 0x54;
        netif->hwaddr[2] = 0x00;
        netif->hwaddr[3] = 0x12;
        netif->hwaddr[4] = 0x34;
        netif->hwaddr[5] = 0x57;  /* Different from Net0 */
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
        if (!ip4_addr_isany(netif_ip4_addr(netif))) {
            dhcp_bound = true;
        }
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
    sys_check_timeouts();
}

/*
 * Inbound notification handler (from ICS_Inbound)
 */
void inbound_ready_handle(void)
{
    handle_inbound_notification();
}

/*
 * Component initialization
 */
void pre_init(void)
{
    DEBUG_INFO("%s: Initializing x86 ethernet driver...\n", COMPONENT_NAME);

    /* Initialize connection table */
    memset(plc_connections, 0, sizeof(plc_connections));

    /* Set default PLC IP */
    ip4addr_aton(DEFAULT_PLC_IP, &plc_ip_addr);

    /* Initialize connection state sharing */
    own_state = (volatile struct connection_state_table *)net1_conn_state;
    peer_state = (volatile struct connection_state_table *)net0_conn_state;
    if (own_state) {
        memset((void *)own_state, 0, sizeof(*own_state));
    }
}

static int init_ethernet(ps_io_ops_t *io_ops)
{
    int error;

    error = ps_calloc(&io_ops->malloc_ops, 1, sizeof(*eth_driver), (void **)&eth_driver);
    if (error) {
        DEBUG_ERROR("%s: Failed to allocate eth_driver\n", COMPONENT_NAME);
        return error;
    }

    ethif_intel_config_t *eth_config = calloc(1, sizeof(ethif_intel_config_t) + sizeof(ps_irq_t));
    if (eth_config == NULL) {
        DEBUG_ERROR("%s: Failed to allocate eth_config\n", COMPONENT_NAME);
        return -1;
    }

    *eth_config = (ethif_intel_config_t) {
        .bar0 = (void *)eth_mmio,
        .prom_mode = 1,
        .num_irqs = 1
    };

    eth_config->irq_info[0] = (ps_irq_t) {
        .type = PS_IOAPIC,
        .ioapic = { .ioapic = 0, .pin = 10, .level = 1, .polarity = 1, .vector = 10 }
    };

    error = ethif_e82574_init(eth_driver, *io_ops, eth_config);
    if (error) {
        DEBUG_ERROR("%s: Failed to init Intel e1000: %d\n", COMPONENT_NAME, error);
        return error;
    }

    eth_driver->cb_cookie = NULL;
    eth_driver->i_cb = ethdriver_callbacks;

    DEBUG_INFO("%s: Intel e1000 driver initialized\n", COMPONENT_NAME);
    return 0;
}

static int init_rx_buffers(ps_io_ops_t *io_ops)
{
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

    error = init_ethernet(io_ops);
    if (error) {
        return error;
    }

    error = init_rx_buffers(io_ops);
    if (error) {
        return error;
    }

    lwip_init();

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

    dhcp_start(&netif_data);

    initialization_successful = true;
    DEBUG_INFO("%s: Initialization complete, waiting for DHCP...\n", COMPONENT_NAME);

    return 0;
}

CAMKES_POST_INIT_MODULE_DEFINE(net1_driver_init, post_init_setup);

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
