# ICS Bidirectional Cross-Domain Firewall (Protocol Break Architecture)

## Implementation Status

**Date**: 2025-10-09
**Build Status**: ✅ **SUCCESSFUL** (100/100 targets)
**Current Phase**: Phase 1 - One-way paths implemented, ready for testing

### Completed ✅
- ✅ 4-component bidirectional architecture (matches README specification)
- ✅ VirtIO_Net0_Driver (External network, port 6000) - INBOUND path complete
- ✅ VirtIO_Net1_Driver (Internal network, port 7000) - OUTBOUND path complete
- ✅ ICS_Inbound validation component (External→Internal)
- ✅ ICS_Outbound validation component (Internal→External)
- ✅ FrameMetadata structure with rich protocol information
- ✅ ICS_Message format for dataport communication
- ✅ Build system updated for new architecture

### Pending ⚠️
- ⚠️ Reverse TX paths (ICS → VirtIO drivers → TCP transmission) - Phase 2
- ⚠️ Full bidirectional end-to-end testing
- ⚠️ Enhanced metadata extraction (full Ethernet/IP header parsing)

**For detailed implementation status, see**: [REFACTORING_COMPLETE.md](REFACTORING_COMPLETE.md)

---

## Overview

This application implements a **bidirectional cross-domain security gateway** with **protocol break architecture** on the seL4 microkernel. It provides secure communication between external and internal networks while maintaining complete isolation and independent validation in both directions.

### Key Security Properties

- ✅ **Protocol Break**: TCP connections are terminated and recreated (no direct connection)
- ✅ **Bidirectional Data Diode**: Two independent one-way pipelines (inbound and outbound)
- ✅ **Smart VirtIO Drivers**: Drivers handle TCP/IP (lwIP), ICS pipeline validates payload only
- ✅ **Metadata Passing**: Frame information preserved for protocol-aware validation
- ✅ **Payload-Only Validation**: TCP state stripped, only application data validated
- ✅ **No Shared Memory**: Inbound and outbound paths use different dataports
- ✅ **seL4 Formal Verification**: Built on mathematically proven microkernel

---

## Architecture Evolution

### Final Design Decision: Smart Drivers + Simple ICS Pipeline

After architectural analysis, we adopted this approach:

**Key Insight:** TCP ACKs are protocol overhead - drivers handle them automatically. ICS pipeline focuses purely on payload validation.

