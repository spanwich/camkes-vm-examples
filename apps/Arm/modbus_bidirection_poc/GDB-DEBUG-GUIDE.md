# GDB Debugging Guide for ICS Security Gateway

**Version**: v2.97+ (2025-10-20)
**Purpose**: Persistent GDB debugging for seL4 VM faults and race conditions

---

## Overview

This guide explains how to debug seL4 VM faults using GDB with persistent tmux sessions. The setup allows you to:

- ✅ **Run for extended periods** (hours to weeks) without maintaining SSH connection
- ✅ **Catch VM faults** using GDB watchpoints before seL4's fault handler
- ✅ **Monitor live console output** while debugging
- ✅ **Set breakpoints** and inspect state interactively
- ✅ **Automatically log** all debug sessions

---

## Quick Start

### 1. Prerequisites

**From project build directory** (`~/phd/camkes-vm-examples/build_modbus`):

```bash
# Ensure TAP interfaces are configured
sudo ../projects/vm-examples/apps/Arm/modbus_bidirection_poc/scripts/setup-policy-routing-gateway.sh

# Verify tmux is installed
tmux -V
```

### 2. Start Persistent Debug Session

```bash
cd ~/phd/camkes-vm-examples/build_modbus
./start-persistent-debug.sh
```

This creates a tmux session with 3 panes:

```
┌──────────────────┬──────────────────┐
│ QEMU + GDB       │ GDB Client       │
│ Server           │ (interactive)    │
├──────────────────┴──────────────────┤
│ Live Console Log (tail -f)          │
└─────────────────────────────────────┘
```

### 3. Start Execution

In the **top-right pane** (GDB), type:

```gdb
(gdb) continue
```

Or just: `c`

### 4. Detach and Leave Running

Press: **Ctrl-b d**

You can now disconnect SSH. The session continues running.

### 5. Reconnect Later

```bash
ssh qemu@server
cd ~/phd/camkes-vm-examples/build_modbus
tmux attach -t modbus-debug
```

---

## How It Works

### seL4 VM Fault Detection

The setup uses **GDB watchpoints** to catch VM faults **before** seL4's fault handler:

```gdb
watch *(int*)0x10
```

**Why address 0x10?**
From crash logs: `FAULT HANDLER: data fault... on address 0x10, pc = 0x38308`

This watchpoint triggers when code attempts to access the null pointer offset that causes crashes.

### Automatic Fault Analysis

When GDB stops on a fault, it automatically logs:

1. **Timestamp** - When fault occurred
2. **Program Counter (PC)** - Exact instruction location
3. **All ARM Registers** - Full CPU state
4. **Backtrace** - Call stack showing how we got here
5. **Stack Contents** - Memory dump for analysis
6. **Disassembly** - Instructions around PC

All saved to: `logs/gdb-<timestamp>.log`

---

## Session Management

### Navigating Panes

```
Ctrl-b ←  →  ↑  ↓    Navigate between panes
Ctrl-b d              Detach (session keeps running)
Ctrl-b [              Enter scroll mode (q to exit)
```

### Pane Functions

| Pane | Purpose | Key Info |
|------|---------|----------|
| **Top-left** | QEMU with GDB server | Shows QEMU startup messages |
| **Top-right** | GDB client (interactive) | **Use this for debugging** |
| **Bottom** | Live console log | Real-time output with timestamps |

### Checking Status While Detached

```bash
./check-debug-status.sh
```

Shows:
- Session running status
- Last 20 lines from each pane
- Recent log files
- System uptime

---

## GDB Commands Reference

### Basic Debugging

```gdb
(gdb) continue              # Resume execution
(gdb) interrupt             # Pause execution (Ctrl-C)
(gdb) backtrace             # Show call stack
(gdb) info registers        # Show all CPU registers
(gdb) x/10i $pc             # Disassemble 10 instructions at PC
```

### Breakpoints

```gdb
(gdb) break <function>      # Break at function
(gdb) break *0x38308        # Break at specific address (PC from fault)
(gdb) watch <variable>      # Watch variable changes
(gdb) info breakpoints      # List all breakpoints
(gdb) delete <num>          # Delete breakpoint
```

### Examining Memory

```gdb
(gdb) x/64xw $sp            # Examine 64 words from stack pointer
(gdb) x/s 0x40000           # Examine string at address
(gdb) print <variable>      # Print variable value
```

### Custom Commands (from gdb-sel4-debug.txt)

```gdb
(gdb) check-fault           # Examine current PC and nearby instructions
```

---

## Log Files

All logs saved in `build_modbus/logs/`:

| File Pattern | Content |
|--------------|---------|
| `console-<timestamp>.log` | Full QEMU console output |
| `gdb-<timestamp>.log` | GDB session with fault analysis |

