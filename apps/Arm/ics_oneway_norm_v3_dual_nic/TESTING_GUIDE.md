# ICS Bidirectional Firewall - Testing Guide

**Last Updated**: 2025-10-09
**Current Phase**: Phase 1 - One-way paths ready for testing

---

## Prerequisites

### System Requirements
- ✅ Build completed successfully (100/100 targets)
- ✅ QEMU installed (qemu-system-aarch64)
- ✅ netcat installed (`nc` command)
- ✅ Terminal multiplexer (tmux/screen) or multiple terminal windows

### Build Location
```bash
cd ~/phd/camkes-vm-examples/build
```

---

## Quick Start - Interactive Testing

### Terminal Setup

You'll need **2 terminal windows**:
- **Terminal 1**: QEMU simulation (guest system)
- **Terminal 2**: Test client (host network commands)

---

## Test Scenario 1: INBOUND Path (External → Internal)

### Overview
Tests the data flow from external network (port 6000) through the ICS gateway to internal network.

```
External Client (nc localhost 6000)
    ↓
VirtIO_Net0_Driver (TCP server port 6000)
    ↓ extracts metadata
ICS_Inbound (validates external→internal)
    ↓ logs and forwards
VirtIO_Net1_Driver (receives validated message)
    ↓ [Phase 2: creates new TCP session]
Internal Network
```

### Step-by-Step Commands

#### Terminal 1: Start Simulation
```bash
cd ~/phd/camkes-vm-examples/build

# Start QEMU with dual NICs
./simulate --extra-qemu-args="\
  -netdev user,id=net0,hostfwd=tcp::6000-:6000 \
  -device virtio-net-device,netdev=net0 \
  -netdev user,id=net1,hostfwd=tcp::7000-:7000 \
  -device virtio-net-device,netdev=net1"
```

**Wait for initialization messages**:
```
VirtIO_Net0_Driver: Ready for bidirectional ICS traffic (INBOUND: TCP:6000 → ICS_Inbound)
VirtIO_Net1_Driver: Ready for bidirectional ICS traffic (OUTBOUND: TCP:7000 → ICS_Outbound)
ICS_Inbound: Ready to validate external→internal traffic
ICS_Outbound: Ready to validate internal→external traffic
```

#### Terminal 2: Send Test Data
```bash
# Test 1: Simple message
echo "Hello from external network" | nc localhost 6000

# Test 2: Multi-line data
cat <<EOF | nc localhost 6000
Line 1: External data
Line 2: Protocol break test
Line 3: Bidirectional gateway
EOF

# Test 3: Binary-safe test
echo -n "Test with null bytes: $(printf '\x00\x01\x02')" | nc localhost 6000
```

### Expected Output (Terminal 1 - QEMU)

**VirtIO_Net0_Driver** should show:
```
VirtIO_Net0_Driver: TCP connection accepted
VirtIO_Net0_Driver: INBOUND: Forwarding 29 bytes to ICS_Inbound (proto=TCP, src_port=XXXXX, dst_port=6000)
```

**ICS_Inbound** should show:
```
ICS_Inbound: Frame Metadata:
  EtherType: 0x0800
  IP Protocol: 6 (TCP)
  Src Port: XXXXX
  Dst Port: 6000
  Payload: offset=0, length=29
ICS_Inbound: ALLOW - Message passed validation
ICS_Inbound: Forwarded message to internal network
```

**VirtIO_Net1_Driver** should show:
```
[Phase 2 pending: Will receive message and create new TCP session]
```

### Success Criteria ✅
- ✅ VirtIO_Net0_Driver receives TCP connection
- ✅ Metadata extracted (ports, protocol type)
- ✅ ICS_Message created and forwarded to ICS_Inbound
- ✅ ICS_Inbound logs metadata and validates
- ✅ Message forwarded to VirtIO_Net1_Driver
- ⚠️ **Phase 2**: VirtIO_Net1_Driver creates new TCP session (pending)

---

## Test Scenario 2: OUTBOUND Path (Internal → External)

### Overview
Tests the data flow from internal network (port 7000) through the ICS gateway to external network.

```
Internal Client (nc localhost 7000)
    ↓
VirtIO_Net1_Driver (TCP server port 7000)
    ↓ extracts metadata
ICS_Outbound (validates internal→external)
    ↓ logs and forwards
VirtIO_Net0_Driver (receives validated message)
    ↓ [Phase 2: creates new TCP session]
External Network
```

### Step-by-Step Commands

#### Terminal 1: QEMU Running
(Keep simulation from Test 1 running, or restart if needed)

#### Terminal 2: Send Test Data
```bash
# Test 1: Simple message to port 7000
echo "Hello from internal network" | nc localhost 7000

# Test 2: Multi-line data
cat <<EOF | nc localhost 7000
Line 1: Internal data
Line 2: Reverse path test
Line 3: Outbound validation
EOF
```