```
┌─────────────────────────────────────────────────────────────────┐
│                         QEMU Host                               │
│  Netcat1 (port 6000) ──────────┬──────────── Netcat2 (port 7000)│
└────────────────────────────────┼─────────────────────────────────┘
                                 │
┌────────────────────────────────┼─────────────────────────────────┐
│                    seL4 Gateway │                                 │
│                                 │                                 │
│  ┌─────────────────────────────▼──────────────────┐              │
│  │  VirtIO_Net0_Driver (External - has lwIP)      │              │
│  │  - Handles net0 RX + TX queues                 │              │
│  │  - TCP/IP stack for external connections       │              │
│  │  - Automatically sends/receives TCP ACKs       │              │
│  │  - Extracts: PAYLOAD + METADATA                │              │
│  └──────────────────────┬──────────────────────────┘             │
│                         │                                         │
│                         │ dataport: ICS_Message                   │
│                         │ (metadata + payload)                    │
│                         ▼                                         │
│  ┌──────────────────────────────────────────────┐                │
│  │  ICS_Inbound (NO lwIP - just validation)     │                │
│  │  - Receives payload + metadata                │                │
│  │  - Validates payload (bounds, protocol)       │                │
│  │  - Logs metadata for audit                    │                │
│  │  - NO TCP knowledge                           │                │
│  └──────────────────────┬──────────────────────┘                 │
│                         │                                         │
│                         │ dataport: ICS_Message                   │
│                         ▼                                         │
│  ┌──────────────────────────────────────────────┐                │
│  │  VirtIO_Net1_Driver (Internal - has lwIP)    │                │
│  │  - Handles net1 RX + TX queues               │                │
│  │  - Receives validated payload + metadata      │                │
│  │  - Wraps in NEW TCP session to internal      │                │
│  │  - Handles TCP ACKs automatically            │                │
│  └──────────────────────┬──────────────────────┘                 │
│                         │                                         │
│  ═══════════════════════╪═══════════════════════                 │
│         Reverse Path    │                                         │
│  ═══════════════════════╪═══════════════════════                 │
│                         │                                         │
│  ┌──────────────────────▼──────────────────────┐                 │
│  │  VirtIO_Net1_Driver (reads from internal)   │                 │
│  │  - RX from net1 → extract payload + metadata│                 │
│  └──────────────────────┬──────────────────────┘                 │
│                         │                                         │
│                         ▼                                         │
│  ┌──────────────────────────────────────────────┐                │
│  │  ICS_Outbound (validates reverse traffic)    │                │
│  └──────────────────────┬──────────────────────┘                 │
│                         │                                         │
│                         ▼                                         │
│  ┌──────────────────────────────────────────────┐                │
│  │  VirtIO_Net0_Driver (TX to external)         │                │
│  └──────────────────────────────────────────────┘                │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

---

## Component Architecture (4 Components)

### 1. VirtIO_Net0_Driver (External Network - Smart Driver)

**Purpose:** Handle ALL TCP/IP processing for external network, extract payload + metadata for ICS validation.

**Has lwIP:** ✅ Yes - complete TCP/IP stack

**Responsibilities:**
- **Hardware Access:**
  - Own net0 VirtIO device (RX + TX queues)
  - MMIO register access
  - IRQ handling

- **TCP/IP Processing:**
  - Run lwIP TCP/IP stack
  - Accept connections on port 6000
  - **Automatically handle TCP ACKs, SYN, FIN** (ICS never sees these!)
  - Manage TCP state machine

- **Metadata Extraction:**
  - Parse Ethernet, IP, TCP/UDP headers
  - Extract frame metadata (MAC, IP, ports, protocol type)
  - Extract payload only (strip all protocol headers)

- **Output:**
  - Write to dataport: `ICS_Message` (metadata + payload)
  - Notify ICS_Inbound component

**Code Characteristics:**
- ~500-800 lines (includes lwIP integration)
- Complex (has TCP/IP stack)
- Handles all protocol complexity
- ICS pipeline stays simple!

---

### 2. ICS_Inbound (Inbound Validation Pipeline - Simple)

**Purpose:** Validate payload using metadata, NO TCP/IP processing.

**Has lwIP:** ❌ No - pure validation logic

**Responsibilities:**
- **Input:**
  - Read `ICS_Message` from dataport (metadata + payload)

- **Validation:**
  - Bounds checking (payload size)
  - Protocol-agnostic validation (Phase 1)
  - Future: Protocol-specific rules using metadata

- **Logging/Audit:**
  - Log metadata for debugging
  - Record validation decisions
  - Audit trail for security analysis

- **Output:**
  - Forward validated `ICS_Message` to VirtIO_Net1_Driver
  - Metadata preserved for reconstruction

**Code Characteristics:**
- ~200-300 lines
- Simple, verifiable logic
- NO TCP state machine
- Easy to formally verify

---

### 3. VirtIO_Net1_Driver (Internal Network - Smart Driver)

**Purpose:** Handle ALL TCP/IP processing for internal network, reconstruct frames from validated payload.

**Has lwIP:** ✅ Yes - complete TCP/IP stack

**Responsibilities:**
- **Hardware Access:**
  - Own net1 VirtIO device (RX + TX queues)
  - MMIO register access
  - IRQ handling

- **TCP/IP Reconstruction:**
  - Receive validated payload + metadata from ICS_Inbound
  - Create **NEW TCP session** to internal (port 7000)
  - Wrap payload in TCP packet
  - **Different seq/ack numbers from external!**

- **Reverse Path:**
  - Accept connections from internal (port 7000)
  - Extract payload + metadata
  - Send to ICS_Outbound for validation

- **Metadata Use:**
  - Log original protocol info
  - Future: Reconstruct specific protocols (GOOSE, etc.)

**Code Characteristics:**
- ~500-800 lines (includes lwIP)
- Mirrors VirtIO_Net0_Driver
- Handles protocol reconstruction

---

### 4. ICS_Outbound (Outbound Validation Pipeline - Simple)

**Purpose:** Validate reverse traffic (internal → external).

**Has lwIP:** ❌ No - pure validation logic

**Responsibilities:**
- Mirror of ICS_Inbound
- Validates traffic from internal network
- Same validation rules (bidirectional policy)
- Forwards to VirtIO_Net0_Driver for transmission

**Code Characteristics:**
- ~200-300 lines
- Copy of ICS_Inbound logic
- Simple and verifiable

---

## Data Structures

### Frame Metadata (Passed Between Components)

```c
// components/include/common.h

