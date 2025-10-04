# Virtio-Net Memory Probe Test

## Purpose

This directory contains a modified version of `vm_freertos` with a virtio-net MMIO memory probe test. The test verifies that FreeRTOS can access the virtio-net device memory region **before implementing a full driver**.

## What This Test Does

The test probes the virtio-net MMIO region at `0x0a000000` and reads the following registers:

1. **Magic Number** (offset 0x000): Expected `0x74726976` ("virt")
2. **Version** (offset 0x004): Expected `0x00000002` (virtio v1.0)
3. **Device ID** (offset 0x008): Expected `0x00000001` (network device)
4. **Vendor ID** (offset 0x00c): Expected `0x554D4551` (QEMU)
5. **Status Register** (offset 0x070): Test write access

## Files Modified

### CAmkES Configuration

**File:** `qemu-arm-virt/devices.camkes`

Added:
```c
/* Virtio-Net MMIO region */
#define VIRTIO_NET_MMIO_BASE 0x0a000000
#define VIRTIO_NET_MMIO_SIZE 0x200

vm0.dtb = dtb([
    {"path": "/pl011@9000000"},
    {"path": "/virtio_mmio@a000000"},  // NEW
]);

vm0.untyped_mmios = [
    "0x8040000:12",    // GIC
    "0x40000000:29",   // RAM
    "0x0a000000:12",   // Virtio-Net MMIO (NEW)
];
```

### FreeRTOS Source

**Files:**
- `/home/konton-otome/phd/freertos_vexpress_a9/Source/virtio_probe.c` - Probe test implementation
- `/home/konton-otome/phd/freertos_vexpress_a9/Source/main.c` - Updated to call `virtio_probe_test()`
- `/home/konton-otome/phd/freertos_vexpress_a9/build_debug.sh` - Updated to compile virtio_probe.c

## How to Build and Test

### Step 1: Build FreeRTOS with Virtio Probe

```bash
cd /home/konton-otome/phd/freertos_vexpress_a9
./build_debug.sh normal  # or 'debug' for memory debug version
```

This will:
- Compile `virtio_probe.c`
- Link it with FreeRTOS
- Generate `freertos_image.bin`
- Copy to both `vm_freertos` and `vm_freertos_net` directories

### Step 2: Build seL4 VM with Virtio Support

```bash
cd /home/konton-otome/phd/camkes-vm-examples
rm -rf build && mkdir build && cd build

# Use your working build method - example:
env PYTHONPATH=/home/konton-otome/phd/camkes-vm-examples/projects/camkes-tool:/home/konton-otome/phd/camkes-vm-examples/projects/capdl/python-capdl-tool \
cmake -G Ninja \
  -DCAMKES_VM_APP=vm_freertos_net \
  -DPLATFORM=qemu-arm-virt \
  -DSIMULATION=1 \
  -DLibUSB=OFF \
  -DSEL4_CACHE_DIR=/home/konton-otome/phd/camkes-vm-examples/.sel4_cache \
  -C ../projects/vm-examples/settings.cmake \
  ../projects/vm-examples

ninja
```

### Step 3: Run QEMU with Virtio-Net Device

**CRITICAL:** You must add `-netdev` and `-device virtio-net-device` to QEMU command!

```bash
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

## Expected Output

### Success Case (Virtio Device Detected)

```
========================================
   Virtio-Net MMIO Probe Test
========================================

Virtio MMIO Base: 0x0A000000

[1] Reading MMIO Magic Number...
    Expected: 0x74726976 ('virt')
    Reading from offset 0x000...
    Result: 0x74726976
    ✓ MAGIC NUMBER VALID!

[2] Reading Device Version...
    Expected: 0x00000002 (virtio v1.0) or 0x00000001 (legacy)
    Reading from offset 0x004...
    Result: 0x00000002
    ✓ Virtio v1.0 (modern)

[3] Reading Device ID...
    Expected: 0x00000001 (network device)
    Reading from offset 0x008...
    Result: 0x00000001
    ✓ NETWORK DEVICE DETECTED!

[4] Reading Vendor ID...
    Expected: 0x554D4551 (QEMU)
    Reading from offset 0x00c...
    Result: 0x554D4551
    ✓ QEMU Vendor ID Confirmed

[5] Testing Write Access...
    Writing 0 to STATUS register (offset 0x070) to reset device...
    Status after write: 0x00000000
    ✓ Write access confirmed (device reset)

========================================
   Probe Test Summary
========================================
✓✓✓ SUCCESS ✓✓✓
Virtio-Net device is accessible!
MMIO region is properly mapped to FreeRTOS guest.
You can now proceed to implement the virtio-net driver.
========================================
```

### Failure Case (No Virtio Device)

```
========================================
   Virtio-Net MMIO Probe Test
========================================

Virtio MMIO Base: 0x0A000000

[1] Reading MMIO Magic Number...
    Expected: 0x74726976 ('virt')
    Reading from offset 0x000...
    Result: 0xFFFFFFFF
    ✗ Read returned 0xFFFFFFFF (device not mapped or bus error)

...

========================================
   Probe Test Summary
========================================
✗✗✗ FAILED ✗✗✗
MMIO region is NOT accessible.
Possible causes:
  1. QEMU not started with -device virtio-net-device
  2. CAmkES devices.camkes missing virtio MMIO mapping
  3. Wrong MMIO address (check QEMU device tree)
  4. seL4 VMM not exposing virtio device to guest
