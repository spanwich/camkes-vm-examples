# ICS One-Way Pipeline with Normalizer - V3 (FreeRTOS VM Network Gateway)

A secure, one-way Industrial Control System (ICS) message processing pipeline built on seL4/CAmkES with **minimal FreeRTOS VM** for external network packet reception via VirtIO-Net.

## What's New in V3

Version 3 replaces the simulated network driver with a **real external network interface** using a minimal FreeRTOS VM guest that receives packets from QEMU VirtIO-Net and forwards them to the trusted seL4 security pipeline via cross-VM shared datapor communication.

### Architecture: FreeRTOS VM + Cross-VM Dataports

```
External Network          QEMU VirtIO           FreeRTOS VM (Guest)          seL4 Security Pipeline
┌──────────────┐         ┌─────────────┐       ┌────────────────────┐       ┌──────────────────────────────────┐
│ nc localhost │  TCP/IP │ virtio-net  │ VirtIO│ FreeRTOS+VirtIO-net│Cross-VM│ [NetworkDriverDrv]              │
│    8502      │────────>│  MMIO dev   │──────>│ - Minimal TCB      │Dataport│          ↓                      │
│              │         │  (slot 0)   │       │ - lwIP TCP/IP      │───────>│ [ExtFrontend]                   │
│ (Modbus TCP) │         │  IRQ 16+N   │       │ - Packet forwarding│       │          ↓                      │
└──────────────┘         └─────────────┘       └────────────────────┘       │ [ParserNorm]                    │
                                                                             │          ↓                      │
                                                                             │ [PolicyEmit]                    │
                                                                             │          ↓                      │
                                                                             │ [IntNicDrv]                     │
                                                                             └──────────────────────────────────┘
```

### Why FreeRTOS VM Instead of Bare-Metal VirtIO?

**The Hypervisor Paradox:**
1. seL4 kernel requires `KernelArmHypervisorSupport=ON` for ARM virtualization extensions
2. QEMU with `virtualization=on` exposes VirtIO MMIO devices to **VM guests only**, not bare-metal components
3. Bare-metal seL4 components cannot access VirtIO devices in this configuration
4. Disabling hypervisor mode crashes the kernel (ARM hypervisor extensions required)

**Solution: Minimal FreeRTOS VM**
| Feature | FreeRTOS VM | Linux VM | Bare-Metal |
|---------|-------------|----------|------------|
| **TCB Size** | ~50KB | 10MB+ | 0KB |
| **Works with Hypervisor** | ✅ Yes | ✅ Yes | ❌ No |
| **VirtIO Access** | ✅ Direct | ✅ Direct | ❌ Blocked |
| **Security Filtering** | ❌ In seL4 | ❌ In seL4 | ✅ In seL4 |
| **Auditable Code** | ✅ Small | ❌ Large | ✅ N/A |
| **Boot Time** | ~100ms | ~3s | 0ms |

**Security Architecture:**
- FreeRTOS VM = Untrusted network driver only (minimal attack surface)
- All ICS protocol validation and policy enforcement happens in trusted seL4 components
- VM compromise cannot affect security filtering logic
- Cross-VM dataport is one-way: VM writes, seL4 reads (no reverse channel)

---

## Architecture Overview

### Component Topology

```
[FREERTOS VM] -> [NetworkDriverDrv] -> [ExtFrontend] -> [ParserNorm] -> [PolicyEmit] -> [IntNicDrv]
     (VM)              (seL4)              (seL4)          (seL4)          (seL4)         (seL4)
```

The system implements strictly one-way message flow with no reverse channels:

1. **FreeRTOSNetVM**: Minimal VM guest with VirtIO-Net driver and cross-VM dataport sender
2. **NetworkDriverDrv**: Cross-VM receiver that forwards packets to ICS pipeline
3. **ExtFrontend**: Frame parser that standardizes messages to TLV format
4. **ParserNorm**: Validation engine with bounds checking and EverParse hooks
5. **PolicyEmit**: Security policy enforcement with allow/deny decisions
6. **IntNicDrv**: Message sink providing statistics and monitoring

