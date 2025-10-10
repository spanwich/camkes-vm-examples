# Modbus Bidirectional POC - ICS Security Gateway

**Protocol**: Modbus TCP (expandable to DNP3, EtherNet/IP)
**Architecture**: Bidirectional cross-domain security gateway with protocol break
**Platform**: seL4/CAmkES with dual VirtIO network drivers
**Status**: ✅ Working - Gateway forwarding operational

## Project Objective

This proof-of-concept demonstrates a **transparent, drop-in ICS security gateway** that can be deployed into existing industrial control systems with **minimal configuration changes**.

### Key Goals:

1. **Transparent Interception**: Act as man-in-the-middle between SCADA and PLC
   - SCADA thinks it's talking directly to PLC
   - PLC thinks it's talking directly to SCADA
   - No changes needed to existing SCADA or PLC configuration

2. **Physical Network Isolation**: Enforce security through network segmentation
   - Separate physical switches for external and secure networks
   - Same IP addressing for backward compatibility
   - Complete isolation of critical infrastructure

3. **GRFICS Compatibility**: Drop-in deployment for GRFICS ICS simulator
   - Works with existing GRFICS SCADA (192.168.90.5)
   - Works with existing GRFICS PLC (192.168.95.2)
   - Just insert firewall between existing networks - no reconfiguration needed

4. **Protocol-Break Security**: Validate and sanitize ICS traffic
   - Terminate incoming connections (no direct TCP path)
   - Validate Modbus commands and responses
   - Create new isolated connections to destination
   - Prevent session-based attacks and unauthorized commands

### Deployment Scenario:

```
[SCADA 192.168.90.5]
         ↓
    (External Network: 192.168.90.0/24)
         ↓
     [Router]
         ↓
    (Internal Network: 192.168.95.0/24) ← Original network
         ↓
  [Our Modbus POC Firewall] ← Drop-in security layer
    • Net0: 192.168.95.2 (pretends to be PLC)
    • Net1: 192.168.90.5 (pretends to be SCADA)
         ↓
    (Secure Subnet: 192.168.95.0/24) ← Isolated switch
         ↓
   [Real PLC 192.168.95.2]
```

**Result**: Complete security validation layer with zero changes to existing industrial systems.

## Quick Start

### Build
```bash
cd /home/iamfo470/phd/camkes-vm-examples
mkdir -p build_modbus && cd build_modbus
../init-build.sh -DPLATFORM=qemu-arm-virt \
  -DAARCH32=TRUE \
  -DCAMKES_APP=modbus_bidirection_poc
ninja
```

### Run
```bash
./simulate --extra-qemu-args="-global virtio-mmio.force-legacy=false \
  -netdev user,id=net0,hostfwd=tcp::6000-:6000 \
  -device virtio-net-device,netdev=net0 \
  -netdev user,id=net1,hostfwd=tcp::7000-:7000 \
  -device virtio-net-device,netdev=net1"
```

### Test

**Terminal 1** - Send to external network:
```bash
nc localhost 6000
Hello from external
```

**Terminal 2** - Listen for forwarded messages from internal:
```bash
nc -lk 18000
# Receives: Hello from external
```

**Terminal 3** - Send to internal network:
```bash
nc localhost 7000
Hello from internal
```

**Terminal 4** - Listen for forwarded messages from external:
```bash
nc -lk 19000
# Receives: Hello from internal
```

## Architecture

```
┌────────────────────────────────────────────────────────────┐
│                     External Network                        │
│              (Port 6000 - Untrusted Zone)                   │
└──────────────────────────┬─────────────────────────────────┘
                           │
                    ┌──────▼──────────┐
                    │ VirtIO_Net0_Drv │ ◄─── TCP Server :6000
                    │ (External NIC)  │
                    └──────┬──────────┘
                           │ INBOUND
                    ┌──────▼──────────┐
                    │  ICS_Inbound    │ ◄─── Validate External→Internal
                    │  (Firewall)     │
                    └──────┬──────────┘
                           │
                    ┌──────▼──────────┐
                    │ VirtIO_Net1_Drv │ ◄─── Creates client to 10.0.2.2:18000
                    │ (Internal NIC)  │      (QEMU gateway → host port 18000)
                    └──────┬──────────┘
                           │
┌──────────────────────────▼─────────────────────────────────┐
│                     Internal Network                        │
│               (Port 7000 - Trusted Zone)                    │
└──────────────────────────┬─────────────────────────────────┘
                           │ OUTBOUND (reverse path)
                    ┌──────▼──────────┐
                    │ VirtIO_Net1_Drv │ ◄─── TCP Server :7000
                    └──────┬──────────┘
                           │
                    ┌──────▼──────────┐
                    │  ICS_Outbound   │ ◄─── Validate Internal→External
                    │  (Firewall)     │
                    └──────┬──────────┘
                           │
                    ┌──────▼──────────┐
                    │ VirtIO_Net0_Drv │ ◄─── Creates client to 10.0.2.2:19000
                    └─────────────────┘      (QEMU gateway → host port 19000)
```

