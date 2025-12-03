# Network Setup Guide - Bridge Architecture

## Quick Start

### One-Time Installation (with Persistence)

Run this command to configure bridges and make them persistent across reboots:

```bash
cd /home/qemu/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc
sudo ./scripts/install-persistent-networking.sh
```

This script will:
1. ✅ Create bridges br0 and br1
2. ✅ Attach physical NICs (ens224, ens256) to bridges
3. ✅ Create TAP interfaces (tap0, tap1)
4. ✅ Install systemd service for auto-start on boot
5. ✅ Enable the service

After installation, **bridges will automatically be created on every reboot**.

---

## Manual Setup (Without Persistence)

If you only want to set up bridges for this session (won't persist across reboots):

```bash
cd /home/qemu/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc
sudo ./scripts/setup-bridge-networking.sh
```

---

## Architecture Overview

### Bridge Configuration (Layer 2)

```
┌─────────────────────────────────────────────────────────┐
│  External Network (192.168.96.0/24 - SCADA side)       │
│    ↓                                                    │
│  ens224 → br0 (bridge) → tap0                          │
│    ↓                                                    │
│  QEMU nic0 (VirtIO_Net0): 192.168.96.2/24             │
│    ↓ [Connection tracking + IP rewriting]              │
│    ↓ [ICS validation pipeline]                         │
│    ↓                                                    │
│  QEMU nic1 (VirtIO_Net1): 192.168.95.1/24             │
│    ↓                                                    │
│  tap1 → br1 (bridge) → ens256                          │
│    ↓                                                    │
│  Internal Network (192.168.95.0/24 - PLC side)         │
└─────────────────────────────────────────────────────────┘
```

### Key Characteristics

- **Pure Layer 2 Forwarding**: No NAT, no SNAT
- **QEMU Owns Gateway IPs**: 192.168.96.2 (Net0), 192.168.95.1 (Net1)
- **No Host IPs**: Physical NICs and TAP interfaces have NO IP addresses
- **Original IP Preservation**: SCADA and PLC IPs preserved for validation
- **Protocol-Break Architecture**: TCP connections terminated and recreated in QEMU

---

## Verification Commands

### Check Bridge Status

```bash
# Show all bridge members
bridge link show

# Expected output:
# 3: ens224: <BROADCAST,MULTICAST,UP,LOWER_UP> master br0
# 4: tap0: <BROADCAST,MULTICAST,UP,LOWER_UP> master br0
# 5: ens256: <BROADCAST,MULTICAST,UP,LOWER_UP> master br1
# 6: tap1: <BROADCAST,MULTICAST,UP,LOWER_UP> master br1
```

### Check Interface IPs (Should be EMPTY)

```bash
# Physical NICs should have NO IP addresses
ip addr show ens224 ens256 tap0 tap1
```

### Check Systemd Service

```bash
# Check service status
sudo systemctl status qemu-bridges.service

# View service logs
sudo journalctl -u qemu-bridges.service -n 50
```

### Test Bridge Connectivity

From external network (SCADA side):
```bash
# Should reach QEMU nic0
ping 192.168.96.2
```

From internal network (PLC side):
```bash
# Should reach QEMU nic1
ping 192.168.95.1
```

---

## Systemd Service Management

### Enable/Disable Auto-Start

```bash
# Enable (start on boot)
sudo systemctl enable qemu-bridges.service

# Disable (don't start on boot)
sudo systemctl disable qemu-bridges.service
```

### Manual Start/Stop

```bash
# Start bridges
sudo systemctl start qemu-bridges.service

# Stop bridges
sudo systemctl stop qemu-bridges.service

# Restart bridges
sudo systemctl restart qemu-bridges.service
```

### Check Service Status

```bash
# Detailed status
sudo systemctl status qemu-bridges.service

# Is service enabled?
systemctl is-enabled qemu-bridges.service

# Is service active?
systemctl is-active qemu-bridges.service
```

---

## Troubleshooting

### Issue: Bridges Not Created After Reboot

**Check:**
```bash
sudo systemctl status qemu-bridges.service
```

**Fix:**
```bash
# Re-enable service
sudo systemctl enable qemu-bridges.service

# Start manually
sudo systemctl start qemu-bridges.service
```

### Issue: Physical NICs Have IP Addresses

**Check:**
```bash
ip addr show ens224 ens256
```

**Fix:**
```bash
# Flush IPs
sudo ip addr flush dev ens224
sudo ip addr flush dev ens256

# Or restart bridge service
sudo systemctl restart qemu-bridges.service
```

### Issue: TAP Interfaces Missing

**Check:**
```bash
ip link show tap0 tap1
```

**Fix:**
```bash
# Restart bridge service
sudo systemctl restart qemu-bridges.service
```

### Issue: Bridge Members Not Attached

**Check:**
```bash
bridge link show
```

**Fix:**
```bash
# Reattach manually
sudo ip link set ens224 master br0
sudo ip link set tap0 master br0
sudo ip link set ens256 master br1
sudo ip link set tap1 master br1
```

---

## Files Created

| File | Purpose |
|------|---------|
| `scripts/setup-bridge-networking.sh` | Main bridge configuration script |
| `scripts/install-persistent-networking.sh` | One-time installation with persistence |
| `scripts/qemu-bridges.service` | Systemd service file |
| `/etc/systemd/system/qemu-bridges.service` | Installed service (after running install script) |

---

## Uninstall

To remove persistent networking:

```bash
# Disable and stop service
sudo systemctl disable qemu-bridges.service
sudo systemctl stop qemu-bridges.service

# Remove service file
sudo rm /etc/systemd/system/qemu-bridges.service

# Reload systemd
sudo systemctl daemon-reload

# Manual cleanup (optional)
sudo ip link set br0 down
sudo ip link set br1 down
sudo ip link delete br0
sudo ip link delete br1
sudo ip link delete tap0
sudo ip link delete tap1
```

---

## Next Steps After Network Setup

1. **Start QEMU Gateway**:
   ```bash
   cd /home/qemu/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc
   ./scripts/run-remote.sh
   ```

2. **Verify QEMU Console**:
   Look for these messages:
   ```
   ✅ VirtIO_Net0_Driver: Using STATIC IP configuration:
   ✅   IP:      192.168.96.2/24
   ✅   Gateway: 192.168.96.1 (pfSense)
   ✅ VirtIO_Net0_Driver: TCP Echo Server listening on port 502

   ✅ VirtIO_Net1_Driver: Using STATIC IP configuration:
   ✅   IP:      192.168.95.1/24
   ✅ VirtIO_Net1_Driver: TCP Echo Server listening on port 502
   ```

3. **Test SCADA ↔ PLC Communication**:
   - SCADA should be able to connect to PLC IP (192.168.95.2)
   - Traffic will transparently flow through the gateway
   - All Modbus traffic will be validated by ICS components

---

## References

- Main README: `README.md`
- GDB Debug Guide: `GDB-DEBUG-GUIDE.md`
- VirtIO Race Condition Analysis: `VIRTIO_RACE_CONDITION_ANALYSIS.md`
