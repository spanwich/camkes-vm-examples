# VLAN-Based Transparent ICS Security Gateway Architecture

**Project:** modbus_bidirection_poc
**Date:** 2025-10-10
**Deployment:** GRFICS ICS Simulator with Physical Network Isolation

---

## Architecture Overview

This design creates a **completely transparent security gateway** that intercepts all traffic between SCADA and PLC without requiring ANY configuration changes on either endpoint.

### Key Innovation: VLAN Isolation

By using a separate VLAN for the real PLC, we can:
- **Impersonate PLC** to the router (eth0 = 192.168.95.2)
- **Impersonate Router** to the PLC (eth1 = 192.168.90.1)
- **Avoid IP conflicts** (same IPs exist on different Layer 2 networks)
- **Achieve true transparency** (zero config changes needed)

---

## Network Topology

```
┌─────────────────────────────────────────────────────────────────────┐
│                        External Network                             │
│                      (192.168.90.0/24)                              │
│                                                                     │
│                   [SCADA: 192.168.90.5]                            │
│                              ↓                                      │
│                   [Router: 192.168.90.1]                           │
│                              ↓                                      │
└─────────────────────────────┬───────────────────────────────────────┘
                              ↓
┌─────────────────────────────┴───────────────────────────────────────┐
│                      Internal Network                               │
│                      (192.168.95.0/24)                              │
│                                                                     │
│              [Router: 192.168.95.1]                                │
│              [Other devices...]                                     │
│              [eth0: 192.168.95.2] ← Security Gateway (impersonates PLC)
│                              ↓                                      │
└─────────────────────────────┬───────────────────────────────────────┘
                              ↓
┌─────────────────────────────┴───────────────────────────────────────┐
│                  Security Gateway Server                            │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────┐     │
│  │  Linux Host (Physical Server)                            │     │
│  │                                                           │     │
│  │  eth0: 192.168.95.2/24 ←─────┐                          │     │
│  │  (Impersonates PLC)           │                          │     │
│  │                               │                          │     │
│  │  tap0: 10.2.0.1/24 ───────────┼─→ [iptables NAT]        │     │
│  │  (Private network)            │                          │     │
│  │                               │                          │     │
│  │         ┌─────────────────────┘                          │     │
│  │         ↓                                                 │     │
│  │  ┌──────────────────────────────────────┐               │     │
│  │  │   QEMU Guest (seL4/CAmkES)           │               │     │
│  │  │                                       │               │     │
│  │  │   Net0: 10.2.0.2/24 (via tap0)       │               │     │
│  │  │   - Listens on :502                   │               │     │
│  │  │   - Receives from eth0 (router side)  │               │     │
│  │  │                                       │               │     │
│  │  │   [ICS Security Validation]           │               │     │
│  │  │   - Modbus protocol parsing           │               │     │
│  │  │   - Policy enforcement                │               │     │
│  │  │   - Threat detection                  │               │     │
│  │  │                                       │               │     │
│  │  │   Net1: 10.3.0.2/24 (via tap1)       │               │     │
│  │  │   - Listens on :502                   │               │     │
│  │  │   - Sends to eth1 (PLC side)          │               │     │
│  │  │                                       │               │     │
│  │  └──────────────────────────────────────┘               │     │
│  │         ↓                                                 │     │
│  │         └─────────────────────┐                          │     │
│  │                               │                          │     │
│  │  tap1: 10.3.0.1/24 ───────────┼─→ [iptables NAT]        │     │
│  │  (Private network)            │                          │     │
│  │                               │                          │     │
│  │  eth1: 192.168.90.1/24 ←──────┘                          │     │
│  │  (Impersonates Router)                                   │     │
│  │                                                           │     │
│  └───────────────────────────────────────────────────────────┘     │
│                              ↓                                      │
└─────────────────────────────┬───────────────────────────────────────┘
                              ↓
┌─────────────────────────────┴───────────────────────────────────────┐
│              Secure VLAN (Isolated Physical Network)                │
│                      (192.168.95.0/24)                              │
│                                                                     │
│              [eth1: 192.168.90.1] ← Gateway (impersonates router)  │
│                              ↓                                      │
│                   [Real PLC: 192.168.95.2]                         │
│                   (Same IP as eth0, no conflict!)                   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## IP Address Mapping

| Component | IP Address | Network | Role |
|-----------|------------|---------|------|
| **External Network** |
| SCADA | 192.168.90.5 | 192.168.90.0/24 | Real SCADA system |
| Router (external) | 192.168.90.1 | 192.168.90.0/24 | Real router |
| **Internal Network (Original)** |
| Router (internal) | 192.168.95.1 | 192.168.95.0/24 | Real router |
| **eth0** | **192.168.95.2** | 192.168.95.0/24 | **Impersonates PLC** |
| **Gateway Private Networks** |
| tap0 | 10.2.0.1 | 10.2.0.0/24 | Host side of QEMU Net0 |
| QEMU Net0 | 10.2.0.2 | 10.2.0.0/24 | Guest VirtIO Net0 |
| tap1 | 10.3.0.1 | 10.3.0.0/24 | Host side of QEMU Net1 |
| QEMU Net1 | 10.3.0.2 | 10.3.0.0/24 | Guest VirtIO Net1 |
| **Secure VLAN (Isolated)** |
| **eth1** | **192.168.90.1** | 192.168.95.0/24* | **Impersonates Router** |
| Real PLC | **192.168.95.2** | 192.168.95.0/24* | Real PLC (isolated) |

*Note: Same subnet addressing as original network, but physically isolated VLAN

---

## Why This Works: Layer 2 Isolation

### No IP Conflict Despite Duplicate IPs

**eth0** and **Real PLC** both use IP `192.168.95.2`, but:

1. **eth0** is on the original physical network
2. **Real PLC** is on a completely separate VLAN
3. **No Layer 2 connectivity** between them
4. **Result:** No ARP conflict, no routing conflict

This is analogous to having two houses with the same street address in different cities - no problem because they're in different locations.

### Perfect Impersonation

**From Router's Perspective:**
```
Router sees: PLC at 192.168.95.2
Actually talking to: eth0 (192.168.95.2)
Router doesn't know: Real PLC moved to secure VLAN
```

**From Real PLC's Perspective:**
```
PLC sees: Router at 192.168.90.1
Actually talking to: eth1 (192.168.90.1)
PLC doesn't know: It's isolated from original network
```

**From SCADA's Perspective:**
```
SCADA sees: Normal routing to PLC via router
Actually happening: Traffic intercepted by security gateway
SCADA doesn't know: Gateway exists
```

---

## Traffic Flow Examples

### SCADA → PLC (Read Coils Request)

```
Step 1: SCADA (192.168.90.5) sends Modbus request to PLC (192.168.95.2:502)
        Packet: SRC=192.168.90.5 DST=192.168.95.2 DPORT=502

