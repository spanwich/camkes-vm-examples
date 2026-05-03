# Tier 1: VirtIO Device Discovery Test

**Status**: Ready to build and test
**Date**: 2025-10-04
**Goal**: Prove that CAmkES can discover the same virtio-net device that sDDF uses

---

## What This Is

This is a **simplified proof-of-concept** that demonstrates CAmkES can access and initialize the QEMU virtio-net device using the same approach as sDDF's ethernet driver.

**What it does**:
- ✅ Detects virtio MMIO device at physical address 0xa003000
- ✅ Verifies virtio version 2 (modern virtio 1.0+)
- ✅ Confirms device type is network (ID 0x01)
- ✅ Completes full virtio initialization handshake
- ✅ Reads MAC address from device configuration
- ✅ Initializes RX and TX virtqueues

**What it does NOT do** (yet):
- ❌ No packet transmission/reception (Tier 2)
- ❌ No sDDF queues (simplified for testing)
- ❌ No bridge to VM (Tier 3)

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│         QEMU virtio-net-device                       │
│         (virtio-mmio @ 0xa003000, IRQ 79)            │
│  CRITICAL: -global virtio-mmio.force-legacy=false    │
└────────────────────┬─────────────────────────────────┘
                     │ MMIO + IRQ
                     ↓
┌────────────────────────────────────────────────────────┐
│        CAmkES Component: EthernetDriver                │
│  - Source: components/EthernetDriver/ethernet_driver.c│
│  - Ported from: sDDF drivers/network/virtio/ethernet.c│
│  - Maps: virtio MMIO regs (uncached I/O)             │
│  - Maps: hw_ring_buffer (virtqueue memory)           │
│  - Initializes: RX and TX virtqueues                 │
│  - Status: Device discovery ONLY (no packet I/O)     │
└────────────────────────────────────────────────────────┘
```

**Key Difference from sDDF**:
- sDDF uses Microkit (different framework)
- This uses CAmkES (same framework as vm_freertos)
- Proves interoperability is possible

---

## Files Created

### Component Implementation

1. **`components/EthernetDriver/ethernet_driver.c`**
   - Main driver implementation
   - Ported from sDDF's `drivers/network/virtio/ethernet.c`
   - Simplified: Only device discovery, no packet I/O yet
   - Detailed logging of initialization sequence

2. **`components/EthernetDriver/EthernetDriver.camkes`**
   - CAmkES component definition
   - Dataports for MMIO and ring buffer
   - Interrupt connection for IRQ 79

### System Assembly

3. **`vm_ethernet_test.camkes`**
   - Test assembly configuration
   - Boots ONLY the EthernetDriver (no VM)
   - Hardware resource mappings
   - Memory configuration

### Build Configuration

4. **`CMakeLists.txt`** (modified)
   - Added EthernetDriver component build
   - Set `vm_ethernet_test.camkes` as active root server
   - Commented out full VM system (for Tier 1 testing)

### Testing

5. **`simulate_tier1.sh`**
   - QEMU simulation script
   - Includes CRITICAL sDDF flags
   - Port forwarding for future networking tests

---

## Build Instructions

### Prerequisites

Ensure you're in the seL4 development environment:
```bash
source ~/phd/sel4-dev-env/bin/activate
cd ~/phd/camkes-vm-examples
```

### Build Process

```bash
# Clean any previous build
rm -rf build

# Create build directory
mkdir build && cd build

# Configure with sDDF-compatible Python environment
env PYTHONPATH=../projects/camkes-tool:../projects/capdl/python-capdl-tool \
cmake -G Ninja \
  -DCAMKES_VM_APP=vm_freertos_net \
  -DPLATFORM=qemu-arm-virt \
  -DSIMULATION=1 \
  -DLibUSB=OFF \
  -DSEL4_CACHE_DIR=../.sel4_cache \
  -C ../projects/vm-examples/settings.cmake \
  ../projects/vm-examples

# Build
ninja
```

**Expected output**: `build/images/capdl-loader-image-arm-qemu-arm-virt`

---

## Running the Test

### Method 1: Using the Custom Script (Recommended)

```bash
cd ~/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/vm_freertos_net
BUILD_DIR=../../../../build ./simulate_tier1.sh
```

The script automatically:
- Checks if the image exists
- Displays configuration
- Launches QEMU with correct flags
- Shows colored output

### Method 2: Manual QEMU Command

```bash
cd ~/phd/camkes-vm-examples/build

