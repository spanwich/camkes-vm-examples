# Session Continuation Prompt - October 25, 2025
## ICS Dual-NIC Gateway: SCADA-PLC Communication Debugging

**Session Date**: 2025-10-25
**Current Version**: v2.157 (ready to build and test)
**Status**: Code changes complete, awaiting build and test
**Location**: `/home/qemu/phd/camkes-vm-examples/build_modbus`

---

## Executive Summary

We're debugging an seL4-based ICS (Industrial Control System) bidirectional gateway that bridges SCADA (192.168.1.x) and PLC (192.168.2.x) networks using two separate lwIP network stacks.

**Current Problem**: SCADA can communicate with PLC for a brief period, then communication breaks with pbuf double-free assertion crash.

**Root Cause Identified**: Net0 driver (SCADA-facing) has 6 manual `pbuf_free()` calls in its TCP receive callback, violating lwIP ownership rules. lwIP also frees these pbufs, causing double-free crashes.

**Fix Implemented**: v2.157 removes all 6 manual `pbuf_free()` calls from Net0's `tcp_echo_recv()` callback, matching the fixes we applied to Net1 in v2.155.

---

## Recent Progress Timeline

### v2.154 (October 24)
- **Problem**: Dual critical bugs blocking communication
  1. pbuf double-free (Net1 driver)
  2. CAmkES event system failure (91% packet loss)
- **Fix**: RST deduplication race condition
- **Analysis**: Documented in `/home/qemu/phd/research-docs/v2.154-dual-critical-bugs-analysis.md`

### v2.155 (October 24)
- **Problem**: Net1 driver manually calls `pbuf_free()` 8 times in recv callback
- **Fix**: Removed all 8 manual `pbuf_free()` calls from Net1 driver
- **Result**: Net1 now correctly lets lwIP handle pbuf lifecycle
- **Analysis**: Documented in `/home/qemu/phd/research-docs/v2.155-lwip-best-practices-audit.md`

### v2.156 (October 25)
- **Problem**: lwIP PCB (Protocol Control Block) management violations
  - Net1 called `tcp_abort()` from recv callback (line 2563)
  - Net1 called `tcp_abort()` from event handler (line 3156)
- **Fix**:
  - Recv callback: Return `ERR_ABRT` instead of calling `tcp_abort()`
  - Event handler: NULL all callbacks first, then use `tcp_close()` with `tcp_abort()` fallback
- **User Feedback**: "This fix the error that blocks the communication between SCADA and PLC"
- **Test Result**: Communication works briefly, then breaks (same symptom as before)
- **Documentation**: Added lwIP best practices to README.md and CLAUDE.md

### v2.157 (October 25 - CURRENT)
- **Problem Discovered**: Net0 driver STILL has 6 manual `pbuf_free()` calls (we only fixed Net1!)
- **Root Cause**: pbuf double-free assertion crashes Net0's recv callback
- **Fix Applied**: Removed all 6 `pbuf_free()` calls from Net0's `tcp_echo_recv()` callback
- **Status**: Code changes complete, ready to build and test
- **Analysis**: Documented in `/home/qemu/phd/research-docs/v2.156-net0-pbuf-double-free-analysis.md`

---

## Architecture Overview

```
SCADA (192.168.1.100)
    ↓
[Net0 Driver - lwIP Stack #1]
    ↓ (dataport + event notification)
[Net1 Driver - lwIP Stack #2]
    ↓
PLC (192.168.2.100)
```

### Component Details

**VirtIO_Net0_Driver**:
- SCADA-facing network driver (192.168.1.x)
- lwIP stack listening on 192.168.1.1:502 (Modbus TCP)
- Receives SCADA requests, forwards to Net1 via dataport
- Receives PLC responses from Net1, forwards to SCADA
- **Current Issue**: 6 manual `pbuf_free()` calls in `tcp_echo_recv()` callback

**VirtIO_Net1_Driver**:
- PLC-facing network driver (192.168.2.x)
- lwIP stack with client connections to PLC at 192.168.2.100:502
- Receives SCADA requests from Net0, forwards to PLC
- Receives PLC responses, forwards to Net0 via dataport
- **Status**: Fixed in v2.155 and v2.156 (no pbuf violations)

**Communication Flow**:
1. SCADA request → Net0 recv callback
2. Net0 writes to `inbound_dataport`, emits `inbound_ready` event
3. Net1 `inbound_ready_handle()` reads dataport, forwards to PLC
4. PLC response → Net1 recv callback
5. Net1 writes to `outbound_dataport`, emits `outbound_ready` event
6. Net0 `outbound_ready_handle()` reads dataport, forwards to SCADA

