#!/bin/bash
# Network Setup with Policy-Based Routing for ICS Security Gateway

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}ICS Gateway - Policy-Based Routing Setup${NC}"
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

ETH0_IP="192.168.96.2"
ETH1_IP="192.168.95.1"
TAP0_IP="10.2.0.1"
TAP1_IP="10.3.0.1"
QEMU_NET0_IP="10.2.0.2"
QEMU_NET1_IP="10.3.0.2"

USER="${SUDO_USER:-$USER}"

echo "Config: ${ETH0_NAME}=${ETH0_IP} ${ETH1_NAME}=${ETH1_IP} ${TAP0_NAME}=${TAP0_IP} ${TAP1_NAME}=${TAP1_IP}"
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
echo -e "${YELLOW}[1/8] Cleanup${NC}"
ip netns del ns_internal 2>/dev/null || true
ip netns del ns_secure 2>/dev/null || true
ip link delete ${TAP0_NAME} 2>/dev/null || true
ip link delete ${TAP1_NAME} 2>/dev/null || true

# Step 2: Configure Physical NICs
echo -e "${YELLOW}[2/8] Configure NICs${NC}"
ip link set ${ETH0_NAME} down 2>/dev/null || true
ip addr flush dev ${ETH0_NAME}
ip addr add ${ETH0_IP}/24 dev ${ETH0_NAME}
ip link set ${ETH0_NAME} up

ip link set ${ETH1_NAME} down 2>/dev/null || true
ip addr flush dev ${ETH1_NAME}
ip addr add ${ETH1_IP}/24 dev ${ETH1_NAME}
ip link set ${ETH1_NAME} up

# Step 3: Create TAP interfaces
echo -e "${YELLOW}[3/8] Create TAP interfaces${NC}"
ip tuntap add dev ${TAP0_NAME} mode tap user ${USER}
ip addr add ${TAP0_IP}/24 dev ${TAP0_NAME}
ip link set ${TAP0_NAME} up

ip tuntap add dev ${TAP1_NAME} mode tap user ${USER}
ip addr add ${TAP1_IP}/24 dev ${TAP1_NAME}
ip link set ${TAP1_NAME} up

# Step 4: Enable IP forwarding
echo -e "${YELLOW}[4/8] Enable IP forwarding${NC}"
sysctl -w net.ipv4.ip_forward=1 >/dev/null

# Add default route via pfSense if not present
if ! ip route | grep -q "default via 192.168.96.1"; then
    ip route add default via 192.168.96.1 dev ${ETH0_NAME} metric 200 2>/dev/null || true
fi

# Step 5: Policy-Based Routing (CRITICAL!)
echo -e "${YELLOW}[5/8] Configure policy-based routing (prevents kernel bypass)${NC}"

# Create custom routing tables
grep -q "to_qemu_net0" /etc/iproute2/rt_tables || echo "100 to_qemu_net0" >> /etc/iproute2/rt_tables
grep -q "to_qemu_net1" /etc/iproute2/rt_tables || echo "101 to_qemu_net1" >> /etc/iproute2/rt_tables

# Remove existing policy rules
ip rule del iif ${ETH0_NAME} lookup to_qemu_net0 2>/dev/null || true
ip rule del iif ${ETH1_NAME} lookup to_qemu_net1 2>/dev/null || true

# Add policy routing rules
ip rule add iif ${ETH0_NAME} lookup to_qemu_net0 priority 100
ip rule add iif ${ETH1_NAME} lookup to_qemu_net1 priority 101

# Populate custom routing tables
ip route flush table to_qemu_net0 2>/dev/null || true
ip route add default via ${QEMU_NET0_IP} dev ${TAP0_NAME} table to_qemu_net0
ip route add 192.168.95.0/24 via ${QEMU_NET0_IP} dev ${TAP0_NAME} table to_qemu_net0

ip route flush table to_qemu_net1 2>/dev/null || true
ip route add default via ${QEMU_NET1_IP} dev ${TAP1_NAME} table to_qemu_net1
ip route add 192.168.96.0/24 via ${QEMU_NET1_IP} dev ${TAP1_NAME} table to_qemu_net1

# Step 6: iptables FORWARD rules
echo -e "${YELLOW}[6/8] Configure iptables FORWARD rules (block direct NIC routing)${NC}"