qemu-system-aarch64 \
    -machine virt,virtualization=on,highmem=off,secure=off \
    -cpu cortex-a53 \
    -m 2G \
    -nographic \
    -serial mon:stdio \
    -global virtio-mmio.force-legacy=false \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::5555-:1237 \
    -kernel images/capdl-loader-image-arm-qemu-arm-virt
```

**Exit QEMU**: Press `Ctrl-A` then `X`

---

## Expected Output

### Success Scenario

You should see output like this:

```
╔══════════════════════════════════════════════════════════╗
║         EthernetDriver Component - Tier 1               ║
║      VirtIO Device Discovery Proof of Concept           ║
║              (CAmkES Port of sDDF Driver)                ║
╚══════════════════════════════════════════════════════════╝

EthernetDriver: Component started

EthernetDriver: Mapping hardware resources...
════════════════════════════════════════════════════════════
EthernetDriver:   VirtIO MMIO regs: vaddr=0x...
EthernetDriver:   HW ring buffer:   vaddr=0x..., paddr=0x5fff0000
EthernetDriver:   ✓ Ring buffer cleared

╔══════════════════════════════════════════════════════════╗
║     VirtIO Device Initialization Sequence               ║
╚══════════════════════════════════════════════════════════╝

EthernetDriver: [Step 1/8] Device Detection
════════════════════════════════════════════════════════════
EthernetDriver: Checking virtio magic value...
EthernetDriver:   Expected: 0x74726976 ("virt")
EthernetDriver:   Read:     0x74726976
EthernetDriver:   ✓ Magic value correct!

EthernetDriver: Checking virtio version...
EthernetDriver:   Expected: 0x2 (modern virtio 1.0+)
EthernetDriver:   Read:     0x2
EthernetDriver:   ✓ Version correct!

EthernetDriver: Checking device type...
EthernetDriver:   Device ID: 0x1
EthernetDriver:   Vendor ID: 0x554d4551 (QEMU)
EthernetDriver:   ✓ Device is virtio-net!

[... more initialization steps ...]

EthernetDriver: Reading MAC address from device...
EthernetDriver:   MAC: 52:54:00:12:34:56
EthernetDriver:   Status: Link UP

EthernetDriver: Setting up virtqueue 0...
EthernetDriver:   Max queue size: 512
EthernetDriver:   Descriptor ring: vaddr=0x..., paddr=0x5fff0000
EthernetDriver:   Available ring:  vaddr=0x..., paddr=0x...
EthernetDriver:   Used ring:       vaddr=0x..., paddr=0x...
EthernetDriver:   ✓ Virtqueue 0 ready!

[... same for virtqueue 1 ...]

╔══════════════════════════════════════════════════════════╗
║                                                          ║
║              ✓✓✓ SUCCESS! ✓✓✓                          ║
║                                                          ║
║  VirtIO network device successfully discovered and      ║
║  initialized using CAmkES!                              ║
║                                                          ║
║  This proves that the same virtio-net device that       ║
║  sDDF uses can be accessed from CAmkES components.      ║
║                                                          ║
║  Next step: Implement packet TX/RX logic (Tier 2)       ║
║                                                          ║
╚══════════════════════════════════════════════════════════╝

