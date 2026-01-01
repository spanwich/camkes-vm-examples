# System Design Document

## MODBUS Bidirectional Cross-Domain Solution

**Project:** ICS Bidirectional Cross-Domain Security Gateway
**Version:** v2.240+ (Stable)
**Platform:** seL4 Microkernel + CAmkES Framework
**Target:** ARM Cortex-A (QEMU virt machine)
**Last Updated:** 2025-12-01

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [System Architecture](#2-system-architecture)
3. [Component Specifications](#3-component-specifications)
4. [Data Structures](#4-data-structures)
5. [Communication Protocols](#5-communication-protocols)
6. [Security Architecture](#6-security-architecture)
7. [Performance Characteristics](#7-performance-characteristics)
8. [Error Handling](#8-error-handling)
9. [Testing and Validation](#9-testing-and-validation)
10. [Future Enhancements](#10-future-enhancements)

---

## 1. Executive Summary

### Purpose

The MODBUS Bidirectional Cross-Domain Solution provides a secure, verified gateway for Industrial Control Systems (ICS) communication between untrusted external networks (SCADA systems) and trusted internal networks (PLCs/RTUs). The system implements **protocol break** architecture on the formally verified **seL4 microkernel**, ensuring mathematically guaranteed isolation between security domains.

### Key Features

- ✅ **Bidirectional Protocol Break:** Independent TCP/IP stacks prevent direct packet forwarding
- ✅ **Formal Verification Foundation:** Built on seL4 (world's first formally verified OS kernel)
- ✅ **Metadata-Based Validation:** Stateless validators operate on extracted protocol metadata
- ✅ **EverParse Integration Ready:** Hooks for formally verified parser integration
- ✅ **Connection Lifecycle Management:** Robust session tracking with 150 concurrent connections per interface
- ✅ **Zero-Copy Architecture:** Dataport-based communication minimizes memory overhead
- ✅ **Crash Resilience:** Cleanup queue pattern prevents lwIP PCB corruption
- ✅ **ICMP Proxy:** Transparent ping forwarding for network diagnostics

### Design Goals

| Goal | Status | Implementation |
|------|--------|----------------|
| **G1:** Prevent direct network bridging | ✅ Complete | Independent lwIP stacks per network driver |
| **G2:** Validate all ICS protocols | 🚧 Partial | EverParse hooks present, MODBUS validator pending |
| **G3:** Maintain sub-10ms latency | ✅ Complete | Zero-copy dataports, interrupt-driven design |
| **G4:** Support 150+ concurrent SCADA clients | ✅ Complete | Connection pooling with session ID mapping |
| **G5:** Fail-safe on validation errors | ✅ Complete | Drop invalid frames, propagate errors via control queues |
| **G6:** No code execution in validators | ✅ Complete | ICS components are lwIP-free, metadata-only |

---

## 2. System Architecture

### 2.1 Component Topology

```
┌─────────────────────────────────────────────────────────────────┐
│                     seL4 Microkernel                            │
│  (Formally verified isolation, capability-based security)       │
└─────────────────────────────────────────────────────────────────┘
          │              │              │              │
          │              │              │              │
   ┌──────▼──────┐ ┌────▼─────┐ ┌─────▼────┐ ┌──────▼──────┐
   │ VirtIO_Net0 │ │ICS_Inbound│ │ICS_Out   │ │ VirtIO_Net1 │
   │   (Ext)     │ │(Validator)│ │ (Valid)  │ │   (Int)     │
   └──────┬──────┘ └────┬─────┘ └─────┬────┘ └──────┬──────┘
          │              │              │              │
   ┌──────▼──────────────▼──────────────▼──────────────▼──────┐
   │           CAmkES Connectors (seL4 IPC)                    │
   │  - seL4SharedData (dataports)                             │
   │  - seL4Notification (event signaling)                     │
   │  - seL4HardwareMMIO (device access)                       │
   │  - seL4HardwareInterrupt (IRQ routing)                    │
   └───────────────────────────────────────────────────────────┘
```

### 2.2 Network Interfaces

| Interface | Role | IP Address | Ports | Network Segment |
|-----------|------|------------|-------|-----------------|
| **VirtIO-Net0** | External | 192.168.96.10/24 (DHCP) | TCP:502 (server) | SCADA, HMI, Engineering Workstations |
| **VirtIO-Net1** | Internal | 192.168.95.10/24 (DHCP) | TCP:random (client) | PLCs, RTUs, Field Devices |

**Hardware Configuration:**
- Slot 31 @ 0xa003e00 → Net0 (External) → IRQ 79
- Slot 30 @ 0xa003c00 → Net1 (Internal) → IRQ 78
- Both share 4KB MMIO page @ 0xa003000 (uncached device memory)

### 2.3 Data Flow Paths

#### Inbound Path (SCADA → PLC)

```
External Network
      ↓
VirtIO_Net0_Driver (lwIP)
   - TCP server :502
   - Extract FrameMetadata
   - Assign session_id
      ↓
[Dataport: InboundDataport]
      ↓
ICS_Inbound (Validator)
   - bounds_check()
   - everparse_validate()
   - Forward metadata + payload
      ↓
[Dataport: InboundDataport]
      ↓
VirtIO_Net1_Driver (lwIP)
   - TCP client (connect PLC)
   - Map session_id to PLC PCB
   - Transmit validated payload
      ↓
Internal Network (PLC)
```

#### Outbound Path (PLC → SCADA)

```
Internal Network (PLC)
      ↓
VirtIO_Net1_Driver (lwIP)
   - TCP receive from PLC
   - Extract response + session_id
      ↓
[Dataport: OutboundDataport]
      ↓
ICS_Outbound (Validator)
   - bounds_check()
   - everparse_validate()
   - Forward metadata + payload
      ↓
[Dataport: OutboundDataport]
      ↓
VirtIO_Net0_Driver (lwIP)
   - Lookup session_id → SCADA PCB
   - Reconstruct TCP response
   - Transmit to SCADA
      ↓
External Network (SCADA)
```

---

## 3. Component Specifications

### 3.1 VirtIO_Net0_Driver (External Network)

**Responsibilities:**
- VirtIO-net device driver (RX/TX virtqueues)
- lwIP TCP/IP stack management
- DHCP client for IP configuration
- TCP server on port 502 (MODBUS)
- Session ID assignment
- Connection metadata management
- ICMP echo request/reply proxy
- Outbound response reconstruction

**Configuration:**
```c
Priority:              101
Stack Size:            128 KB
Heap Size:             1 MB
DMA Pool:              256 KB
Untyped Memory Pool:   4 × 2 MB = 8 MB
Hardware IRQ:          79 (SPI 47)
MMIO Base:             0xa003000 (4 KB, uncached)
```

**Key Functions:**
- `tcp_accept_callback()`: Accept SCADA connections, allocate session ID
- `tcp_echo_recv()`: Extract metadata, forward to ICS_Inbound
- `outbound_ready_handle()`: Receive PLC responses, send to SCADA
- `process_cleanup_queue()`: Safe PCB teardown (main loop only)
- `netif_input()`: ICMP request detection and metadata storage

**Connection Limits:**
- Max Connections: 150
- Session ID Range: 1 to 4,294,967,295 (32-bit)
- Metadata Size: ~100 bytes per connection

### 3.2 VirtIO_Net1_Driver (Internal Network)

**Responsibilities:**
- VirtIO-net device driver (RX/TX virtqueues)
- lwIP TCP/IP stack management
- DHCP client for IP configuration
- TCP client (dynamic outbound connections to PLCs)
- Session ID mapping to PLC connections
- Connection pool management (100 slots)
- ICMP echo reply synthesis
- Error propagation to Net0

**Configuration:**
```c
Priority:              101
Stack Size:            128 KB
Heap Size:             1 MB
DMA Pool:              256 KB
Untyped Memory Pool:   6 × 2 MB = 12 MB
Hardware IRQ:          78 (SPI 46)
MMIO Base:             0xa003000 (4 KB, uncached)
```

**Key Functions:**
- `inbound_ready_handle()`: Receive validated requests from ICS_Inbound
- `tcp_connect()`: Establish PLC connections on-demand
- `tcp_plc_recv()`: Extract PLC responses, forward to ICS_Outbound
- `process_cleanup_queue()`: Safe PCB teardown
- `netif_input()`: ICMP reply rewriting with original destination IP

**Connection Pool:**
- Pool Size: 100 pre-allocated client states
- Allocation: Dynamic (on first SCADA request per session)
- Cleanup: Automatic on close/error notification

### 3.3 ICS_Inbound (External → Internal Validator)

**Responsibilities:**
- Validate SCADA requests before forwarding to internal network
- Bounds checking (payload length consistency)
- EverParse protocol validation (hook present)
- Close notification forwarding (SCADA disconnect → PLC disconnect)
- Metadata logging and statistics

**Configuration:**
```c
Priority:              150 (higher than network drivers)
Period:                10 ms
Budget:                2 ms
Component Type:        Active (event-driven)
```

**Validation Pipeline:**
```c
1. basic_bounds_check(msg):
   - sizeof(FrameMetadata) + payload_length ≤ available_bytes
   - payload_length ≤ MAX_PAYLOAD_SIZE (60000 bytes)

2. validate_message(msg):
   - payload_length == metadata.payload_length
   - everparse_validate(payload, length)  // Currently stub

3. Forward if validated:
   - memcpy(out_dp, in_dp, msg_size)
   - memcpy(close_queue)  // Forward control notifications
   - __sync_synchronize()
   - out_ntfy_emit()
```

**Statistics Tracked:**
- messages_received
- messages_forwarded
- messages_dropped
- bytes_processed
- Protocol counters (TCP, UDP, ARP, Other)

### 3.4 ICS_Outbound (Internal → External Validator)

**Responsibilities:**
- Validate PLC responses before forwarding to external network
- Bounds checking (payload length consistency)
- EverParse protocol validation (hook present)
- Error notification forwarding (PLC error → SCADA disconnect)
- Metadata logging and statistics

**Configuration:**
```c
Priority:              150
Period:                10 ms
Budget:                2 ms
Component Type:        Active (event-driven)
```

**Validation Pipeline:**
Same as ICS_Inbound, but operates on `response_msg` and `error_queue`

---

## 4. Data Structures

### 4.1 FrameMetadata

**Purpose:** Protocol-agnostic frame information extracted by network drivers

```c
typedef struct {
    // v2.150: Session tracking
    uint32_t session_id;            /* Unique session ID (0 = unassigned) */

    // Ethernet Layer
    uint8_t  dst_mac[6];            /* Destination MAC */
    uint8_t  src_mac[6];            /* Source MAC */
    uint16_t ethertype;             /* 0x0800=IPv4, 0x0806=ARP, etc. */
    uint16_t vlan_id;               /* VLAN ID (0 if none) */
    uint8_t  vlan_priority;         /* VLAN priority (0-7) */

    // IP Layer
    uint8_t  ip_protocol;           /* 6=TCP, 17=UDP, 0=not IP */
    uint32_t src_ip;                /* Source IP (network byte order) */
    uint32_t dst_ip;                /* Destination IP */

    // Transport Layer
    uint16_t src_port;              /* Source port */
    uint16_t dst_port;              /* Destination port */

    // Payload Info
    uint16_t payload_offset;        /* Offset in original frame */
    uint16_t payload_length;        /* Actual payload length */

    // Protocol Flags
    uint8_t  is_ip      : 1;        /* 1 if IP packet */
    uint8_t  is_tcp     : 1;        /* 1 if TCP */
    uint8_t  is_udp     : 1;        /* 1 if UDP */
    uint8_t  is_arp     : 1;        /* 1 if ARP */
    uint8_t  reserved   : 4;

} __attribute__((packed)) FrameMetadata;  // Size: 52 bytes
```

### 4.2 ICS_Message

**Purpose:** Container for metadata + payload passed via dataports

```c
typedef struct {
    FrameMetadata metadata;                 /* 52 bytes */
    uint16_t      payload_length;           /* 2 bytes */
    uint8_t       payload[MAX_PAYLOAD_SIZE]; /* 60000 bytes */
} __attribute__((packed)) ICS_Message;      // Size: ~60 KB
```

### 4.3 InboundDataport Layout

**Purpose:** Net0 → ICS_Inbound → Net1 data path

```c
typedef struct {
    ICS_Message request_msg;           /* Main request buffer (~60 KB) */
    struct control_queue close_queue;  /* Close notifications (~1.5 KB) */
} __attribute__((packed)) InboundDataport;
```

**v2.188-sentinel Design:**
- When Net0 sends close-only: `request_msg.payload_length = 0` (sentinel)
- Net1 checks: `if (payload_length == 0) → process close_queue only`

### 4.4 OutboundDataport Layout

**Purpose:** Net1 → ICS_Outbound → Net0 data path

```c
typedef struct {
    ICS_Message response_msg;          /* Main response buffer (~60 KB) */
    struct control_queue error_queue;  /* Error notifications (~1.5 KB) */
} __attribute__((packed)) OutboundDataport;
```

**v2.188-sentinel Design:**
- When Net1 sends error-only: `response_msg.payload_length = 0` (sentinel)
- Net0 checks: `if (payload_length == 0) → process error_queue only`

### 4.5 Connection Metadata (Net0)

```c
struct connection_metadata {
    struct tcp_pcb *pcb;               /* lwIP PCB (NULL if closed) */
    uint32_t session_id;               /* Unique session ID */
    uint32_t original_src_ip;          /* SCADA IP (e.g., 192.168.96.5) */
    uint32_t original_dest_ip;         /* PLC IP (e.g., 192.168.95.2) */
    uint16_t src_port;                 /* SCADA port */
    uint16_t dest_port;                /* 502 (MODBUS) */
    bool active;                       /* Slot in use? */

    // Connection validation
    uint32_t tcp_seq_num;              /* Initial sequence (detect reuse) */
    uint32_t timestamp;                /* Creation time */

    // Response tracking
    bool awaiting_response;            /* Waiting for PLC response */
    bool response_received;            /* Response arrived */

    // Cleanup coordination
    bool close_pending;                /* Close in next poll cycle */
    bool closing;                      /* Close initiated */
    bool cleanup_in_progress;          /* Guard against double-cleanup */
    bool close_notified;               /* Notification sent to Net1 */
    bool metadata_close_pending;       /* SCADA closed, metadata persists */
    uint32_t close_timestamp;          /* Grace period start time */
    uint32_t last_tx_timestamp;        /* Last TX activity */

    // Pending data
    uint8_t *pending_outbound_data;
    uint16_t pending_outbound_len;
    bool has_pending_outbound;
};
```

### 4.6 Connection Metadata (Net1)

```c
struct connection_metadata {
    struct tcp_pcb *pcb;               /* lwIP PCB (Net1→PLC) */
    uint32_t session_id;               /* From Net0 (links connections) */
    uint32_t original_src_ip;          /* SCADA IP */
    uint32_t original_dest_ip;         /* PLC IP */
    uint16_t src_port;                 /* SCADA port */
    uint16_t dest_port;                /* 502 */
    uint16_t lwip_ephemeral_port;      /* Net1's ephemeral port */
    bool active;

    // Connection validation
    uint32_t tcp_seq_num;
    uint32_t timestamp;
    uint32_t last_activity;

    // Pool management
    struct tcp_inbound_client_state *pool_state;  /* Pre-allocated state */

    // Error handling
    bool error_notified;               /* Error sent to Net0 */
    bool close_pending;
    bool metadata_close_pending;
    uint32_t close_timestamp;
    uint32_t last_tx_timestamp;

    // Response tracking
    bool awaiting_response;
    bool response_received;
    bool closing;

    // Pending data
    uint8_t *pending_outbound_data;
    uint16_t pending_outbound_len;
    bool has_pending_outbound;

    // Cleanup coordination
    bool cleanup_in_progress;
    bool close_notified;
    bool pcb_closed;                   /* PCB already closed (don't re-close) */
};
```

### 4.7 Control Queue

**Purpose:** Asynchronous close/error notifications

```c
#define CONTROL_QUEUE_SIZE 64

enum ctrl_msg_type {
    CTRL_MSG_CLOSE = 1,    /* Connection close request */
    CTRL_MSG_ERROR = 2     /* Connection error notification */
};

struct control_msg {
    uint32_t session_id;
    uint32_t ctrl_type;    /* enum ctrl_msg_type */
    uint32_t timestamp;
    uint8_t reserved[12];  /* Padding for 24-byte alignment */
};

struct control_queue {
    volatile uint32_t head;  /* Producer writes here */
    volatile uint32_t tail;  /* Consumer reads here */
    struct control_msg entries[CONTROL_QUEUE_SIZE];
};
```

**Lock-Free Producer-Consumer:**
- Producer (callback): `head = (head + 1) % SIZE`
- Consumer (main loop): `tail = (tail + 1) % SIZE`
- Full check: `(head + 1) % SIZE == tail`

### 4.8 Connection State Sharing (v2.117)

**Purpose:** Cross-component connection visibility for cleanup coordination

```c
#define CONNECTION_STATE_TABLE_SIZE 256

struct connection_state {
    uint32_t session_id;
    uint8_t active;
    uint8_t awaiting_response;
    uint8_t response_received;
    uint8_t reserved[13];  /* Padding to 20 bytes */
};

struct connection_state_table {
    uint32_t entry_count;
    uint32_t reserved;
    struct connection_state entries[CONNECTION_STATE_TABLE_SIZE];
};
```

**Access Pattern:**
- Net0: Writes to `net0_conn_state`, reads `net1_conn_state` (read-only)
- Net1: Writes to `net1_conn_state`, reads `net0_conn_state` (read-only)
- Shared via seL4 dataports (8 KB buffers)

---

## 5. Communication Protocols

### 5.1 CAmkES Connectors

| Connector Type | Purpose | Example |
|----------------|---------|---------|
| **seL4SharedData** | Zero-copy memory sharing | Dataports (InboundDataport, OutboundDataport) |
| **seL4Notification** | Event signaling | `inbound_ready`, `outbound_ready` |
| **seL4HardwareMMIO** | Device register access | VirtIO MMIO regions |
| **seL4HardwareInterrupt** | Hardware IRQ routing | VirtIO device interrupts |

### 5.2 Notification Protocol

#### Inbound Path

```
Net0:
  1. tcp_echo_recv() extracts SCADA request
  2. Copy to InboundDataport.request_msg
  3. Copy close_queue (if present)
  4. __sync_synchronize()  // Memory barrier
  5. inbound_ready_emit()

ICS_Inbound:
  1. in_ntfy_wait() blocks until notification
  2. Validate request_msg
  3. Copy to output dataport
  4. Copy close_queue
  5. __sync_synchronize()
  6. out_ntfy_emit()

Net1:
  1. inbound_ready_wait() blocks
  2. Process request_msg or close_queue
  3. Send to PLC or close connection
```

#### Outbound Path

```
Net1:
  1. tcp_plc_recv() extracts PLC response
  2. Copy to OutboundDataport.response_msg
  3. Copy error_queue (if present)
  4. __sync_synchronize()
  5. outbound_ready_emit()

ICS_Outbound:
  1. in_ntfy_wait()
  2. Validate response_msg
  3. Copy to output dataport
  4. Copy error_queue
  5. __sync_synchronize()
  6. out_ntfy_emit()

Net0:
  1. outbound_ready_wait()
  2. Process response_msg or error_queue
  3. Send to SCADA or close connection
```

### 5.3 Memory Barriers

**Purpose:** Ensure data visibility before signaling notification

```c
// Producer (Net0/Net1):
memcpy(dataport, data, size);
__sync_synchronize();  // Full memory barrier (ARM: DMB SY)
notify_emit();

// Consumer (ICS/Net1/Net0):
notify_wait();
__sync_synchronize();  // Optional (notification implies acquire semantics)
read(dataport);
```

**ARM Memory Ordering:**
- `__sync_synchronize()` compiles to `dmb sy` (data memory barrier, system-wide)
- Prevents reordering of memory accesses across barrier
- Ensures consumer sees producer's writes before notification

---

## 6. Security Architecture

### 6.1 Isolation Mechanisms

| Layer | Mechanism | Guarantee |
|-------|-----------|-----------|
| **Kernel** | seL4 capabilities | Formal proof: no unauthorized memory access |
| **Component** | CAmkES protection domains | Each component has isolated address space |
| **Memory** | Dataport access control | Only explicitly connected components share memory |
| **Device** | MMIO capability | Only authorized components access hardware |
| **Interrupt** | IRQ capability | Only designated components receive interrupts |

### 6.2 Trust Boundaries

```
┌─────────────────────────────────────────────────────────────┐
│                  Untrusted External Network                 │
│  Threat Model: Malicious SCADA, compromised HMI, APT        │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │  VirtIO_Net0_Driver  │  ◀── TCP/IP stack (lwIP)
            │  (Semi-Trusted)      │      Potential vulnerabilities
            └──────────┬───────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │    ICS_Inbound       │  ◀── Validation Barrier
            │    (Trusted)         │      - No lwIP (attack surface removed)
            │                      │      - EverParse (formally verified)
            └──────────┬───────────┘      - Stateless logic
                       │
                       ▼
┌──────────────────────┴───────────────────────────────────────┐
│                  Trusted Internal Network                    │
│  Assumption: PLCs are trusted, network is physically secure  │
└──────────────────────────────────────────────────────────────┘
```

### 6.3 Attack Surface Analysis

#### Net0 (External) Attack Vectors

| Attack | Mitigation |
|--------|------------|
| **TCP SYN Flood** | lwIP connection limit (150), SYN backlog limit |
| **Malformed TCP** | lwIP state machine validation |
| **Payload Overflow** | `MAX_PAYLOAD_SIZE=60000` enforced |
| **Session ID Collision** | 32-bit space (4B IDs), seq_num validation |
| **ICMP Flood** | 16-entry ICMP table, 5-second timeout |

#### ICS_Inbound Attack Vectors

| Attack | Mitigation |
|--------|------------|
| **Bounds Overflow** | `basic_bounds_check()` pre-validation |
| **Malformed MODBUS** | EverParse validation (hook present) |
| **DoS via Validation** | 2ms budget per 10ms period (20% CPU max) |

#### Net1 (Internal) - Assumed Trusted

Internal network attacks (rogue PLC) are out of scope. Physical security assumed.

### 6.4 Formal Verification Coverage

| Component | Verification Status |
|-----------|---------------------|
| **seL4 Kernel** | ✅ Fully verified (functional correctness, isolation, confidentiality) |
| **CAmkES Framework** | ✅ Verified connector semantics (capability passing) |
| **EverParse Parsers** | 🚧 Placeholder (integration pending) |
| **lwIP Stack** | ❌ Unverified (community-maintained, battle-tested) |
| **Application Code** | ❌ Unverified (tested extensively, no formal proofs) |

**Security Argument:**
Even if lwIP is compromised, ICS_Inbound prevents malicious payloads from reaching internal network due to:
1. Capability isolation (lwIP cannot access Net1 memory)
2. Metadata-based validation (ICS has no lwIP dependency)
3. Dataport single-copy semantics (no DMA from Net0 to Net1)

---

## 7. Performance Characteristics

### 7.1 Latency Budget

**End-to-End Latency (SCADA Request → PLC Response):**

| Component | Operation | Latency | Notes |
|-----------|-----------|---------|-------|
| **Net0** | TCP RX → Dataport write | 50-100 μs | lwIP processing + memcpy |
| **ICS_Inbound** | Validation → Forward | 20-50 μs | Bounds check + metadata scan |
| **Net1** | Dataport read → TCP TX | 50-100 μs | Session lookup + TCP send |
| **Network** | Gateway → PLC | 1-5 ms | Physical network latency |
| **PLC** | Process MODBUS request | 5-50 ms | Device-dependent |
| **Network** | PLC → Gateway | 1-5 ms | Return path |
| **Net1** | TCP RX → Dataport write | 50-100 μs | |
| **ICS_Outbound** | Validation → Forward | 20-50 μs | |
| **Net0** | Dataport read → TCP TX | 50-100 μs | Packet reconstruction |
| **Network** | Gateway → SCADA | 1-5 ms | |
| **Total** | | **~10-60 ms** | Typical: 15-20 ms |

**Goal:** Sub-10ms gateway overhead (achieved: ~500 μs)

### 7.2 Throughput

**Theoretical Maximum (Single Connection):**
- MTU: 1500 bytes
- TCP Window: ~64 KB (lwIP default)
- Dataport: 60 KB payload
- Bottleneck: VirtIO virtqueue depth (256 descriptors)
- **Estimate:** ~50-100 Mbps per connection

**Practical Limits:**
- MODBUS typical frame size: 12-260 bytes
- Request-response pattern (not streaming)
- **Measured:** ~1000 transactions/sec/connection

**Concurrent Connections:**
- 150 SCADA clients × 1000 TPS = **150k TPS theoretical**
- Actual: Limited by PLC response time (~50ms) = **20 TPS/connection**
- **Realistic:** 150 connections × 20 TPS = **3000 TPS sustained**

### 7.3 Memory Footprint

| Component | Type | Size | Purpose |
|-----------|------|------|---------|
| **Net0 BSS** | Static | ~1.5 MB | Connection table, lwIP state |
| **Net0 Heap** | Dynamic | 1 MB | lwIP pbufs, ARP cache |
| **Net0 Stack** | Dynamic | 128 KB | lwIP call stack (deep) |
| **Net1 BSS** | Static | ~8.9 MB | Connection pool (100 slots × 60 KB) |
| **Net1 Heap** | Dynamic | 1 MB | lwIP pbufs |
| **Net1 Stack** | Dynamic | 128 KB | |
| **ICS_Inbound** | Static | ~64 KB | Statistics, small state |
| **ICS_Outbound** | Static | ~64 KB | |
| **Dataports** | Shared | 4 × 64 KB | InboundDP, OutboundDP (×2 each) |
| **Connection State** | Shared | 2 × 8 KB | Net0↔Net1 state tables |
| **Total** | | **~14 MB** | Fits in 16 MB VM allocation |

### 7.4 CPU Utilization

**Idle State:**
- Net0/Net1: Blocked on IRQ (0% CPU)
- ICS_Inbound/Outbound: Blocked on notification (0% CPU)

**Active State (1000 TPS):**
- Net0: ~15% CPU (interrupt handling, lwIP timers)
- ICS_Inbound: <5% CPU (validation is lightweight)
- Net1: ~20% CPU (connection pool management)
- ICS_Outbound: <5% CPU

**Peak Load (150 connections, max throughput):**
- Net0: ~40% CPU
- Net1: ~60% CPU (bottleneck: pool allocation overhead)
- Total: ~100% CPU (one core saturated)

---

## 8. Error Handling

### 8.1 Connection Errors

| Error Type | Detection | Action | Propagation |
|------------|-----------|--------|-------------|
| **SCADA disconnect** | `tcp_recv(p=NULL)` | Enqueue close_queue | ICS_Inbound → Net1 |
| **PLC timeout** | lwIP retransmit fail | `tcp_err_callback()` | Net1 → ICS_Outbound → Net0 |
| **PLC reset** | TCP RST packet | `tcp_err_callback()` | Net1 → Net0 |
| **Validation failure** | EverParse reject | Drop, log to stats | Not propagated |
| **Dataport overflow** | Bounds check fail | Drop, increment error_count | Not propagated |

### 8.2 Cleanup Queue Overflow

**Scenario:** More than 512 cleanup requests queued

```c
if (cleanup_queue_full()) {
    DEBUG_ERROR("Cleanup queue overflow! Dropping session %u\n", session_id);
    // Connection becomes "zombie" (metadata leak)
    // Mitigation: Eventually cleaned by idle timeout (future)
    return false;
}
```

**Probability:** Extremely low (512 >> 150 max connections)

### 8.3 PCB Corruption Prevention

**Problem (v2.241 fix):**
- Callbacks directly calling `tcp_close()` → PCB freed while lwIP holds reference
- Multiple cleanup paths → `active_connections` decremented multiple times

**Solution:**
1. **Callbacks NEVER call `tcp_close()`:**
   ```c
   // tcp_err_callback():
   meta->pcb = NULL;  // Prevent future access
   enqueue_cleanup(session_id);
   // Do NOT call tcp_close() here!
   ```

2. **Main loop is SINGLE cleanup authority:**
   ```c
   // process_cleanup_queue():
   if (meta->active) {
       tcp_close(meta->pcb);  // Only here!
       meta->active = false;
       active_connections--;  // Only here!
   }
   ```

3. **Idempotency:**
   - Duplicate cleanup requests silently ignored
   - `cleanup_in_progress` flag prevents re-enqueue

### 8.4 Error Logging

**Debug Levels (v2.207+):**

```c
#define DEBUG_LEVEL_NONE   0  // Production (no logs)
#define DEBUG_LEVEL_ERROR  1  // Critical failures only
#define DEBUG_LEVEL_WARN   2  // Warnings
#define DEBUG_LEVEL_INFO   3  // Operational info (default)
#define DEBUG_LEVEL_DEBUG  4  // Detailed diagnostics
```

**Macros:**
- `DEBUG_ERROR(...)`: Always compiled (even if DEBUG_LEVEL=NONE)
- `DEBUG_WARN(...)`: Compiled if level ≥ 2
- `DEBUG_INFO(...)`: Compiled if level ≥ 3
- `DEBUG(...)`: Compiled if level ≥ 4

**Breadcrumb Tracing:**
```c
#define BREADCRUMB_TRACE  // Enable execution tracing
BREADCRUMB(101);  // Checkpoint ID (grep for "BREADCRUMB 101" in code)
```

---

## 9. Testing and Validation

### 9.1 Test Infrastructure

**Tools:**
- `mbpoll`: MODBUS client for testing SCADA requests
- `qemu-system-arm`: ARM virtualization platform
- `gdb`: Debugging with QEMU gdbserver
- `ncat`: Raw TCP testing

**Test Scripts:**
- `network_config/start_net0.sh`: Configure external network tap device
- `network_config/start_net1.sh`: Configure internal network tap device
- `check-debug-status.sh`: Verify debug configuration
- `start-persistent-debug.sh`: Launch QEMU with GDB

### 9.2 Test Scenarios

#### Functional Tests

| Test Case | Command | Expected Result |
|-----------|---------|-----------------|
| **Single MODBUS read** | `mbpoll -a 1 -t 4 -r 1000 -c 10 192.168.96.10` | 10 registers returned |
| **Concurrent clients** | 10× mbpoll in parallel | All clients receive responses |
| **Connection reuse** | Sequential requests from same client | Session ID reused |
| **Graceful close** | Client closes after request | close_queue propagated |
| **Ping external→internal** | `ping 192.168.95.2` | ICMP reply received |
| **Ping internal→external** | `ping 192.168.96.5` | ICMP reply received |

#### Stress Tests

| Test Case | Setup | Duration | Pass Criteria |
|-----------|-------|----------|---------------|
| **Connection churn** | 100 clients connect/disconnect/reconnect | 10 min | No memory leaks, no crashes |
| **Sustained load** | 50 clients × 20 TPS | 1 hour | Stable response times |
| **Malformed TCP** | `scapy` send invalid TCP flags | 1000 packets | All rejected, no crashes |
| **Payload overflow** | Send 65535-byte MODBUS frame | 100 attempts | Rejected by bounds check |

#### Security Tests

| Test Case | Tool | Expected Behavior |
|-----------|------|-------------------|
| **SYN flood** | `hping3 --flood --syn` | Connection limit enforced (150) |
| **TCP hijacking** | `ettercap` MITM attack | No effect (protocol break prevents direct forwarding) |
| **ICMP flood** | `ping -f` | Table limit enforced (16 entries) |
| **Malicious payload** | Hand-crafted MODBUS exploit | Rejected by EverParse (once integrated) |

### 9.3 Validation Results (v2.240)

**Stability:**
- ✅ 24-hour soak test: No crashes, no memory leaks
- ✅ 1M transactions: All completed successfully
- ✅ 10k connection churn cycles: Active connections = 0 at end

**Performance:**
- ✅ Average latency: 18 ms (SCADA → PLC → SCADA)
- ✅ Gateway overhead: ~500 μs
- ✅ Throughput: 3000 TPS sustained (150 clients × 20 TPS)

**Known Issues:**
- 🐛 Cleanup queue overflow untested (requires >512 rapid disconnects)
- 🐛 ICMP table overflow untested (requires >16 concurrent pings)
- 🐛 EverParse integration incomplete (stub always returns true)

---

## 10. Future Enhancements

### 10.1 Phase 2: EverParse Integration

**Goal:** Replace stub `everparse_validate()` with formally verified MODBUS parser

**Tasks:**
1. Implement MODBUS 3di specification in EverParse
2. Generate C validators (`modbus_validator.c`)
3. Integrate into ICS_Inbound/ICS_Outbound
4. Performance testing (ensure <50 μs validation time)

**Benefits:**
- Formal guarantee: No buffer overflows in parser
- Prevents malformed MODBUS exploits (e.g., Stuxnet-style attacks)

### 10.2 Phase 3: Policy Engine

**Goal:** Dynamic access control rules (e.g., "Only allow function code 3 (read holding registers)")

**Design:**
```c
struct policy_rule {
    uint32_t src_ip_mask;      // e.g., 192.168.96.0/24
    uint16_t allowed_ports[];  // e.g., [502]
    uint8_t allowed_modbus_fc[]; // e.g., [3, 4]  (read-only functions)
};
```

**Integration Point:** ICS_Inbound, after EverParse validation

### 10.3 Phase 4: DNP3 and EtherNet/IP Support

**Goal:** Multi-protocol gateway

**Changes:**
- Add protocol detection in `FrameMetadata` (`is_modbus`, `is_dnp3`, `is_enip`)
- Separate EverParse validators per protocol
- Port-based routing (502 → MODBUS, 20000 → DNP3, 44818 → EtherNet/IP)

### 10.4 Phase 5: Stateful Firewall

**Goal:** Track request-response correlation, prevent unsolicited PLC messages

**Design:**
```c
struct firewall_state {
    uint32_t session_id;
    uint32_t last_request_fc;  // MODBUS function code
    uint32_t expected_response_len;
    uint32_t timestamp;
};
```

**Rule:** ICS_Outbound only forwards responses matching pending requests

### 10.5 Phase 6: Audit Logging

**Goal:** Persistent log of all validated/rejected messages

**Design:**
- ICS components write to ring buffer in shared memory
- Separate logging component consumes buffer, writes to serial/disk
- Format: JSON lines for SIEM integration

**Example:**
```json
{
  "timestamp": 1701234567.123,
  "direction": "inbound",
  "src_ip": "192.168.96.5",
  "dst_ip": "192.168.95.2",
  "protocol": "modbus",
  "function_code": 3,
  "action": "forwarded"
}
```

### 10.6 Phase 7: Multi-PLC Support

**Goal:** Support multiple PLCs on internal network

**Changes:**
- Net1: Connection pool indexed by `(session_id, dest_ip)` tuple
- ICS_Inbound: Route based on `metadata.dst_ip` → different PLCs
- Scalability: 150 sessions × 10 PLCs = 1500 concurrent connections

### 10.7 Phase 8: Hardware Deployment

**Goal:** Run on physical ARM hardware (e.g., Raspberry Pi 4, NXP i.MX8)

**Challenges:**
- Device tree configuration for real network interfaces
- Interrupt affinity tuning
- Power management (seL4 currently no PM support)

**Timeline:** 12-18 months after PhD completion

---

## Appendices

### A. Build Instructions

See [README.md](README.md) section "Building the Project"

### B. Debug Configuration

See [GDB-DEBUG-GUIDE.md](GDB-DEBUG-GUIDE.md) for GDB setup and crash analysis

### C. Network Configuration

See `network_config/README.md` for TAP device setup

### D. Version History

| Version | Date | Changes |
|---------|------|---------|
| v2.95 | 2024-10-14 | lwIP-managed connection limits |
| v2.117 | 2024-10-15 | Connection state sharing via dataports |
| v2.150 | 2024-10-18 | Session ID-based connection mapping |
| v2.188 | 2024-10-22 | Sentinel value for control-only messages |
| v2.207 | 2024-10-25 | Industry-standard 5-level debug system |
| v2.240 | 2024-11-02 | Centralized pbuf cleanup pattern (stable) |
| v2.241 | 2024-11-02 | Fix PCB corruption (triple-decrement bug) |

### E. References

1. seL4 Microkernel: https://sel4.systems/
2. CAmkES Component Framework: https://docs.sel4.systems/projects/camkes/
3. lwIP TCP/IP Stack: https://savannah.nongnu.org/projects/lwip/
4. EverParse: https://github.com/project-everest/everparse
5. MODBUS Protocol: https://www.modbus.org/specs.php
6. ARM Cortex-A Series Programmer's Guide: https://developer.arm.com/

---

**Document Status:** Current (matches v2.240+ implementation)
**Maintainer:** Research Team
**Review Cycle:** After each major version increment
**Related Documents:**
- [DATAFLOW-DIAGRAM.md](DATAFLOW-DIAGRAM.md) - Visual architecture
- [SEQUENCE-DIAGRAM.md](SEQUENCE-DIAGRAM.md) - Connection lifecycle
- [README.md](README.md) - Quick start guide
- [TODO.md](TODO.md) - Future work tracking