---

## Critical lwIP Rules (MUST FOLLOW!)

### Rule 1: NEVER Call pbuf_free() From Recv Callback

**Why**: lwIP's `tcp_input()` ALWAYS frees the pbuf after recv callback returns (tcp_in.c:596-600):

```c
/* lwIP tcp_in.c:596-600 */
if (inseg.p != NULL) {
    pbuf_free(inseg.p);  // lwIP owns this!
    inseg.p = NULL;
}
```

**Correct recv callback pattern**:
```c
static err_t recv_callback(void *arg, struct tcp_pcb *pcb,
                           struct pbuf *p, err_t err) {
    // Error handling
    if (err != ERR_OK) {
        return err;  // ✅ NO pbuf_free()!
    }

    // Connection closed
    if (p == NULL) {
        // Clean up application state
        return ERR_ABRT;  // ✅ lwIP handles tcp_abort() and cleanup
    }

    // Normal processing
    // ... copy data from pbuf ...

    return ERR_OK;  // ✅ lwIP frees pbuf automatically!
}
```

### Rule 2: NEVER Call tcp_abort() From Callbacks

**Why**: PCB may still be in use by lwIP after callback returns.

**Correct pattern**:
- Return `ERR_ABRT` from callback
- lwIP calls `tcp_abort()` internally after callback completes

### Rule 3: Error Callback - PCB Already Freed

**Critical**: When error callback is called, lwIP has ALREADY freed the PCB!

```c
static void error_callback(void *arg, err_t err) {
    // ✅ DO: Clean up application state
    if (meta != NULL) {
        meta->pcb = NULL;
        meta->active = false;
    }

    // ❌ DO NOT: Call tcp_close() or tcp_abort()
    // ❌ DO NOT: Access PCB in any way
}
```

### Rule 4: Safe Cleanup From Main Thread (Event Handler)

**Pattern**: NULL all callbacks before closing:

```c
// From main thread (event handler)
tcp_arg(pcb, NULL);
tcp_recv(pcb, NULL);
tcp_sent(pcb, NULL);
tcp_err(pcb, NULL);
tcp_poll(pcb, NULL, 0);

err_t err = tcp_close(pcb);
if (err != ERR_OK) {
    tcp_abort(pcb);  // Safe now - callbacks NULL
}
```

---

## Build and Test Workflow

### 1. Build System

```bash
# Location
cd /home/qemu/phd/camkes-vm-examples/build_modbus

# Activate Python venv (REQUIRED!)
source /home/qemu/phd/sel4-dev-env/bin/activate

# Build
ninja 2>&1 | tee /tmp/v2.157-build.log

# Check for errors
tail -50 /tmp/v2.157-build.log
```

**Common build issues**:
- Forgot to activate venv → `ModuleNotFoundError: No module named 'aenum'`
- Solution: `source /home/qemu/phd/sel4-dev-env/bin/activate`

### 2. Test Execution

```bash
# Run test with crash dump script
./dump-crash.sh v2.157-test

# Script does:
# 1. Starts QEMU with GDB
# 2. Runs for ~90 seconds
# 3. Captures console output
# 4. Generates crash summary if crash detected
```

**Test environment**:
- SCADA emulator: ScadaBR (docker container)
- PLC emulator: OpenPLC (docker container)
- Network: Two TAP interfaces (tap0 for SCADA, tap1 for PLC)

### 3. Log Analysis

**Primary log location**: `logs/console-YYYYMMDD-HHMMSS.log`

**Crash dumps** (if crash occurred): `crash-dumps/vX.XXX-test-crash-summary-YYYYMMDD-HHMMSS.txt`

---

## Log Analysis Methods

### Method 1: Breadcrumb Tracing

**Purpose**: Track execution flow and packet counts

**Key breadcrumbs**:
```bash
B9003  # Net0 recv callback entry (SCADA request received)
B9004  # Net0 about to emit inbound_ready event
B2000  # Net1 inbound_ready_handle entry (request processing)
B2001  # Net1 processing request from dataport
B3000  # Net0 outbound_ready_handle entry (response processing)
B3001  # Net0 processing response from dataport
```

**Analysis commands**:
```bash
# Count breadcrumbs
grep "B9003" logs/console-20251025-004750.log | wc -l  # SCADA requests received
grep "B2000" logs/console-20251025-004750.log | wc -l  # Net1 processed requests
grep "B3000" logs/console-20251025-004750.log | wc -l  # Net0 processed responses

# Calculate packet loss
# Formula: (received - processed) / received * 100%
```

