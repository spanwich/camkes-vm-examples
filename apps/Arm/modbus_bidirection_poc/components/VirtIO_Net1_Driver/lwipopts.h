/*
 * lwIP configuration for EthernetDriver Tier 4 (TCP Echo Server)
 * Based on sDDF echo_server configuration
 */

#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

/* Core lwIP configuration */
#define NO_SYS                          1       /* No OS threading */
#define LWIP_TIMERS                     1       /* Enable timers for TCP */
#define LWIP_NETCONN                    0       /* Disable netconn API */
#define LWIP_SOCKET                     0       /* Disable socket API */
#define LWIP_RAND                       rand

/* Memory configuration */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (128 * 1024)  /* v2.87: 128KB heap to support 256 PCBs */

/* v2.78: Use static memory pools instead of malloc to prevent cross-component contamination */
#define MEM_LIBC_MALLOC                 0       /* DO NOT use system malloc */
/* #define MEM_USE_POOLS                   1       -- Disabled: requires pool definitions */
#define MEMP_MEM_MALLOC                 0       /* Memory pools are preallocated */

/* ARP configuration */
#define LWIP_ARP                        1
#define ETHARP_SUPPORT_STATIC_ENTRIES   1

/* IP configuration */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0       /* Disable IPv6 for simplicity */

/* ICMP configuration */
#define LWIP_ICMP                       1       /* Enable ping */

/* DHCP configuration */
#define LWIP_DHCP                       1       /* Enable DHCP client */

/* TCP configuration */
#define LWIP_TCP                        1
#define TCP_MSS                         1460
#define TCP_SND_BUF                     (16 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_WND                         (16 * TCP_MSS)
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   2
#define MEMP_NUM_TCP_SEG                TCP_SND_QUEUELEN
#define MEMP_NUM_TCP_PCB                100     /* v2.95: lwIP-managed connection limit - lwIP refuses new connections when exhausted */
#define MEMP_NUM_TCP_PCB_LISTEN         16      /* Max listening sockets (increased from 4) */

/* UDP configuration */
#define LWIP_UDP                        1
#define MEMP_NUM_UDP_PCB                4

/* pbuf configuration */
#define PBUF_POOL_SIZE                  32      /* Match our packet buffer count */
#define PBUF_POOL_BUFSIZE               2048    /* Match PACKET_BUFFER_SIZE */

/* Checksum configuration - let hardware handle it if possible */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1

/* Debugging options - Disabled for production */
#define LWIP_DEBUG                      0
#define LWIP_DBG_MIN_LEVEL              LWIP_DBG_LEVEL_OFF
#define LWIP_DBG_TYPES_ON               LWIP_DBG_OFF
#define TCP_DEBUG                       LWIP_DBG_OFF
#define TCP_INPUT_DEBUG                 LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG                LWIP_DBG_OFF

/* Statistics */
#define LWIP_STATS                      1
#define LWIP_STATS_DISPLAY              1

/* Netif status callback */
#define LWIP_NETIF_STATUS_CALLBACK      1

/* Lightweight protection */
#define SYS_LIGHTWEIGHT_PROT            0

#endif /* __LWIPOPTS_H__ */
