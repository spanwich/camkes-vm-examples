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
#define MEM_SIZE                        (5 * 1024 * 1024)  /* v2.200: Increased to 5MB to eliminate memory exhaustion */

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
#define IP_FORWARD                      1       /* Enable IP forwarding - accept packets for any IP */

/* ICMP configuration */
#define LWIP_ICMP                       1       /* Enable ping */

/* DHCP configuration */
#define LWIP_DHCP                       1       /* Enable DHCP client */

/* TCP configuration */
#define LWIP_TCP                        1

/* ═══════════════════════════════════════════════════════════════════════
 * ICS/SCADA Optimizations for Rapid Connection Cycling (v2.169)
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Standard TCP_MSL=60s causes PCB pool exhaustion in ICS environments where
 * SCADA systems open/close connections every 1-2 seconds for Modbus polling.
 *
 * Problem: TIME_WAIT = 2×MSL = 120s
 *   - At 1 connection/2s rate: 60 PCBs stuck in TIME_WAIT
 *   - Pool exhausts after ~100 connections (MEMP_NUM_TCP_PCB=100)
 *   - Communication stops: "Failed to create TCP PCB - pool exhausted"
 *
 * Solution: Reduce MSL to 30s (TIME_WAIT = 60s)
 *   - Conservative approach (was 1s in v2.168, too aggressive)
 *   - At 1 connection/2s rate: ~30 PCBs in TIME_WAIT (vs 60 at MSL=60s)
 *   - With symmetric error notification (RST), TIME_WAIT should be minimal
 *   - ICS networks are controlled environments - 60s adequate
 *
 * Note: v2.169 adds RST notification to client when pool exhausts,
 *       which should prevent most TIME_WAIT accumulation anyway.
 *
 * See: research-docs/tcp-connection-exhaustion-industry-practices-research.md
 */
#define TCP_MSL                         30000UL  /* 30 seconds (default: 60s) → TIME_WAIT = 60s */

#define TCP_MSS                         1460
#define TCP_SND_BUF                     (16 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_WND                         (16 * TCP_MSS)
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   2
#define MEMP_NUM_TCP_PCB                200     /* v2.200: Increased to 200 for 150 active + TIME_WAIT margin */
#define MEMP_NUM_TCP_SEG                1600    /* v2.200: 8 segments per connection (8 * 200 = 1600) */
#define MEMP_NUM_TCP_PCB_LISTEN         16      /* Max listening sockets (increased from 4) */

/* UDP configuration */
#define LWIP_UDP                        1
#define MEMP_NUM_UDP_PCB                4

/* pbuf configuration */
#define PBUF_POOL_SIZE                  800     /* v2.200: Support 200 connections (200×4=800) */
#define PBUF_POOL_BUFSIZE               2048    /* Match PACKET_BUFFER_SIZE */

/* Checksum configuration
 * CRITICAL: Disable TCP RX checksum validation because we rewrite dest IP
 * before lwIP sees the packet, which invalidates the TCP checksum
 * (TCP checksum includes IP pseudo-header with src/dest IPs)
 */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              0  /* Disabled - we rewrite dest IP */

/* Debugging options - Disabled for production */
#define LWIP_DEBUG                      0                 /* Disable all lwIP debug output */
#define LWIP_DBG_MIN_LEVEL              LWIP_DBG_LEVEL_OFF
#define LWIP_DBG_TYPES_ON               LWIP_DBG_OFF

/* Disable all TCP debugging */
#define TCP_DEBUG                       LWIP_DBG_OFF
#define TCP_INPUT_DEBUG                 LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG                LWIP_DBG_OFF
#define TCPIP_DEBUG                     LWIP_DBG_OFF
#define IP_DEBUG                        LWIP_DBG_OFF
#define ETHARP_DEBUG                    LWIP_DBG_OFF
#define PBUF_DEBUG                      LWIP_DBG_OFF
#define NETIF_DEBUG                     LWIP_DBG_OFF

/* Statistics */
#define LWIP_STATS                      1
#define LWIP_STATS_DISPLAY              1

/* Netif status callback */
#define LWIP_NETIF_STATUS_CALLBACK      1

/* Lightweight protection */
#define SYS_LIGHTWEIGHT_PROT            0

/* v2.163: LWIP_PLATFORM_ASSERT removed from lwipopts.h
 *
 * Now using default from util_libs/liblwip/include/lwip/arch/cc.h
 * which uses while(1) infinite loop instead of abort().
 *
 * This ensures assertions actually HALT the system instead of
 * returning (abort doesn't work in seL4 - no POSIX signals).
 *
 * GDB will catch the infinite loop for debugging.
 */

#endif /* __LWIPOPTS_H__ */
