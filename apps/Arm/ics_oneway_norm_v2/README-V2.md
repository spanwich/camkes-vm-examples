# ❌ ICS One-Way Normalizer V2 - Network Bridge Architecture [OBSOLETE]

> **⚠️ OBSOLETE DESIGN**: This VM-based approach has been superseded by sDDF direct networking architecture due to fundamental QEMU memory limitations. See "Why VM Version Failed" section below.

## Overview

V2 replaces the synthetic traffic generator (ExtNicDrv) with a network-enabled system that receives real external messages through a VM bridge and processes them through the seL4 security pipeline.

## Architecture Changes

### V1 → V2 Transformation
- **V1**: `ExtNicDrv` (synthetic traffic) → `ExtFrontend` → `ParserNorm` → `PolicyEmit` → `IntNicDrv` (console output)
- **V2**: `NetworkNicDrv` (VM messages) → `ExtFrontend` → `ParserNorm` → `PolicyEmit` → `IntNicDrv` (console output)

### Key Components

1. **NetworkNicDrv**: Replaces ExtNicDrv
   - Receives messages from VM via VirtQueue
   - Parses VM messages and converts to MsgHeader format
   - Maintains same output interface to ExtFrontend

2. **VM Bridge Script**: `vm-bridge-script.sh`
   - Dual NIC configuration (eth0: external, eth1: internal)
   - Listens on external network (192.168.1.10:8502)
   - Forwards to seL4 via virtio-console
   - Outputs to internal network (192.168.10.10:8503)

3. **Existing Pipeline**: ExtFrontend, ParserNorm, PolicyEmit, IntNicDrv (unchanged)

## Network Configuration

```
External Network → eth0 (192.168.1.10:8502) → VM Bridge → seL4 Pipeline → eth1 (192.168.10.10:8503) → Internal Network
```

## Testing the Pipeline

### Expected Flow for Each Message:

1. **External Input**: `echo "MODBUS_READ_COILS" | nc 192.168.1.10 8502`

2. **VM Bridge**: Receives on eth0, forwards to seL4 via virtio-console

3. **seL4 Console Output**:
   ```
   NetworkNicDrv: Received 18 bytes from VM: MODBUS_READ_COILS
   NetworkNicDrv: Converted to protocol tag=0x0001, len=18
   NetworkNicDrv: Forwarded message #1 to pipeline
   ExtFrontend: Processing message...
   ParserNorm: Validating message...
   PolicyEmit: Applying policy...
   IntNicDrv: Message processed successfully
   ```

4. **Internal Output**: Message appears on eth1 (192.168.10.10:8503)

### Test Script

Run `./test-v2-pipeline.sh` to test with multiple protocol messages:
- MODBUS_READ_HOLDING_REGISTERS
- DNP3_DATA_REQUEST
- ETHERNET_IP_CLASS_REQUEST
- GENERIC_SENSOR_DATA_12345

## Protocol Detection

NetworkNicDrv automatically detects protocol types:
- Messages containing "MODBUS" → MODBUS_TCP_TAG (0x0001)
- Messages containing "DNP3" → DNP3_TAG (0x0002)
- Messages containing "ETHERNET_IP" → ETHERNET_IP_TAG (0x0003)
- Default → GENERIC_TAG (0x0004)

## Success Criteria

V2 is working correctly when:

1. ✅ **NetworkNicDrv** receives messages from VM and logs parsing
2. ✅ **ExtFrontend** processes messages (same as V1)
3. ✅ **ParserNorm** validates messages (same as V1)
4. ✅ **PolicyEmit** applies policies (same as V1)
5. ✅ **IntNicDrv** outputs processed messages (same as V1)
6. ✅ **Complete trace** visible in seL4 console for each external input

## Build Instructions

```bash
cd ~/phd/camkes-vm-examples
rm -rf build && mkdir build && cd build

# Configure for V2
../init-build.sh -DCAMKES_VM_APP=ics_oneway_norm_v2 -DAARCH32=0 -DPLATFORM=qemu-arm-virt

# Build
ninja

# Run with dual NIC QEMU setup
# (QEMU configuration needed for dual virtio-net interfaces)
```

## Files Modified/Created

### New Files:
- `components/NetworkNicDrv/NetworkNicDrv.c` - VM message receiver
- `components/NetworkNicDrv/NetworkNicDrv.camkes` - Component interface
- `vm-bridge-script.sh` - Dual NIC bridge with seL4 processing
- `test-v2-pipeline.sh` - End-to-end test script