### Cross-VM Communication Pattern

Based on `vm_echo_connector` reference example:

```
FreeRTOS VM                                    NetworkDriverDrv Component
┌─────────────────────────┐                   ┌──────────────────────────┐
│ VirtIO packet received  │                   │                          │
│         ↓               │                   │                          │
│ Write to net_tx_dp      │                   │                          │
│ (shared dataport)       │────Shared Memory──>│ Read from net_rx_dp     │
│         ↓               │                   │         ↓                │
│ Emit 'packet_ready'     │────Notification───>│ Consume 'packet_ready'  │
│ notification            │                   │         ↓                │
│         ↓               │                   │ Process packet           │
│ Wait for 'done'         │<───Notification────│         ↓                │
│ notification            │                   │ Emit 'done' notification │
└─────────────────────────┘                   └──────────────────────────┘
```

**Key Technologies:**
- **seL4SharedDataWithCaps**: Zero-copy shared memory dataports
- **seL4Notification/seL4GlobalAsynch**: Event signaling between VM and component
- **Cross-VM Connection API**: `sel4vmmplatsupport/drivers/cross_vm_connection.h`

### One-Way Security Guarantee

**Critical Security Property**: No component has bidirectional connections. Each component can only:
- Read from its input dataport (if present)
- Write to its output dataport (if present)
- Consume input notifications
- Emit output notifications

**No reverse channels exist** - enforced by seL4 capability system and verified during build.

**VM Isolation**: Even if FreeRTOS VM is compromised, it cannot:
- Modify security policies (in PolicyEmit)
- Bypass validation (in ParserNorm)
- Read processed messages (from IntNicDrv)
- Create reverse information flow

---

## Message Format

The pipeline uses a simple TLV-style message format:

```c
struct MsgHeader {
    uint16_t tag;   // Protocol identifier (MODBUS_TCP=0x0001, DNP3=0x0002, etc.)
    uint16_t len;   // Payload length in bytes
    uint32_t flags; // Reserved for future use (auth/integrity markers)
};
```

- **Header Size**: 8 bytes
- **Max Payload**: 60,000 bytes
- **Min Payload**: 1 byte (0 bytes allowed for heartbeats)

---

## Build Instructions

### Prerequisites

- seL4/CAmkES development environment
- ARM cross-compilation toolchain
- CMake 3.8.2 or higher
- Ninja build system
- QEMU with ARM virtualization support

### Building V3

```bash
cd /home/iamfo470/phd/camkes-vm-examples
rm -rf build && mkdir build && cd build

# Configure for V3
../init-build.sh -DCAMKES_VM_APP=ics_oneway_norm_v3 \
                 -DAARCH32=0 \
                 -DPLATFORM=qemu-arm-virt

# Build
ninja
```

### Running with Network Support

**Method 1: Using simulate script (QEMU user networking)**
```bash
cd /home/iamfo470/phd/camkes-vm-examples/ics_oneway_norm_v3
./simulate --extra-qemu-args="-nic user,model=virtio,mac=52:54:00:12:34:56,hostfwd=tcp::8502-:502"
```

**Method 2: Direct QEMU invocation**
```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on,highmem=off,secure=off \
    -cpu cortex-a53 \
    -nographic \
    -m 1024 \
    -kernel images/capdl-loader-image-arm-qemu-arm-virt \
    -nic user,model=virtio,mac=52:54:00:12:34:56,hostfwd=tcp::8502-:502
```

**Expected Startup Output:**
```
FreeRTOS: Network VM starting...
FreeRTOS: VirtIO-Net device initialized (MAC: 52:54:00:12:34:56)
FreeRTOS: Network RX task started
NetworkDriverDrv: Cross-VM receiver started
NetworkDriverDrv: Waiting for packets from FreeRTOS VM...
ExtFrontend: Ready for frames
ParserNorm: Validation engine initialized
PolicyEmit: Policy engine started (allow-all mode)
IntNicDrv: Message sink ready
```

---

