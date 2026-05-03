# Build Instructions for ICS One-Way Normalizer V2

## Prerequisites

### System Packages
All required packages should already be installed. If you encounter missing dependencies, install:
```bash
sudo apt-get install build-essential cmake ninja-build gcc-arm-none-eabi device-tree-compiler
```

### Python Dependencies
The build requires several Python packages. Install them with:
```bash
pip3 install --break-system-packages plyplus future aenum sortedcontainers ordered-set lxml tempita pyelftools libarchive-c
```

## Build Commands

### 1. Navigate to repository root
```bash
cd ~/phd/camkes-vm-examples
```

### 2. Create and configure build directory
```bash
mkdir -p build_ics_v2
cd build_ics_v2
../init-build.sh -DPLATFORM=qemu-arm-virt -DCAMKES_VM_APP=ics_oneway_norm_v2 -DLibUSB=OFF
```

**Important Flags:**
- `-DPLATFORM=qemu-arm-virt` - Target platform
- `-DCAMKES_VM_APP=ics_oneway_norm_v2` - Application name
- `-DLibUSB=OFF` - Disable USB support (required for qemu-arm-virt)

### 3. Build the project
```bash
ninja
```

Build output will be in: `build_ics_v2/images/capdl-loader-image-arm-qemu-arm-virt`

## Running the Simulation

### Method 1: Using the generated simulate script
```bash
cd ~/phd/camkes-vm-examples/build_ics_v2
./simulate
```

### Method 2: Manual QEMU command
```bash
qemu-system-arm \
    -machine virt,virtualization=on \
    -cpu cortex-a15 \
    -m 512M \
    -nographic \
    -kernel images/capdl-loader-image-arm-qemu-arm-virt
```

## Testing VirtIO Magic Number Access

### What the Test Does

The VM includes an auto-run test script (`/root/test_virtio_magic.sh`) that:

1. **Reads VirtIO MMIO Registers** at addresses 0x0a000000, 0x0a000200, etc.
2. **Checks Magic Number** (should be 0x74726976 = "virt" in little-endian)
3. **Identifies Device Types** (network, console, block, etc.)

### Expected Output

When the VM boots, you should see:

```
=== VirtIO Magic Number Test ===
Testing VM access to VirtIO MMIO registers

--- VirtIO Device 0 ---
Base Address: 0x0a000000
Magic Number: 0x74726976
✓ VALID VIRTIO DEVICE FOUND!

Reading additional VirtIO registers:
  Version (offset 0x04): 0x00000002
  DeviceID (offset 0x08): 0x00000001
  VendorID (offset 0x0C): 0x554d4551
  Device Type: Network (virtio-net)

--- VirtIO Device 1 ---
Base Address: 0x0a000200
Magic Number: 0x74726976
✓ VALID VIRTIO DEVICE FOUND!
  Device Type: Console
```

### Troubleshooting

#### If you see "bus error" or "cannot read":
- The VirtIO MMIO region is NOT properly mapped
- Check `untyped_mmios` configuration in [icf.camkes:181-187](icf.camkes#L181-L187)
- Verify it includes: `"0xa000000:16"` for VirtIO MMIO access

#### If you see "Not a VirtIO device":
- The address mapping is correct but QEMU isn't providing VirtIO devices
- Add `-device virtio-net-device` to QEMU command line

#### If devmem is not available:
- The buildroot rootfs may not include devmem utility
- The test script will report this and suggest alternatives

## VirtIO MMIO Configuration

### Critical untyped_mmios Settings

In [icf.camkes:181-187](icf.camkes#L181-L187):

```c
vm0.untyped_mmios = [
    "0x8040000:12",     // GIC VCPU interface
    "0xa000000:16",     // VirtIO MMIO devices - CRITICAL!
    "0x10040000:17",    // PCI MMIO
    "0x3eff0000:16",    // PCI IO ports
    "0x40000000:29",    // Linux RAM
];
```

**Address Breakdown:**
- `0xa000000:16` means region from 0x0a000000 to 0x0a00ffff (2^16 = 64KB)
- This covers all 32 VirtIO device slots (0xa000000 + n*0x200, n=0..31)

### VirtIO Device Layout

QEMU provides 32 VirtIO device slots:
- Slot 0: 0x0a000000 (usually virtio-net)
- Slot 1: 0x0a000200 (usually virtio-console)
- Slot 2: 0x0a000400
- ...
- Slot 31: 0x0a003e00

Each slot is 512 bytes (0x200).

## Clean Build

To rebuild from scratch:
```bash
cd ~/phd/camkes-vm-examples
rm -rf build_ics_v2
mkdir build_ics_v2
cd build_ics_v2
../init-build.sh -DPLATFORM=qemu-arm-virt -DCAMKES_VM_APP=ics_oneway_norm_v2 -DLibUSB=OFF
ninja
```

## Build Time

- **First build**: ~5-10 minutes (compiles seL4 kernel, musllibc, etc.)
- **Incremental builds**: ~30 seconds - 2 minutes

## Build Artifacts

Key output files:
- `build_ics_v2/images/capdl-loader-image-arm-qemu-arm-virt` - Bootable image
- `build_ics_v2/simulate` - QEMU launch script
- `build_ics_v2/ast.pickle` - CAmkES AST
- `build_ics_v2/capdl-loader.cdl` - CapDL specification

## Manual Testing in VM

Once the VM boots, you can manually run the test:

```bash
# In the VM serial console:
/root/test_virtio_magic.sh
```

Or use `devmem` directly:

```bash
# Read magic number from VirtIO device 0
devmem 0x0a000000 32

# Expected output: 0x74726976
```

## Success Criteria

✅ **Build succeeds** without errors
✅ **VM boots** in QEMU and shows login prompt
✅ **VirtIO test runs** automatically on boot
✅ **Magic number 0x74726976** is read successfully
✅ **No bus errors** when accessing 0x0a000000

## Next Steps

After confirming VirtIO access works:

1. **Enable VirtIO Network** - Add `-netdev user,id=net0 -device virtio-net-device,netdev=net0` to QEMU
2. **Test ICS Pipeline** - Send network packets through the security gateway
3. **Verify One-Way Flow** - Confirm no reverse information channels exist

## Contact

For issues, check:
- seL4 documentation: https://docs.sel4.systems/
- CAmkES manual: https://docs.sel4.systems/projects/camkes/
- This PhD research project documentation in `~/phd/research-docs/`