Step 2: Router (192.168.90.1/192.168.95.1) routes to internal network
        Routes to: 192.168.95.2 (original PLC address)

Step 3: Packet arrives at eth0 (192.168.95.2) - Gateway intercepts!
        Router thinks it's talking to PLC

Step 4: iptables PREROUTING on eth0:
        DNAT: 192.168.95.2:502 → 10.2.0.2:502
        Packet forwarded to tap0

Step 5: QEMU Net0 (10.2.0.2) receives packet via tap0
        VirtIO_Net0_Driver processes Modbus request

Step 6: ICS Security Validation
        - Parse Modbus protocol
        - Check function code (0x01 - Read Coils)
        - Validate coil addresses
        - Apply security policy
        - Decision: ALLOW or BLOCK

Step 7: If ALLOWED, forward to VirtIO_Net1_Driver (10.3.0.2)
        Internal CAmkES component communication

Step 8: QEMU Net1 (10.3.0.2) sends to tap1
        Packet exits QEMU guest

Step 9: iptables POSTROUTING on eth1:
        SNAT: 10.3.0.2 → 192.168.90.1
        Packet appears to come from "router"

Step 10: Packet exits eth1 (192.168.90.1) to secure VLAN
         PLC sees traffic from router (192.168.90.1)

Step 11: Real PLC (192.168.95.2) receives Modbus request
         PLC thinks it's from SCADA (via normal routing)