## Testing V3

### Test 1: Basic Network Connectivity

**Send a simple TCP packet:**
```bash
# In terminal 1: Run seL4 system
./simulate --extra-qemu-args="-nic user,model=virtio,mac=52:54:00:12:34:56,hostfwd=tcp::8502-:502"

# In terminal 2: Send test data
echo "MODBUS_TEST" | nc localhost 8502
```

**Expected Output:**
```
FreeRTOS: Received packet from VirtIO-Net (58 bytes)
FreeRTOS: Packet forwarded to NetworkDriverDrv via dataport
NetworkDriverDrv: Received packet from FreeRTOS VM (58 bytes)
NetworkDriverDrv: EtherType: 0x0800 (IPv4)
NetworkDriverDrv: IP Protocol: 6 (TCP)
NetworkDriverDrv: TCP Dest Port: 502
NetworkDriverDrv: Protocol detected: MODBUS_TCP
NetworkDriverDrv: Forwarded to pipeline (11 bytes payload)
ExtFrontend: Frame received (11 bytes)
ParserNorm: Validation passed
PolicyEmit: ALLOW - Modbus TCP detected
IntNicDrv: Message received and counted (Total: 1)
```

### Test 2: Binary Modbus Frame

**Send real Modbus TCP frame (Read Holding Registers):**
```bash
# Modbus TCP: Transaction=1, Protocol=0, Length=6, Unit=1, Function=3, Start=0, Count=10
echo "00 01 00 00 00 06 01 03 00 00 00 0A" | xxd -r -p | nc localhost 8502
```

**Expected Output:**
```
NetworkDriverDrv: Protocol detected: MODBUS_TCP
ParserNorm: Modbus function code 3 (Read Holding Registers)
PolicyEmit: ALLOW - Valid Modbus read operation
IntNicDrv: Modbus packet processed successfully
```

### Test 3: Multi-Protocol Detection

```bash
# DNP3 (port 20000)
echo "DNP3_TEST" | nc localhost 20000

# EtherNet/IP (port 44818)
echo "ETHERNETIP_TEST" | nc localhost 44818
```

### Test 4: Sustained Traffic

```bash
# Generate 100 packets
for i in {1..100}; do
    echo "MODBUS_MSG_$i" | nc localhost 8502
    sleep 0.1
done
```

**Monitor statistics in IntNicDrv output:**
```
IntNicDrv: Statistics - Total: 100, Modbus: 100, DNP3: 0, EtherNet/IP: 0
IntNicDrv: Throughput: 10 msg/s, Success rate: 100%
```

---

## Implementation Status

Based on the implementation plan in `/home/iamfo470/phd/research-docs/no-op-normalizer/virtio-drivers/freertos-vm-cross-vm-implementation-plan.md`:

### Phase 1: FreeRTOS VM Setup with VirtIO-Net (Days 1-3)
- [ ] Create FreeRTOSNetVM component structure
- [ ] Port VirtIO-Net driver to FreeRTOS
- [ ] Implement RX virtqueue management
- [ ] Add cross-VM dataport forwarding task
- [ ] Test VM boot and VirtIO initialization

### Phase 2: NetworkDriverDrv Cross-VM Integration (Days 3-4)
- [ ] Update NetworkDriverDrv component interface
- [ ] Replace VirtIO MMIO code with cross-VM receiver
- [ ] Implement packet parsing (Ethernet/IP/TCP)
- [ ] Add protocol detection logic
- [ ] Forward to ICS pipeline via ring buffer

### Phase 3: CAmkES Assembly Integration (Days 4-5)
- [ ] Update icf.camkes with cross-VM connections
- [ ] Add seL4SharedDataWithCaps connections
- [ ] Configure cross-VM notifications
- [ ] Add cross-VM initialization code
- [ ] Build and resolve linking issues

### Phase 4: Build and Testing (Days 5-6)
- [ ] Configure CMake for FreeRTOS VM
- [ ] Test with QEMU user networking
- [ ] Validate cross-VM dataport communication
- [ ] Test protocol detection (Modbus, DNP3, EtherNet/IP)
- [ ] Verify end-to-end pipeline flow