========================================
```

## Troubleshooting

### Issue 1: Magic number reads as 0xFFFFFFFF

**Cause:** MMIO region not accessible (bus error)

**Solutions:**
1. Check QEMU command has `-device virtio-net-device`
2. Verify `devices.camkes` has `"0x0a000000:12"` in `untyped_mmios`
3. Check seL4 build didn't fail during AST generation

### Issue 2: Magic number reads as 0x00000000

**Cause:** Memory is accessible but no device present

**Solutions:**
1. Verify QEMU device tree has virtio device:
   ```bash
   qemu-system-aarch64 -M virt,dumpdtb=virt.dtb \
     -netdev user,id=net0 \
     -device virtio-net-device,netdev=net0 \
     -nographic
   dtc -I dtb -O dts virt.dtb -o virt.dts
   less virt.dts  # Look for virtio_mmio@a000000
   ```
2. Check if device is at different address (QEMU may vary)

### Issue 3: Device ID is not 0x1

**Cause:** Different virtio device at this address

**Solution:**
- Check QEMU device tree for multiple virtio devices
- First device might be console, network is second at `0x0a000200`

## Critical: VBAR (Vector Base Address Register) Setup

⚠️ **IMPORTANT:** For interrupt handling to work, you MUST relocate the interrupt vector table from 0x0!

### Why VBAR Relocation is Needed

By default, ARM processors expect interrupt vectors at address `0x00000000`. However:
1. Address 0x0 may not be writable in virtualized environment
2. FreeRTOS code starts at `0x40000000` (guest RAM base)
3. Without VBAR relocation, interrupts will jump to wrong addresses

### How to Configure VBAR

**In FreeRTOS startup code** (before enabling interrupts):

```c
// Define interrupt vector table location
extern uint32_t __vector_table_start__;  // From linker script

void setup_vbar(void) {
    uint32_t vector_table_addr = (uint32_t)&__vector_table_start__;

    // Set VBAR (Vector Base Address Register)
    __asm volatile("MCR p15, 0, %0, c12, c0, 0" : : "r"(vector_table_addr));

    uart_puts("VBAR set to: 0x");
    uart_put_hex(vector_table_addr);
    uart_puts("\r\n");
}
```

**Recommended vector table location:** `0x40010000` (64KB offset from RAM base)

### Update Required Files

1. **Linker Script** (`link.ld`):
```ld
SECTIONS {
    . = 0x40000000;  /* RAM base */

    .vectors : {
        __vector_table_start__ = .;
        KEEP(*(.vectors))
        . = ALIGN(4);
    }

    .text : {
        *(.text*)
    }
    /* ... rest of sections ... */
}
```

2. **Startup Assembly** (`startup.S`):
```asm
.section .vectors, "ax"
.align 5  /* 32-byte alignment required */

vector_table:
    LDR PC, =_reset_handler
    LDR PC, =_undefined_handler
    LDR PC, =_svc_handler
    LDR PC, =_prefetch_handler
    LDR PC, =_data_abort_handler
    NOP  /* Reserved */
    LDR PC, =_irq_handler        /* IRQ - virtio interrupts come here */
    LDR PC, =_fiq_handler
```

3. **Virtio IRQ Handler**:
```c
void _irq_handler(void) {
    // Read GIC to determine interrupt source
    uint32_t irq_num = /* read from GIC GICC_IAR */;

    if (irq_num == VIRTIO_NET_IRQ) {
        virtio_net_irq_handler();
    }

    // Acknowledge interrupt in GIC
}
```

### Testing VBAR Setup

Add to probe test:
```c
uint32_t vbar;
__asm volatile("MRC p15, 0, %0, c12, c0, 0" : "=r"(vbar));
uart_puts("Current VBAR: 0x");
uart_put_hex(vbar);
uart_puts("\r\n");
```

Expected output: `Current VBAR: 0x40010000` (or your chosen address)

## Next Steps

After successful probe test:

1. **✅ CRITICAL: Setup VBAR and Interrupt Vectors**
   - Relocate vector table to guest RAM
   - Implement IRQ handler skeleton
   - Test interrupt delivery

2. **Implement Virtio-Net Driver** (`NetworkInterface.c`)
   - Initialize virtqueues
   - Handle TX/RX operations
   - Implement interrupt handling (requires VBAR setup above)

3. **Integrate FreeRTOS+TCP Stack**
   - Port stack from official demo
   - Connect to virtio driver
   - Configure IP stack

4. **Test Network Connectivity**
   - DHCP
   - Ping
   - TCP echo client

## Architecture Reference

```
Host Network
    ↓
QEMU Network Backend (-netdev user)
    ↓
QEMU Virtio-Net Device (-device virtio-net-device)
    ↓ MMIO @ 0x0a000000
seL4 Microkernel (passes through MMIO region)
    ↓
seL4 VMM (libvirtio - future integration point)
    ↓ virtio protocol
FreeRTOS Guest (THIS PROBE TEST)
    ↓ (future: NetworkInterface.c driver)
FreeRTOS+TCP Stack
```

## Files in This Directory

- `README_VIRTIO_TEST.md` - This file
- `vm_minimal.camkes` - CAmkES composition (from vm_freertos)
- `qemu-arm-virt/devices.camkes` - **MODIFIED** with virtio MMIO region
- `qemu-arm-virt/freertos_image.bin` - FreeRTOS binary with probe test
- `CMakeLists.txt` - Build configuration
- `settings.cmake` - Platform settings

## Related Documentation

- [FreeRTOS Network Integration Analysis](../../../../research-docs/freertos-network-integration-analysis.md)
- [FreeRTOS Network Pass-Through Architecture](../../../../research-docs/freertos-network-passthrough-architecture.md)
- [Virtio Specification](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html)

## Status

- ✅ CAmkES configuration updated
- ✅ FreeRTOS probe test created
- ✅ Build script updated
- ⏳ Awaiting test execution
- ⏳ Full virtio-net driver implementation (future)