**Example analysis** (from v2.156 test):
```bash
B9003 count: 20  # Net0 received 20 SCADA requests
B2000 count: 6   # Net1 processed 6 requests
B3000 count: 6   # Net0 sent 6 responses

Packet loss: (20-6)/20 = 70%
```

### Method 2: Assertion Analysis

**Pattern**: When lwIP assertion fires:
```
Assertion "condition" failed at line XXX in file.c
```

**Common assertions**:

1. **`Assertion "p != NULL" failed at line 732 in pbuf.c`**
   - Cause: Double-free (pbuf freed twice)
   - Root cause: Application calls `pbuf_free()` + lwIP also calls `pbuf_free()`

2. **`Assertion "pbuf_free: p->ref > 0" failed at line 753 in pbuf.c`**
   - Cause: Reference count already zero
   - Root cause: pbuf freed more times than allocated

3. **`Assertion "tcp_output: pcb != NULL" failed at line 1389 in tcp_out.c`**
   - Cause: Accessing freed PCB
   - Root cause: PCB freed but application still has pointer

**Analysis steps**:
1. Find assertion message in log
2. Look at context before assertion (last 20-50 lines)
3. Check breadcrumbs to identify code path
4. Use `grep` to find relevant debug messages

### Method 3: Connection Tracking

**Debug messages** (example):
```
VirtIO_Net0_Driver: Connection count: 375  # Zombie connections
VirtIO_Net1_Driver: Connection count: 2    # Active connections
```

**Analysis**:
- Large discrepancy → connection leak
- Connections not being cleaned up → error notifications not processed

### Method 4: pbuf Reference Count Tracking

**v2.157 debug output** (example):
```
VirtIO_Net1_Driver: [DEBUG] Freeing pbuf at NORMAL path: p=0x86a338, ref=1, len=35
[LWIP] tcp_in.c:577 - About to pbuf_free(inseg.p=0x86a338, ref=1)
[LWIP] tcp_in.c:577 - About to pbuf_free(inseg.p=0x86a338, ref=0)  ← DOUBLE FREE!
Assertion "pbuf_free: p->ref > 0" failed at line 753 in pbuf.c
```

**Red flag**: Same pbuf pointer freed twice with ref count going to 0 before second free.

---

## Tools and Scripts

### dump-crash.sh

**Purpose**: Run test, capture output, analyze crash

**Usage**:
```bash
./dump-crash.sh <version-tag>
# Example: ./dump-crash.sh v2.157-test
```

**Output files**:
- `logs/console-YYYYMMDD-HHMMSS.log` - Full console output
- `crash-dumps/<version>-crash-summary-YYYYMMDD-HHMMSS.txt` - Crash analysis (if crash detected)
- `crash-dumps/<version>-gdb-console-YYYYMMDD-HHMMSS.log` - GDB output
- `crash-dumps/<version>-qemu-console-YYYYMMDD-HHMMSS.log` - QEMU console output

**What script does**:
1. Starts tmux session with QEMU and GDB
2. Runs test for ~90 seconds
3. Captures all output
4. Checks for crashes/assertions
5. Generates summary report

### start-persistent-debug.sh

**Purpose**: Manual debugging with persistent tmux session

**Usage**:
```bash
./start-persistent-debug.sh
# Attach to session: tmux attach -t modbus-debug
```

**Tmux layout**:
- Pane 0: QEMU console
- Pane 1: GDB console

### GDB Commands (When Debugging)

```bash
# In GDB pane
(gdb) c                    # Continue execution
(gdb) bt                   # Backtrace after crash
(gdb) info registers       # Register dump
(gdb) x/10x $sp            # Examine stack
(gdb) break tcp_input      # Breakpoint on lwIP function
```

---

## File Locations

### Source Code
```
/home/qemu/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc/
├── components/
│   ├── VirtIO_Net0_Driver/
│   │   └── virtio_net0_driver.c  (v2.157 - pbuf fixes applied)
│   ├── VirtIO_Net1_Driver/
│   │   └── virtio_net1_driver.c  (v2.156 - already fixed)
│   └── include/
│       ├── common.h              (ICS message structures)
│       ├── control_queue.h       (Lock-free SPSC queue)
│       └── connection_state.h    (Dataport state sharing)
├── ics_dual_nic.camkes          (CAmkES assembly)
└── README.md                     (lwIP best practices documented)
```

### Build Output
```
/home/qemu/phd/camkes-vm-examples/build_modbus/
├── images/
│   └── capdl-loader-image-arm-qemu-arm-virt  (Final bootable image)
├── logs/                         (Console logs)
├── crash-dumps/                  (Crash analysis reports)
├── dump-crash.sh                 (Test script)
└── start-persistent-debug.sh     (Manual debug script)
```

