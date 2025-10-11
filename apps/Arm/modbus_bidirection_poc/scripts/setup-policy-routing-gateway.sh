#!/bin/bash
# Network Setup with Pure Routing for ICS Security Gateway (NO NAT)
#
# Architecture: Transparent L3 gateway using policy routing
# - Traffic preserves original source/destination IPs
# - QEMU gateway acts as transparent security filter
# - No address translation (NAT-less design)

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}ICS Gateway - Pure Routing Setup (NO NAT)${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
   echo -e "${RED}ERROR: This script must be run as root (use sudo)${NC}"
   exit 1
fi

# Configuration
ETH0_NAME="${ETH0_NAME:-ens224}"
ETH1_NAME="${ETH1_NAME:-ens256}"
TAP0_NAME="tap0"
TAP1_NAME="tap1"

# Physical NIC IPs (gateway management IPs)
ETH0_IP="192.168.96.2"
ETH1_IP="192.168.95.1"

# TAP interface IPs (QEMU gateway endpoint IPs)
TAP0_IP="10.2.0.1"
TAP1_IP="10.3.0.1"
QEMU_NET0_IP="10.2.0.2"
QEMU_NET1_IP="10.3.0.2"

# Network topology
SCADA_NETWORK="192.168.90.0/24"     # SCADA devices (internal)
PLC_NETWORK="192.168.95.0/24"       # PLC devices (external)
PFSENSE_IP="192.168.96.1"           # pfSense router

USER="${SUDO_USER:-$USER}"

echo "Network Topology:"
echo "  SCADA Network: ${SCADA_NETWORK} (via pfSense)"
echo "  PLC Network:   ${PLC_NETWORK} (direct)"
echo "  Gateway NICs:  ${ETH0_NAME}=${ETH0_IP}, ${ETH1_NAME}=${ETH1_IP}"
echo "  QEMU TAPs:     ${TAP0_NAME}=${TAP0_IP}, ${TAP1_NAME}=${TAP1_IP}"
echo ""

# Verify interfaces exist
if ! ip link show ${ETH0_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: ${ETH0_NAME} not found${NC}"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    exit 1
fi

if ! ip link show ${ETH1_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: ${ETH1_NAME} not found${NC}"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    exit 1
fi

# Step 1: Cleanup
echo -e "${YELLOW}[1/7] Cleanup old configuration${NC}"
ip netns del ns_internal 2>/dev/null || true
ip netns del ns_secure 2>/dev/null || true
ip link delete ${TAP0_NAME} 2>/dev/null || true
ip link delete ${TAP1_NAME} 2>/dev/null || true

# Step 2: Configure Physical NICs
echo -e "${YELLOW}[2/7] Configure physical NICs${NC}"
ip link set ${ETH0_NAME} down 2>/dev/null || true
ip addr flush dev ${ETH0_NAME}
ip addr add ${ETH0_IP}/24 dev ${ETH0_NAME}
ip link set ${ETH0_NAME} up

ip link set ${ETH1_NAME} down 2>/dev/null || true
ip addr flush dev ${ETH1_NAME}
ip addr add ${ETH1_IP}/24 dev ${ETH1_NAME}
ip link set ${ETH1_NAME} up

# Step 3: Create TAP interfaces
echo -e "${YELLOW}[3/7] Create TAP interfaces${NC}"
ip tuntap add dev ${TAP0_NAME} mode tap user ${USER}
ip addr add ${TAP0_IP}/24 dev ${TAP0_NAME}
ip link set ${TAP0_NAME} up

ip tuntap add dev ${TAP1_NAME} mode tap user ${USER}
ip addr add ${TAP1_IP}/24 dev ${TAP1_NAME}
ip link set ${TAP1_NAME} up

# Step 4: Enable IP forwarding and disable reverse path filtering
echo -e "${YELLOW}[4/7] Enable IP forwarding${NC}"
sysctl -w net.ipv4.ip_forward=1 >/dev/null
sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null
sysctl -w net.ipv4.conf.default.rp_filter=0 >/dev/null
sysctl -w net.ipv4.conf.${ETH0_NAME}.rp_filter=0 >/dev/null
sysctl -w net.ipv4.conf.${ETH1_NAME}.rp_filter=0 >/dev/null
sysctl -w net.ipv4.conf.${TAP0_NAME}.rp_filter=0 >/dev/null
sysctl -w net.ipv4.conf.${TAP1_NAME}.rp_filter=0 >/dev/null
echo "  → IP forwarding enabled"
echo "  → Reverse path filtering disabled (required for asymmetric routing)"

# Step 5: Main routing table (for return traffic)
echo -e "${YELLOW}[5/7] Configure main routing table${NC}"

# Default route via pfSense
ip route del default 2>/dev/null || true
ip route add default via ${PFSENSE_IP} dev ${ETH0_NAME} metric 100
echo "  → Default route: via ${PFSENSE_IP} (pfSense)"

# Route to SCADA network via pfSense
ip route del ${SCADA_NETWORK} 2>/dev/null || true
ip route add ${SCADA_NETWORK} via ${PFSENSE_IP} dev ${ETH0_NAME}
echo "  → SCADA route: ${SCADA_NETWORK} via ${PFSENSE_IP}"

# Route to PLC network via ens256
ip route del ${PLC_NETWORK} 2>/dev/null || true
ip route add ${PLC_NETWORK} dev ${ETH1_NAME} src ${ETH1_IP}
echo "  → PLC route: ${PLC_NETWORK} via ${ETH1_NAME}"

# Step 6: Policy-Based Routing (Force all traffic through QEMU)
echo -e "${YELLOW}[6/7] Configure policy-based routing (QEMU enforcement)${NC}"

# Remove old routing table entries from rt_tables
sed -i '/to_qemu_net0/d' /etc/iproute2/rt_tables 2>/dev/null || true
sed -i '/to_qemu_net1/d' /etc/iproute2/rt_tables 2>/dev/null || true
sed -i '/to_qemu/d' /etc/iproute2/rt_tables 2>/dev/null || true

# Create custom routing table
echo "100 to_qemu" >> /etc/iproute2/rt_tables
echo "  → Routing table created: 100 to_qemu"

# Remove ALL existing policy rules with priority 100 and 101 (safer cleanup)
while ip rule del priority 100 2>/dev/null; do :; done
while ip rule del priority 101 2>/dev/null; do :; done

# Add policy routing rules: ALL traffic from NICs goes through QEMU
ip rule add from all iif ${ETH0_NAME} lookup to_qemu priority 100
ip rule add from all iif ${ETH1_NAME} lookup to_qemu priority 100
echo "  → Policy rule: All traffic from ${ETH0_NAME} → QEMU"
echo "  → Policy rule: All traffic from ${ETH1_NAME} → QEMU"

# Populate QEMU routing table (using table ID 100 to avoid lookup issues)
ip route flush table 100 2>/dev/null || true

# Traffic from ens224 (external side) → tap0 (Net0)
ip route add ${SCADA_NETWORK} via ${QEMU_NET0_IP} dev ${TAP0_NAME} table to_qemu
ip route add ${PLC_NETWORK} via ${QEMU_NET0_IP} dev ${TAP0_NAME} table to_qemu
ip route add default via ${QEMU_NET0_IP} dev ${TAP0_NAME} table to_qemu

echo "  → QEMU table: ${SCADA_NETWORK} → tap0 (Net0)"
echo "  → QEMU table: ${PLC_NETWORK} → tap0 (Net0)"
echo "  → QEMU table: default → tap0 (Net0)"

# Step 7: iptables FORWARD rules (block NIC-to-NIC bypass)
echo -e "${YELLOW}[7/7] Configure iptables FORWARD rules${NC}"

# Remove old rules
iptables -D FORWARD -i ${TAP0_NAME} -o ${ETH0_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${TAP0_NAME} -o ${ETH1_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${TAP1_NAME} -o ${ETH0_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${TAP1_NAME} -o ${ETH1_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${ETH0_NAME} -o ${TAP0_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${ETH0_NAME} -o ${TAP1_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${ETH1_NAME} -o ${TAP0_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${ETH1_NAME} -o ${TAP1_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${ETH0_NAME} -o ${ETH1_NAME} -j DROP 2>/dev/null || true
iptables -D FORWARD -i ${ETH1_NAME} -o ${ETH0_NAME} -j DROP 2>/dev/null || true

# Add FORWARD rules (TAP ↔ NIC allowed, NIC ↔ NIC blocked)
iptables -A FORWARD -i ${TAP0_NAME} -o ${ETH0_NAME} -j ACCEPT
iptables -A FORWARD -i ${TAP0_NAME} -o ${ETH1_NAME} -j ACCEPT
iptables -A FORWARD -i ${TAP1_NAME} -o ${ETH0_NAME} -j ACCEPT
iptables -A FORWARD -i ${TAP1_NAME} -o ${ETH1_NAME} -j ACCEPT
iptables -A FORWARD -i ${ETH0_NAME} -o ${TAP0_NAME} -j ACCEPT
iptables -A FORWARD -i ${ETH0_NAME} -o ${TAP1_NAME} -j ACCEPT
iptables -A FORWARD -i ${ETH1_NAME} -o ${TAP0_NAME} -j ACCEPT
iptables -A FORWARD -i ${ETH1_NAME} -o ${TAP1_NAME} -j ACCEPT
iptables -A FORWARD -i ${ETH0_NAME} -o ${ETH1_NAME} -j DROP
iptables -A FORWARD -i ${ETH1_NAME} -o ${ETH0_NAME} -j DROP

echo "  → FORWARD: TAP ↔ NIC traffic allowed"
echo "  → FORWARD: NIC ↔ NIC traffic blocked (forces QEMU path)"

# Step 8: NAT for QEMU outbound traffic
echo -e "${YELLOW}[8/8] Configure SNAT for QEMU outbound traffic${NC}"

# Remove old NAT rules
iptables -t nat -D POSTROUTING -s 10.3.0.0/24 -o ${ETH1_NAME} -j SNAT --to-source ${ETH1_IP} 2>/dev/null || true
iptables -t nat -D POSTROUTING -s 10.2.0.0/24 -o ${ETH0_NAME} -j SNAT --to-source ${ETH0_IP} 2>/dev/null || true
iptables -t nat -D POSTROUTING -s 192.168.95.0/24 -o ${ETH1_NAME} -j SNAT --to-source ${ETH1_IP} 2>/dev/null || true

# CRITICAL: SNAT for Net1 outbound (tap1 → ens256)
# lwIP cannot bind to external IPs (192.168.90.5), so it uses 10.3.0.2
# We translate 10.3.0.2 → gateway IP when exiting ens256
iptables -t nat -A POSTROUTING -s 10.3.0.0/24 -o ${ETH1_NAME} -j SNAT --to-source ${ETH1_IP}

# NO SNAT needed for Net0 - it now uses real PLC IP (192.168.95.2)
# Net0's replies will have src=192.168.95.2 which is routable

echo "  → SNAT: 10.3.0.2 (Net1) → ${ETH1_IP} when exiting ${ETH1_NAME}"
echo "  → NO SNAT for Net0: uses real PLC IP (192.168.95.2)"
echo ""
echo "  Reason: Net1 uses 10.3.0.2 (internal) and needs SNAT"
echo "  Net0 uses 192.168.95.2 (PLC IP) which is already routable"

# Verification
echo ""
echo -e "${GREEN}✓ Configuration Complete (Hybrid: Policy Routing + SNAT)${NC}"
echo ""

echo -e "${BLUE}Physical NICs:${NC}"
ip -4 addr show ${ETH0_NAME} | grep inet | awk '{print "  " $0}'
ip -4 addr show ${ETH1_NAME} | grep inet | awk '{print "  " $0}'
echo ""

echo -e "${BLUE}TAP Interfaces:${NC}"
ip -4 addr show ${TAP0_NAME} | grep inet | awk '{print "  " $0}'
ip -4 addr show ${TAP1_NAME} | grep inet | awk '{print "  " $0}'
echo ""

echo -e "${BLUE}Main Routing Table:${NC}"
ip route | grep -E "${SCADA_NETWORK}|${PLC_NETWORK}|default" | sed 's/^/  /'
echo ""

echo -e "${BLUE}Policy Routing Rules:${NC}"
ip rule list | grep -E "100" | sed 's/^/  /'
echo ""

echo -e "${BLUE}QEMU Routing Table (to_qemu):${NC}"
ip route show table to_qemu | sed 's/^/  /'
echo ""

echo -e "${BLUE}iptables FORWARD (TAP↔NIC allowed, NIC↔NIC blocked):${NC}"
iptables -L FORWARD -n -v --line-numbers | grep -E "ACCEPT|DROP|Chain" | head -15 | sed 's/^/  /'
echo ""

echo -e "${BLUE}NAT Rules (POSTROUTING):${NC}"
iptables -t nat -L POSTROUTING -n -v | grep -E "SNAT|MASQUERADE|Chain" | sed 's/^/  /'
echo ""

echo -e "${YELLOW}Architecture Summary:${NC}"
echo "  1. INBOUND: Traffic from ${ETH0_NAME}/${ETH1_NAME} → FORCED through tap0 (policy routing)"
echo "  2. QEMU gateway preserves ORIGINAL IPs in metadata (no DNAT)"
echo "  3. Net1 binds to IP_ADDR_ANY (uses 10.3.0.2) and connects to metadata.dst_ip"
echo "  4. OUTBOUND: tap1 → SNAT translates 10.3.0.2 → ${ETH1_IP}"
echo "  5. PLC sees request from gateway IP (${ETH1_IP})"
echo "  6. Return traffic uses conntrack DNAT back to 10.3.0.2"
echo ""
echo "  Data Flow Example:"
echo "    SCADA (192.168.90.5:X) → PLC (192.168.95.2:502)"
echo "    ens224 → tap0 (preserved) → Net0 → metadata(src=192.168.90.5, dst=192.168.95.2)"
echo "    ICS pipeline → Net1"
echo "    Net1: bind(0.0.0.0:0) → local_ip=10.3.0.2, connect(192.168.95.2:502)"
echo "    tap1 sends: src=10.3.0.2:Y, dst=192.168.95.2:502"
echo "    SNAT: src=10.3.0.2 → ${ETH1_IP}"
echo "    ens256 → PLC sees: src=${ETH1_IP}:Y, dst=192.168.95.2:502"
echo "    PLC responds: src=192.168.95.2:502, dst=${ETH1_IP}:Y"
echo "    DNAT (conntrack): dst=${ETH1_IP}:Y → 10.3.0.2:Y"
echo "    tap1 → Net1 → ICS → Net0 → tap0 → ens224 → SCADA"
echo ""

echo -e "${YELLOW}Next: Start QEMU gateway with ./scripts/run-remote.sh${NC}"
echo ""