## Key Design Principles

### 1. Protocol Break Architecture
- **No direct connection** between external and internal networks
- Each message creates a **new, isolated TCP connection**
- Source connection terminates at driver, new connection created at destination
- **Security benefit**: No session state exploits, no connection hijacking

### 2. QEMU Gateway Networking
- Guest uses **10.0.2.2** (QEMU gateway) to reach host
- lwIP automatically assigns **ephemeral source ports** (49152-65535)
- Only **destination ports** are configured (18000, 19000)
- **Critical**: Cannot connect to guest's own IP (10.0.2.15) - must use gateway

### 3. Smart Drivers + Simple Validation
- **VirtIO drivers** handle TCP/IP stack (lwIP)
- **ICS components** validate application payload only
- Metadata (IP, ports, protocol) passed for context-aware validation
- ACKs handled automatically by drivers (not in ICS pipeline)

## Configuration

### Port Mapping (Configurable in driver source code)

**VirtIO_Net1_Driver** (Internal):
```c
#define TCP_SERVER_PORT 7000              // Listen for internal connections
#define INBOUND_FORWARD_IP "10.0.2.2"     // QEMU gateway to reach host
#define INBOUND_FORWARD_PORT 18000         // Host port for external→internal
```

**VirtIO_Net0_Driver** (External):
```c
#define TCP_SERVER_PORT 6000              // Listen for external connections
#define OUTBOUND_FORWARD_IP "10.0.2.2"    // QEMU gateway to reach host
#define OUTBOUND_FORWARD_PORT 19000        // Host port for internal→external
```

### Files to Modify for Modbus

1. **Protocol Detection** - Add Modbus parser:
   ```
   components/ICS_Inbound/ics_inbound.c
   components/ICS_Outbound/ics_outbound.c
   ```

2. **Validation Rules** - Implement Modbus-specific checks:
   - Function code validation (0x01-0x17)
   - Register address range checks
   - Data length validation
   - Exception handling

3. **Metadata Structure** - Extend for Modbus (if needed):
   ```c
   // In components/include/common.h
   typedef struct {
       // ... existing fields ...
       uint8_t modbus_function_code;
       uint16_t modbus_register_addr;
   } FrameMetadata;
   ```

## Project Structure

```
modbus_bidirection_poc/
├── CMakeLists.txt              # Build configuration
├── ics_dual_nic.camkes         # CAmkES component assembly
├── settings.cmake              # Platform settings
├── README.md                   # This file
└── components/
    ├── VirtIO_Net0_Driver/     # External NIC (port 6000)
    ├── VirtIO_Net1_Driver/     # Internal NIC (port 7000)
    ├── ICS_Inbound/            # External→Internal validation
    ├── ICS_Outbound/           # Internal→External validation
    └── include/common.h        # Shared data structures
```

## Security Properties

✅ **Protocol Break**: TCP connections terminated and recreated (no direct path)
✅ **Bidirectional Isolation**: Independent validation for each direction
✅ **Stateless Operation**: Each message validated independently
✅ **Metadata Preservation**: Protocol-aware validation without TCP state
✅ **seL4 Formal Verification**: Mathematical proof of kernel security
✅ **No Shared Memory**: Inbound/outbound use separate dataports

## Testing Checklist

- [ ] External port 6000 receives TCP connections
- [ ] Internal port 7000 receives TCP connections
- [ ] ICS_Inbound validates and forwards external→internal
- [ ] ICS_Outbound validates and forwards internal→external
- [ ] Messages appear at destination (nc -lk 18000 / nc -lk 19000)
- [ ] Connection closes after message transmission
- [ ] Server returns to listening state
- [ ] Metadata extraction working (check QEMU logs)

## Common Issues

**"Connection refused"**
- Ensure QEMU started with correct `--extra-qemu-args`
- Verify port forwarding: `hostfwd=tcp::6000-:6000,hostfwd=tcp::7000-:7000`

**"Messages not appearing at destination"**
- Check if `nc -lk` is running on destination ports (18000, 19000)
- Use `-k` flag to keep netcat listening after connections

**"netcat closes immediately"**
- This is **expected behavior** - each message = new connection
- Use `nc -lk` to keep listening, or use a real server

## Next Steps for Modbus Implementation

1. **Add Modbus Parser** - Detect Modbus TCP frames (MBAP header)
2. **Implement Validation** - Function code whitelist, register range checks
3. **Error Handling** - Modbus exception codes
4. **Logging** - Modbus-specific transaction logging
5. **Testing** - Use `pymodbus` or `modpoll` for realistic testing

## References

- [ICS Gateway Architecture Documentation](../../../../../../research-docs/ics-dual-nic-gateway-architecture.md)
- [Modbus TCP Specification](http://www.modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf)
- [seL4 Manual](https://sel4.systems/Info/Docs/seL4-manual-latest.pdf)
- [VirtIO Specification v1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