### Expected Output (Terminal 1 - QEMU)

**VirtIO_Net1_Driver** should show:
```
VirtIO_Net1_Driver: TCP connection accepted
VirtIO_Net1_Driver: OUTBOUND: Forwarding 29 bytes to ICS_Outbound (proto=TCP, src_port=XXXXX, dst_port=7000)
```

**ICS_Outbound** should show:
```
ICS_Outbound: Frame Metadata:
  EtherType: 0x0800
  IP Protocol: 6 (TCP)
  Src Port: XXXXX
  Dst Port: 7000
  Payload: offset=0, length=29
ICS_Outbound: ALLOW - Message passed validation
ICS_Outbound: Forwarded message to external network
```

**VirtIO_Net0_Driver** should show:
```
[Phase 2 pending: Will receive message and create new TCP session]
```

### Success Criteria ✅
- ✅ VirtIO_Net1_Driver receives TCP connection
- ✅ Metadata extracted (ports, protocol type)
- ✅ ICS_Message created and forwarded to ICS_Outbound
- ✅ ICS_Outbound logs metadata and validates
- ✅ Message forwarded to VirtIO_Net0_Driver
- ⚠️ **Phase 2**: VirtIO_Net0_Driver creates new TCP session (pending)

---

## Test Scenario 3: Concurrent Bidirectional Traffic

### Overview
Test both paths simultaneously to verify independent operation.

### Commands

#### Terminal 2: Send to both ports
```bash
# Shell 1: External traffic
while true; do
  echo "External-$(date +%s)" | nc localhost 6000
  sleep 2
done

# Shell 2 (new window): Internal traffic
while true; do
  echo "Internal-$(date +%s)" | nc localhost 7000
  sleep 2
done
```

### Expected Behavior
- Both ICS_Inbound and ICS_Outbound should process messages concurrently
- No interference between paths
- Statistics counters increment independently

---

## Test Scenario 4: Protocol Statistics Validation

### Overview
Verify ICS components track protocol statistics correctly.

### Commands

#### Terminal 2: Send various protocols
```bash
# TCP traffic (default)
for i in {1..10}; do
  echo "TCP message $i" | nc localhost 6000
done

# After 10 messages, check QEMU output
```

### Expected Output
```
=== ICS_Inbound Statistics ===
Received: 10, Forwarded: 10, Dropped: 0
TCP: 10, UDP: 0, ARP: 0, Other: 0
==============================
```

---

## Test Scenario 5: Payload Size Testing

### Overview
Test various payload sizes from minimum to maximum.

### Commands

```bash
# Small payload (1 byte)
echo -n "X" | nc localhost 6000

# Medium payload (1 KB)
head -c 1024 /dev/urandom | nc localhost 6000

# Large payload (60 KB - near MAX_PAYLOAD_SIZE)
head -c 60000 /dev/urandom | nc localhost 6000

# Check logs for payload length validation
```

### Expected Behavior
- Small payloads accepted
- Large payloads accepted (up to 60000 bytes)
- Payloads > 60000 should be truncated with warning

---

## Debugging and Monitoring

### View Real-Time Logs

#### Option 1: QEMU Console Output
Watch Terminal 1 for component messages

#### Option 2: Save to File
```bash
./simulate --extra-qemu-args="..." 2>&1 | tee simulation.log

# In another terminal:
tail -f simulation.log | grep -E "ICS_Inbound|ICS_Outbound|VirtIO"
```

### Key Log Messages to Watch For

**Successful INBOUND**:
```
VirtIO_Net0_Driver: INBOUND: Forwarding X bytes to ICS_Inbound
ICS_Inbound: ALLOW - Message passed validation
ICS_Inbound: Forwarded message to internal network
```

**Successful OUTBOUND**:
```
VirtIO_Net1_Driver: OUTBOUND: Forwarding X bytes to ICS_Outbound
ICS_Outbound: ALLOW - Message passed validation
ICS_Outbound: Forwarded message to external network
```

**Validation Rejection** (if implemented):
```
ICS_Inbound: REJECT - Payload too large (X > 60000)
```

---

## Common Issues and Troubleshooting

### Issue 1: Cannot Connect to Port 6000 or 7000

**Symptoms**:
```bash
$ echo "test" | nc localhost 6000
Connection refused
```

**Causes**:
- QEMU not started
- Port forwarding not configured
- VirtIO drivers not initialized

**Solutions**:
1. Verify QEMU is running: `ps aux | grep qemu`
2. Check port forwarding in simulate command
3. Check QEMU logs for driver initialization messages

### Issue 2: No Messages in QEMU Console

