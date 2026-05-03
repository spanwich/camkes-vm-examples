# GDB Catch-All Fault Debugging Guide

**Date**: 2025-10-20
**Version**: v2.101 Enhanced Debugging

---

## Overview

This guide explains how to catch **ANY** memory fault in seL4, not just specific addresses. The key insight is that seL4 memory virtualization means fault addresses can be anywhere - we need to catch faults at the **kernel level** and at the **faulting PC**.

## The Problem with Watchpoint-Only Debugging

**Why watching address 0x10 isn't enough:**

```
Traditional approach:
  watch *(int*)0x10   ← Only catches access to 0x10

Problem:
  - Fault address depends on NULL pointer offset
  - Different code paths access different offsets
  - seL4 virtual memory means addresses can be remapped
  - We need to catch the fault BEFORE it reaches seL4's handler
```

## Multi-Level Fault Catching Strategy

The new `gdb-catch-all-faults.txt` configuration catches faults at **3 levels**:

### Level 1: Kernel Data Abort Handler

```gdb
break *0xe0010274    # c_handle_data_fault()
```

**What it does:**
- Catches **EVERY** data abort in the system
- Breaks **before** seL4 prints "FAULT HANDLER:"
- Shows the faulting PC in the LR register
- Works for any fault address (0x10, 0x20, 0x1000, etc.)

**Why it works:**
- ARM CPU catches data abort → jumps to kernel exception vector
- seL4 kernel's `c_handle_data_fault()` is called
- GDB intercepts this function call
- We can inspect exact fault context

### Level 2: Application Faulting PC

```gdb
break *0x39868    # Known crash location in tcp_output
```

**What it does:**
- Breaks at the exact instruction that will fault
- Shows register state **before** the crash
- Allows single-stepping to see the fault happen

**Why it works:**
- We know from crash logs that PC=0x39868 causes the fault
- Breaking here lets us see register values before the NULL dereference
- Can examine memory around the fault

### Level 3: NULL Region Watchpoints

```gdb
watch *(int*)0x0
watch *(int*)0x10
watch *(int*)0x20
```

**What it does:**
- Hardware watchpoints on common NULL+offset addresses
- Catches read/write attempts to these addresses

**Limitations:**
- ARM only supports 2-4 hardware watchpoints
- Won't catch offsets beyond what we watch
- Level 1 (kernel breakpoint) is more comprehensive

---

## How to Use

### Step 1: Terminal 1 - Start QEMU with GDB Server

```bash
cd /home/qemu/phd/camkes-vm-examples/build_modbus
./run-remote-gdb.sh
```

**What happens:**
- QEMU starts with `-s -S` flags
- GDB server listens on localhost:1234
- Execution paused, waiting for GDB to connect
- Network configured (tap0, tap1)

**Expected output:**
```
Starting QEMU with GDB server on localhost:1234...
Waiting for GDB connection...
(QEMU waiting)
```

### Step 2: Terminal 2 - Launch GDB with Fault Catching

```bash
cd /home/qemu/phd/camkes-vm-examples/build_modbus
./debug-catch-all.sh
```

**What happens:**
- GDB loads kernel symbols (`kernel/kernel.elf`)
- Loads application image (`capdl-loader-image-arm-qemu-arm-virt`)
- Sets breakpoints on all fault handlers
- Connects to QEMU's GDB server
- Displays active breakpoints