### Analyzing Crash Logs

```bash
# View console output
tail -100 logs/console-*.log

# Search for faults
grep -i "fault" logs/console-*.log

# View GDB analysis
cat logs/gdb-*.log
```

---

## Debugging Scenarios

### Scenario 1: Catching Known Fault at 0x10

The fault from production logs:
```
FAULT HANDLER: data fault from net0_drv.net0_drv_0_control (ID 0x41)
on address 0x10, pc = 0x38308
```

**GDB Setup** (already configured):
```gdb
(gdb) watch *(int*)0x10     # Watchpoint on fault address
(gdb) continue
```

**When it triggers**, GDB stops and shows:
- PC at 0x38308 (or nearby)
- Backtrace showing call path
- Registers showing what caused null pointer access

### Scenario 2: Debugging Race Conditions

For race conditions that occur after extended runtime:

1. Start session: `./start-persistent-debug.sh`
2. In GDB: `continue`
3. Detach: `Ctrl-b d`
4. Disconnect SSH and wait (hours/days)
5. Reconnect and check `logs/gdb-*.log`

### Scenario 3: Setting Custom Breakpoints

```gdb
# In top-right GDB pane
(gdb) break virtio_net0_driver.c:2691    # If symbols available
(gdb) break *0x38308                      # Direct address
(gdb) continue
```

---

## Troubleshooting

### Session Won't Start

**Problem**: `tmux has-session -t modbus-debug` already exists

**Solution**:
```bash
tmux kill-session -t modbus-debug
./start-persistent-debug.sh
```

### GDB Can't Connect

**Problem**: GDB shows "Connection refused"

**Solution**:
```bash
# Kill QEMU and restart
pkill -9 qemu-system-arm
tmux kill-session -t modbus-debug
./start-persistent-debug.sh
```

### Watchpoint Doesn't Trigger

**Problem**: Fault occurs but GDB didn't stop

**Explanation**: seL4 VM faults happen in virtualized guest space. GDB watchpoints may not catch all VM-internal faults.

**Solution**: Use console log monitoring:
```bash
tail -f logs/console-*.log | grep --line-buffered -i "fault"
```

### Network Not Working

**Problem**: Connections not established

**Solution**: Use `run-remote-gdb.sh` which has proven network setup:
```bash
# Manually test network
./run-remote-gdb.sh
# Then connect GDB from another terminal:
gdb-multiarch -ex 'target remote :1234' images/capdl-loader-image-arm-qemu-arm-virt
```

---

## Files Reference

| File | Purpose | Location |
|------|---------|----------|
| `start-persistent-debug.sh` | Main launcher | `build_modbus/` |
| `run-remote-gdb.sh` | QEMU with GDB server | `build_modbus/` |
| `gdb-sel4-debug.txt` | GDB initialization commands | `build_modbus/` |
| `check-debug-status.sh` | Status checker | `build_modbus/` |

---

## Advanced: Custom GDB Scripts

Edit `gdb-sel4-debug.txt` to add custom debugging:

```gdb
# Add custom breakpoint
break my_function

# Add custom watchpoint
watch my_variable

# Add custom display
define hook-stop
    echo Custom debug info\n
    print my_variable
end
```

Then reload:
```gdb
(gdb) source gdb-sel4-debug.txt
```

---

## Comparison with Other Debug Methods

| Method | Pros | Cons | Use Case |
|--------|------|------|----------|
| **This GDB Setup** | Interactive, breakpoints, persistent | Requires GDB knowledge | Development, fault analysis |
| `run-with-crash-capture.sh` | Automatic, memory dumps | No interaction | Production monitoring |
| `run-remote.sh` | Simple, fast | No debugging | Quick testing |
| Console logging only | Lightweight | Post-mortem only | Long-term stability tests |

---

## Best Practices

1. **Always detach** (`Ctrl-b d`) instead of terminating SSH when running long tests
2. **Check logs regularly** during week-long runs: `./check-debug-status.sh`
3. **Save important logs** before restarting: `cp logs/gdb-*.log ~/backups/`
4. **Use specific watchpoints** instead of generic ones for better performance
5. **Monitor system resources** if running for weeks: `top -b -n 1 | grep qemu`

---

## Support

For issues with this debugging setup:

1. Check `logs/` for error messages
2. Verify TAP interfaces: `ip link show tap0 tap1`
3. Check tmux sessions: `tmux list-sessions`
4. Verify GDB multiarch: `which gdb-multiarch`

For project-specific issues, see `README.md` and version-specific documentation.

---

**Last Updated**: 2025-10-20
**Tested With**: seL4 kernel debug build, GDB multiarch 12.1+, tmux 3.2+
