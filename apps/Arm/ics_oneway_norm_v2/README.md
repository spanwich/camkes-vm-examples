# ICS One-Way Pipeline with Normalizer

A secure, one-way Industrial Control System (ICS) message processing pipeline built on seL4/CAmkES. This project implements a minimal but complete ICS security gateway with validation, normalization, and policy enforcement.

## Architecture Overview

```
[EXT NIC DRV] -> [EXT FRONTEND] -> [PARSER/NORMALIZER] -> [POLICY+EMITTER] -> [INT NIC DRV]
```

### Component Topology

The system implements a strictly one-way message flow with no reverse channels:

1. **ExtNicDrv**: Traffic generator producing synthetic ICS protocol frames
2. **ExtFrontend**: Frame parser that standardizes messages to TLV format
3. **ParserNorm**: Validation engine with bounds checking and EverParse hooks
4. **PolicyEmit**: Security policy enforcement with allow/deny decisions
5. **IntNicDrv**: Message sink providing statistics and monitoring

### One-Way Guarantee

**Critical Security Property**: No component has bidirectional connections. Each component can only:
- Read from its input dataport (if present)
- Write to its output dataport (if present)
- Consume input notifications
- Emit output notifications

**No reverse channels exist** - this is enforced by the CAmkES capability system and verified during build.

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

## Capability Rights Matrix

| Component | Input Dataport | Output Dataport | Input Notification | Output Notification | Timer | Serial |
|-----------|:---:|:---:|:---:|:---:|:---:|:---:|
| ExtNicDrv     | -   | W   | -   | E   | C   | R   |
| ExtFrontend   | R   | W   | C   | E   | -   | R   |
| ParserNorm    | R   | W   | C   | E   | -   | R   |
| PolicyEmit    | R   | W   | C   | E   | -   | R   |
| IntNicDrv     | R   | -   | C   | -   | C   | R   |

**Legend**: R=Read, W=Write, C=Consume, E=Emit, -=No Access

**Security Verification**: ExtNicDrv has no read access to PolicyEmit or IntNicDrv components, ensuring no information can flow backward through the pipeline.

## Build Instructions

### Prerequisites

- seL4/CAmkES development environment
- ARM cross-compilation toolchain
- CMake 3.8.2 or higher
- Ninja build system

### Building

1. **Navigate to the project**:
   ```bash
   cd /path/to/camkes-vm-examples/projects/vm-examples/apps/Arm/ics_oneway_norm
   ```

2. **Create build directory**:
   ```bash
   mkdir build && cd build
   ```

3. **Configure build**:
   ```bash
   cmake -DCMAKE_TOOLCHAIN_FILE=../../../../../../../kernel/gcc.cmake \
         -DPLATFORM=qemu-arm-virt \
         -DCAMKES_VM_APP=ics_oneway_norm \
         -G Ninja \
         ..
   ```

4. **Build the application**:
   ```bash
   ninja
   ```

### Running in QEMU

```bash
./simulate
```

Expected output shows each component starting up and processing messages:

```
ExtNicDrv: Generating traffic every 100ms (includes ~10% malformed messages)
ExtFrontend: Processing and validating incoming frames
ParserNorm: Performing bounds checking and EverParse validation
PolicyEmit: Applying security policies (Phase 1: allow-all)
IntNicDrv: Forwarding approved messages, printing statistics every 1s
```

## Testing and Validation

### Functional Tests

1. **Pipeline Flow Test**: Verify messages flow from ExtNicDrv to IntNicDrv
2. **Malformed Message Test**: Confirm bad messages are dropped at appropriate stages
3. **Rate Limiting Test**: Validate component performance under sustained load
4. **Statistics Test**: Verify accurate counting and reporting

### Security Tests

1. **Capability Verification**: Confirm no reverse dataport/notification connections exist
2. **Bounds Checking**: Test with oversized, undersized, and malformed headers
3. **Isolation Test**: Verify components cannot access unauthorized resources
4. **Information Flow**: Confirm no data leakage backward through pipeline

### Performance Tests