**Symptoms**: Connection succeeds but no log messages

**Causes**:
- Component not logging
- lwIP not initialized
- TCP server not started

**Solutions**:
1. Check for initialization messages in QEMU output
2. Look for "Ready for bidirectional ICS traffic" messages
3. Verify DEBUG_VERBOSE is enabled in driver source

### Issue 3: Messages Logged But Not Forwarded

**Symptoms**: VirtIO driver logs message but ICS component doesn't receive

**Causes**:
- Dataport not configured
- Notification not emitted
- ICS component not waiting on notification

**Solutions**:
1. Check CAmkES assembly (ics_dual_nic.camkes) for dataport connections
2. Verify `xxx_emit()` calls in drivers
3. Check ICS component `run()` function has event loop

---

## Performance Testing

### Throughput Test

```bash
# Generate 100 messages rapidly
for i in {1..100}; do
  echo "Message $i" | nc localhost 6000 -w 0.1
done

# Check statistics:
# - All 100 messages should be received
# - All 100 should be forwarded
# - Dropped count should be 0
```

### Concurrent Connection Test

```bash
# Open multiple connections simultaneously
for i in {1..5}; do
  (echo "Connection $i" | nc localhost 6000) &
done
wait

# Check for proper handling of MAX_TCP_CONNECTIONS (8)
```

---

## Test Results Documentation

### Test Report Template

```markdown
## Test Run: [Date/Time]

### Environment
- Build: [commit hash or build number]
- Platform: qemu-arm-virt (AArch64)
- QEMU Version: [version]

### Test 1: INBOUND Path
- Status: ✅ PASS / ❌ FAIL / ⚠️ PARTIAL
- Messages Sent: X
- Messages Received by Net0: X
- Messages Validated by ICS_Inbound: X
- Messages Forwarded to Net1: X
- Issues: [describe any issues]

### Test 2: OUTBOUND Path
- Status: ✅ PASS / ❌ FAIL / ⚠️ PARTIAL
- [same metrics as Test 1]

### Notes
- [Any observations, warnings, or issues]
```

---

## Next Steps (Phase 2)

### What's Missing (Reverse TX Paths)

Currently tested: **One-way data collection only**
- ✅ External → VirtIO_Net0 → ICS_Inbound → VirtIO_Net1 (collects data)
- ✅ Internal → VirtIO_Net1 → ICS_Outbound → VirtIO_Net0 (collects data)

**Phase 2 will add**:
- ⚠️ VirtIO_Net1 → Create new TCP session → Internal network
- ⚠️ VirtIO_Net0 → Create new TCP session → External network

### Full Bidirectional Test (Phase 2)

```bash
# Terminal 1: External listener
nc -l 6000

# Terminal 2: Internal client (via QEMU)
# [Inside guest] echo "test" | nc 10.0.3.15 7000

# Expected: Message appears in Terminal 1
# Flow: Internal → Net1 → ICS_Outbound → Net0 → External
```

---

## Quick Reference Commands

### Start Simulation
```bash
cd ~/phd/camkes-vm-examples/build
./simulate --extra-qemu-args="-netdev user,id=net0,hostfwd=tcp::6000-:6000 -device virtio-net-device,netdev=net0 -netdev user,id=net1,hostfwd=tcp::7000-:7000 -device virtio-net-device,netdev=net1"
```

### Test INBOUND
```bash
echo "test inbound" | nc localhost 6000
```

### Test OUTBOUND
```bash
echo "test outbound" | nc localhost 7000
```

### Stop Simulation
```
Ctrl-C in QEMU terminal
```

---

## Success Checklist

### Phase 1 Testing (Current)
- [ ] VirtIO_Net0_Driver initializes successfully
- [ ] VirtIO_Net1_Driver initializes successfully
- [ ] ICS_Inbound component starts
- [ ] ICS_Outbound component starts
- [ ] Can connect to port 6000 (external)
- [ ] Can connect to port 7000 (internal)
- [ ] VirtIO_Net0 logs INBOUND message reception
- [ ] ICS_Inbound logs metadata and validation
- [ ] VirtIO_Net1 logs OUTBOUND message reception
- [ ] ICS_Outbound logs metadata and validation
- [ ] Statistics counters increment correctly
- [ ] No crashes or errors during testing

### Phase 2 Testing (Future)
- [ ] VirtIO_Net1 creates TCP session to internal network
- [ ] VirtIO_Net0 creates TCP session to external network
- [ ] End-to-end bidirectional communication works
- [ ] Protocol break verified (no direct connection)
- [ ] TCP sessions properly terminated and recreated

---

**For Questions or Issues**: See [PROJECT_STATUS.md](PROJECT_STATUS.md) or [README.md](README.md)
