# Modbus Gateway Bidirectional Issues - Investigation Notes

**Date:** 2025-10-11
**System:** ICS Bidirectional Cross-Domain Security Gateway
**Components:** Net0 (External), Net1 (Internal), ICS_Inbound, ICS_Outbound

## Executive Summary

After implementing bidirectional TCP support and fixing multiple issues (PCB linking, packet burst limiting, connection metadata storage), the gateway now forwards traffic in both directions but encounters three critical issues:

1. **Truncated TCP packets** with "24 bytes missing" on wire
2. **Connection table exhaustion** (MAX_CONNECTIONS limit reached)
3. **Metadata lookup failures** for ephemeral ports

## Issue 1: Truncated TCP Packets on Wire

### Symptoms

tcpdump on ens256 (internal network) shows:
```
10:18:54.169829 IP truncated-ip - 24 bytes missing! (tos 0x0, ttl 255, id 252, offset 0, flags [none], proto TCP (6), length 52)
    192.168.95.1.64073 > 192.168.95.2.502:  [|tcp]
```

**Key observations:**
- IP header says length=52 bytes (should include full TCP header + data)
- tcpdump sees truncated packet with 24 bytes missing
- TCP header is incomplete (`[|tcp]` means parser couldn't read full header)
- Source is Net1 (192.168.95.1), destination is PLC (192.168.95.2)

### Console Logs (MSG #267)

```
VirtIO_Net1_Driver: INBOUND: Received message from ICS_Inbound
VirtIO_Net1_Driver: Payload size: 12 bytes
VirtIO_Net1_Driver: 📝 Stored metadata [62]: 192.168.90.5:44380 → 192.168.95.2:502
VirtIO_Net1_Driver: ⚠️  TX: No metadata found for TCP port 64085 → 502
VirtIO_Net1_Driver: ⚠️  TX: No metadata found for TCP port 64085 → 502
VirtIO_Net1_Driver: ⚠️  TX: No metadata found for TCP port 64085 → 502
VirtIO_Net1_Driver: ⚠️  TX: No metadata found for TCP port 64085 → 502
```

**Critical discrepancy:**
- Net1 stores metadata for SCADA port **44380**
- But TX tries to look up metadata for port **64085**
- Port mismatch → metadata lookup fails → IP restoration may fail

### Root Cause Analysis

**Hypothesis 1: Packet Length Miscalculation**
- Net1's `netif_output()` may be setting wrong packet length in VirtIO descriptor
- Or pbuf chain not fully copied to TX buffer
- VirtIO transmits partial packet based on descriptor length

**Hypothesis 2: IP Restoration Corruption**
- When metadata lookup fails, IP restoration code path may corrupt packet
- Checksum recalculation with partial data → invalid length field
- TCP header partially overwritten during failed restoration

**Hypothesis 3: Ephemeral Port Confusion**
- lwIP assigns ephemeral port **64085** for Net1's outbound TCP connection
- But metadata stored with SCADA's port **44380**
- Metadata lookup uses wrong port tuple → restoration fails → packet corruption

### Investigation Steps

1. **Verify packet buffer copying:**
   - Check `pbuf_copy_partial()` return value in `netif_output()`
   - Add hex dump of full TX buffer before VirtIO submission
   - Confirm copied length == pbuf->tot_len

2. **Verify VirtIO descriptor length:**
   - Print descriptor buffer address and length before VREG_WRITE(QUEUE_NOTIFY)
   - Confirm descriptor length matches actual packet size
   - Check if chained descriptors (header + packet) have correct lengths

3. **Trace IP restoration path:**
   - Add detailed logging when metadata lookup fails
   - Print packet before/after IP restoration attempt
   - Verify checksum calculation doesn't corrupt packet structure

4. **Port mapping investigation:**
   - When storing metadata: print both SCADA port and lwIP ephemeral port
   - When looking up metadata: print actual ports being searched
   - Determine correct port tuple for metadata key

## Issue 2: Connection Table Exhaustion

### Symptoms

```
VirtIO_Net1_Driver: ⚠️  Connection table full! Dropping metadata.
VirtIO_Net1_Driver: INBOUND: Failed to store connection metadata (table full)
```

**Impact:** New connections rejected, gateway stops forwarding traffic

### Current Configuration

```c
#define MAX_CONNECTIONS 64
```

Both Net0 and Net1 use 64-entry connection tracking tables.

### Root Cause Analysis

**Hypothesis 1: Connections Not Being Cleaned Up**
- Old connections remain in table after TCP close
- No timeout mechanism to expire stale entries
- Table fills up over time during normal operation

**Hypothesis 2: Connection Leaks**
- TCP error conditions don't remove metadata
- `connection_remove()` not called in all close paths
- `tcp_err` callback may not be registered properly

**Hypothesis 3: Table Size Too Small**
- 64 connections insufficient for production ICS traffic
- Multiple SCADA clients + polling intervals → high connection count
- Need dynamic analysis of actual connection patterns

### Investigation Steps

1. **Add connection table monitoring:**
   ```c
   void print_connection_table_stats(void) {
       int active = 0, pcb_linked = 0, stale = 0;
       for (int i = 0; i < MAX_CONNECTIONS; i++) {
           if (connection_table[i].active) {
               active++;
               if (connection_table[i].pcb != NULL) pcb_linked++;
               // Check if PCB state is CLOSED/TIME_WAIT
           }
       }
       printf("Connection table: %d active, %d PCB-linked, %d stale\n",
              active, pcb_linked, stale);
   }
   ```

2. **Verify cleanup paths:**
   - Check if `tcp_err_callback` is registered (should remove metadata)
   - Verify `tcp_echo_err()` calls `connection_remove()`
   - Add logging to all `connection_remove()` calls

3. **Add timeout mechanism:**
   - Implement periodic scan for stale connections
   - Remove entries where PCB is NULL or PCB->state == CLOSED
   - Consider TCP keepalive or application-level heartbeat

4. **Increase table size (short-term fix):**
   ```c
   #define MAX_CONNECTIONS 256  // Test with larger table
   ```

## Issue 3: Metadata Lookup Failures - Port Mismatch

### Symptoms

```
VirtIO_Net1_Driver: 📝 Stored metadata [62]: 192.168.90.5:44380 → 192.168.95.2:502
VirtIO_Net1_Driver: ⚠️  TX: No metadata found for TCP port 64085 → 502
```

**Port discrepancy:**
- Stored with SCADA port: **44380**
- Looking up with lwIP ephemeral port: **64085**

### Root Cause Analysis

**Confirmed cause:** Incorrect metadata lookup key in `netif_output()`

Current lookup code (line 589-605 in Net1):
```c
struct tcphdr *tcp = ...;
uint16_t src_port = ntohs(tcp->source);  /* lwIP ephemeral port (64085) */
uint16_t dest_port = ntohs(tcp->dest);   /* SCADA's port (44380) */

/* Lookup by destination port */
for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (connection_table[i].active &&
        connection_table[i].dest_port == src_port &&  /* Our port 502 */
        connection_table[i].src_port == dest_port) {  /* SCADA's port */
        meta = &connection_table[i];
        break;
    }
}
```

**Problem:** This lookup assumes:
- `tcp->source` (lwIP's port) should match `connection_table[i].dest_port` (502)
- `tcp->dest` (SCADA port) should match `connection_table[i].src_port` (44380)

But in reality:
- `tcp->source` = lwIP's ephemeral port (64085) - **NOT 502!**
- `tcp->dest` = SCADA's port (44380) ✓

### Solution

**Option 1: Store lwIP ephemeral port in metadata**
When creating connection in `inbound_ready_handle()`:
```c
/* After tcp_connect(), get assigned ephemeral port */
meta->lwip_ephemeral_port = pcb->local_port;
```

Then in `netif_output()`, look up by ephemeral port:
```c
if (connection_table[i].active &&
    connection_table[i].lwip_ephemeral_port == src_port &&
    connection_table[i].src_port == dest_port) {
    meta = &connection_table[i];
    break;
}
```

**Option 2: Link PCB to metadata**
Store PCB pointer in metadata, look up by PCB:
```c
struct connection_metadata* connection_lookup_by_pcb(struct tcp_pcb *pcb);
```

This requires passing PCB to `netif_output()`, which may need lwIP interface changes.

**Recommended:** Option 1 - store ephemeral port explicitly

## QEMU VirtIO Issue

### Symptoms

```
qemu-system-arm: virtio: bogus descriptor or out of resources
refill_rx_queue() call #1048072: 1/32 buffers free (available to refill)
VirtIO_Net0_Driver: ✅ Refilled RX queue with 1 buffers (avail_idx now=352)
```

**Likely cause:** Descriptor wraparound handling issue or buffer exhaustion under high traffic

**Not critical** - system recovers by refilling. Monitor for crashes.

## System Configuration

### Network Topology
```
SCADA (192.168.90.5) ←→ Net0 (192.168.96.2) ←→ ICS_Inbound ←→
                                                 ↕
                                            ICS_Outbound
                                                 ↕
                        Net1 (192.168.95.1) ←→ PLC (192.168.95.2)
```

### Current Versions
- **Net0:** v2.32-packet-burst-limit
- **Net1:** v2.30-store-connection-metadata
- **ICS_Inbound/Outbound:** Stateless validators

### Recent Fixes Applied
1. PCB-metadata linking (v2.30)
2. Packet burst limiting to prevent notification starvation (v2.32)
3. Bidirectional dataport routing (v2.28, v2.31)
4. Connection metadata storage in Net1 (v2.30)

## Debug Actions for Tomorrow

### Immediate Priorities

1. **Fix metadata lookup (highest priority)**
   - Add `lwip_ephemeral_port` field to `struct connection_metadata`
   - Store ephemeral port after `tcp_connect()` in `inbound_ready_handle()`
   - Update lookup logic in `netif_output()` to use ephemeral port
   - Expected result: "No metadata found" warnings disappear

2. **Fix packet truncation**
   - Add hex dumps before VirtIO TX to verify packet integrity
   - Check if IP restoration corrupts packets when metadata missing
   - Verify VirtIO descriptor lengths match actual packet size
   - Expected result: tcpdump shows complete TCP packets

3. **Implement connection cleanup**
   - Add periodic connection table scan (every 30 seconds)
   - Remove entries with NULL PCB or CLOSED state
   - Add table statistics logging (active/stale/available)
   - Expected result: Table doesn't fill up over time

### Secondary Actions

4. **Increase connection table size**
   - Change `MAX_CONNECTIONS` from 64 to 256
   - Monitor actual usage patterns
   - Determine optimal size for production

5. **Add comprehensive logging**
   - Print connection table state when full
   - Log all `connection_add()` / `connection_remove()` calls
   - Track connection lifecycle: create → active → cleanup

6. **End-to-end testing**
   - Run continuous Modbus polling for 5+ minutes
   - Monitor connection table utilization
   - Verify responses reach SCADA correctly
   - Check for packet loss or corruption

## Success Criteria

✅ **Gateway fully operational:**
- No "No metadata found" warnings
- No truncated packets on wire
- Connection table doesn't exhaust
- Bidirectional Modbus traffic flows continuously
- PLC responses reach SCADA reliably

## References

- **Project:** `/home/iamfo470/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc`
- **Net0 Driver:** `components/VirtIO_Net0_Driver/virtio_net0_driver.c`
- **Net1 Driver:** `components/VirtIO_Net1_Driver/virtio_net1_driver.c`
- **CAmkES Assembly:** `ics_dual_nic.camkes`
- **Traffic Flow Legend:** Lines 1-71 of `ics_dual_nic.camkes`

## Notes

- All fixes pushed to GitHub: `spanwich/camkes-vm-examples` master branch
- Last commit: `074f5a4` - Net1 connection metadata storage
- Build system: Manual process (CMAKE incompatible with seL4/CAmkES)
