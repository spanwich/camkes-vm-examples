# Virtio-Net Probe Test - Quick Start Guide

## TL;DR - Just Run This

```bash
# 1. Build FreeRTOS with probe test (EASY WAY!)
cd /home/konton-otome/phd/freertos_vexpress_a9
./build_virtio_test.sh

# OR specify custom output location:
# ./build_debug.sh normal /path/to/output/dir binary_name

# 2. Build seL4 (use your working method)
cd /home/konton-otome/phd/camkes-vm-examples
rm -rf build && mkdir build && cd build
# ... your working cmake/ninja commands ...
ninja

# 3. Run with QEMU (MUST include virtio-net!)
qemu-system-aarch64 \
  -machine virt,virtualization=on,highmem=off,secure=off \
  -cpu cortex-a53 \
  -m 2048M \
  -serial mon:stdio \
  -nographic \
  -netdev user,id=net0 \
  -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -kernel images/capdl-loader-image-arm-qemu-arm-virt
```

## What to Look For

### Success Output:
```
[1] Reading MMIO Magic Number...
    Result: 0x74726976
    ✓ MAGIC NUMBER VALID!

...

✓✓✓ SUCCESS ✓✓✓
Virtio-Net device is accessible!
```

### Failure Output:
```
[1] Reading MMIO Magic Number...
    Result: 0xFFFFFFFF
    ✗ Read returned 0xFFFFFFFF (device not mapped or bus error)
```

**If you see failure:** Check you added `-netdev user,id=net0 -device virtio-net-device,netdev=net0` to QEMU!

## Common Issues

### "Build fails - virtio_probe.o not found"
```bash
cd /home/konton-otome/phd/freertos_vexpress_a9
./build_debug.sh normal  # Run this first!
```

### "seL4 build fails"
Check that `devices.camkes` has:
```c
"0x0a000000:12",  // in untyped_mmios array
```

### "All values are 0xFFFFFFFF"
QEMU missing virtio device. Add to command line:
```bash
-netdev user,id=net0 -device virtio-net-device,netdev=net0
```

## Next Steps After Success

1. **Read:** [README_VIRTIO_TEST.md](README_VIRTIO_TEST.md) for full details
2. **Fix VBAR:** See "Critical: VBAR Setup" section
3. **Implement driver:** See [freertos-network-passthrough-architecture.md](../../../../research-docs/freertos-network-passthrough-architecture.md)

## Files Involved

- **Probe test:** `/home/konton-otome/phd/freertos_vexpress_a9/Source/virtio_probe.c`
- **Config:** `qemu-arm-virt/devices.camkes`
- **Build script:** `/home/konton-otome/phd/freertos_vexpress_a9/build_debug.sh`

## Expected Timeline

- ✅ Probe test (this step): Done
- ⏳ VBAR setup: 1-2 days
- ⏳ Basic driver: 1-2 weeks
- ⏳ Full network stack: 4-6 weeks total