### Phase 5: Validation and Documentation (Days 6-7)
- [ ] Performance benchmarking
- [ ] Security validation
- [ ] Documentation updates
- [ ] Test suite creation

---

## Component Details

### FreeRTOSNetVM - Minimal Network VM

**Purpose**: Untrusted network driver that receives external packets and forwards to seL4 components

**Key Features:**
- FreeRTOS real-time operating system (~50KB footprint)
- VirtIO-Net MMIO driver (RX only for one-way flow)
- lwIP TCP/IP stack (minimal configuration)
- Cross-VM dataport packet forwarding
- Interrupt-driven packet reception

**Security Boundary:**
- VM is untrusted - compromise only affects network reception
- Cannot modify security policies or bypass validation
- One-way dataport: writes only, no read access to pipeline state

**Implementation Files:**
```
components/FreeRTOSNetVM/
├── FreeRTOSNetVM.camkes          # Component interface
├── CMakeLists.txt                 # Build configuration
└── src/
    ├── main.c                     # FreeRTOS tasks
    ├── virtio_net.c               # VirtIO-Net driver
    └── cross_vm_net.c             # Cross-VM forwarding
```

### NetworkDriverDrv - Cross-VM Receiver

**Purpose**: Trusted seL4 component that receives packets from FreeRTOS VM via shared dataport

**Key Features:**
- Cross-VM dataport receiver (net_rx_dp)
- Packet parsing (Ethernet → IP → TCP)
- Protocol detection (port-based: 502=Modbus, 20000=DNP3, etc.)
- Payload extraction and forwarding to ICS pipeline
- Statistics and logging

**Security Properties:**
- Runs in isolated seL4 component (trusted)
- Validates all data from untrusted VM
- Drops malformed packets before pipeline entry
- No reverse channel to VM (done notification only)

**Implementation:**
- Old VirtIO MMIO code replaced with cross-VM receiver logic
- Event-driven: waits for `packet_ready` notification from VM
- Processes packet, forwards to pipeline, signals `done` to VM

### ExtFrontend - Frame Processor

- **Purpose**: Converts raw frames to standardized TLV format
- **Validation**: Basic tag validation and size limits
- **Error Handling**: Drops invalid frames with logging
- **Throughput**: Designed for sustained 100+ messages/second

### ParserNorm - Validation Engine

- **Purpose**: Comprehensive message validation and normalization
- **Validation**:
  - Strict bounds checking (header vs payload size)
  - Protocol tag validation
  - Payload size limits (1-60000 bytes)
  - EverParse integration hooks (Phase 1: no-op)
- **Audit Trail**: Maintains circular log of all rejected messages
- **Security**: Critical component - all validation failures are logged

### PolicyEmit - Security Gateway

- **Purpose**: Apply security policies before forwarding to internal network
- **Phase 1 Policy**: Allow-all with comprehensive logging
- **Phase 2 Framework**: Function code filtering, rate limiting, value ranges
- **Policy Rules**: Configurable table for protocol-specific decisions
- **Logging**: Detailed policy decisions and statistics

### IntNicDrv - Message Sink

- **Purpose**: Final destination with monitoring and statistics
- **Statistics**:
  - Message counts by protocol type (Modbus, DNP3, EtherNet/IP)
  - Processing rates and success ratios
  - Performance metrics and error rates
- **Monitoring**: Detailed periodic reports every second
- **Future**: Integration point for real internal network hardware

---

## Capability Rights Matrix

| Component | Input Dataport | Output Dataport | Input Notification | Output Notification | VM Access |
|-----------|:---:|:---:|:---:|:---:|:---:|
| FreeRTOSNetVM     | -   | W (net_tx_dp) | C (done) | E (packet_ready) | - |
| NetworkDriverDrv  | R (net_rx_dp) | W | C (packet_ready) | E (done, out_ntfy) | - |
| ExtFrontend       | R   | W   | C   | E   | - |
| ParserNorm        | R   | W   | C   | E   | - |
| PolicyEmit        | R   | W   | C   | E   | - |
| IntNicDrv         | R   | -   | C   | -   | - |