**Expected output:**
```
╔════════════════════════════════════════════════════════════╗
║  seL4 CATCH-ALL FAULT DEBUGGER                             ║
╚════════════════════════════════════════════════════════════╝

Configuration:
  ✓ ARM architecture configured
  ✓ Logging to gdb-fault-log.txt
  ✓ Catching ALL signals and exceptions
  ✓ Breakpoint at 0x39868 (known crash PC)
  ✓ Breakpoints on seL4 fault handlers
  ✓ Watchpoints on NULL region

Active Breakpoints:
Num     Type           Disp Enb Address    What
1       breakpoint     keep y   0x00039868
2       breakpoint     keep y   0xe0010274 c_handle_data_fault
3       breakpoint     keep y   0xe001e3ec handleFault
4       breakpoint     keep y   0xe001e4a8 handleUserLevelFault
5       breakpoint     keep y   0xe0013790 handleVMFault
6       hw watchpoint  keep y              *0x0
7       hw watchpoint  keep y              *0x10
8       hw watchpoint  keep y              *0x20

Ready! Type 'continue' to start execution.
GDB will stop BEFORE any memory fault occurs.
```

### Step 3: Start Execution

In GDB, type:
```gdb
(gdb) continue
```

or just:
```gdb
(gdb) c
```

### Step 4: Wait for Fault

When a fault occurs, GDB will **stop automatically** and display:

```
╔═══════════════════════════════════════════════════════╗
║  KERNEL: c_handle_data_fault() CALLED                ║
║  This is THE data abort handler in seL4 kernel       ║
╚═══════════════════════════════════════════════════════╝

Fault address register (DFAR):
r0             0x26d5      9941
r1             0x0         0
r2             0x0         0
r3             0x0         0      ← NULL pointer!
...
lr             0x39868     0x39868 <tcp_output+1948>   ← Faulting PC

Faulting instruction:
   0x39868 <tcp_output+1948>:  ldr  r3, [r3, #16]    ← Derefs r3+16 = 0+16 = 0x10

┌─── Program Counter ───────────────────────────────────────┐
  PC = 0xe0010274  (kernel fault handler)

┌─── All Registers ─────────────────────────────────────────┐
r0             0x26d5      9941
r1             0x0         0
r2             0x0         0
r3             0x0         0        ← This is the NULL that caused the crash!
r4             0x26d5      9941
...
lr             0x39868     0x39868  ← Return address = faulting instruction
pc             0xe0010274  0xe0010274 <c_handle_data_fault>

┌─── Call Stack ────────────────────────────────────────────┐
#0  c_handle_data_fault () at ...
#1  0x39868 in tcp_output () at tcp_out.c:1234
#2  0xb8a4 in inbound_tcp_connected_callback () at virtio_net1_driver.c:2504
...
```

---

## What to Do When Fault is Caught

### 1. Examine the Faulting Instruction

```gdb
(gdb) x/10i $lr
```

This shows the instruction that caused the fault (saved in Link Register).

**Example output:**
```
   0x39868 <tcp_output+1948>:  ldr  r3, [r3, #16]   ← Fault here: r3=NULL
   0x3986c <tcp_output+1952>:  ldrb r2, [r3, #4]
   ...
```

### 2. Check Which Register is NULL

```gdb
(gdb) find-null-deref
```

**Example output:**
```
=== Searching for NULL dereference ===

r0 = 0x26d5
r1 = 0x0 [NULL]
r2 = 0x0 [NULL]
r3 = 0x0 [NULL]    ← This one caused the fault!
r4 = 0x26d5
...
```

### 3. Dump Full Fault Context

```gdb
(gdb) dump-fault-context
```

**This shows:**
- All registers
- 30 instructions around faulting PC
- Stack dump (128 bytes)
- Local variables
- Function arguments

### 4. Examine Memory Around Fault Site

```gdb
# Try to read where the code attempted to access
(gdb) print/x $r3
$1 = 0x0

(gdb) print/x $r3 + 16
$2 = 0x10    ← This is why we see "fault at address 0x10"!

# Can't read 0x10 (invalid), but we can see the pattern:
(gdb) x/4xw 0x0
0x0:  Cannot access memory at address 0x0
```

### 5. Trace Back to Root Cause

```gdb
# Move up the call stack
(gdb) backtrace 20

# Examine each frame
(gdb) frame 1    # tcp_output
(gdb) info locals
(gdb) info args

(gdb) frame 2    # inbound_tcp_connected_callback
(gdb) info locals
(gdb) info args
```