1. **Sustained Load**: Run for 60+ seconds at 10 messages/second
2. **Buffer Management**: Verify ring buffers handle wrap-around correctly
3. **Memory Usage**: Confirm static allocation with no dynamic memory
4. **Latency**: Measure end-to-end message processing time

## Component Details

### ExtNicDrv - Traffic Generator

- **Purpose**: Simulates external network traffic
- **Traffic Pattern**: 10 messages/second with 10% malformed for testing
- **Protocols**: MODBUS TCP, DNP3, EtherNet/IP, Generic
- **Malformed Types**: Length mismatches, oversized payloads, invalid tags

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
  - Message counts by protocol type
  - Processing rates and success ratios
  - Performance metrics and error rates
- **Monitoring**: Detailed periodic reports every second
- **Future**: Integration point for real internal network hardware

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

## Security Analysis

### Threat Model

**Assumed Threats**:
- Malformed messages from external network
- Protocol-level attacks (buffer overflows, injection)
- Unauthorized function codes or parameter values
- Denial of service via message flooding

**Security Guarantees**:
- **Information Flow Control**: seL4 capability system prevents reverse flow
- **Memory Safety**: Bounds checking prevents buffer overflows
- **Resource Isolation**: Each component runs in isolated address space
- **Audit Trail**: All security decisions are logged for analysis

### TLV Mismatch Protection

The system protects against TLV length/payload mismatches:

1. **Header Validation**: Length field checked against available buffer space
2. **Payload Verification**: Actual payload size verified against header claim
3. **Bounds Enforcement**: Strict limits prevent reading beyond buffer boundaries
4. **Error Handling**: Mismatches trigger immediate message drop and audit log entry

### Capability Confinement

The CAmkES build system enforces capability-based security:

- **Principle of Least Privilege**: Each component has minimal required capabilities
- **No Information Leakage**: External components cannot read internal component state
- **Formal Verification**: seL4 provides mathematical proof of isolation properties
- **Build-Time Verification**: CAmkES prevents creation of unauthorized connections

## Development Notes

### Code Style

- **Language**: C99 standard with seL4/CAmkES extensions
- **Warnings**: Compiles clean with `-Wall -Wextra`
- **Memory**: Static allocation only - no malloc/free
- **Naming**: Consistent component naming (ExtNicDrv, ExtFrontend, etc.)
- **ASCII Only**: US 101-key keyboard compatible (uses '-' not '—' or '–')

### Performance Considerations

- **Ring Buffers**: Power-of-2 sizes for efficient modulo operations
- **Memory Barriers**: ARM-specific barriers for correct multi-core operation
- **Lock-Free**: SPSC (Single Producer Single Consumer) design avoids locking
- **Static Buffers**: Pre-allocated buffers prevent allocation overhead

### Future Enhancements

**Phase 2 Roadmap**:
1. **EverParse Integration**: Replace validation stubs with real parsers
2. **Policy Engine**: Implement configurable security policies
3. **Hardware Integration**: Connect to real network interfaces
4. **Performance Optimization**: Benchmarking and optimization
5. **Formal Verification**: Extend seL4 proofs to application logic

## Troubleshooting

### Common Build Issues

1. **CMake Configuration Errors**: Ensure correct toolchain file path
2. **Missing Dependencies**: Verify seL4/CAmkES development environment
3. **Capability Violations**: Check CAmkES assembly for unauthorized connections
4. **Buffer Size Mismatches**: Ensure consistent Buf dataport sizes

### Runtime Issues

1. **Component Startup Failures**: Check serial output for initialization errors
2. **Message Flow Problems**: Verify ring buffer initialization and validity
3. **Performance Issues**: Monitor statistics output for bottlenecks
4. **Memory Corruption**: Enable debugging and check bounds validation

### Debug Output

Enable detailed logging by modifying component source files:

```c
// Increase verbosity in common.h
#define LOG_DEBUG_ENABLED 1
#define LOG_TRACE_ENABLED 1
```

## License

SPDX-License-Identifier: BSD-2-Clause

This project is licensed under the BSD 2-Clause License, consistent with seL4 and CAmkES licensing.