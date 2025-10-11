# Tomorrow's Debugging Session - Quick Start Prompt

**Copy and paste this to Claude tomorrow to resume work:**

---

Hi! I need help debugging the ICS Bidirectional Modbus Gateway. Yesterday we fixed several critical issues (PCB linking, packet burst limiting, bidirectional routing), but three problems remain:

## Problem Summary

1. **TCP packets truncated on wire** - tcpdump shows "24 bytes missing", packets incomplete
2. **Connection table exhaustion** - "Connection table full" after ~60 connections
3. **Metadata lookup failures** - "No metadata found for TCP port 64085 → 502"

## Root Cause (Already Identified)

The **metadata lookup uses wrong port**:
- Net1 stores metadata with SCADA port **44380**
- But looks up using lwIP's ephemeral port **64085**
- Lookup fails → IP restoration fails → packet corruption

## What to Fix First

**Priority 1: Fix metadata lookup in Net1**

File: `/home/iamfo470/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc/components/VirtIO_Net1_Driver/virtio_net1_driver.c`

Steps:
1. Add `uint16_t lwip_ephemeral_port` field to `struct connection_metadata` (line ~55-62)
2. In `inbound_ready_handle()` after `tcp_connect()`, store the ephemeral port:
   ```c
   meta->lwip_ephemeral_port = pcb->local_port;
   ```
3. In `netif_output()` lookup logic (line ~600-605), change to:
   ```c
   if (connection_table[i].active &&
       connection_table[i].lwip_ephemeral_port == src_port &&  /* Match ephemeral */
       connection_table[i].src_port == dest_port) {            /* Match SCADA port */
       meta = &connection_table[i];
       break;
   }
   ```

**Expected result:** "No metadata found" warnings disappear, packets not truncated

## Documentation

Full investigation notes in:
`/home/iamfo470/phd/research-docs/modbus-gateway-issues-2025-10-11.md`

This file has:
- Detailed root cause analysis
- Console logs and tcpdump output
- Step-by-step investigation plan
- All three issues documented

## Todo List

There's a todo list with 8 items tracking all fixes needed. Use `TodoWrite` to update progress.

## Current State

- Project: `/home/iamfo470/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc`
- Versions: Net0 v2.32, Net1 v2.30
- Repository: `spanwich/camkes-vm-examples` master branch
- Last commit: `074f5a4` - Net1 connection metadata storage

## What I Need

Please help me:
1. Fix the metadata lookup (Priority 1)
2. Add hex dumps to debug packet truncation
3. Implement connection table cleanup
4. Test bidirectional traffic continuously

Let's start with Priority 1 - fixing the metadata lookup!

---

**End of prompt - ready to paste tomorrow**
