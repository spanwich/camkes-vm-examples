# GRFICS ICS Security Gateway - Deployment Guide

## Overview

This guide walks through deploying the Modbus Bidirectional POC as a transparent security gateway between GRFICS SCADA and PLC systems.

## Network Topology

```
[SCADA 192.168.90.5]
         ↓
    (External Network: 192.168.90.0/24)
         ↓
     [Router]
         ↓
    (Internal Network: 192.168.95.0/24) ← Original GRFICS network
         ↓
  ┌────────────────────────────────────────────┐
  │   Security Gateway (Our Modbus POC)        │
  │                                            │
  │  Net0: 192.168.95.2 (pretends to be PLC)  │
  │        Listens on :502                     │
  │                                            │
  │  Net1: 192.168.90.5 (pretends to be SCADA)│
  │        Listens on :502                     │
  └────────────────────────────────────────────┘
         ↓
    (Secure Subnet: 192.168.95.0/24) ← Isolated switch
         ↓
   [Real PLC 192.168.95.2]
```

## Prerequisites

### Hardware Requirements
- Server with 2 physical NICs (or 1 NIC + VLAN support)
- At least 2GB RAM
- x86_64 or ARM64 processor

### Software Requirements
- Linux kernel with TAP/TUN support
- QEMU (qemu-system-arm)
- iproute2 (for ip command)
- Optional: bridge-utils (if using physical NIC bridging)

### Network Setup
- **eth0**: Connected to internal network (192.168.95.0/24)
- **eth1**: Connected to secure subnet switch (192.168.95.0/24 - isolated)
- Router configured to route between 192.168.90.0/24 ↔ 192.168.95.0/24

## Step-by-Step Deployment

### Step 1: Build the Security Gateway

```bash
cd /home/iamfo470/phd/camkes-vm-examples
mkdir -p build_modbus && cd build_modbus

../init-build.sh -DPLATFORM=qemu-arm-virt \
  -DAARCH32=TRUE \
  -DCAMKES_APP=modbus_bidirection_poc

ninja
```

**Verify build succeeded:**
```bash
ls -lh images/capdl-loader-image-arm-qemu-arm-virt
```

### Step 2: Setup TAP Networking

```bash
cd ../projects/vm-examples/apps/Arm/modbus_bidirection_poc
sudo ./setup-tap-networking.sh
```

**This creates:**
- `tap0`: 192.168.95.2/24 (Internal network - PLC IP)
- `tap1`: 192.168.90.5/24 (Secure subnet - SCADA IP)

**Verify TAP interfaces:**
```bash
ip addr show tap0
ip addr show tap1
```

### Step 3: Connect TAP to Physical NICs (Optional)

If deploying with physical network isolation:

```bash
# Bridge tap0 to internal network NIC (eth0)
sudo brctl addbr br_internal
sudo brctl addif br_internal tap0
sudo brctl addif br_internal eth0
sudo ip link set br_internal up

# Bridge tap1 to secure subnet NIC (eth1)
sudo brctl addbr br_secure
sudo brctl addif br_secure tap1
sudo brctl addif br_secure eth1
sudo ip link set br_secure up
```

### Step 4: Start the Security Gateway

```bash
./run-grfics-deployment.sh
```

**Expected output:**
```
========================================
Starting GRFICS ICS Security Gateway
========================================

TAP Interface Status:
  tap0: 192.168.95.2/24
  tap1: 192.168.90.5/24

QEMU Configuration:
  Network 0 (Internal):
    - TAP: tap0
    - MAC: 52:54:00:12:34:56
    - Guest IP: 192.168.95.2 (pretends to be PLC)
    - Listens on: Modbus port 502

  Network 1 (Secure Subnet):
    - TAP: tap1
    - MAC: 52:54:00:12:34:57
    - Guest IP: 192.168.90.5 (pretends to be SCADA)
    - Listens on: Modbus port 502

Starting QEMU...
```

### Step 5: Verify System Startup

Watch for these messages in QEMU console:

```
✅ VirtIO_Net0_Driver: Using STATIC IP configuration (GRFICS deployment):
✅   IP:      192.168.95.2 (pretends to be PLC)
✅ VirtIO_Net0_Driver: TCP Echo Server listening on port 502

✅ VirtIO_Net1_Driver: Using STATIC IP configuration (GRFICS deployment):
✅   IP:      192.168.90.5 (pretends to be SCADA)
✅ VirtIO_Net1_Driver: TCP Echo Server listening on port 502

✅ ICS_Inbound: Ready to validate external→internal traffic
✅ ICS_Outbound: Ready to validate internal→external traffic
```

### Step 6: Test with GRFICS

**On GRFICS SCADA machine (192.168.90.5):**

The SCADA will automatically try to connect to PLC at 192.168.95.2:502. No configuration changes needed!

**Monitor traffic in QEMU console:**

**SCADA → PLC (Inbound):**
```
VirtIO_Net0_Driver: TCP connection accepted from 192.168.90.x
VirtIO_Net0_Driver: INBOUND: Forwarding X bytes to ICS_Inbound
ICS_Inbound: Validating Modbus request...
ICS_Inbound: ALLOW - Forwarding to internal network
VirtIO_Net1_Driver: Creating client connection to 192.168.95.2:502
VirtIO_Net1_Driver: Sent X bytes to real PLC
```