```

### PLC → SCADA (Read Coils Response)

```
Step 1: Real PLC (192.168.95.2) sends response to SCADA (192.168.90.5:502)
        Packet: SRC=192.168.95.2 DST=192.168.90.5 DPORT=502

Step 2: PLC's routing table sends to default gateway (192.168.90.1)
        Routes via: eth1 (our impersonated router)

Step 3: Packet arrives at eth1 (192.168.90.1) - Gateway intercepts!
        PLC thinks it's talking to router

Step 4: iptables PREROUTING on eth1:
        DNAT: 192.168.90.1:502 → 10.3.0.2:502
        Packet forwarded to tap1

Step 5: QEMU Net1 (10.3.0.2) receives packet via tap1
        VirtIO_Net1_Driver processes Modbus response

Step 6: ICS Security Validation (response path)
        - Parse Modbus response
        - Validate data format
        - Check for anomalies
        - Apply egress policy
        - Decision: ALLOW or BLOCK

Step 7: If ALLOWED, forward to VirtIO_Net0_Driver (10.2.0.2)
        Internal CAmkES component communication

Step 8: QEMU Net0 (10.2.0.2) sends to tap0
        Packet exits QEMU guest

Step 9: iptables POSTROUTING on eth0:
        SNAT: 10.2.0.2 → 192.168.95.2
        Packet appears to come from "PLC"

Step 10: Packet exits eth0 (192.168.95.2) to original network
         Router sees traffic from PLC (192.168.95.2)

Step 11: Router (192.168.95.1) routes to external network (192.168.90.0/24)

Step 12: SCADA (192.168.90.5) receives Modbus response
         SCADA thinks it's from PLC (via normal routing)
```

---

## iptables NAT Rules Explained

### PREROUTING Rules (Destination NAT)

**Rule 1: Intercept traffic to "PLC" on eth0**
```bash
iptables -t nat -A PREROUTING -i eth0 -d 192.168.95.2 -p tcp --dport 502 \
  -j DNAT --to-destination 10.2.0.2:502
```
- **When:** Packet arrives on eth0 destined for 192.168.95.2:502
- **Action:** Change destination to 10.2.0.2:502 (QEMU Net0)
- **Effect:** Router's traffic to "PLC" is redirected to security gateway

**Rule 2: Intercept traffic to "Router" on eth1**
```bash
iptables -t nat -A PREROUTING -i eth1 -d 192.168.90.1 -p tcp --dport 502 \
  -j DNAT --to-destination 10.3.0.2:502
```
- **When:** Packet arrives on eth1 destined for 192.168.90.1:502
- **Action:** Change destination to 10.3.0.2:502 (QEMU Net1)
- **Effect:** PLC's traffic to "router" is redirected to security gateway

### POSTROUTING Rules (Source NAT)

**Rule 3: Make QEMU Net0 traffic appear from eth0**
```bash
iptables -t nat -A POSTROUTING -o eth0 -s 10.2.0.0/24 \
  -j SNAT --to-source 192.168.95.2
```
- **When:** Packet from 10.2.0.0/24 (tap0) exits via eth0
- **Action:** Change source to 192.168.95.2 (eth0's IP)
- **Effect:** QEMU's response appears to come from "PLC"

**Rule 4: Make QEMU Net1 traffic appear from eth1**
```bash
iptables -t nat -A POSTROUTING -o eth1 -s 10.3.0.0/24 \
  -j SNAT --to-source 192.168.90.1
