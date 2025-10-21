# Testing Guide: v2.43-breadcrumb-only

## Quick Start

### 1. Build the Project

```bash
cd ~/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc/build
ninja
```

### 2. Run with Console Logging

```bash
cd ~/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc/build

# Run with automatic logging
../scripts/run-remote-with-log.sh
```

This will:
- Start QEMU with TAP networking
- Capture ALL console output to `logs/console-<timestamp>.log`
- Show last 50 lines when you exit (Ctrl-A X)
- Extract breadcrumbs automatically
- Detect crashes automatically

### 3. Analyze Crash Logs

```bash
# Analyze the most recent log
../scripts/analyze-crash-log.sh latest

# Or analyze a specific log file
../scripts/analyze-crash-log.sh logs/console-20251012-143022.log
```

This will show:
- 📍 Breadcrumb sequence (last 30 breadcrumbs)
- 🔍 VM fault detection
- 🔍 seL4 fault detection
- 📋 Version info
- 🔧 Component status
- 📄 Last 20 lines of output

### 4. Share Crash Log

```bash
# View the full log
cat logs/console-<timestamp>.log

# Or copy to research docs
cp logs/console-<timestamp>.log ~/phd/research-docs/v2.43-crash-breadcrumbs.log
```

## Expected Output

### Startup (Normal)

```
VirtIO_Net0_Driver: Component started
VirtIO_Net0_Driver: 🔖 NET0 SOFTWARE VERSION: v2.43-breadcrumb-only
VirtIO_Net0_Driver: 🔧 MODE: SILENT + BREADCRUMB_TRACE
VirtIO_Net0_Driver: ⚠️  WARNING: Testing race condition with breadcrumbs only

VirtIO_Net1_Driver: Component started
VirtIO_Net1_Driver: 🔖 NET1 SOFTWARE VERSION: v2.43-breadcrumb-only
VirtIO_Net1_Driver: 🔧 MODE: SILENT + BREADCRUMB_TRACE
VirtIO_Net1_Driver: ⚠️  WARNING: Testing race condition with breadcrumbs only
```

### During Operation (Breadcrumbs Only)

```
B2000
B2001
B2003
B2005
B2007
B2009
B2010
B2012
B2013
B2015
B2016
B2018
B2019
B2020
B1000
B1005
B1008
B1009
...
```

### Crash Example

```
B2000
B2001
B2003
B2005
B2007
[vm exit] vcpu fault: vm_fault_type: seL4_DataFault pc: 0x395a0 fault_addr: 0x4
```

**Interpretation**: Crash occurred after B2007 ("Creating new TCP PCB"), before B2009 ("PCB created successfully")

## Breadcrumb Reference (Quick)

### Net1 (Internal/PLC) - Critical Path
- **2000**: Entry - ICS_Inbound notification
- **2005**: Payload validated
- **2007**: Creating TCP PCB
- **2009**: PCB created ✅
- **2012**: tcp_bind succeeded
- **2015**: Metadata stored
- **2018**: tcp_connect succeeded
- **2020**: Exit - Complete

### Net1 - PLC Response Handler
- **1000**: PLC response received
- **1005**: Metadata lookup
- **1009**: Notification sent
- **1011**: Connection closed

### Net0 (External/SCADA) - Outbound Path
- **3000**: Entry - ICS_Outbound notification
- **3006**: Lookup connection
- **3008**: Connection found
- **3009**: tcp_write attempt
- **3011**: tcp_write succeeded
- **3013**: Exit - Complete

## Troubleshooting

### No Breadcrumbs in Log

**Problem**: Log file exists but no "B<number>" markers

**Solution**: Verify BREADCRUMB_TRACE is enabled:
```bash
grep "BREADCRUMB_TRACE" components/include/common.h
# Should show: #define BREADCRUMB_TRACE 1
```

### Log File Not Created

**Problem**: `logs/` directory doesn't exist or no files

**Solution**:
```bash
# Create logs directory manually
mkdir -p build/logs

# Verify script permissions
chmod +x scripts/run-remote-with-log.sh
```

### TAP Interface Errors

**Problem**: Script complains about missing tap0/tap1

**Solution**: Run network setup first:
```bash
sudo ./scripts/setup-policy-routing-gateway.sh
```

## Reverting to Verbose Mode (if needed)

If you need to revert to stable DEBUG_NORMAL mode:

```bash
# 1. Edit common.h
sed -i 's/BREADCRUMB_TRACE 1/BREADCRUMB_TRACE 0/' components/include/common.h

# 2. Edit Net0
sed -i 's/DEBUG_LEVEL_SILENT   1/DEBUG_LEVEL_SILENT   0/' components/VirtIO_Net0_Driver/virtio_net0_driver.c
sed -i 's/DEBUG_LEVEL_NORMAL   0/DEBUG_LEVEL_NORMAL   1/' components/VirtIO_Net0_Driver/virtio_net0_driver.c

# 3. Edit Net1
sed -i 's/DEBUG_LEVEL_SILENT   1/DEBUG_LEVEL_SILENT   0/' components/VirtIO_Net1_Driver/virtio_net1_driver.c
sed -i 's/DEBUG_LEVEL_NORMAL   0/DEBUG_LEVEL_NORMAL   1/' components/VirtIO_Net1_Driver/virtio_net1_driver.c

# 4. Edit ICS components
sed -i 's/DEBUG_SILENT 1/DEBUG_SILENT 0/' components/ICS_Inbound/ICS_Inbound.c
sed -i 's/DEBUG_SILENT 1/DEBUG_SILENT 0/' components/ICS_Outbound/ICS_Outbound.c

# 5. Rebuild
cd build && ninja
```

## Files and Locations

- **Test Script**: `scripts/run-remote-with-log.sh`
- **Analysis Script**: `scripts/analyze-crash-log.sh`
- **Log Directory**: `build/logs/`
- **Log Format**: `logs/console-YYYYMMDD-HHMMSS.log`
- **Breadcrumb Docs**: `BREADCRUMB_TRACE.md`
- **Config Summary**: `v2.43-CHANGES.txt`

## Original Script (No Logging)

If you need the original behavior (console output, no file):

```bash
../scripts/run-remote.sh
```