### Research Documentation
```
/home/qemu/phd/research-docs/
├── v2.154-dual-critical-bugs-analysis.md
├── v2.155-lwip-best-practices-audit.md
└── v2.156-net0-pbuf-double-free-analysis.md
```

### Configuration
```
/home/qemu/phd/CLAUDE.md          (Critical lessons documented)
/home/qemu/phd/sel4-dev-env/      (Python venv - must activate!)
```

---

## Next Steps (What to Do Now)

### Step 1: Build v2.157

```bash
cd /home/qemu/phd/camkes-vm-examples/build_modbus
source /home/qemu/phd/sel4-dev-env/bin/activate
ninja 2>&1 | tee /tmp/v2.157-build.log
```

**Expected result**: Clean build, no errors

**If build fails**:
- Check `/tmp/v2.157-build.log` for errors
- Common issue: Forgot to activate venv

### Step 2: Test v2.157

```bash
./dump-crash.sh v2.157-test
```

**Wait ~90 seconds** for test to complete.

### Step 3: Analyze Results

**Success criteria**:
```bash
# Check for pbuf assertions
grep "pbuf.*failed" logs/console-*.log
# Should find: NOTHING (0 results)

# Check breadcrumb counts
grep "B9003" logs/console-*.log | wc -l  # SCADA requests
grep "B2000" logs/console-*.log | wc -l  # Net1 processed
grep "B3000" logs/console-*.log | wc -l  # Responses sent

# Success: B9003 ≈ B2000 ≈ B3000 (minimal packet loss)
```

**If pbuf assertion STILL occurs**:
1. Check which component crashed (Net0 or Net1)
2. Search for remaining `pbuf_free()` calls:
   ```bash
   grep -n "pbuf_free(" components/VirtIO_Net*/virtio_net*_driver.c
   ```
3. Analyze crash context in log

**If no crash but high packet loss**:
- This is the **CAmkES event system bug** (separate issue)
- Event notifications not triggering handlers reliably
- Will need to investigate event wiring or implement polling

### Step 4: Document Results

Create test report:
```bash
/home/qemu/phd/research-docs/v2.157-test-results.md
```

Include:
- Breadcrumb counts
- Crash status (yes/no)
- Assertion messages (if any)
- Duration of successful communication
- Packet loss percentage

---

## Known Issues and Workarounds

### Issue 1: CAmkES Event System (70-91% Packet Loss)

**Symptom**:
- Net0 emits `inbound_ready` event 23 times
- Net1 `inbound_ready_handle()` only called 2-6 times
- Requests sit in dataport unprocessed

**Root cause**: seL4Notification edge-triggered, multiple signals coalesce

**Status**: NOT YET FIXED (separate from pbuf issue)

**Potential solutions**:
1. Fix event wiring in `ics_dual_nic.camkes`
2. Implement polling loop in main thread
3. Hybrid approach: Events + periodic polling to drain queue

### Issue 2: pbuf Double-Free (v2.157 Should Fix This!)

**Symptom**:
```
Assertion "p != NULL" failed at line 732 in pbuf.c
```

**Root cause**: Manual `pbuf_free()` calls in recv callbacks

**Fixed in**:
- v2.155: Net1 driver (8 calls removed)
- v2.157: Net0 driver (6 calls removed)

**Verification**: `grep "pbuf_free" components/*/virtio_net*_driver.c` should show only:
- Netif input callbacks (we own pbuf) ✅
- Error paths in process_rx_packets (we own pbuf) ✅
- NO calls in TCP recv callbacks ✅

---

## Critical Debug Queries

### Quick Health Check

```bash
# Latest log file
ls -lt logs/console-*.log | head -1

# Check for crashes
grep -i "assert\|fault\|crash" logs/console-*.log

# Breadcrumb summary
echo "B9003 (Net0 recv):" $(grep "B9003" logs/console-*.log | wc -l)
echo "B2000 (Net1 process):" $(grep "B2000" logs/console-*.log | wc -l)
echo "B3000 (Net0 send):" $(grep "B3000" logs/console-*.log | wc -l)

# Connection counts (check for leaks)
grep "Connection count:" logs/console-*.log | tail -10
```

### Detailed pbuf Analysis

```bash
# Find all pbuf_free calls in drivers
grep -n "pbuf_free(" components/VirtIO_Net0_Driver/virtio_net0_driver.c
grep -n "pbuf_free(" components/VirtIO_Net1_Driver/virtio_net1_driver.c

# Check for pbuf debug messages
grep "pbuf.*ref=" logs/console-*.log

# Find pbuf assertions
grep "pbuf.*failed" logs/console-*.log
```

