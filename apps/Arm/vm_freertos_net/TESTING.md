# Tier 2 Echo Server - Testing Guide

## Quick Start

The easiest way to test the echo server:

```bash
cd /home/konton-otome/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/vm_freertos_net

# Make the test script executable
chmod +x test_echo.sh

# Run the test script
./test_echo.sh
```

Select one of the three testing methods when prompted.

## Testing Methods

### Option 1: TAP Networking (Recommended - requires root)

**Best option for real packet testing!**

```bash
sudo ./test_echo.sh
# Select option 1
```

This creates a TAP interface that allows direct Ethernet frame injection.

**To test from another terminal:**

```bash
# Install arping if needed
sudo apt-get install arping iputils-arping

# Send ARP packets (Layer 2)
sudo arping -I tap0 192.168.100.2

# You should see in QEMU console:
#   EthernetDriver: 📥 RX packet #1: XX bytes
#   EthernetDriver: 📤 TX echo #1: XX bytes
```

**Advantages:**
- ✅ Real Layer 2 (Ethernet) communication
- ✅ Can send any packet type
- ✅ Most realistic testing

**Disadvantages:**
- ⚠️ Requires root privileges
- ⚠️ Need to setup TAP interface

### Option 2: Socket Backend (Easy - no root needed)

**Good for development/debugging without root access.**

```bash
./test_echo.sh
# Select option 2
```

This creates a TCP socket that QEMU uses as a network backend.

**To test from another terminal:**

```bash
# Send test packets using the Python script
python3 test_packet_sender.py
```

The script will:
1. Connect to QEMU's socket (localhost:1234)
2. Send a raw Ethernet frame
3. Wait for the echo response
4. Verify the echoed packet

**Advantages:**
- ✅ No root required
- ✅ Easy to script
- ✅ Automated testing possible

**Disadvantages:**
- ⚠️ Less realistic than TAP
- ⚠️ QEMU socket backend may have quirks

### Option 3: User Networking (Verification only)

**Only useful to verify initialization, won't see packet echoes.**

```bash
./test_echo.sh
# Select option 3
```

This uses QEMU's default user networking (SLIRP).

**Why it doesn't work for echo testing:**
- User networking operates at Layer 3 (IP)
- Our echo server operates at Layer 2 (Ethernet)
- Packets don't reach the virtio-net device as raw Ethernet frames

**Use this to:**
- ✅ Verify the echo server initializes correctly
- ✅ Check that virtqueues are set up
- ✅ Confirm RX buffers are allocated

## What to Look For

### Successful Initialization

You should see:
```
╔══════════════════════════════════════════════════════════╗
║         EthernetDriver Component - Tier 2               ║
║      VirtIO Echo Server - Packet RX/TX Test             ║
╚══════════════════════════════════════════════════════════╝

✓ Device is virtio-net!
✓ Feature negotiation successful!
✓ MAC: 52:54:00:12:34:56
✓ Virtqueue 0 ready!
✓ Virtqueue 1 ready!
✓ Device initialization complete!

✓ RX queue filled with 32 buffers
✓ Echo server is LIVE! Waiting for packets...
✓ Statistics: RX=0 TX=0 Dropped=0
```

### Successful Packet Echo

When a packet is received and echoed, you should see:
```
EthernetDriver: 📥 RX packet #1: 60 bytes
EthernetDriver: 📤 TX echo #1: 60 bytes
```

The statistics should increment:
```
Statistics: RX=1 TX=1 Dropped=0
```

## Troubleshooting

### No packets received

**Check:**
1. Is the echo server initialized? Look for "Echo server is LIVE!"
2. Are you using TAP or socket backend? (User networking won't work)
3. Is the MAC address correct? (`52:54:00:12:34:56`)
4. Are interrupts working? (Check for "⚡ IRQ:" messages)

### Packets received but not echoed

**Check:**
1. Look for error messages in QEMU console
2. Check statistics for dropped packets
3. Verify TX queue is working
4. Check for "Failed to echo packet" messages

### Build errors

```bash
cd /home/konton-otome/phd/camkes-vm-examples/build
ninja clean
env PYTHONPATH=../projects/camkes-tool:../projects/capdl/python-capdl-tool \
    ../init-build.sh \
    -DCAMKES_VM_APP=vm_freertos_net \
    -DAARCH64=1 \
    -DPLATFORM=qemu-arm-virt \
    -DSIMULATION=1 \
    -DLibUSB=OFF
ninja
```

### TAP interface issues

```bash
# List TAP interfaces
ip link show tap0

# Delete and recreate
sudo ip link delete tap0
sudo ip tuntap add dev tap0 mode tap
sudo ip link set tap0 up
sudo ip addr add 192.168.100.1/24 dev tap0
```

### Socket connection refused

Make sure QEMU is running with socket backend before running the test script.

## Manual Testing with scapy

If you have scapy installed:

```python
from scapy.all import *

# Create a test packet
pkt = Ether(dst='52:54:00:12:34:56', src='00:11:22:33:44:55') / \
      Raw(load='Hello Echo Server!')

# Send via TAP interface (requires root)
sendp(pkt, iface='tap0', verbose=True)

# Listen for echo
sniff(iface='tap0', count=1, timeout=5)
```

## Advanced: Monitor Mode

To see detailed QEMU virtio traffic:

```bash
qemu-system-aarch64 \
    ... \
    -trace 'virtio*' \
    -D /tmp/qemu-trace.log
```

Check `/tmp/qemu-trace.log` for virtio operations.

## Expected Performance

- **Packet rate**: Limited by interrupt processing (~1000 pps)
- **Latency**: ~1ms (interrupt + processing + echo)
- **Buffer capacity**: 32 packets
- **Max packet size**: 2048 bytes

## Next Steps

Once echo testing works:

1. **Tier 3a**: Add IP/TCP stack (lwIP)
2. **Tier 3b**: Integrate with FreeRTOS VM guest
3. **Tier 3c**: Bridge to sDDF for high performance

## Files

- `test_echo.sh` - Main test script
- `test_packet_sender.py` - Python packet sender
- `TESTING.md` - This file
- `ethernet_driver.c` - Echo server implementation

## Support

Check the research documentation:
- `tier2-echo-server-implementation.md` - Implementation details
- `tier1-virtio-camkes-success.md` - Device discovery