EthernetDriver: Component initialization complete.
EthernetDriver: Entering idle state (no packet processing yet).
```

### Failure Scenarios

#### If magic value is wrong:
```
EthernetDriver:   ✗ MAGIC VALUE MISMATCH!
```
→ **Fix**: Check memory mapping in CAmkES assembly

#### If version is wrong (most common):
```
EthernetDriver:   ✗ VERSION MISMATCH!
EthernetDriver:   This likely means QEMU was started without:
EthernetDriver:   -global virtio-mmio.force-legacy=false
```
→ **Fix**: Use the provided `simulate_tier1.sh` script

#### If device ID is wrong:
```
EthernetDriver:   ✗ NOT A NETWORK DEVICE!
```
→ **Fix**: Check QEMU command includes `-device virtio-net-device`

---

## Troubleshooting

### Build Fails with "Cannot find EthernetDriver.camkes"

**Cause**: CAmkES import path not set correctly

**Fix**: Add to `CMakeLists.txt`:
```cmake
CAmkESAddImportPath(components/EthernetDriver)
```

### Component Crashes at `regs->MagicValue`

**Cause**: MMIO mapping failed or wrong physical address

**Fix**: Verify in `vm_ethernet_test.camkes`:
```camkes
hardware_virtio_mmio_regs.physical_address = 0xa003000;
hardware_virtio_mmio_regs.set_access_as_cached = false;
```

### Version Check Fails (Version = 0x1)

**Cause**: QEMU using legacy virtio (version 1)

**Fix**: Ensure QEMU command includes:
```bash
-global virtio-mmio.force-legacy=false
```

This flag was discovered from sDDF CI configuration and is CRITICAL.

### No Output / Silent Boot

**Cause**: CAmkES component priority issue or serial not working

**Fix**: Check component priority in assembly:
```camkes
eth_driver.priority = 101;
eth_driver._priority = 101;
```

---

## Next Steps: Tier 2

Once Tier 1 succeeds, the next phase is:

### Tier 2: Packet Transmission/Reception

1. **Implement packet TX path**:
   - Allocate transmit buffers
   - Fill virtqueue descriptors
   - Notify device via `QueueNotify`
   - Handle TX completion IRQs

2. **Implement packet RX path**:
   - Pre-populate RX virtqueue with buffers
   - Handle RX IRQs
   - Extract received packets
   - Process packet data

3. **Test with actual traffic**:
   - Send test packet
   - Receive ping response
   - Verify packet integrity

**Estimated effort**: 1-2 days

### Tier 3: Bridge to VM

After packet I/O works in the driver:

1. **Create NetworkBridge component**:
   - North interface: Connect to EthernetDriver
   - South interface: Connect to FreeRTOS VM virtqueues
   - Translate packets between both sides

2. **Integrate FreeRTOS network stack**:
   - Add FreeRTOS+TCP or lwIP to guest
   - Implement virtqueue network interface
   - Test end-to-end connectivity

**Estimated effort**: 2-3 days

---

## Key Learnings from sDDF

### Critical Discovery: QEMU Flag

The flag `-global virtio-mmio.force-legacy=false` is **absolutely critical**.

Without it:
- QEMU creates virtio-mmio version 1 (legacy)
- Driver expects version 2 (modern virtio 1.0+)
- Initialization fails with version mismatch

This flag was found in sDDF CI scripts:
- File: `sDDF/ci/examples/echo_server.py`
- Not mentioned in sDDF user documentation
- Essential for QEMU compatibility

### Physical Address Layout (from sDDF)

| Address | Size | Purpose |
|---------|------|---------|
| 0xa003000 | 0x1000 | VirtIO MMIO registers |
| 0x5fff0000 | 0x10000 | Hardware ring buffer (virtqueues) |
| 0x5fef0000 | 0x100000 | RX data region (future) |
| 0x5fdf0000 | 0x100000 | TX data region (future) |

These addresses match sDDF's successful configuration.

### Device Features

From sDDF, we know the device supports:
- `VIRTIO_F_VERSION_1`: Modern virtio protocol
- `VIRTIO_NET_F_MAC`: MAC address in config space
- `VIRTIO_NET_F_STATUS`: Link status

We negotiate only what we need (MAC + VERSION_1).

---

## References

### sDDF Source Code

- **Original driver**: `/home/konton-otome/phd/sDDF/drivers/network/virtio/ethernet.c`
- **Success documentation**: `/home/konton-otome/phd/research-docs/sddf-network-success.md`
- **CI configuration**: `/home/konton-otome/phd/sDDF/ci/examples/echo_server.py`

### VirtIO Specification

- **Version**: VirtIO 1.2
- **Section 4.2**: MMIO Transport
- **Section 5.1**: Network Device

### CAmkES Documentation

- **VM Helpers**: `projects/camkes-arm-vm/camkes-arm-vm-helpers.cmake`
- **Hardware connectors**: seL4HardwareMMIO, seL4HardwareInterrupt
- **Dataports**: Shared memory regions

---

## Author Notes

This Tier 1 implementation was created to:
1. Prove CAmkES can access the same hardware as sDDF
2. Validate the approach before building full integration
3. Provide a working foundation for Tier 2/3 development

**Status**: ✅ Ready for testing
**Confidence**: High (based on sDDF's proven success)

---

**Last Updated**: 2025-10-04
**Next Milestone**: Build and test Tier 1, then proceed to packet I/O