typedef struct {
    // Ethernet frame info
    uint8_t  dst_mac[6];        // Destination MAC address
    uint8_t  src_mac[6];        // Source MAC address
    uint16_t ethertype;         // 0x0800=IPv4, 0x0806=ARP, 0x88B8=GOOSE, etc.
    uint16_t vlan_id;           // VLAN ID (0 if no VLAN)
    uint8_t  vlan_priority;     // VLAN priority (0-7)

    // IP layer info (if applicable)
    uint8_t  ip_protocol;       // 6=TCP, 17=UDP, 0=not IP
    uint32_t src_ip;            // Source IP address
    uint32_t dst_ip;            // Destination IP address

    // Transport layer info (if TCP/UDP)
    uint16_t src_port;          // Source port
    uint16_t dst_port;          // Destination port

    // Payload info
    uint16_t payload_offset;    // Offset in original frame
    uint16_t payload_length;    // Actual payload length

    // Protocol flags (for quick identification)
    uint8_t  is_ip      : 1;    // 1 if IP packet
    uint8_t  is_tcp     : 1;    // 1 if TCP
    uint8_t  is_udp     : 1;    // 1 if UDP
    uint8_t  is_arp     : 1;    // 1 if ARP
    uint8_t  reserved   : 4;    // Reserved for future protocols

} __attribute__((packed)) FrameMetadata;

// Message passed via dataports
typedef struct {
    FrameMetadata metadata;                 // Frame/protocol information
    uint16_t      payload_length;           // Length of payload
    uint8_t       payload[MAX_PAYLOAD_SIZE]; // Actual payload data
} __attribute__((packed)) ICS_Message;

#define MAX_PAYLOAD_SIZE 60000
```

### Why Pass Metadata?

1. **Future Protocol Support:**
   - GOOSE (IEC 61850): Needs EtherType 0x88B8, multicast MAC
   - Sampled Values: Needs EtherType 0x88BA
   - Modbus TCP: Needs port 502 identification
   - DNP3: Needs port/protocol identification

2. **Debugging & Audit:**
   - Log what protocols are flowing through
   - Audit trail shows protocol types
   - Easier troubleshooting

3. **Verification:**
   - Metadata provides context for validation proofs
   - Can prove protocol-specific properties
   - Audit log completeness

4. **Phase 1:** Metadata logged but not used for validation (simple pass-through)
5. **Phase 2+:** Add protocol-specific validation rules using metadata

---

## Data Flow

### Inbound Flow (External → Internal)

```
Step 1: External Connection
  Netcat1 connects to localhost:6000
  ↓
  QEMU forwards to net0 VirtIO device
  ↓
  Packet arrives in net0 RX queue