**Look for:**
- Which variable is NULL that shouldn't be?
- What condition should have prevented this path?
- Where should the pointer have been initialized?

### 6. Continue or Quit

```gdb
# Continue execution (will likely crash again)
(gdb) continue

# Quit GDB
(gdb) quit
```

---

## Understanding the Output

### When GDB Catches the Fault

**You'll see MULTIPLE breakpoint hits:**

1. **First**: Application-level breakpoint (PC 0x39868)
   - This is the instruction about to fault
   - Registers show NULL pointers
   - Can single-step to see the fault happen

2. **Second**: Kernel-level breakpoint (c_handle_data_fault)
   - seL4 kernel is handling the fault
   - Fault address and FSR (Fault Status Register) visible
   - PC shows where the fault originated (from LR)

### Register Analysis

**Key registers to check:**

- **PC**: Program Counter (where we are now - in kernel if caught at level 1)
- **LR**: Link Register (where we came from - the faulting instruction if in kernel)
- **SP**: Stack Pointer (for stack dumps)
- **r0-r12**: General purpose (look for NULL values)

**ARM Fault Registers:**

- **DFAR**: Data Fault Address Register (exact address that was accessed)
- **DFSR**: Data Fault Status Register (why the fault occurred)

---

## Advanced: Catching Specific Memory Regions

If you want to catch access to a specific memory range:

```gdb
# Watch a specific address range (requires hardware watchpoints)
watch -l *(int*)0x1000
watch -l *(int*)0x2000

# Conditional breakpoint (software, no limit)
break tcp_output if $r3 == 0
```

---

## Troubleshooting

### Problem: "Cannot access memory at address 0x..."

**Cause**: Trying to read invalid memory (expected for faults!)

**Solution**: This is normal - you're debugging a memory fault. Focus on:
- Which register held the invalid address?
- Where did that register value come from?
- What should have initialized it?

### Problem: Breakpoint not hit

**Possible causes:**

1. **QEMU not waiting for GDB**
   - Check QEMU was started with `./run-remote-gdb.sh`
   - Look for "Waiting for GDB connection..."

2. **Address changed after rebuild**
   - Re-check crash log for latest PC address
   - Update breakpoint: `break *0x<new-address>`

3. **Crash happens elsewhere**
   - Good! The kernel breakpoint will still catch it
   - Check which breakpoint was hit

### Problem: Too many breakpoint hits (kernel breakpoint)

**Cause**: `c_handle_data_fault` catches EVERY data abort, including:
- Page faults (normal during startup)
- Permissions faults (normal for seL4)
- Our actual crash

**Solution**: Add condition:

```gdb
# Only break if faulting PC is in our component
break *0xe0010274 if ($lr > 0x8000 && $lr < 0x100000)
```

---

## Files Created

- **gdb-catch-all-faults.txt**: GDB command script with all breakpoints
- **debug-catch-all.sh**: Launcher script for easy use
- **gdb-fault-log.txt**: Automatic log of all GDB output (created when GDB runs)

---

## Summary

**Old approach** (limited):
```
watch *(int*)0x10   → Only catches access to 0x10
```

**New approach** (comprehensive):
```
1. break *0xe0010274       → Catches ALL data aborts in kernel
2. break *0x39868          → Catches specific known crash PC
3. watch *(int*)0x10       → Catches common NULL offsets
```

**Result**: **ANY** memory fault will be caught and analyzed, regardless of:
- Fault address (0x10, 0x20, 0x1000, etc.)
- seL4 virtual memory remapping
- Which component crashes
- What code path triggered it

The system will stop **before** seL4 prints "FAULT HANDLER:", giving you complete access to fault context, registers, and memory state.

---

**Next Steps**: Try running with v2.101 to see if the tcp_output fix prevents the crash. If it still crashes, GDB will catch it with full context!
