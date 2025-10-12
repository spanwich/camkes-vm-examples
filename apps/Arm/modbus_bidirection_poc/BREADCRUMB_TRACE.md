# Breadcrumb Tracing System

## Overview

Minimal breadcrumb tracing system for debugging race conditions without verbose output.
Ultra-minimal format: `B<id>\n` where `<id>` is a numeric breadcrumb marker.

## Enable/Disable

Edit `components/include/common.h`:
```c
#define BREADCRUMB_TRACE 1  // Enable breadcrumbs
#define BREADCRUMB_TRACE 0  // Disable breadcrumbs (default)
```

## Breadcrumb Numbering Scheme

### VirtIO_Net1_Driver (virtio_net1_driver.c)

#### 1000-1011: `inbound_tcp_recv_callback()` - PLC Response Handling
- 1000: Entry - PLC response received
- 1001: Connection closed by remote
- 1002: Error in receive
- 1003: NULL dataport check
- 1004: Preparing ICS message
- 1005: Looking up metadata
- 1006: Metadata found
- 1007: Metadata NOT found (fallback)
- 1008: Copying payload
- 1009: Emitting notification to ICS_Outbound
- 1010: Response sent to ICS_Outbound (notification emitted)
- 1011: **Keeping connection alive for Net0 response** (v2.44 FIX - DO NOT CLOSE!)

#### 2000-2020: `inbound_ready_handle()` - ICS_Inbound Message Processing
- 2000: Entry - ICS_Inbound notification received
- 2001: Checking dataport
- 2002: NULL dataport error
- 2003: Reading ICS message
- 2004: Invalid payload size error
- 2005: Payload size valid
- 2006: **Connection validation & reuse** (v2.49: 5-tuple + TCP seq + state check)
  - ✅ Validates: Same SCADA session, sequence number match, ESTABLISHED state
  - ✅ Reuses: If all validation passes (supports >MTU, HTTP keep-alive)
  - 🧹 Cleans up: If validation fails (tcp_abort + metadata clear)
- 2007: Creating new TCP PCB
- 2008: Failed to create PCB
- 2009: PCB created successfully
- 2010: Attempting tcp_bind
- 2011: tcp_bind failed
- 2012: tcp_bind succeeded
- 2013: Storing connection metadata
- 2014: Metadata storage failed (table full)
- 2015: Metadata stored successfully
- 2016: Attempting tcp_connect
- 2017: tcp_connect failed
- 2018: tcp_connect succeeded
- 2019: Metadata complete, memory barrier done
- 2020: Exit - inbound_ready_handle complete

### VirtIO_Net0_Driver (virtio_net0_driver.c)

#### 3000-3013: `outbound_ready_handle()` - ICS_Outbound Response Forwarding
- 3000: Entry - ICS_Outbound notification received
- 3001: Checking dataport
- 3002: NULL dataport error
- 3003: Reading ICS message
- 3004: Invalid payload size error
- 3005: Payload size valid
- 3006: Looking up connection metadata
- 3007: Connection not found or NULL PCB
- 3008: **Connection validation** (v2.50: seq + state + sanity checks before tcp_write)
  - ✅ Validates: TCP sequence number matches, state is ESTABLISHED, ports valid
  - ⚠️ Drops: If validation fails (dead/reused SCADA connection)
  - 🛡️ Protection: Prevents sending data to wrong/dead connections
- 3009: Attempting tcp_write
- 3010: tcp_write failed
- 3011: tcp_write succeeded, flushing output
- 3012: tcp_output complete
- 3013: Exit - outbound_ready_handle complete
- 3014: **Stale PCB detected** (v2.62: dangling pointer cleanup)
  - ⚠️ Condition: PCB state != ESTABLISHED (connection closed by Net1)
  - 🧹 Action: Removes stale metadata, prevents crash
  - 🛡️ Protection: Catches freed PCB before dereferencing other fields

### Reserved Ranges

- **4000-4999**: Reserved for Net0 additional functions
- **5000-5999**: Reserved for ICS_Inbound component
- **6000-6999**: Reserved for ICS_Outbound component
- **7000-7999**: Reserved for IRQ handlers
- **8000-8999**: Reserved for lwIP callbacks

## Crash Analysis Guide

### Known Crash Points (from v2.42-SILENT-TEST)

1. **Net0 fault at address 0x10** (pc = 0x382b8)
   - Look for last breadcrumb in 3000-3013 range
   - Likely during `outbound_ready_handle()` processing

2. **Net1 fault at address 0x4** (pc = 0x395a0)
   - Look for last breadcrumb in 1000-1011 or 2000-2020 range
   - Likely during PLC response or connection setup

3. **Net1 IRQ fault at address 0x4** (pc = 0x38f70)
   - IRQ handler issue (not yet instrumented)

### Testing Strategy

1. **Enable breadcrumbs**: Set `BREADCRUMB_TRACE=1` in `common.h`
2. **Rebuild**: Clean build to ensure macro is applied
3. **Run on console**: Test locally (where crashes occur)
4. **Identify last breadcrumb**: Find highest B<id> before crash
5. **Incremental removal**: Remove breadcrumbs after crash point
6. **Repeat**: Test again to narrow down crash location

### Example Output Analysis

```
B2000  ← ICS_Inbound notification received
B2001  ← Checking dataport
B2003  ← Reading ICS message
B2005  ← Payload valid
[CRASH] ← Crash occurred after B2005, before B2007
```

This indicates the crash is between "Payload valid" and "Creating new TCP PCB".

## Next Steps

1. Enable BREADCRUMB_TRACE in `common.h`
2. Rebuild with breadcrumb support
3. Test on server console (NOT SSH)
4. Analyze crash output to find last breadcrumb
5. Add more granular breadcrumbs if needed between last breadcrumb and crash
6. Consider adding IRQ handler breadcrumbs (7000 range)

## SSH vs Console Behavior

**Critical Finding**:
- SSH (network serial) → System works
- Local console → System crashes
- Silent mode → System crashes

This suggests serial I/O contention or IRQ conflict, not just timing issues.

## Memory Barriers

Memory barriers (`__sync_synchronize()`) are NECESSARY but NOT SUFFICIENT.
- Required at: notification emit, metadata updates, PCB pointer updates
- Not sufficient to fix the underlying race condition

## Status

- ✅ Breadcrumb infrastructure added to common.h
- ✅ Net1 inbound_tcp_recv_callback instrumented (1000-1011)
- ✅ Net1 inbound_ready_handle instrumented (2000-2020)
- ✅ Net0 outbound_ready_handle instrumented (3000-3013)
- ⏳ IRQ handlers not yet instrumented
- ⏳ lwIP callbacks not yet instrumented
- ⏳ Testing with BREADCRUMB_TRACE=1 pending