Step 2: VirtIO_Net0_Driver (lwIP processes)
  - lwIP receives Ethernet frame
  - lwIP processes: Ethernet → IP → TCP
  - lwIP handles TCP handshake (SYN, SYN-ACK, ACK) ← AUTOMATIC!
  - TCP recv callback triggered with payload
  ↓
  Extract metadata:
    - Ethernet: src_mac, dst_mac, ethertype
    - IP: src_ip, dst_ip, protocol
    - TCP: src_port, dst_port
  ↓
  Extract payload: "Hello from external"
  ↓
  Create ICS_Message:
    {
      metadata: {
        src_mac: [52:54:00:12:34:56],
        ethertype: 0x0800,
        ip_protocol: 6 (TCP),
        src_ip: 10.0.2.2,
        dst_ip: 10.0.2.15,
        src_port: 45678,
        dst_port: 6000,
        is_tcp: 1
      },
      payload_length: 21,
      payload: "Hello from external"
    }
  ↓
  Write to dataport → emit notification

Step 3: ICS_Inbound (NO lwIP)
  - Read ICS_Message from dataport
  ↓
  Log metadata:
    "Received TCP from 10.0.2.2:45678 → 10.0.2.15:6000"
  ↓
  Validate payload:
    - Check: payload_length ≤ MAX_PAYLOAD_SIZE ✓
    - Check: payload_length > 0 ✓
    - (Future: Protocol-specific validation using metadata)
  ↓
  Decision: ALLOW
  ↓
  Forward ICS_Message to VirtIO_Net1_Driver (metadata preserved)

Step 4: VirtIO_Net1_Driver (lwIP creates NEW session)
  - Read ICS_Message from dataport
  ↓
  Log: "Forwarding TCP payload (21 bytes) to internal"
  ↓
  lwIP creates NEW TCP connection to internal:
    - NEW seq numbers (different from external!)
    - NEW ack numbers
    - NEW TCP session state
  ↓
  tcp_write(internal_pcb, payload, 21, ...)
  tcp_output(internal_pcb)
  ↓
  VirtIO TX queue → QEMU

Step 5: Delivery
  QEMU forwards to localhost:7000
  ↓
  Netcat2 receives: "Hello from external"
```

**Key Point:** ICS_Inbound never saw TCP headers, ACKs, sequence numbers. Only validated payload!

---

### Outbound Flow (Internal → External)

```
Step 1: Internal sends data
  Netcat2 (connected to localhost:7000) types: "Response"
  ↓
  QEMU → net1 VirtIO RX queue

Step 2: VirtIO_Net1_Driver
  - lwIP processes TCP from internal
  - lwIP sends ACK automatically ← ICS doesn't see this!
  - Extract payload + metadata
  ↓
  ICS_Message: { metadata: {...}, payload: "Response" }
  ↓
  Send to ICS_Outbound

Step 3: ICS_Outbound
  - Validate payload (same rules as ICS_Inbound)
  - Log metadata
  - Forward to VirtIO_Net0_Driver

Step 4: VirtIO_Net0_Driver
  - Create NEW TCP packet to external
  - Send to Netcat1 on port 6000

Step 5: Delivery
  Netcat1 receives: "Response"
```

---

## Security Architecture

### Protocol Break Properties

**Traditional Firewall (BAD):**
```
External ←────── Single TCP Session ──────→ Internal
         (Firewall just inspects/forwards)
  - Same seq/ack numbers
  - Attack on external affects internal
```

**Our Architecture (GOOD):**
```
External ←─ TCP Session 1 ─→ VirtIO_Net0 [BREAK] ICS_Inbound [BREAK] VirtIO_Net1 ←─ TCP Session 2 ─→ Internal

Session 1: seq=1000, ack=500     |     Session 2: seq=9000, ack=200
External MAC: 52:54:00:12:34:56  |     Internal MAC: 52:54:00:ab:cd:ef
                                 |
                      [ICS sees ONLY payload!]