### Modified Files:
- `icf.camkes` - Updated to use NetworkNicDrv with VirtQueue connections

### Removed Files:
- `components/ExtNicDrv/` - Replaced by NetworkNicDrv

## Differences from V1

| Aspect | V1 | V2 |
|--------|----|----|
| Input Source | Synthetic traffic generator | Real network via VM bridge |
| NetworkNicDrv | N/A | VirtQueue receiver from VM |
| ExtNicDrv | Synthetic message generator | Removed |
| External Interface | None | eth0 (192.168.1.10:8502) |
| Internal Interface | None | eth1 (192.168.10.10:8503) |
| VM Component | None | Bridge script with dual NICs |
| Testing | Internal traffic only | External netcat → seL4 → internal |

## Next Steps for V2.1

V2.1 will enhance this architecture with:
- Separate external and internal VMs for complete network isolation
- Enhanced security with no direct VM-to-VM communication
- All inter-VM data flows through seL4 security processing only

## Debug Information

If V2 is not working:

1. **Check seL4 console** for NetworkNicDrv initialization messages
2. **Verify VirtQueue setup** - look for VirtQueue initialization success
3. **Test VM bridge independently** - verify dual NIC configuration
4. **Check virtio-console** - ensure /dev/virtio-ports/vport0p1 exists
5. **Run test script** - ./test-v2-pipeline.sh provides comprehensive testing

The key success indicator is seeing the complete message trace through all 5 pipeline components in the seL4 console when sending external messages via netcat.

---

## ❌ Why VM Version Failed

### Root Cause: VirtIO Memory Requirements vs QEMU Limitations

The VM-based V2 architecture failed due to fundamental incompatibility between VirtIO networking requirements and QEMU AArch64 memory constraints:

#### VirtIO Networking Memory Requirements:
- **VM RAM**: 512MB for Linux guest
- **VirtIO Buffers**: 256MB for network packet queues
- **Total Required**: 768MB contiguous physical memory

#### QEMU AArch64 Memory Limitations:
- **Available Physical Memory**: Only 512MB (`[60000000..80000000)`)
- **Kernel/Rootserver Overhead**: ~36MB occupied (`60000000..60242000`)
- **Usable Memory**: ~476MB available
- **VirtIO Allocation Failure**: Cannot allocate 256MB contiguous blocks

#### Technical Analysis:

```
<<seL4(CPU 0)>>: Untyped Retype: Insufficient memory (1 * 268435456 bytes needed, 134217728 bytes available).
```

**Translation**: seL4 cannot allocate 256MB (268435456 bytes) for VirtIO networking because only 128MB (134217728 bytes) remains after VM RAM allocation.

#### QEMU Configuration Limitations:
- **highmem=off**: Required by seL4, but limits addressing to 32-bit space
- **Memory Layout**: QEMU only provides single 512MB region regardless of `-m` parameter
- **VirtQueue Constraint**: VirtIO networking requires large contiguous memory blocks

#### Alternative Approaches Attempted:
1. ✅ **Increased QEMU memory** (`-m 2G`, `-m 3G`) → No effect on available regions
2. ✅ **Enabled highmem** (`highmem=on`) → Still limited to 512MB physical memory
3. ✅ **Modified memory base addresses** → Memory regions outside available range
4. ✅ **Alternative memory pool configurations** → Insufficient for 256MB requirement

### Conclusion: Architectural Limitation

VirtIO networking on QEMU AArch64 with seL4 is **fundamentally incompatible** due to:
- Hardware memory constraints (512MB total)
- VirtIO memory overhead (256MB minimum)
- seL4 memory management (capability-based allocation)

This led to adoption of **sDDF direct networking architecture** which bypasses VM layer entirely and uses efficient shared memory queues (2MB regions) instead of VirtIO's 256MB requirement.

### Migration Path: V2 → sDDF Architecture

The solution is **sDDF-based direct networking**:
```
❌ External → QEMU → VM → VirtQueues → seL4 (Failed: 256MB allocation)
✅ External → QEMU → eth_driver → sDDF_queues → seL4 (Success: 2MB allocation)
```

**Benefits**:
- **No VM overhead**: Direct hardware access
- **Efficient memory usage**: 2MB shared regions vs 256MB VirtIO buffers
- **Proven compatibility**: Working sDDF examples on qemu_virt_aarch64
- **Better performance**: Eliminates VM context switching