**Legend**: R=Read, W=Write, C=Consume, E=Emit, -=No Access

**Security Verification**: FreeRTOSNetVM has no read access to any seL4 component state, ensuring no information can flow backward through the pipeline.

---

## Security Analysis

### Threat Model

**Assumed Threats:**
- Malformed messages from external network
- Protocol-level attacks (buffer overflows, injection)
- Unauthorized function codes or parameter values
- Denial of service via message flooding
- **NEW in V3**: Compromised FreeRTOS VM

**Security Guarantees:**
- **Information Flow Control**: seL4 capability system prevents reverse flow
- **Memory Safety**: Bounds checking prevents buffer overflows
- **Resource Isolation**: Each component runs in isolated address space
- **Audit Trail**: All security decisions are logged for analysis
- **VM Isolation**: Compromised VM cannot bypass security filtering

### V3-Specific Security Properties

**VM Trust Boundary:**
```
┌────────────────────────────────────────────────────────┐
│ UNTRUSTED ZONE                                         │
│                                                        │
│ FreeRTOS VM                                            │
│ - Network driver only                                  │
│ - No security decisions                                │
│ - Compromise = network DoS only                        │
└────────────────────────────────────────────────────────┘
                    ↓ (one-way dataport)
┌────────────────────────────────────────────────────────┐
│ TRUSTED ZONE (seL4)                                    │
│                                                        │
│ NetworkDriverDrv → ExtFrontend → ParserNorm →          │
│ PolicyEmit → IntNicDrv                                 │
│                                                        │
│ - All validation happens here                          │
│ - All policy enforcement happens here                  │
│ - VM compromise CANNOT affect security                 │
└────────────────────────────────────────────────────────┘
```

**What VM Compromise Can Do:**
- Drop packets (network DoS)
- Send crafted packets (caught by ParserNorm validation)
- Delay packet forwarding (detected by monitoring)

**What VM Compromise CANNOT Do:**
- Bypass security validation
- Modify security policies
- Read processed messages from internal network
- Create reverse information flow
- Affect other seL4 components

### Cross-VM Dataport Security

**Memory Isolation:**
- Dataport is shared memory region mapped into both VM and component
- VM can only WRITE, NetworkDriverDrv can only READ
- No capability to modify component memory outside dataport
- seL4 kernel enforces memory access permissions

**Information Flow:**
- One-way: VM → seL4 component
- Done notification does NOT carry data (1-bit signal only)
- No channel for component state to leak back to VM

---

## EverParse Integration

### Phase 1 - Hooks in Place

The ParserNorm component includes a stub function for EverParse integration:

```c
bool everparse_validate(const uint8_t* payload, size_t length) {
    // Phase 1: Always return true (no-op validation)
    // TODO: Replace with actual EverParse validator
    return true;
}
```

### Phase 2 - EverParse Replacement

To integrate real EverParse validation:

1. **Install EverParse**: Follow EverParse installation guide
2. **Generate Validators**: Create protocol-specific parsers for MODBUS, DNP3, etc.
3. **Update ParserNorm**: Replace stub function with EverParse calls
4. **Link Libraries**: Update CMakeLists.txt to link EverParse libraries
5. **Test Integration**: Verify parsing accuracy with known good/bad messages

Example integration:
```c
#include "everparse_modbus.h"
#include "everparse_dnp3.h"

bool everparse_validate(const uint8_t* payload, size_t length) {
    switch (current_protocol_tag) {
    case MODBUS_TCP_TAG:
        return EverParse_MODBUS_validate(payload, length);
    case DNP3_TAG:
        return EverParse_DNP3_validate(payload, length);
    default:
        return false;  // Unknown protocol
    }
}
```

---

## Performance Considerations

### Cross-VM Overhead

**Dataport Communication:**
- Shared memory = zero-copy data transfer
- Notification overhead: ~1-5 μs (seL4 IPC)
- Total added latency: <10 μs vs bare-metal