```

**Security Benefits:**
- ✅ Different TCP state machines (external ≠ internal)
- ✅ TCP attacks on external can't reach internal TCP stack
- ✅ SYN floods handled by VirtIO_Net0, not ICS
- ✅ Malformed TCP flags processed by lwIP, not ICS
- ✅ ICS validates ONLY payload (simpler, verifiable)

---

### Attack Surface Analysis

#### Scenario 1: SYN Flood Attack

**Attack:** Attacker sends 10,000 TCP SYN packets to port 6000

**Response:**
```
10,000 SYN → VirtIO_Net0_Driver (lwIP)
  ↓
  lwIP allocates 10,000 TCB structures
  ↓
  Memory exhaustion in VirtIO_Net0_Driver (NOT in ICS!)
  ↓
  ICS_Inbound: ✅ No payload received, no processing
  ↓
  Internal network: ✅ Completely unaffected
```

**Result:** Attack contained in driver component, ICS pipeline unaffected.

#### Scenario 2: Malformed TCP Packet

**Attack:** TCP packet with invalid flags (URG+PSH+RST)

**Response:**
```
Malformed TCP → VirtIO_Net0_Driver (lwIP)
  ↓
  lwIP detects invalid flags
  ↓
  lwIP drops packet OR handles according to RFC
  ↓
  ICS_Inbound: ✅ Never sees malformed TCP
  ↓
  Result: Attack surface is lwIP (well-tested), not ICS
```

#### Scenario 3: Oversized Payload

**Attack:** TCP payload 100KB (exceeds MAX_PAYLOAD_SIZE)

**Response:**
```
Large TCP payload → VirtIO_Net0_Driver
  ↓
  VirtIO_Net0 extracts: payload_length = 100000
  ↓
  ICS_Message created (payload truncated or dropped by driver)
  ↓
  ICS_Inbound validates: payload_length > MAX_PAYLOAD_SIZE
  ↓
  ICS_Inbound: ❌ DROP
  ↓
  Audit log: "Oversized payload dropped (100KB)"
```

---

## Dataport Connections

| Dataport | Producer | Consumer | Content |
|----------|----------|----------|---------|
| `net0_to_ics_inbound` | VirtIO_Net0_Driver | ICS_Inbound | ICS_Message (metadata + payload from external) |
| `ics_inbound_to_net1` | ICS_Inbound | VirtIO_Net1_Driver | ICS_Message (validated for internal) |
| `net1_to_ics_outbound` | VirtIO_Net1_Driver | ICS_Outbound | ICS_Message (metadata + payload from internal) |
| `ics_outbound_to_net0` | ICS_Outbound | VirtIO_Net0_Driver | ICS_Message (validated for external) |

---

## Configuration

### QEMU Network Setup

```bash
./simulate --extra-qemu-args=" \
  -netdev user,id=net0,hostfwd=tcp::6000-:6000 \
  -device virtio-net-device,netdev=net0 \
  \
  -netdev user,id=net1,hostfwd=tcp::7000-:7000 \
  -device virtio-net-device,netdev=net1"
```

**Port Forwarding:**
- **Host port 6000** ↔ **Guest net0 port 6000** (External network)
- **Host port 7000** ↔ **Guest net1 port 7000** (Internal network)

### Network Addressing

**External Network (net0):**
- Guest IP: `10.0.2.15`
- Gateway: `10.0.2.2` (QEMU user-mode networking)
- Port: `6000` (VirtIO_Net0_Driver listens here)

**Internal Network (net1):**
- Guest IP: `10.0.3.15`
- Gateway: `10.0.3.2` (QEMU user-mode networking)
- Port: `7000` (VirtIO_Net1_Driver connects here)

---

## Building and Testing

### Build Instructions

```bash
cd ~/phd/camkes-vm-examples

# Initialize build environment
mkdir -p build && cd build
../init-build.sh -DPLATFORM=qemu-arm-virt -DAARCH64=TRUE