### CAmkES Event Debug

```bash
# Check emit counts
grep "About to emit" logs/console-*.log | wc -l

# Check handler invocations
grep "_handle\(\)" logs/console-*.log | wc -l

# Queue depth (if messages queued but not processed)
grep "queue.*depth\|queue.*size" logs/console-*.log
```

---

## Expected v2.157 Test Outcomes

### Best Case (Full Fix)
```
✅ No pbuf assertions
✅ SCADA-PLC continuous communication
✅ Low packet loss (< 5%)
✅ No connection leaks
✅ System runs for full 90-second test period
```

### Good Case (pbuf Fixed, Event Issue Remains)
```
✅ No pbuf assertions
✅ SCADA-PLC communication works
❌ High packet loss (70-91%) due to CAmkES event system
⚠️  Some connection leaks due to unprocessed error notifications
✅ System runs without crashing
```

### Bad Case (More pbuf Issues)
```
❌ pbuf assertion still occurs
❌ System crashes before 90 seconds
🔍 Need to find additional pbuf_free() calls we missed
🔍 Or different code path causing pbuf corruption
```

---

## Communication Protocol

### User's Observation (v2.156)
> "The problem is back to state where the SCADA can communicate with PLC for a brief period, then the communication is broken. Please help check the log and see if you can point out what went wrong at this point. I pause the GDB almost immediately after SCADA show communication error status."

**What this tells us**:
1. Initial communication works (first few packets)
2. Then crash/hang occurs
3. User can see error in SCADA UI
4. System hangs at that point

**Why "brief period" works**:
- First few packets don't trigger double-free immediately
- Depends on memory allocation state and ref counts
- Eventually ref count goes negative → assertion fires
- System halts

**What v2.157 should achieve**:
- Remove "brief period" limitation
- Enable continuous communication
- No crashes after N packets

---

## Version History Quick Reference

| Version | Date | Focus | Status |
|---------|------|-------|--------|
| v2.153 | Oct 24 | Lock-free control queues | Baseline |
| v2.154 | Oct 24 | RST deduplication fix | Partial fix |
| v2.155 | Oct 24 | Net1 pbuf_free removal | Net1 fixed |
| v2.156 | Oct 25 | lwIP PCB management | Net1 PCB fixed |
| v2.157 | Oct 25 | Net0 pbuf_free removal | **CURRENT** |

---

## Critical Files Modified in v2.157

**File**: `components/VirtIO_Net0_Driver/virtio_net0_driver.c`

**Changes**:
1. Line 2572: Removed `pbuf_free(p)` from error path
2. Line 2585: Removed `pbuf_free(p)` from stale callback path
3. Line 2621: Removed `pbuf_free(p)` from dataport NULL path
4. Line 2675: Removed `pbuf_free(p)` from connection closed path
5. Line 2683: Removed `pbuf_free(p)` from PCB mismatch path
6. Line 2810: Removed entire `pbuf_free()` section + added comprehensive comment
7. Line 4073: Updated version string to v2.157

**All changes**: Enforce lwIP pbuf ownership - application NEVER calls `pbuf_free()` in recv callbacks.

---

## Success Checklist

After v2.157 test completes, verify:

- [ ] Build completed without errors
- [ ] Test ran for full duration (~90 seconds)
- [ ] No pbuf assertions in log
- [ ] No page faults or crashes
- [ ] SCADA can send requests continuously (not just "brief period")
- [ ] PLC responses reach SCADA
- [ ] Breadcrumb counts show packet processing (B9003, B2000, B3000)
- [ ] Connection counts stable (no massive leaks)
- [ ] System remains responsive throughout test

If all checked: **v2.157 successfully fixed pbuf double-free bug!**

If high packet loss but no crash: **CAmkES event system issue** (next focus for v2.158)

---

## Contact Points for Help

**Research documentation**: `/home/qemu/phd/research-docs/`

**Critical lessons**: `/home/qemu/phd/CLAUDE.md`

**Project README**: `components/README.md` (lwIP best practices section)

**Git history**:
```bash
git log --oneline --graph --all -20
```

**Recent commits**:
- `1149edd`: v2.156 lwIP PCB fixes
- `0387dc7`: Research documentation
- `7f8d178`: Submodule updates

---

## End of Prompt

**Next action**: Build and test v2.157, then analyze results using methods described above.

**Expected time**: ~5 minutes (build) + ~90 seconds (test) + ~5 minutes (analysis)

**Goal**: Verify pbuf double-free bug is completely fixed, enabling continuous SCADA-PLC communication.
