# Remote Server Setup Guide

## Interface Name Configuration

Your server has interfaces named `ens224` and `ens256` instead of `eth0` and `eth1`.

**You have two options:**

### Option 1: Use Environment Variables (Recommended)

Set environment variables when running the setup script:

```bash
# Configure network with your actual interface names
ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E ./setup-vlan-networking.sh
```

**Why this is better:**
- ✅ No file editing required
- ✅ Works with any interface names
- ✅ Safer (no system changes)
- ✅ Easy to verify: `ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E env | grep ETH`

### Option 2: Edit the Script Directly

Alternatively, edit `setup-vlan-networking.sh` and change these lines:

```bash
# Find these lines (around line 46-47):
ETH0_NAME="${ETH0_NAME:-eth0}"
ETH1_NAME="${ETH1_NAME:-eth1}"

# Change to:
ETH0_NAME="${ETH0_NAME:-ens224}"
ETH1_NAME="${ETH1_NAME:-ens256}"
```

## Complete Deployment Steps for Your Server

### 1. Verify Interface Names

```bash
ip link show

# Expected output:
# ...
# 3: ens224: <BROADCAST,MULTICAST> ...
# 4: ens256: <BROADCAST,MULTICAST> ...
```

### 2. Extract Deployment Package

```bash
tar xzf modbus_gateway_deploy.tar.gz
cd modbus_gateway_deploy
```

### 3. Configure Network (Using Environment Variables)

```bash
# Set interface names and run setup
ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E ./setup-vlan-networking.sh
```

**This configures:**
- `ens224` → 192.168.95.2/24 (impersonates PLC)
- `ens256` → 192.168.90.1/24 (impersonates router)
- `tap0` → 10.2.0.1/24 (QEMU Net0 gateway)
- `tap1` → 10.3.0.1/24 (QEMU Net1 gateway)
- iptables NAT rules
- IP forwarding

### 4. Verify Configuration

```bash
# Check interfaces
ip addr show ens224  # Should have 192.168.95.2/24
ip addr show ens256  # Should have 192.168.90.1/24
ip addr show tap0    # Should have 10.2.0.1/24
ip addr show tap1    # Should have 10.3.0.1/24

# Check iptables NAT rules
sudo iptables -t nat -L -n -v | grep -E "10.2.0.2|10.3.0.2"
# Should show 4 rules (2 DNAT, 2 SNAT)

# Check IP forwarding
cat /proc/sys/net/ipv4/ip_forward
# Should be: 1
```

### 5. Start Security Gateway

```bash
./run-remote.sh
```

**The script auto-detects interface names**, so it should work without modification!

Expected output:
```
========================================
VLAN-based ICS Security Gateway
========================================

Network Configuration:

Physical NICs:
  ens224: 192.168.95.2/24 (impersonates PLC)
  ens256: 192.168.90.1/24 (impersonates router)

TAP Interfaces (Private Networks):
  tap0: 10.2.0.1/24 (QEMU Net0 gateway)
  tap1: 10.3.0.1/24 (QEMU Net1 gateway)

Starting QEMU...
```

### 6. Test Connectivity

**From the gateway server itself:**
```bash
# Test QEMU guest connectivity
ping 10.2.0.2  # Should reach QEMU Net0
ping 10.3.0.2  # Should reach QEMU Net1
```

**From another machine on the network:**
```bash
# From router or another device on 192.168.95.0/24
ping 192.168.95.2  # Should reach ens224

# From PLC or device on secure VLAN
ping 192.168.90.1  # Should reach ens256
```

## Troubleshooting

### "Interface not found" error

If you see errors about `eth0` or `eth1` not existing:

**Solution:** Use environment variables when running setup:
```bash
ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E ./setup-vlan-networking.sh
```

### Scripts still reference eth0/eth1

**For setup-vlan-networking.sh:**
- Uses `${ETH0_NAME}` variable, so environment override works

**For setup-iptables.sh:**
- Currently hardcoded - you may need to edit or run after setup-vlan-networking.sh

**For run-remote.sh:**
- Auto-detects interfaces by IP address (no changes needed!)

### Making Persistent Across Reboots

**Option 1: iptables-persistent**
```bash
sudo apt install iptables-persistent
sudo netfilter-persistent save
```

**Option 2: Systemd Service**

Create `/etc/systemd/system/ics-gateway-network.service`:
```ini
[Unit]
Description=ICS Security Gateway Network Setup
After=network.target

[Service]
Type=oneshot
Environment="ETH0_NAME=ens224"
Environment="ETH1_NAME=ens256"
ExecStart=/home/user/modbus_gateway_deploy/setup-vlan-networking.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

Enable:
```bash
sudo systemctl enable ics-gateway-network.service
sudo systemctl start ics-gateway-network.service
```

## Quick Reference

| What | Command |
|------|---------|
| **Setup network** | `ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E ./setup-vlan-networking.sh` |
| **Setup iptables only** | `sudo ./setup-iptables.sh` |
| **Start gateway** | `./run-remote.sh` |
| **Check interfaces** | `ip addr show` |
| **Check NAT rules** | `sudo iptables -t nat -L -n -v` |
| **Check IP forwarding** | `cat /proc/sys/net/ipv4/ip_forward` |
| **Capture traffic on tap0** | `sudo tcpdump -i tap0 -n port 502` |
| **Capture traffic on tap1** | `sudo tcpdump -i tap1 -n port 502` |
| **Kill QEMU** | `pkill -9 qemu` or press `Ctrl-A X` in QEMU console |

## Your Specific Configuration

Based on your interface names:

```bash
# Interface mapping:
ens224 → 192.168.95.2/24  # Impersonates PLC (originally eth0)
ens256 → 192.168.90.1/24  # Impersonates router (originally eth1)

# Physical connections:
ens224: Connected to original internal network (192.168.95.0/24)
ens256: Connected to secure VLAN/isolated switch

# Network topology:
[Router 192.168.95.1] ←→ [ens224: 192.168.95.2]
                              ↓ (NAT via iptables)
                          [tap0: 10.2.0.1] ←→ [QEMU Net0: 10.2.0.2]
                              ↓ (Security gateway validation)
                          [QEMU Net1: 10.3.0.2] ←→ [tap1: 10.3.0.1]
                              ↓ (NAT via iptables)
                          [ens256: 192.168.90.1] ←→ [Real PLC: 192.168.95.2]
```

## Important Notes

⚠️ **Real PLC must be on secure VLAN (connected via ens256)**
- The real PLC MUST be physically isolated from the original network
- Same IP (192.168.95.2) exists on both ens224 and secure VLAN
- No IP conflict because they're on different Layer 2 networks

⚠️ **PLC routing configuration**
- PLC's default gateway should be: 192.168.90.1 (your ens256)
- Test from PLC: `ping 192.168.90.1`

⚠️ **After reboot, you must re-run setup**
- TAP interfaces are ephemeral
- iptables rules are not persistent
- Use systemd service or iptables-persistent for auto-setup
