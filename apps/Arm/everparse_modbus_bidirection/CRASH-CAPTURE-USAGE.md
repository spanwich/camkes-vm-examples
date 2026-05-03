# Crash Capture Usage Guide

## Quick Start for Week-Long Testing

### 1. Run with Automatic Crash Capture (Recommended)

```bash
cd /home/iamfo470/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc

# Run with crash monitoring (no performance overhead)
./scripts/run-with-crash-capture.sh
```

**What it does:**
- ✅ Monitors serial output for crashes
- ✅ Auto-dumps QEMU memory when crash detected
- ✅ Saves complete logs with timestamps
- ✅ Creates crash summary file
- ✅ **Zero performance overhead** - runs at full speed

### 2. Run with GDB Debugging (Advanced)

```bash
# Enable GDB server (slight performance overhead)
./scripts/run-with-crash-capture.sh --gdb

# In another terminal, attach GDB:
arm-none-eabi-gdb -x scripts/crash-gdb-monitor.gdb
```

**What it does:**
- ✅ Everything from option 1
- ✅ Plus: GDB attached for source-level debugging
- ✅ Breaks at exact crash location
- ✅ Shows stack traces, local variables
- ⚠️  ~5-10% performance overhead

## After a Crash Occurs

### Files Generated

The crash capture system creates:

```
/home/iamfo470/phd/logs/modbus-gateway/
├── gateway-20251020_143022.log              # Full session log
├── crash-memory-20251020_143022.dump        # QEMU RAM dump (2GB)
├── crash-registers-20251020_143022.txt      # Register state
└── CRASH-SUMMARY-20251020_143022.txt        # Analysis starting point
```

### Step 1: Read Crash Summary

```bash
cat /home/iamfo470/phd/logs/modbus-gateway/CRASH-SUMMARY-*.txt
```

This gives you:
- Crash timestamp
- PC address
- Fault address
- Last 50 log lines before crash

### Step 2: Find Exact Crash Location

#### Method A: From PC Address (Fast)

```bash
# Example: PC = 0x38308

# Disassemble Net0 binary
arm-none-eabi-objdump -d \
    /home/iamfo470/phd/camkes-vm-examples/build_modbus/CMakeFiles/net0_drv.instance.bin.dir/net0_drv.instance.bin \
    | grep -A 20 "38308:"

# Or search all binaries
find build_modbus -name "*.o" -exec \
    arm-none-eabi-objdump -d {} \; | grep -B 5 -A 10 "38308:"
```

#### Method B: With Symbol Files (Accurate)

```bash
arm-none-eabi-addr2line -e \
    build_modbus/CMakeFiles/net0_drv.instance.bin.dir/net0_drv.instance.bin \
    -f -p -a 0x38308
```

This shows:
- Source file name
- Function name
- Line number

### Step 3: Analyze Memory Dump (Advanced)

#### Extract Connection Table from Memory

```python
#!/usr/bin/env python3
# analyze-crash-dump.py

import struct

# Load memory dump
with open('/home/iamfo470/phd/logs/modbus-gateway/crash-memory-20251020_143022.dump', 'rb') as f:
    memory = f.read()

# Find connection_table pattern
# (This requires knowing the memory layout - see below)

# Example: Search for PCB pointers
for offset in range(0, len(memory), 4):
    ptr = struct.unpack('<I', memory[offset:offset+4])[0]
    if 0x1a0000 <= ptr <= 0x1b0000:  # Known PCB range
        print(f"Potential PCB at offset 0x{offset:x}: 0x{ptr:x}")
```

#### Alternative: Use QEMU Monitor Live

If crash hasn't occurred yet but you want to inspect state:

```bash
# Connect to monitor
socat - UNIX-CONNECT:/tmp/qemu-monitor-*.sock

# Commands:
(qemu) info registers
(qemu) x/32xw 0x38308          # Memory at PC
(qemu) x/100xw 0x1fadf0        # Stack dump
(qemu) info mtree              # Memory map
```

## Crash Analysis Checklist

When analyzing a crash:

- [ ] 1. Read CRASH-SUMMARY file
- [ ] 2. Note PC address and fault address
- [ ] 3. Disassemble PC location to see instruction
- [ ] 4. Check fault address offset:
  - 0x0-0x4: NULL pointer dereference
  - 0x10: Likely `pcb->callback_arg` or `pcb->state`
  - 0x14-0x20: Other PCB fields
- [ ] 5. Find which component crashed (net0_drv vs net1_drv)
- [ ] 6. Look at register values (R0-R12, SP, LR, PC)
- [ ] 7. Check last 100 lines before crash in log
- [ ] 8. Look for patterns:
  - "PLC closed connection" before crash?
  - "metadata not found" warnings?
  - Connection count anomalies?
- [ ] 9. Check breadcrumbs (B1000-B3013)
- [ ] 10. Correlate with source code

## Common Crash Patterns

### Pattern 1: Crash at 0x10 (This Bug)

```
FAULT HANDLER: data fault from net0_drv on address 0x10
PC: 0x38308
```

**Likely cause**: Accessing `pcb->state` or `pcb->callback_arg` after PCB freed

**Look for**:
- Recent "PLC closed connection" message
- Net1 set PCB to NULL
- Net0 didn't see the NULL (cache issue)

### Pattern 2: Crash at 0x0

```
FAULT HANDLER: data fault on address 0x0
```

**Likely cause**: Direct NULL pointer dereference

**Look for**:
- Missing NULL checks
- `meta->pcb` being NULL

### Pattern 3: Stack Overflow

```
FAULT HANDLER: data fault on address 0x1fb000
SP: 0x1fafff
```

**Likely cause**: Stack overflow (recursion or large locals)

**Look for**:
- Deep call chains
- Large arrays on stack

## Preventive Logging

To make future crashes easier to debug, add to your code:

```c
// Before ANY PCB access:
if (meta == NULL) {
    printf("DIAGNOSTIC: meta is NULL at %s:%d\n", __FILE__, __LINE__);
    return;
}

if (meta->pcb == NULL) {
    printf("DIAGNOSTIC: PCB is NULL at %s:%d\n", __FILE__, __LINE__);
    printf("DIAGNOSTIC: Connection: %u.%u.%u.%u:%u\n",
           (meta->original_src_ip >> 24) & 0xFF,
           /* ... */);
    return;
}

// Only then safe to use meta->pcb
```

## Long-Running Test Recommendations

For your week-long test:

1. **Use production mode** (not GDB, for performance)
   ```bash
   ./scripts/run-with-crash-capture.sh
   ```

2. **Run in screen/tmux** so it survives SSH disconnects
   ```bash
   screen -S modbus-test
   ./scripts/run-with-crash-capture.sh
   # Ctrl+A, D to detach

   # Reattach later:
   screen -r modbus-test
   ```

3. **Monitor disk space** (logs can get large)
   ```bash
   # Check every few hours
   du -sh /home/iamfo470/phd/logs/modbus-gateway/
   ```

4. **Set up log rotation** (optional)
   ```bash
   # Keep only last 7 days
   find /home/iamfo470/phd/logs/modbus-gateway/ -mtime +7 -delete
   ```

## Questions?

If crash happens, send me:
1. CRASH-SUMMARY file
2. Last 200 lines from main log
3. PC and fault address
4. I can help analyze!

---

**Version**: v1.0 (2025-10-20)
**Author**: Crash capture system for modbus_bidirection_poc
