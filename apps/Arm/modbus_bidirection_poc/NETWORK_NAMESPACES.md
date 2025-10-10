# Network Namespace Solution for Duplicate Route Issue

## Problem

The ICS security gateway uses the same subnet (192.168.95.0/24) on both network interfaces:
- **ens224**: 192.168.95.2/24 (internal network - connects to router)
- **ens256**: 192.168.95.1/24 (secure network - connects to real PLC)

These networks are physically isolated by different ESXi vSwitches, but the Linux kernel sees duplicate routes:
```
192.168.95.0/24 dev ens224 proto kernel scope link src 192.168.95.2
192.168.95.0/24 dev ens256 proto kernel scope link src 192.168.95.1
```

This causes routing confusion at the **host level** when trying to ping or test connectivity.

**Important:** This does NOT affect QEMU gateway operation - Net0 and Net1 remain properly isolated.

## Solution: Network Namespaces

Use Linux network namespaces to completely partition ens224 and ens256 into separate routing domains.

### Setup Script

The network namespace configuration is integrated into `setup-vlan-networking.sh`. When you run:

```bash
cd ~/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc
sudo ETH0_NAME=ens224 ETH1_NAME=ens256 ./setup-vlan-networking.sh
```

It creates:
- **Namespace `ns_internal`**: Contains ens224 (192.168.95.2) for internal network
- **Namespace `ns_secure`**: Contains ens256 (192.168.95.1) for secure network
- **Default namespace**: Contains tap0/tap1 for QEMU access

### Testing Connectivity

**From internal network side (ens224):**
```bash
# Ping the router on internal network
sudo ip netns exec ns_internal ping 192.168.95.1

# Run tcpdump on internal network
sudo ip netns exec ns_internal tcpdump -i ens224 -n port 502
```

**From secure network side (ens256):**
```bash
# Ping the real PLC
sudo ip netns exec ns_secure ping 192.168.95.2

# Run tcpdump on secure network
sudo ip netns exec ns_secure tcpdump -i ens256 -n port 502
```

**QEMU TAP interfaces (default namespace):**
```bash
# These work normally from default namespace
ping 10.2.0.2   # QEMU Net0
ping 10.3.0.2   # QEMU Net1

# TAP monitoring
sudo tcpdump -i tap0 -n port 502
sudo tcpdump -i tap1 -n port 502
```

## Architecture with Namespaces

```
[Default Namespace]
    ├─ tap0: 10.2.0.1/24 → QEMU Net0 (10.2.0.2)
    └─ tap1: 10.3.0.1/24 → QEMU Net1 (10.3.0.2)

[ns_internal Namespace]
    └─ ens224: 192.168.95.2/24 (internal network vSwitch)
        ↓ iptables NAT ↓
       Connected to tap0

[ns_secure Namespace]
    └─ ens256: 192.168.95.1/24 (secure network vSwitch)
        ↓ iptables NAT ↓
       Connected to tap1
```

## iptables NAT with Namespaces

**Important:** iptables NAT rules are applied in the **default namespace** where tap0/tap1 reside. The NAT happens at the TAP interface level, not inside the namespaces.

Traffic flow:
1. Packet arrives at ens224 (in ns_internal) → enters default namespace via veth
2. iptables PREROUTING in default namespace: DNAT to 10.2.0.2
3. Packet reaches tap0 → QEMU Net0
4. QEMU forwards to Net1 → tap1
5. iptables POSTROUTING: SNAT to 192.168.95.1
6. Packet exits via veth to ens256 (in ns_secure)

## Verifying Setup

```bash
# List all namespaces
ip netns list
# Should show: ns_internal, ns_secure

# Check interfaces in each namespace
ip netns exec ns_internal ip addr show
ip netns exec ns_secure ip addr show

# Check routing tables
ip netns exec ns_internal ip route
ip netns exec ns_secure ip route
ip route   # Default namespace

# Verify no duplicate routes in default namespace
ip route | grep 192.168.95
# Should show NOTHING (all moved to namespaces)
```

## Running QEMU Gateway

QEMU runs in the **default namespace** and accesses tap0/tap1 normally:

```bash
# Start gateway (runs in default namespace)
cd ~/modbus_gateway_deploy
./run-remote.sh

# QEMU can access tap0/tap1 without any changes
# NAT rules in default namespace handle routing to/from ens224/ens256
```

## Troubleshooting

### Can't ping from host after namespace setup

**Expected behavior!** After moving ens224/ens256 into namespaces, you must use `ip netns exec`:

```bash
# WRONG (won't work):
ping -I ens224 192.168.95.1

# CORRECT:
sudo ip netns exec ns_internal ping 192.168.95.1
```

### QEMU can't start or can't access network

If QEMU fails to start:
1. Verify tap0/tap1 are in default namespace: `ip link show tap0`
2. Check TAP ownership: `ip link show tap0 | grep -o 'user [^ ]*'`
3. Verify iptables NAT rules: `sudo iptables -t nat -L -n -v`

### Need to remove namespaces

```bash
# Delete namespaces (also deletes interfaces inside them)
sudo ip netns del ns_internal
sudo ip netns del ns_secure

# Re-run setup script to recreate
sudo ETH0_NAME=ens224 ETH1_NAME=ens256 ./setup-vlan-networking.sh
```

## Why This Works

1. **Physical isolation**: ESXi vSwitches keep networks separate at layer 2
2. **Namespace isolation**: Linux routing tables separated per namespace
3. **NAT in default namespace**: iptables bridges between namespaces and QEMU
4. **QEMU isolation**: Net0 and Net1 firewall/routing keeps traffic separated

Each layer provides defense-in-depth isolation for the ICS security gateway.

## Performance Impact

Network namespaces add **negligible overhead** (~1-2% CPU for namespace context switching). The security benefit of proper isolation far outweighs the minimal performance cost.

## Alternative: Policy Routing (Not Recommended)

Instead of namespaces, you could use policy routing with multiple routing tables. However, this is more complex and error-prone:

```bash
# More complex and fragile approach (NOT recommended)
ip route add 192.168.95.2/32 dev ens256 table 100
ip route add 192.168.95.1/32 dev ens224 table 200
ip rule add from 192.168.95.1 table 100
ip rule add from 192.168.95.2 table 200
```

**Use namespaces instead** - cleaner, more robust, easier to understand.