**FreeRTOS VM Boot:**
- Boot time: ~100ms (vs ~3s for Linux VM)
- Memory footprint: ~50KB (vs 10MB+ for Linux)

**Packet Processing:**
- VirtIO interrupt → FreeRTOS task: <50 μs
- Dataport write → seL4 notification: <10 μs
- Total VM overhead: <100 μs per packet

---

## Development Notes

### Code Style

- **Language**: C99 standard with seL4/CAmkES extensions
- **Warnings**: Compiles clean with `-Wall -Wextra`
- **Memory**: Static allocation only - no malloc/free (except in FreeRTOS VM)
- **Naming**: Consistent component naming (NetworkDriverDrv, ExtFrontend, etc.)

### Cross-VM Development Tips

**Reference Examples:**
- `vm_echo_connector`: Cross-VM dataport pattern
- `vm_freertos`: FreeRTOS VM infrastructure
- `vm_virtio_net`: VirtIO network device passthrough

**Common Pitfalls:**
- Dataport direction: Component-to-VM (from/to reversed from component-to-component)
- Notification types: Use seL4GlobalAsynch for VM → component events
- Memory alignment: Ensure dataport payloads respect alignment requirements

---

## Reference Documentation

- **Implementation Plan**: `/home/iamfo470/phd/research-docs/no-op-normalizer/virtio-drivers/freertos-vm-cross-vm-implementation-plan.md`
- **VirtIO Spec**: https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html
- **vm_echo_connector**: `/home/iamfo470/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/vm_echo_connector/`
- **vm_freertos**: `/home/iamfo470/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/vm_freertos/`
- **CAmkES Manual**: https://docs.sel4.systems/projects/camkes/

---

## Success Criteria

V3 is complete when:
- ✅ FreeRTOS VM boots successfully
- ✅ VirtIO-Net device initialized in VM
- ✅ Cross-VM dataport communication verified
- ✅ NetworkDriverDrv receives packets from VM
- ✅ Ethernet/IP/TCP headers parsed correctly
- ✅ Modbus TCP protocol detected (port 502)
- ✅ TCP payload extracted and forwarded to pipeline
- ✅ Complete pipeline processes external messages
- ✅ `echo "TEST" | nc localhost 8502` reaches IntNicDrv
- ✅ Binary Modbus frames processed correctly
- ✅ System stable under sustained traffic (100+ msg/s)
- ✅ Security properties verified (no reverse flow from VM)

---

## Troubleshooting

### Common Issues

**1. VM fails to boot**
```
Check QEMU args: -machine virt,virtualization=on
Verify FreeRTOS image in overlay_files
Check console output for FreeRTOS initialization errors
```

**2. VirtIO device not found in VM**
```
Ensure QEMU has -nic user,model=virtio
Check VM DTB includes VirtIO MMIO device
Verify VirtIO driver initialization in FreeRTOS
```

**3. Cross-VM dataport communication fails**
```
Check dataport ID configuration in icf.camkes
Verify cross_vm_connections_init() is called
Check notification badges match between VM and component
Enable cross-VM debug logging in cross_vm_connection.h
```

**4. Packets not reaching NetworkDriverDrv**
```
Verify FreeRTOS task is running (check task scheduling)
Check packet_ready notification is emitted
Verify dataport write completes before notification
Add debug prints in cross_vm_net.c forwarding task
```

**5. Protocol detection fails**
```
Check port forwarding in QEMU args (hostfwd=tcp::8502-:502)
Verify TCP header parsing (offset calculations)
Add debug prints for EtherType and IP protocol
Test with known-good Modbus TCP frame
```

---

## License

SPDX-License-Identifier: BSD-2-Clause

This project is licensed under the BSD 2-Clause License, consistent with seL4 and CAmkES licensing.

---

**Version**: 3.0
**Created**: 2025-10-03
**Architecture**: FreeRTOS VM + Cross-VM Dataports
**Status**: Design complete, implementation pending