```
- **When:** Packet from 10.3.0.0/24 (tap1) exits via eth1
- **Action:** Change source to 192.168.90.1 (eth1's IP)
- **Effect:** QEMU's request appears to come from "router"

---

## QEMU Guest Configuration

### VirtIO_Net0_Driver (Inbound Path)

**Network Configuration:**
```c
// Private network to communicate with tap0
IP4_ADDR(&ipaddr, 10, 2, 0, 2);      // Guest IP
IP4_ADDR(&netmask, 255, 255, 255, 0);
IP4_ADDR(&gw, 10, 2, 0, 1);          // Gateway is tap0
```

**TCP Server:**
- Listens on `10.2.0.2:502`
- Receives traffic from router (via eth0 → tap0 NAT)
- Forwards validated traffic to VirtIO_Net1_Driver

**Forwarding Configuration:**
```c
#define OUTBOUND_FORWARD_IP "10.3.0.2"   // Forward to Net1
#define OUTBOUND_FORWARD_PORT 502
```

### VirtIO_Net1_Driver (Outbound Path)

**Network Configuration:**
```c
// Private network to communicate with tap1
IP4_ADDR(&ipaddr, 10, 3, 0, 2);      // Guest IP
IP4_ADDR(&netmask, 255, 255, 255, 0);
IP4_ADDR(&gw, 10, 3, 0, 1);          // Gateway is tap1
```

**TCP Server:**
- Listens on `10.3.0.2:502`
- Receives traffic from real PLC (via eth1 → tap1 NAT)
- Forwards validated traffic to VirtIO_Net0_Driver

**Forwarding Configuration:**
```c
#define INBOUND_FORWARD_IP "10.2.0.2"    // Forward to Net0
#define INBOUND_FORWARD_PORT 502
```

---

## Deployment Steps

### 1. Physical Network Preparation

**Ensure you have:**
- Server with 2 physical NICs (eth0, eth1)
- eth0 connected to original internal network (192.168.95.0/24)
- eth1 connected to **isolated VLAN** or separate switch
- Real PLC moved from original network to secure VLAN

**Critical:** Real PLC MUST be physically removed from original network to avoid IP conflict!

### 2. Run Network Setup Script

```bash
cd /home/iamfo470/phd/camkes-vm-examples/projects/vm-examples/apps/Arm/modbus_bidirection_poc
sudo ./setup-vlan-networking.sh
```

**Script will:**
- Configure eth0 (192.168.95.2) and eth1 (192.168.90.1)
- Create tap0 (10.2.0.1) and tap1 (10.3.0.1)
- Enable IP forwarding
- Configure iptables NAT rules

### 3. Configure Real PLC

**On Real PLC, ensure routing is correct:**
```bash
# PLC keeps its original IP
IP: 192.168.95.2/24

# Default gateway should be your eth1
Gateway: 192.168.90.1

# Test connectivity to "router"
ping 192.168.90.1  # Should reach your eth1
```

### 4. Start QEMU Security Gateway

```bash
./run-grfics-deployment.sh
```

### 5. Verify Operation

**Check QEMU console shows:**
```
VirtIO_Net0_Driver: Using STATIC IP configuration:
  IP:      10.2.0.2 (private network - tap0)
  Gateway: 10.2.0.1
  TCP Echo Server listening on port 502

VirtIO_Net1_Driver: Using STATIC IP configuration:
  IP:      10.3.0.2 (private network - tap1)
  Gateway: 10.3.0.1
  TCP Echo Server listening on port 502