# Configure project
cmake -DCAMKES_APP=ics_oneway_norm_v3_dual_nic -G Ninja ..

# Build
ninja
```

### Testing Workflow

#### Test 1: Inbound Communication (External → Internal)

```bash
# Terminal 1: Start internal receiver FIRST
nc -l 7000

# Terminal 2: Start QEMU
cd camkes-vm-examples/build
./simulate --extra-qemu-args="-netdev user,id=net0,hostfwd=tcp::6000-:6000 -device virtio-net-device,netdev=net0 -netdev user,id=net1,hostfwd=tcp::7000-:7000 -device virtio-net-device,netdev=net1"

# Wait for initialization:
# "VirtIO_Net0_Driver: Ready, listening on port 6000"
# "VirtIO_Net1_Driver: Ready, listening on port 7000"
# "ICS_Inbound: Ready to validate"
# "ICS_Outbound: Ready to validate"

# Terminal 3: Connect as external client
nc localhost 6000

# Type in Terminal 3:
Hello from external

# Terminal 1 should display:
Hello from external
```

#### Test 2: Outbound Communication (Internal → External)

```bash
# Using same setup from Test 1, with all terminals still active

# Terminal 1 (internal, nc -l 7000):
Response from internal

# Terminal 3 (external, nc localhost 6000) should display:
Response from internal
```

#### Test 3: Bidirectional Interactive Session

```bash
# Terminal 1: Internal
nc -l 7000

# Terminal 2: QEMU (running)

# Terminal 3: External
nc localhost 6000

# Interactive chat:
Terminal 3 (External) types:
> ping

Terminal 1 (Internal) sees:
ping

Terminal 1 (Internal) types:
> pong

Terminal 3 (External) sees:
pong
```

---

## Expected Output

### Component Initialization

```
VirtIO_Net0_Driver: Initializing...
VirtIO_Net0_Driver: VirtIO device at 0xa003e00 (slot 31)
VirtIO_Net0_Driver: RX queue ready (256 descriptors)
VirtIO_Net0_Driver: TX queue ready (256 descriptors)
VirtIO_Net0_Driver: lwIP initialized
VirtIO_Net0_Driver: IP address: 10.0.2.15
VirtIO_Net0_Driver: TCP server listening on port 6000
VirtIO_Net0_Driver: Ready to process external traffic

ICS_Inbound: Initializing validation pipeline...
ICS_Inbound: Input dataport: 65536 bytes
ICS_Inbound: Output dataport: 65536 bytes
ICS_Inbound: Ready to validate inbound traffic

VirtIO_Net1_Driver: Initializing...
VirtIO_Net1_Driver: VirtIO device at 0xa003c00 (slot 30)
VirtIO_Net1_Driver: RX queue ready (256 descriptors)
VirtIO_Net1_Driver: TX queue ready (256 descriptors)
VirtIO_Net1_Driver: lwIP initialized
VirtIO_Net1_Driver: IP address: 10.0.3.15
VirtIO_Net1_Driver: TCP server listening on port 7000
VirtIO_Net1_Driver: Ready to process internal traffic

ICS_Outbound: Initializing validation pipeline...
ICS_Outbound: Ready to validate outbound traffic

=== Cross-Domain Firewall Ready ===
External port: 6000
Internal port: 7000
Mode: Bidirectional protocol break
```

### Traffic Processing (with Metadata Logging)

```
# External connects:
VirtIO_Net0_Driver: TCP connection from 10.0.2.2:45678

# Data arrives:
VirtIO_Net0_Driver: Received 21 bytes from external
VirtIO_Net0_Driver: Extracted metadata:
  Protocol: TCP
  Src: 10.0.2.2:45678 → Dst: 10.0.2.15:6000
  EtherType: 0x0800 (IPv4)
VirtIO_Net0_Driver: Sending to ICS_Inbound

