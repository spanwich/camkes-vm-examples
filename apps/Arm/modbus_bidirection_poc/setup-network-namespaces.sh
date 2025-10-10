#!/bin/bash
# Network Namespace Setup for Duplicate Route Isolation
#
# This script moves ens224 and ens256 into separate network namespaces
# to avoid routing confusion from duplicate 192.168.95.0/24 routes.
#
# Usage:
#   sudo ./setup-network-namespaces.sh
#   ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E ./setup-network-namespaces.sh

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}==========================================${NC}"
echo -e "${GREEN}Network Namespace Setup${NC}"
echo -e "${GREEN}==========================================${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
   echo -e "${RED}ERROR: This script must be run as root (use sudo)${NC}"
   exit 1
fi

# Configuration
ETH0_NAME="${ETH0_NAME:-ens224}"
ETH1_NAME="${ETH1_NAME:-ens256}"
NS_INTERNAL="ns_internal"
NS_SECURE="ns_secure"

echo -e "${YELLOW}Configuration:${NC}"
echo "  Internal network interface: ${ETH0_NAME}"
echo "  Secure network interface: ${ETH1_NAME}"
echo "  Internal namespace: ${NS_INTERNAL}"
echo "  Secure namespace: ${NS_SECURE}"
echo ""

# Verify interfaces exist
if ! ip link show ${ETH0_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: ${ETH0_NAME} not found!${NC}"
    echo "Available interfaces:"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    exit 1
fi

if ! ip link show ${ETH1_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: ${ETH1_NAME} not found!${NC}"
    echo "Available interfaces:"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    exit 1
fi

# Step 1: Create namespaces
echo -e "${YELLOW}[1/4] Creating network namespaces...${NC}"

# Delete if they already exist
ip netns del ${NS_INTERNAL} 2>/dev/null || true
ip netns del ${NS_SECURE} 2>/dev/null || true

# Create fresh namespaces
ip netns add ${NS_INTERNAL}
ip netns add ${NS_SECURE}

echo "  ✓ Created ${NS_INTERNAL}"
echo "  ✓ Created ${NS_SECURE}"
echo ""

# Step 2: Move interfaces to namespaces
echo -e "${YELLOW}[2/4] Moving interfaces to namespaces...${NC}"

# Move ens224 to ns_internal
ip link set ${ETH0_NAME} netns ${NS_INTERNAL}
echo "  ✓ Moved ${ETH0_NAME} to ${NS_INTERNAL}"

# Move ens256 to ns_secure
ip link set ${ETH1_NAME} netns ${NS_SECURE}
echo "  ✓ Moved ${ETH1_NAME} to ${NS_SECURE}"
echo ""

# Step 3: Configure interfaces inside namespaces
echo -e "${YELLOW}[3/4] Configuring interfaces in namespaces...${NC}"

# Configure ens224 in ns_internal (192.168.95.2)
ip netns exec ${NS_INTERNAL} ip addr add 192.168.95.2/24 dev ${ETH0_NAME}
ip netns exec ${NS_INTERNAL} ip link set ${ETH0_NAME} up
ip netns exec ${NS_INTERNAL} ip link set lo up
echo "  ✓ ${NS_INTERNAL}/${ETH0_NAME}: 192.168.95.2/24"

# Configure ens256 in ns_secure (192.168.95.1)
ip netns exec ${NS_SECURE} ip addr add 192.168.95.1/24 dev ${ETH1_NAME}
ip netns exec ${NS_SECURE} ip link set ${ETH1_NAME} up
ip netns exec ${NS_SECURE} ip link set lo up
echo "  ✓ ${NS_SECURE}/${ETH1_NAME}: 192.168.95.1/24"
echo ""

# Step 4: Verify configuration
echo -e "${YELLOW}[4/4] Verifying namespace configuration...${NC}"

# Check ns_internal
INTERNAL_IP=$(ip netns exec ${NS_INTERNAL} ip -4 addr show ${ETH0_NAME} | grep inet | awk '{print $2}')
echo "  ${NS_INTERNAL}/${ETH0_NAME}: ${INTERNAL_IP}"

# Check ns_secure
SECURE_IP=$(ip netns exec ${NS_SECURE} ip -4 addr show ${ETH1_NAME} | grep inet | awk '{print $2}')
echo "  ${NS_SECURE}/${ETH1_NAME}: ${SECURE_IP}"

# Verify no duplicate routes in default namespace
DUPLICATE_ROUTES=$(ip route | grep -c "192.168.95" || echo "0")
if [ "$DUPLICATE_ROUTES" -eq "0" ]; then
    echo "  ✓ No duplicate routes in default namespace"
else
    echo -e "  ${YELLOW}⚠ Warning: Still have routes to 192.168.95.0/24 in default namespace${NC}"
fi
echo ""

echo -e "${GREEN}==========================================${NC}"
echo -e "${GREEN}✓ Network Namespace Setup Complete!${NC}"
echo -e "${GREEN}==========================================${NC}"
echo ""

echo -e "${BLUE}Namespace Summary:${NC}"
echo ""
ip netns list | while read ns; do
    echo "  Namespace: $ns"
    ip netns exec $ns ip addr show | grep -E "^[0-9]+:|inet " | sed 's/^/    /'
    echo ""
done

echo -e "${BLUE}Usage Examples:${NC}"
echo ""
echo "Test internal network (ens224 side):"
echo "  sudo ip netns exec ${NS_INTERNAL} ping 192.168.95.1"
echo "  sudo ip netns exec ${NS_INTERNAL} tcpdump -i ${ETH0_NAME} -n port 502"
echo ""
echo "Test secure network (ens256 side):"
echo "  sudo ip netns exec ${NS_SECURE} ping 192.168.95.2"
echo "  sudo ip netns exec ${NS_SECURE} tcpdump -i ${ETH1_NAME} -n port 502"
echo ""
echo "Test QEMU TAP interfaces (default namespace):"
echo "  ping 10.2.0.2   # QEMU Net0"
echo "  ping 10.3.0.2   # QEMU Net1"
echo ""

echo -e "${YELLOW}Important Notes:${NC}"
echo ""
echo "1. Interfaces are now ONLY accessible via 'ip netns exec'"
echo "2. QEMU and TAP interfaces remain in default namespace (unchanged)"
echo "3. iptables NAT rules operate in default namespace"
echo "4. To remove namespaces: sudo ip netns del ${NS_INTERNAL} ${NS_SECURE}"
echo ""

echo -e "${GREEN}✓ Ready to start QEMU gateway!${NC}"
echo "  Run: cd ~/modbus_gateway_deploy && ./run-remote.sh"
echo ""