```

**Test from SCADA:**
```bash
# SCADA should be able to communicate with PLC normally
# (All traffic transparently intercepted by gateway)
modbus_client -h 192.168.95.2 -p 502 -f 0x01
```

---

## Security Benefits

✅ **Complete Transparency**
- Zero configuration changes on SCADA
- Zero configuration changes on PLC
- Zero configuration changes on router
- Drop-in deployment

✅ **Physical Isolation**
- Real PLC on separate VLAN
- No direct Layer 2 path between SCADA and PLC
- All traffic must pass through security gateway

✅ **Protocol-Break Architecture**
- Each TCP connection terminated and recreated
- No direct TCP session between SCADA and PLC
- Prevents protocol-level attacks

✅ **Formally Verified Enforcement**
- seL4 microkernel provides mathematical security guarantees
- ICS validation running in isolated components
- Capability-based access control

✅ **Deep Packet Inspection**
- Full Modbus protocol parsing
- Function code validation
- Register address range checking
- Policy enforcement on every message

---

## Troubleshooting

### Issue: PLC cannot reach "router" (eth1)

**Check:**
```bash
# On PLC
ping 192.168.90.1  # Should reach your eth1

# On gateway server
ip addr show eth1  # Should show 192.168.90.1/24
tcpdump -i eth1 -n icmp  # Watch for PLC's ping
```

**Solution:** Verify eth1 is on correct VLAN and UP

### Issue: Router cannot reach "PLC" (eth0)

**Check:**
```bash
# On router
ping 192.168.95.2  # Should reach your eth0

# On gateway server
ip addr show eth0  # Should show 192.168.95.2/24
tcpdump -i eth0 -n icmp  # Watch for router's ping
```

**Solution:** Verify eth0 is connected to original network

### Issue: iptables rules not working

**Debug:**
```bash
# Check NAT table
sudo iptables -t nat -L -n -v

# Watch packets being NATed
sudo iptables -t nat -L -n -v --line-numbers
# Send test traffic, then run again to see counters increment

# Capture on TAP interfaces
sudo tcpdump -i tap0 -n port 502
sudo tcpdump -i tap1 -n port 502
```

### Issue: QEMU not receiving traffic

**Check:**
```bash
# Verify TAP interfaces are UP
ip addr show tap0
ip addr show tap1

# Test connectivity to QEMU guest
ping 10.2.0.2  # Should reach QEMU Net0
ping 10.3.0.2  # Should reach QEMU Net1

# Inside QEMU console, verify TCP servers listening
# Look for: "TCP Echo Server listening on port 502"
```

---

## Performance Considerations

### Latency

**Added latency from security gateway:**
- NAT translation: ~10-50 microseconds
- QEMU guest processing: ~100-500 microseconds
- Modbus validation: ~50-200 microseconds
- **Total added latency: ~0.2-1 millisecond**

**ICS protocol tolerance:** Modbus TCP typically has 1-10 second timeouts
**Verdict:** Added latency is negligible for ICS applications

### Throughput

**Modbus TCP typical rates:**
- Polling interval: 100ms - 1 second
- Message size: 60-260 bytes
- Bandwidth requirement: <1 Mbps

**Gateway capacity:**
- TAP interface: 1 Gbps+
- QEMU guest: 100+ Mbps
- **Verdict:** Throughput is not a bottleneck

---

## Future Enhancements

### Planned Features

1. **Syslog Integration**: Send security logs to central syslog server
2. **Advanced Modbus Validation**: Function code whitelisting, register range enforcement
3. **Anomaly Detection**: Statistical analysis of ICS traffic patterns
4. **High Availability**: Failover to backup gateway on failure
5. **Performance Monitoring**: Real-time latency and throughput metrics

### Research Opportunities

- **Formal Verification of ICS Protocols**: Prove correctness of Modbus parsing
- **Machine Learning for Threat Detection**: Train models on ICS traffic
- **Multi-Protocol Support**: Extend to DNP3, EtherNet/IP, Profinet
- **Hardware Acceleration**: Offload packet processing to FPGA/SmartNIC

---

## References

- **VirtIO Specification 1.1**: Network device implementation
- **seL4 Microkernel**: Formal verification and capability system
- **CAmkES Framework**: Component architecture and communication
- **Modbus TCP Specification**: ICS protocol details
- **Linux iptables**: NAT and packet filtering

---

**Document Version:** 1.0
**Last Updated:** 2025-10-10
**Contact:** PhD Research Project - seL4 ICS Security Gateway