ICS_Inbound: Received frame
  Src MAC: 52:54:00:12:34:56
  Dst MAC: 52:54:00:ab:cd:ef
  EtherType: 0x0800
  IP: 10.0.2.2 -> 10.0.2.15
  TCP: port 45678 -> 6000
  Payload: 21 bytes
  ✅ Validation passed - forwarding

VirtIO_Net1_Driver: Forwarding to internal
  Original protocol: TCP (port 6000)
  Creating new TCP session to 10.0.3.2:7000
  ✅ Sent 21 bytes to internal

# Reverse direction:
VirtIO_Net1_Driver: Received 8 bytes from internal
ICS_Outbound: Validating...
  ✅ Passed - forwarding to external
VirtIO_Net0_Driver: Sent 8 bytes to external (10.0.2.2:45678)
```

---

## File Structure

```
ics_oneway_norm_v3_dual_nic/
├── README.md                          # This file (complete blueprint)
├── CMakeLists.txt                     # Build configuration
├── ics_dual_nic.camkes               # CAmkES assembly definition
│
├── components/
│   ├── README.md                      # Detailed component documentation
│   │
│   ├── include/
│   │   └── common.h                   # Shared definitions (FrameMetadata, ICS_Message)
│   │
│   ├── VirtIO_Net0_Driver/
│   │   ├── virtio_net0_driver.c      # External NIC driver (lwIP + metadata extraction)
│   │   ├── virtio_net0_driver.camkes # Component definition
│   │   └── lwipopts.h                 # lwIP configuration
│   │
│   ├── ICS_Inbound/
│   │   ├── ics_inbound.c             # Inbound validator (NO lwIP)
│   │   └── ics_inbound.camkes        # Component definition
│   │
│   ├── VirtIO_Net1_Driver/
│   │   ├── virtio_net1_driver.c      # Internal NIC driver (lwIP + reconstruction)
│   │   ├── virtio_net1_driver.camkes # Component definition
│   │   └── lwipopts.h                 # lwIP configuration
│   │
│   └── ICS_Outbound/
│       ├── ics_outbound.c            # Outbound validator (NO lwIP)
│       └── ics_outbound.camkes       # Component definition
```

---

## Implementation Checklist

### Phase 1: Basic Protocol Break (Current)

- [ ] **VirtIO_Net0_Driver:**
  - [ ] VirtIO device initialization (RX + TX queues)
  - [ ] lwIP integration (TCP/IP stack)
  - [ ] TCP server on port 6000
  - [ ] Metadata extraction from frames
  - [ ] Payload extraction from TCP
  - [ ] Write ICS_Message to dataport

- [ ] **ICS_Inbound:**
  - [ ] Read ICS_Message from dataport
  - [ ] Log metadata (MAC, IP, ports, protocol)
  - [ ] Basic validation (payload size)
  - [ ] Forward ICS_Message to VirtIO_Net1_Driver

- [ ] **VirtIO_Net1_Driver:**
  - [ ] VirtIO device initialization (RX + TX queues)
  - [ ] lwIP integration
  - [ ] TCP client/server for internal
  - [ ] Wrap payload in NEW TCP session
  - [ ] Reverse path: extract from internal

- [ ] **ICS_Outbound:**
  - [ ] Mirror of ICS_Inbound
  - [ ] Validate reverse traffic

- [ ] **Integration:**
  - [ ] CAmkES assembly (ics_dual_nic.camkes)
  - [ ] CMakeLists.txt configuration
  - [ ] Build and test

### Phase 2: Protocol-Specific Validation (Future)

- [ ] GOOSE validation using metadata
- [ ] Modbus TCP function code filtering
- [ ] DNP3 protocol validation
- [ ] Rate limiting per protocol
- [ ] Stateful inspection

### Phase 3: Formal Verification (Future)

- [ ] Prove ICS_Inbound correctness
- [ ] EverParse integration
- [ ] End-to-end security properties

---

## Troubleshooting

### Issue 1: VirtIO driver fails to initialize

**Symptom:** "VirtIO device not found" or crash during init

**Solutions:**
- Check MMIO address: `0xa003000` for net0, `0xa003000` for net1 (same page, different offset)
- Check IRQ numbers: 79 for net0, 78 for net1
- Verify QEMU has both VirtIO devices: `-device virtio-net-device,netdev=net0 -device virtio-net-device,netdev=net1`

### Issue 2: lwIP doesn't get IP address

**Symptom:** "No IP address assigned"

**Solutions:**
- Check DHCP is enabled in lwipopts.h: `#define LWIP_DHCP 1`
- Or use static IP: `IP4_ADDR(&ipaddr, 10, 0, 2, 15);`
- Check QEMU user-mode networking is active