**PLC → SCADA (Outbound):**
```
VirtIO_Net1_Driver: TCP connection accepted from 192.168.95.x
VirtIO_Net1_Driver: OUTBOUND: Forwarding X bytes to ICS_Outbound
ICS_Outbound: Validating Modbus response...
ICS_Outbound: ALLOW - Forwarding to external network
VirtIO_Net0_Driver: Creating client connection to 192.168.90.5:502
VirtIO_Net0_Driver: Sent X bytes to real SCADA
```

## Testing Checklist

- [ ] TAP interfaces created successfully (tap0, tap1)
- [ ] QEMU starts without errors
- [ ] Both VirtIO drivers initialize with correct IPs
- [ ] TCP servers listening on port 502
- [ ] ICS_Inbound and ICS_Outbound ready
- [ ] GRFICS SCADA can communicate with PLC (no reconfiguration)
- [ ] Traffic appears in QEMU logs
- [ ] Modbus commands are validated and forwarded
- [ ] Responses flow back correctly

## Configuration Reference

### Current IP Configuration

| Component | IP Address | Port | Role |
|-----------|------------|------|------|
| GRFICS SCADA | 192.168.90.5 | 502 | Real SCADA (unchanged) |
| Net0 (tap0) | 192.168.95.2 | 502 | Pretends to be PLC |
| Net1 (tap1) | 192.168.90.5 | 502 | Pretends to be SCADA |
| Real PLC | 192.168.95.2 | 502 | Real PLC (on secure switch) |

### Port Mapping

**Inbound Path (SCADA → PLC):**
1. SCADA (192.168.90.5) sends to PLC (192.168.95.2:502)
2. Net0 intercepts (listening on 192.168.95.2:502)
3. ICS_Inbound validates
4. Net1 forwards to real PLC (192.168.95.2:502 on secure switch)

**Outbound Path (PLC → SCADA):**
1. PLC (192.168.95.2) sends to SCADA (192.168.90.5:502)
2. Net1 intercepts (listening on 192.168.90.5:502)
3. ICS_Outbound validates
4. Net0 forwards to real SCADA (192.168.90.5:502)

## Troubleshooting

### Issue: TAP interfaces not found
**Solution:**
```bash
sudo ./setup-tap-networking.sh
```

### Issue: Permission denied on TAP
**Solution:**
```bash
sudo chown $USER:$USER /dev/net/tun
# Or run QEMU with sudo (not recommended)
```

### Issue: SCADA cannot reach PLC
**Check:**
1. Router is forwarding between 192.168.90.0/24 and 192.168.95.0/24
2. TAP interfaces are UP: `ip link show tap0 tap1`
3. Security gateway is running
4. Firewall rules not blocking traffic

### Issue: Messages not appearing in QEMU logs
**Check:**
1. TAP interfaces have correct IPs
2. SCADA is sending to 192.168.95.2:502
3. Routing is configured correctly
4. Use `tcpdump` on TAP interfaces:
   ```bash
   sudo tcpdump -i tap0 -n port 502
   sudo tcpdump -i tap1 -n port 502
   ```

### Issue: Connection refused
**Check:**
1. Both VirtIO drivers initialized successfully
2. TCP servers are listening on port 502
3. lwIP stack initialized correctly

## Advanced: Bridge Mode vs TAP Mode

### Current Setup (TAP Mode)
- TAP interfaces have their own IPs
- Works for testing without physical NICs
- QEMU guest connects via TAP

### Production Setup (Bridge Mode)
- TAP interfaces bridged to physical NICs
- True transparent proxy
- No IP on TAP (just on bridge)

**Convert to bridge mode:**
```bash
# Remove IPs from TAP
sudo ip addr flush dev tap0
sudo ip addr flush dev tap1

# Add to bridges
sudo brctl addbr br_internal
sudo brctl addif br_internal tap0 eth0
sudo ip link set br_internal up

sudo brctl addbr br_secure
sudo brctl addif br_secure tap1 eth1
sudo ip link set br_secure up
```

## Cleanup

To remove TAP interfaces:
```bash
sudo ip link delete tap0
sudo ip link delete tap1

# If using bridges:
sudo ip link set br_internal down
sudo brctl delbr br_internal
sudo ip link set br_secure down
sudo brctl delbr br_secure
```

## Next Steps

1. **Add Modbus Validation**: Implement Modbus-specific parsing in ICS_Inbound/ICS_Outbound
2. **Enable Syslog**: Send logs to central syslog server
3. **Add Monitoring**: Track Modbus function codes, register access patterns
4. **Implement Policies**: Whitelist/blacklist specific Modbus commands
5. **Performance Testing**: Measure latency impact on ICS operations

## Security Considerations

✅ **Protocol Break**: Each connection is terminated and recreated (no direct TCP path)
✅ **Stateless Validation**: Each message validated independently
✅ **Physical Isolation**: Secure subnet on separate switch
✅ **seL4 Verification**: Formally verified microkernel guarantees
✅ **Transparent Deployment**: No changes to SCADA or PLC

⚠️ **Current Limitations:**
- Logging to console only (add syslog for production)
- Basic validation (extend for Modbus-specific rules)
- No persistent state (consider adding for anomaly detection)

## Support

For issues or questions:
1. Check QEMU logs for detailed error messages
2. Use `tcpdump` to verify network traffic
3. Review seL4/CAmkES documentation
4. Consult GRFICS simulator documentation

---

**Deployment Checklist:**
- [ ] System built successfully
- [ ] TAP interfaces configured
- [ ] Physical NICs connected (if applicable)
- [ ] QEMU running with TAP networking
- [ ] GRFICS SCADA communicating through gateway
- [ ] Logs showing validated traffic
- [ ] Both directions working (inbound + outbound)