# Remove old rules
iptables -D FORWARD -i ${TAP0_NAME} -o ${ETH0_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${TAP1_NAME} -o ${ETH1_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${ETH0_NAME} -o ${TAP0_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${ETH1_NAME} -o ${TAP1_NAME} -j ACCEPT 2>/dev/null || true
iptables -D FORWARD -i ${ETH0_NAME} -o ${ETH1_NAME} -j DROP 2>/dev/null || true
iptables -D FORWARD -i ${ETH1_NAME} -o ${ETH0_NAME} -j DROP 2>/dev/null || true

# Add FORWARD rules
iptables -A FORWARD -i ${TAP0_NAME} -o ${ETH0_NAME} -j ACCEPT
iptables -A FORWARD -i ${TAP1_NAME} -o ${ETH1_NAME} -j ACCEPT
iptables -A FORWARD -i ${ETH0_NAME} -o ${TAP0_NAME} -j ACCEPT
iptables -A FORWARD -i ${ETH1_NAME} -o ${TAP1_NAME} -j ACCEPT
iptables -A FORWARD -i ${ETH0_NAME} -o ${ETH1_NAME} -j DROP
iptables -A FORWARD -i ${ETH1_NAME} -o ${ETH0_NAME} -j DROP

# Step 7: iptables NAT rules
echo -e "${YELLOW}[7/8] Configure iptables NAT rules${NC}"
iptables -t nat -F

# DNAT: Incoming traffic to QEMU
iptables -t nat -A PREROUTING -i ${ETH0_NAME} -d ${ETH0_IP} -p tcp --dport 502 -j DNAT --to-destination ${QEMU_NET0_IP}:502
iptables -t nat -A PREROUTING -i ${ETH1_NAME} -d ${ETH1_IP} -p tcp --dport 502 -j DNAT --to-destination ${QEMU_NET1_IP}:502

# SNAT: Outgoing traffic from QEMU
iptables -t nat -A POSTROUTING -o ${ETH0_NAME} -s 10.2.0.0/24 -j SNAT --to-source ${ETH0_IP}
iptables -t nat -A POSTROUTING -o ${ETH1_NAME} -s 10.3.0.0/24 -j SNAT --to-source ${ETH1_IP}

# Step 8: Verify configuration
echo -e "${YELLOW}[8/8] Verification${NC}"
echo ""

echo -e "${GREEN}✓ Configuration Complete${NC}"
echo ""

echo -e "${BLUE}Physical NICs:${NC}"
ip -4 addr show ${ETH0_NAME} | grep inet | awk '{print "  " $0}'
ip -4 addr show ${ETH1_NAME} | grep inet | awk '{print "  " $0}'
echo ""

echo -e "${BLUE}TAP Interfaces:${NC}"
ip -4 addr show ${TAP0_NAME} | grep inet | awk '{print "  " $0}'
ip -4 addr show ${TAP1_NAME} | grep inet | awk '{print "  " $0}'
echo ""

echo -e "${BLUE}Policy Routing Rules:${NC}"
ip rule list | grep -E "100|101" | sed 's/^/  /'
echo ""

echo -e "${BLUE}Custom Routing Tables:${NC}"
echo "  to_qemu_net0:"
ip route show table to_qemu_net0 | sed 's/^/    /'
echo "  to_qemu_net1:"
ip route show table to_qemu_net1 | sed 's/^/    /'
echo ""

echo -e "${BLUE}iptables NAT (DNAT/SNAT):${NC}"
iptables -t nat -L PREROUTING -n -v --line-numbers | grep -E "DNAT|Chain" | sed 's/^/  /'
iptables -t nat -L POSTROUTING -n -v --line-numbers | grep -E "SNAT|Chain" | sed 's/^/  /'
echo ""

echo -e "${BLUE}iptables FORWARD (TAP↔NIC allowed, NIC↔NIC blocked):${NC}"
iptables -L FORWARD -n -v --line-numbers | grep -E "ACCEPT|DROP|Chain" | head -10 | sed 's/^/  /'
echo ""

echo -e "${YELLOW}Next: Start QEMU gateway with ./scripts/run-remote.sh${NC}"
echo ""