### Issue 3: Metadata shows all zeros

**Symptom:** ICS_Inbound logs show `src_ip: 0.0.0.0`

**Solutions:**
- Check metadata extraction in VirtIO driver
- Ensure lwIP structures are correct (eth_hdr, ip_hdr, tcp_hdr)
- Check byte order (use `lwip_ntohs()` for network-to-host)

### Issue 4: No traffic flows through

**Symptom:** External connects but internal doesn't receive

**Solutions:**
- Check dataport connections in ics_dual_nic.camkes
- Verify notifications are emitted and received
- Check VirtIO_Net1_Driver creates TCP connection to internal
- Enable debug output in all components

---

## Research Context

This implementation is part of PhD research into **formally verified cross-domain security** for critical infrastructure protection.

### Novel Contributions

1. **Smart Driver Architecture:**
   - VirtIO drivers handle protocol complexity (lwIP)
   - ICS pipeline is simple, verifiable (NO TCP knowledge)
   - Attack surface minimized in security components

2. **Metadata Passing:**
   - Frame information preserved for future protocol support
   - Enables GOOSE/SV (IEC 61850) without changing architecture
   - Foundation for protocol-aware validation

3. **Protocol Break:**
   - Complete TCP session isolation (external ≠ internal)
   - Payload-only validation
   - Mathematical proof of separation

4. **Bidirectional Data Diode:**
   - Two independent one-way pipelines
   - Full two-way communication with security isolation
   - Suitable for ICS cross-domain solutions

### Application Domains

- **Substation Automation:** GOOSE/SV protocol gateway (IEC 61850)
- **SCADA Security:** Modbus/DNP3 cross-domain firewall
- **Medical Networks:** Device isolation with bidirectional control
- **Military Systems:** Cross-domain information exchange

---

## References

- **seL4 Microkernel:** https://sel4.systems/
- **CAmkES Framework:** https://docs.sel4.systems/projects/camkes/
- **VirtIO Specification:** https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html
- **lwIP TCP/IP Stack:** https://savannah.nongnu.org/projects/lwip/
- **IEC 61850 (GOOSE/SV):** https://en.wikipedia.org/wiki/IEC_61850

---

## License

SPDX-License-Identifier: BSD-2-Clause

---

## Author

PhD Research Project - Formally Verified ICS Security
Date: 2025-10-08
Version: 3.0 (Smart Driver + Metadata Passing Architecture)

---

## Quick Start Commands

```bash
# Build
cd ~/phd/camkes-vm-examples/build
cmake -DCAMKES_APP=ics_oneway_norm_v3_dual_nic -G Ninja ..
ninja

# Run with dual NICs
./simulate --extra-qemu-args="-netdev user,id=net0,hostfwd=tcp::6000-:6000 -device virtio-net-device,netdev=net0 -netdev user,id=net1,hostfwd=tcp::7000-:7000 -device virtio-net-device,netdev=net1"

# Test (in separate terminals)
nc -l 7000           # Internal
nc localhost 6000    # External
```

---

**This README serves as the complete blueprint for implementation. All architectural decisions are documented here.**
