# CRITICAL LESSON: Never Access PCB Fields Directly (Offset 0x10 Crash)

**Date**: 2025-10-13
**Versions**: v2.95 (Net0), v2.96 (Net1)
**Status**: ✅ RESOLVED - First stable version running 10+ minutes without crashes

## Executive Summary

**The Problem**: System repeatedly crashed with "data fault at address 0x10"
**The Root Cause**: Accessing lwIP `tcp_pcb` struct fields directly instead of through lwIP APIs
**The Solution**: Remove ALL direct PCB field accesses, only use lwIP APIs

## The Crash Pattern

```
FAULT HANDLER: data fault from net0_drv on address 0x10, pc = 0x383a4
FAULT HANDLER: r3: 0x0
FAULT HANDLER: r12: 0x10
```

**What 0x10 means**: Offset 16 bytes into `struct tcp_pcb`, typically the `callback_arg` or `state` field.

## Why It Crashes: The Race Condition

### The Unsafe Pattern (WRONG!)

```c
if (meta->pcb != NULL) {              // Check 1: PCB exists ✓
    // ... time passes ...
    if (meta->pcb->state != ESTABLISHED) {  // ← CRASH HERE!
        // lwIP freed PCB between check and access!
    }
}
```

### The Race Condition Timeline

1. **Thread 1** (our code): Check `meta->pcb != NULL` → passes ✓
2. **lwIP timer fires**: Connection timeout, calls `tcp_free(pcb)` → PCB freed
3. **Thread 1**: Access `pcb->state` at offset 0x10 → **PAGE FAULT!**

The problem: **No locking** between NULL check and field access!

## Where We Made This Mistake

### Net0 (Fixed in v2.95)

**Location**: `outbound_ready_handle()` line 2604
```c
// v2.93 code (WRONG):
if (meta->pcb->state != ESTABLISHED) {
    printf("PCB in wrong state (%d), cannot send!\n", meta->pcb->state);
    meta->pcb = NULL;
    meta->active = false;
    return;
}
```

**Why it crashed**:
- Response arrives from Net1
- We check `pcb != NULL` ✓
- lwIP timer fires, frees PCB
- We access `pcb->state` → **CRASH at 0x10!**

**Fix (v2.95)**:
```c
// Don't access ANY PCB fields!
// Let tcp_write() handle validation internally

err_t err = tcp_write(meta->pcb, data, len, TCP_WRITE_FLAG_COPY);
if (err != ERR_OK) {
    // tcp_write() tells us if PCB is invalid
}
```

### Net1 (Fixed in v2.96)

**Location 1**: Connection reuse validation (lines 2773-2800)
```c
// v2.95 code (WRONG):
if (existing_pcb->snd_nxt != existing_meta->tcp_seq_num) { ... }
if (existing_pcb->state != ESTABLISHED) { ... }
if (existing_pcb->local_port == 0) { ... }
```

**Location 2**: Stale connection cleanup (line 3093)
```c
// v2.95 code (WRONG):
if (stale_pcb == NULL || stale_pcb->state != ESTABLISHED) {
    tcp_abort(stale_pcb);  // Also unsafe from main loop!
}
```

**Fix (v2.96)**:
```c
// Don't validate - just cleanup and create new connection
if (existing_meta) {
    goto cleanup_and_create_new;  // Always safe to create new
}

// Only cleanup if PCB already NULL (freed by lwIP)
if (stale_pcb == NULL) {
    connection_table[i].active = false;
}
```

## The Golden Rule

### ❌ NEVER DO THIS:
```c
if (pcb != NULL) {
    // Accessing ANY field is unsafe!
    pcb->state          // ← CRASH
    pcb->snd_nxt        // ← CRASH
    pcb->local_port     // ← CRASH
    pcb->callback_arg   // ← CRASH (offset 0x10!)
}
```

### ✅ ALWAYS DO THIS:
```c
if (pcb != NULL) {
    // Only pass to lwIP APIs
    err_t err = tcp_write(pcb, ...);    // Safe!
    err = tcp_close(pcb);                // Safe!
    tcp_abort(pcb);                      // Safe (but only in callbacks!)
}
```

## Why lwIP APIs Are Safe

lwIP's internal functions have **proper locking and validation**:

```c
// Inside lwIP's tcp_write():
err_t tcp_write(struct tcp_pcb *pcb, ...) {
    LWIP_ASSERT_CORE_LOCKED();      // Ensures no concurrent access

    if (pcb->state != ESTABLISHED) {
        return ERR_CONN;             // Safely returns error
    }

    // ... safe to access fields here
}
```

**We don't have this locking!** So we can't safely access PCB fields.

## Additional Rules

### Rule 1: Never call tcp_abort() from main loop
```c
// WRONG (from main loop):
tcp_abort(pcb);  // Immediately frees PCB while lwIP may still reference it

// RIGHT (from callback or via lwIP):
return ERR_ABRT;  // Let lwIP call tcp_abort() internally
```

### Rule 2: Let lwIP manage PCB lifecycle
```c
// WRONG:
tcp_close(pcb);
meta->pcb = NULL;        // Don't clean up immediately!
meta->active = false;

// RIGHT:
tcp_close(pcb);
// Keep metadata! lwIP will call our callback when done
// Callback will clean up metadata
```

### Rule 3: Trust lwIP's error returns
```c
// Don't pre-validate PCB state
err_t err = tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
if (err == ERR_CONN || err == ERR_RST) {
    // Connection closed - now we know it's safe to clean up
    meta->pcb = NULL;
}
```

## Performance Trade-offs

### v2.95 and earlier: Connection Reuse
- ✅ Fast: Reuse existing connections
- ❌ **Crashes**: Accessing PCB fields causes 0x10 faults

### v2.96: Always Create New Connection
- ✅ **Stable**: No crashes, runs 10+ minutes
- ⚠️ Slight overhead: Creates new connection each time
- ✅ Acceptable: ICS traffic is typically low-rate

For ICS applications, **stability >> performance**. The overhead of creating new connections is negligible compared to industrial control loop times (100ms - 1000ms).

## Testing Results

**Before v2.95**: System crashed within seconds to minutes
**After v2.95**: Net0 stable, Net1 still crashed
**After v2.96**: **Both stable, 10+ minutes runtime** ✅

## Key Takeaway

**lwIP owns the PCB lifecycle. We're just borrowing pointers.**

Think of PCBs like file descriptors:
- You can check if FD is valid
- You pass FD to system calls (read/write/close)
- You **don't** peek at kernel's internal FD data structures

Same with PCBs:
- Check if `pcb != NULL`
- Pass to lwIP APIs (tcp_write/tcp_close)
- **Don't** access `pcb->state` or other fields directly

## References

- lwIP documentation: "PCB lifecycle is managed by the stack"
- v2.85 lesson: "DO NOT access any PCB fields"
- v2.93 regression: Added state check (caused crashes)
- v2.95/v2.96: Removed all field accesses (stable!)

## Version History

- **v2.93**: Added PCB state check → **introduced 0x10 crashes**
- **v2.94**: Other fixes, kept state check → **still crashing**
- **v2.95**: Removed state check in Net0 → **Net0 stable**
- **v2.96**: Removed ALL PCB accesses in Net1 → **Both stable!**

---

**Final Wisdom**: When working with lwIP, remember:
> "If you think you need to access a PCB field, you're probably wrong. Use an lwIP API instead."

This lesson cost us days of debugging. Don't repeat it!
