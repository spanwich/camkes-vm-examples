# TCP Connection Lifecycle - Sequence Diagrams

## MODBUS Bidirectional Cross-Domain Solution

**Version:** v2.240+ (Stable)
**Last Updated:** 2025-12-01

---

## Table of Contents

1. [Normal Connection Flow (Request-Response)](#1-normal-connection-flow-request-response)
2. [SCADA-Initiated Close](#2-scada-initiated-close)
3. [PLC Connection Error](#3-plc-connection-error)
4. [Connection Cleanup](#4-connection-cleanup)
5. [ICMP Ping Proxy](#5-icmp-ping-proxy)

---

## 1. Normal Connection Flow (Request-Response)

### Scenario: SCADA reads holding registers from PLC

```
SCADA          Net0           ICS_In        Net1           PLC
(Ext)         (lwIP)         (Validator)    (lwIP)        (Int)
  │              │                │            │             │
  │──TCP SYN────>│                │            │             │
  │              │                │            │             │
  │              │ tcp_accept()   │            │             │
  │              │ - Allocate     │            │             │
  │              │   session=1234 │            │             │
  │              │ - Store meta   │            │             │
  │<─SYN+ACK─────│                │            │             │
  │              │                │            │             │
  │──ACK────────>│                │            │             │
  │              │                │            │             │
  │              │ [Connection    │            │             │
  │              │  established]  │            │             │
  │              │                │            │             │
  │──MODBUS Req─>│                │            │             │
  │ (Read Regs)  │                │            │             │
  │              │                │            │             │
  │              │ tcp_echo_recv()│            │             │
  │              │ - Extract meta │            │             │
  │              │ - Copy payload │            │             │
  │              │                │            │             │
  │              │─Inbound DP────>│            │             │
  │              │ {session:1234, │            │             │
  │              │  payload:...}  │            │             │
  │              │─Notify────────>│            │             │
  │              │                │            │             │
  │              │                │ Validate   │             │
  │              │                │ - bounds   │             │
  │              │                │ - EverParse│             │
  │              │                │            │             │
  │              │                │─Forward───>│             │
  │              │                │            │             │
  │              │                │            │ Lookup      │
  │              │                │            │ session=1234│
  │              │                │            │ → Not found │
  │              │                │            │             │
  │              │                │            │ tcp_connect()│
  │              │                │            │────────────>│
  │              │                │            │             │
  │              │                │            │<─SYN+ACK────│
  │              │                │            │             │
  │              │                │            │──ACK───────>│
  │              │                │            │             │
  │              │                │            │ Connected   │
  │              │                │            │ - Store meta│
  │              │                │            │ - session:  │
  │              │                │            │   1234      │
  │              │                │            │             │
  │              │                │            │──MODBUS────>│
  │              │                │            │   Request   │
  │              │                │            │             │
  │              │                │            │             │ Process
  │              │                │            │             │ request
  │              │                │            │             │
  │              │                │            │<─MODBUS─────│
  │              │                │            │  Response   │
  │              │                │            │             │
  │              │                │            │ tcp_plc_recv│
  │              │                │            │ - Extract   │
  │              │                │            │   response  │
  │              │                │            │             │
  │              │                │<─Outbound DP─────────────│
  │              │                │  {session:  │             │
  │              │                │   1234}     │             │
  │              │                │<─Notify─────│             │
  │              │                │             │             │
  │              │                │ Validate    │             │
  │              │                │ - bounds    │             │
  │              │                │ - EverParse │             │
  │              │                │             │             │
  │              │<───Forward─────│             │             │
  │              │                │             │             │
  │              │ Lookup session │             │             │
  │              │ - Reconstruct  │             │             │
  │              │   TCP packet   │             │             │
  │              │                │             │             │
  │<─MODBUS Resp─│                │             │             │
  │              │                │             │             │
  │──ACK────────>│                │             │             │
  │              │                │             │             │
```

**Key Points:**

- **Session ID Assignment:** Net0 assigns unique session ID on SCADA connection
- **Lazy Connection:** Net1 only connects to PLC when first request arrives
- **Connection Reuse:** Subsequent requests on same session reuse existing Net1→PLC connection
- **Metadata Preservation:** Original IP addresses stored throughout lifecycle
- **Bidirectional Flow:** Data flows through both validation components

---

## 2. SCADA-Initiated Close

### Scenario: SCADA gracefully closes connection

```
SCADA          Net0           ICS_In        Net1           PLC
(Ext)         (lwIP)         (Validator)    (lwIP)        (Int)
  │              │                │            │             │
  │ [Active]     │                │            │             │
  │ session=1234 │                │            │      Active │
  │              │                │            │  session=1234│
  │              │                │            │             │
  │──FIN────────>│                │            │             │
  │              │                │            │             │
  │              │ tcp_recv(p=NULL)            │             │
  │              │ - Find meta    │            │             │
  │              │ - session=1234 │            │             │
  │              │ - pcb->state   │            │             │
  │              │   check        │            │             │
  │              │                │            │             │
  │              │ Enqueue Close  │            │             │
  │              │ to close_queue │            │             │
  │              │ {session:1234, │            │             │
  │              │  type:CLOSE}   │            │             │
  │              │                │            │             │
  │              │─Inbound DP────>│            │             │
  │              │ payload_len=0  │            │             │
  │              │ (sentinel)     │            │             │
  │              │─Notify────────>│            │             │
  │              │                │            │             │
  │              │                │ Forward    │             │
  │              │                │ close_queue│             │
  │              │                │            │             │
  │              │                │─Forward───>│             │
  │              │                │            │             │
  │              │                │            │ Process     │
  │              │                │            │ close_queue │
  │              │                │            │             │
  │              │                │            │ Lookup      │
  │              │                │            │ session=1234│
  │              │                │            │             │
  │              │                │            │ tcp_close() │
  │              │                │            │────────────>│
  │              │                │            │             │
  │              │                │            │<─FIN+ACK────│
  │              │                │            │             │
  │              │                │            │──ACK───────>│
  │              │                │            │             │
  │              │                │            │             │
  │              │                │            │ Cleanup     │
  │              │                │            │ metadata    │
  │              │                │            │             │
  │              │ Poll callback  │            │             │
  │              │ - Check        │            │             │
  │              │   close_pending│            │             │
  │              │                │            │             │
  │              │ tcp_close()    │            │             │
  │<─FIN+ACK─────│                │            │             │
  │              │                │            │             │
  │──ACK────────>│                │            │             │
  │              │                │            │             │
  │              │ Cleanup        │            │             │
  │              │ metadata       │            │             │
  │              │                │            │             │
```

**Key Points:**

- **Close Detection:** `tcp_recv(p=NULL)` indicates graceful close
- **Notification Queue:** Close event propagated through ICS_Inbound
- **Sentinel Value:** `payload_length=0` signals control-only message
- **Cascading Close:** Net1 closes PLC connection, then Net0 closes SCADA
- **Cleanup Coordination:** Both drivers use cleanup queues for safe PCB teardown

---

## 3. PLC Connection Error

### Scenario: PLC connection timeout or reset

```
SCADA          Net0           ICS_In        Net1           PLC
(Ext)         (lwIP)         (Validator)    (lwIP)        (Int)
  │              │                │            │             │
  │ [Active]     │                │            │             │
  │ session=1234 │                │            │   [Active]  │
  │              │                │            │   session=  │
  │              │                │            │   1234      │
  │              │                │            │             │
  │              │                │            │             X (Down)
  │              │                │            │             │
  │              │                │            │ TCP timeout │
  │              │                │            │ retransmit  │
  │              │                │            │ fails       │
  │              │                │            │             │
  │              │                │            │ tcp_err_cb()│
  │              │                │            │ - ERR_ABRT  │
  │              │                │            │ - Find meta │
  │              │                │            │ - session=  │
  │              │                │            │   1234      │
  │              │                │            │             │
  │              │                │            │ Enqueue     │
  │              │                │            │ to error_q  │
  │              │                │            │ {session:   │
  │              │                │            │  1234,      │
  │              │                │            │  type:ERROR}│
  │              │                │            │             │
  │              │                │            │ Set pcb=NULL│
  │              │                │            │ (prevent    │
  │              │                │            │  double-free│
  │              │                │            │             │
  │              │                │<─Outbound DP─────────────│
  │              │                │  payload_  │             │
  │              │                │  len=0     │             │
  │              │                │  (sentinel)│             │
  │              │                │<─Notify─────│             │
  │              │                │             │             │
  │              │                │ Forward     │             │
  │              │                │ error_queue │             │
  │              │                │             │             │
  │              │<───Forward─────│             │             │
  │              │                │             │             │
  │              │ Process        │             │             │
  │              │ error_queue    │             │             │
  │              │                │             │             │
  │              │ Lookup         │             │             │
  │              │ session=1234   │             │             │
  │              │                │             │             │
  │              │ tcp_abort()    │             │             │
  │              │ (RST to SCADA) │             │             │
  │<─RST─────────│                │             │             │
  │              │                │             │             │
  │              │ Cleanup        │             │             │
  │              │ metadata       │             │             │
  │              │                │             │             │
  │              │                │             │ Cleanup     │
  │              │                │             │ metadata    │
  │              │                │             │ (in poll)   │
  │              │                │             │             │
```

**Key Points:**

- **Error Callback:** lwIP calls `tcp_err_callback()` on connection failure
- **Error Queue:** Propagated through ICS_Outbound to notify Net0
- **Immediate Cleanup:** `pcb=NULL` prevents double-free
- **Hard Reset:** Net0 sends RST to SCADA (no graceful close possible)
- **Asynchronous Cleanup:** Main loop processes cleanup queue safely

---

## 4. Connection Cleanup

### Internal cleanup queue processing (Net0 and Net1)

```
     Main Loop                  lwIP Callbacks           Cleanup Queue
         │                            │                        │
         │                            │                        │
         │                            │ Connection close       │
         │                            │ or error occurs        │
         │                            │                        │
         │                            │ Enqueue cleanup        │
         │                            │───────────────────────>│
         │                            │ {session_id: 1234,     │
         │                            │  timestamp: now}       │
         │                            │                        │
         │                            │ Set pcb=NULL           │
         │                            │ (prevent double-close) │
         │                            │                        │
         │                            │ Return immediately     │
         │                            │ (do NOT call tcp_close)│
         │                            │                        │
         │                            │                        │
         │ Poll tick                  │                        │
         │                            │                        │
         │ process_cleanup_queue()    │                        │
         │────────────────────────────────────────────────────>│
         │                            │                        │
         │<───────────────────────────────────────────────────>│
         │ while (queue not empty) {  │                  Dequeue
         │   Dequeue request          │                  entry
         │   Lookup session_id        │                        │
         │   if (meta->active) {      │                        │
         │     tcp_close(pcb)         │                        │
         │     free pool_state        │                        │
         │     meta->active = false   │                        │
         │     active_connections--   │                        │
         │   }                        │                        │
         │ }                          │                        │
         │                            │                        │
         │ Continue main loop         │                        │
         │                            │                        │
```

**Critical Rules (v2.241 fix):**

1. **Callbacks NEVER call tcp_close/tcp_abort:**
   - Only enqueue cleanup request
   - Set `pcb=NULL` immediately
   - Return to lwIP immediately

2. **Main loop is SINGLE cleanup authority:**
   - Only `process_cleanup_queue()` decrements counters
   - Only main loop calls `tcp_close()`
   - Prevents race conditions and double-free

3. **Protection against double-enqueue:**
   - `cleanup_in_progress` flag
   - Duplicate detection in queue processor

---

## 5. ICMP Ping Proxy

### Scenario: SCADA pings PLC through gateway

```
SCADA          Net0           ICS_In        Net1           PLC
(Ext)         (lwIP)         (Validator)    (lwIP)        (Int)
  │              │                │            │             │
  │─ICMP Echo───>│                │            │             │
  │ Request      │                │            │             │
  │ (ping PLC)   │                │            │             │
  │              │                │            │             │
  │              │ netif_input()  │            │             │
  │              │ - IP layer     │            │             │
  │              │ - ICMP layer   │            │             │
  │              │                │            │             │
  │              │ Extract meta:  │            │             │
  │              │ - dest_ip      │            │             │
  │              │ - icmp_id      │            │             │
  │              │ - icmp_seq     │            │             │
  │              │                │            │             │
  │              │ Store in       │            │             │
  │              │ icmp_table[]   │            │             │
  │              │                │            │             │
  │              │─Inbound DP────>│            │             │
  │              │ {ICMP request} │            │             │
  │              │─Notify────────>│            │             │
  │              │                │            │             │
  │              │                │ Validate   │             │
  │              │                │            │             │
  │              │                │─Forward───>│             │
  │              │                │            │             │
  │              │                │            │ ip_output() │
  │              │                │            │────────────>│
  │              │                │            │             │
  │              │                │            │             │ Process
  │              │                │            │             │ ping
  │              │                │            │             │
  │              │                │            │<─ICMP Echo──│
  │              │                │            │  Reply      │
  │              │                │            │             │
  │              │                │            │ netif_input │
  │              │                │            │ - Lookup    │
  │              │                │            │   icmp_table│
  │              │                │            │ - Find orig │
  │              │                │            │   dest_ip   │
  │              │                │            │             │
  │              │                │            │ Synthesize  │
  │              │                │            │ reply from  │
  │              │                │            │ original IP │
  │              │                │            │             │
  │              │                │<─Outbound DP─────────────│
  │              │                │  {ICMP     │             │
  │              │                │   reply}   │             │
  │              │                │<─Notify─────│             │
  │              │                │             │             │
  │              │<───Forward─────│             │             │
  │              │                │             │             │
  │              │ Lookup         │             │             │
  │              │ icmp_table     │             │             │
  │              │ - Restore      │             │             │
  │              │   original src │             │             │
  │              │                │             │             │
  │<─ICMP Echo───│                │             │             │
  │  Reply       │                │             │             │
  │ (from PLC IP)│                │             │             │
  │              │                │             │             │
```

**Key Points:**

- **Metadata Preservation:** ICMP ID/sequence stored in lookup table
- **IP Address Translation:** Reply appears to come from original PLC IP
- **Timeout Handling:** Old entries cleaned up after 5 seconds
- **Table Size:** 16 concurrent pings supported
- **Transparent Proxy:** SCADA sees direct ping response from PLC

---

## Connection State Machine

### Net0 Connection States

```
┌─────────────┐
│   CLOSED    │◀──────────────────────────────┐
└──────┬──────┘                               │
       │                                      │
       │ tcp_accept()                         │
       │ Allocate session_id                  │
       │                                      │
       ▼                                      │
┌─────────────┐                               │
│   ACTIVE    │                               │
│ awaiting_   │                               │
│ response=T  │                               │
└──────┬──────┘                               │
       │                                      │
       │ Outbound response                    │
       │ received from ICS                    │
       │                                      │
       ▼                                      │
┌─────────────┐                               │
│   ACTIVE    │                               │
│ response_   │                               │
│ received=T  │                               │
└──────┬──────┘                               │
       │                                      │
       │ tcp_recv(p=NULL)                     │
       │ OR error_queue                       │
       │                                      │
       ▼                                      │
┌─────────────┐                               │
│   CLOSING   │                               │
│ close_      │                               │
│ pending=T   │                               │
└──────┬──────┘                               │
       │                                      │
       │ Poll callback                        │
       │ tcp_close()                          │
       │                                      │
       ▼                                      │
┌─────────────┐                               │
│  CLEANUP    │                               │
│  QUEUED     │                               │
└──────┬──────┘                               │
       │                                      │
       │ process_cleanup_queue()              │
       │ Free metadata                        │
       │                                      │
       └──────────────────────────────────────┘
```

### Net1 Connection States

```
┌─────────────┐
│   CLOSED    │◀──────────────────────────────┐
└──────┬──────┘                               │
       │                                      │
       │ Inbound request                      │
       │ Session not found                    │
       │ tcp_connect()                        │
       │                                      │
       ▼                                      │
┌─────────────┐                               │
│ CONNECTING  │                               │
└──────┬──────┘                               │
       │                                      │
       │ tcp_connected_cb()                   │
       │ Send request to PLC                  │
       │                                      │
       ▼                                      │
┌─────────────┐                               │
│   ACTIVE    │                               │
│ awaiting_   │                               │
│ response=T  │                               │
└──────┬──────┘                               │
       │                                      │
       │ tcp_plc_recv()                       │
       │ Response from PLC                    │
       │                                      │
       ▼                                      │
┌─────────────┐                               │
│   ACTIVE    │                               │
│ response_   │                               │
│ received=T  │                               │
└──────┬──────┘                               │
       │                                      │
       │ close_queue from ICS                 │
       │ OR tcp_err_callback()                │
       │                                      │
       ▼                                      │
┌─────────────┐                               │
│   CLOSING   │                               │
│ close_      │                               │
│ pending=T   │                               │
└──────┬──────┘                               │
       │                                      │
       │ Poll callback                        │
       │ tcp_close()                          │
       │                                      │
       ▼                                      │
┌─────────────┐                               │
│  CLEANUP    │                               │
│  QUEUED     │                               │
└──────┬──────┘                               │
       │                                      │
       │ process_cleanup_queue()              │
       │ Free pool_state + metadata           │
       │                                      │
       └──────────────────────────────────────┘
```

---

## Error Scenarios

### 1. Cleanup Queue Overflow

```
Multiple rapid disconnects → Cleanup queue full

Net0/Net1:
  │
  ▼
cleanup_queue.head - cleanup_queue.tail >= CLEANUP_QUEUE_SIZE - 1
  │
  ▼
Drop cleanup request (log error)
  │
  ▼
Connection becomes "zombie" (metadata leak)
  │
  ▼
Eventually cleaned by idle timeout (future enhancement)
```

**Mitigation:** Queue size = 512 (much larger than MAX_CONNECTIONS=150)

### 2. Session ID Collision

```
Net0 assigns session_id=1234 twice (wraparound after 4B sessions)

Net1:
  │
  ▼
Lookup session_id=1234 → Found existing active connection
  │
  ▼
Check tcp_seq_num (initial sequence number)
  │
  ▼
Mismatch → Old connection, proceed with new request
Match → Reuse existing connection
```

**Protection:** 32-bit session space makes collision extremely rare

### 3. ICS Validation Failure

```
SCADA sends malformed MODBUS frame

Net0 → ICS_Inbound:
  │
  ▼
everparse_validate() returns false
  │
  ▼
ICS_Inbound: Drop message, increment stats.messages_dropped
  │
  ▼
Net1: Never receives request
  │
  ▼
SCADA: Timeout, retransmit
```

**Result:** Gateway acts as firewall, blocks invalid traffic

---

## Timing Diagrams

### Connection Reuse (Fast Path)

```
Time    SCADA          Net0           Net1           PLC
0ms      │──Req1──────>│              │              │
         │              │─────────────>│              │
         │              │              │──Req1──────>│
20ms     │              │              │<─Resp1──────│
         │              │<─────────────│              │
         │<─Resp1───────│              │              │
         │              │              │              │
50ms     │──Req2──────>│              │              │
         │              │─────────────>│              │
         │              │              │ Lookup       │
         │              │              │ session      │
         │              │              │ → FOUND      │
         │              │              │ (reuse PCB)  │
         │              │              │──Req2──────>│
70ms     │              │              │<─Resp2──────│
         │              │<─────────────│              │
         │<─Resp2───────│              │              │
```

**Benefit:** No connection setup overhead on subsequent requests

### Cascading Timeout

```
Time    SCADA          Net0           Net1           PLC
0ms      │──Req────────>│              │              │
         │              │─────────────>│              │
         │              │              │──Req───────>│
         │              │              │              X (Down)
         │              │              │              │
         │              │              │ Retransmit   │
3s       │              │              │ (3 attempts) │
         │              │              │              │
         │              │              │              │
10s      │              │              │ TCP timeout  │
         │              │              │ tcp_err_cb() │
         │              │              │ Enqueue error│
         │              │<─Error Queue─│              │
         │              │ tcp_abort()  │              │
         │<─RST─────────│              │              │
```

**Behavior:** PLC failure propagates to SCADA within 10-15 seconds

---

**Document Status:** Current (matches v2.240+ implementation)
**Maintenance:** Update when connection lifecycle changes
**Related Documents:**
- [DATAFLOW-DIAGRAM.md](DATAFLOW-DIAGRAM.md) - System architecture
- [SYSTEM-DESIGN.md](SYSTEM-DESIGN.md) - Implementation details
- [README.md](README.md) - System overview
