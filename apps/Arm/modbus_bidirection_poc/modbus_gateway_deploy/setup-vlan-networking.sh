#!/bin/bash
# VLAN-based TAP Network Setup for GRFICS ICS Security Gateway
#
# Architecture:
# [SCADA 192.168.90.5] → [Router 192.168.90.1/192.168.95.1] → [Original Network]
#                                                                      ↓
#                                                     [eth0: 192.168.95.2] ← Impersonates PLC
#                                                                      ↓
#                                                         [tap0: 10.2.0.1]
#                                                                      ↓
#                                                         [QEMU Gateway]
#                                                                      ↓
#                                                         [tap1: 10.3.0.1]
#                                                                      ↓
#                                                      [eth1: 192.168.90.1] ← Impersonates Router
#                                                                      ↓
#                                                   [Secure VLAN - Isolated]
#                                                                      ↓
#                                                    [Real PLC: 192.168.95.2]

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}=======================================================${NC}"
echo -e "${GREEN}GRFICS ICS Security Gateway - VLAN Network Setup${NC}"
echo -e "${GREEN}=======================================================${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
   echo -e "${RED}ERROR: This script must be run as root (use sudo)${NC}"
   exit 1
fi

# Configuration
# NOTE: Update these to match your server's interface names
# Use 'ip link show' to find interface names (e.g., ens224, ens256, enp0s3, etc.)
# You can override by setting environment variables:
#   ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E ./setup-vlan-networking.sh
ETH0_NAME="${ETH0_NAME:-eth0}"          # Physical NIC on original network
ETH1_NAME="${ETH1_NAME:-eth1}"          # Physical NIC on secure VLAN
TAP0_NAME="tap0"                         # TAP for QEMU Net0
TAP1_NAME="tap1"                         # TAP for QEMU Net1

# IP addresses
ETH0_IP="192.168.95.2"    # Impersonate PLC to router
ETH1_IP="192.168.90.1"    # Impersonate router to real PLC
TAP0_IP="10.2.0.1"        # Private network to QEMU Net0
TAP1_IP="10.3.0.1"        # Private network to QEMU Net1
NETMASK="255.255.255.0"

# QEMU guest IPs (for iptables rules)
QEMU_NET0_IP="10.2.0.2"
QEMU_NET1_IP="10.3.0.2"

USER="${SUDO_USER:-$USER}"

echo -e "${YELLOW}Configuration:${NC}"
echo "  Physical NICs:"
echo "    ${ETH0_NAME}: ${ETH0_IP}/24 (impersonates PLC on original network)"
echo "    ${ETH1_NAME}: ${ETH1_IP}/24 (impersonates router on secure VLAN)"
echo ""
echo "  TAP Interfaces (private networks):"
echo "    ${TAP0_NAME}: ${TAP0_IP}/24 (connects to QEMU Net0)"
echo "    ${TAP1_NAME}: ${TAP1_IP}/24 (connects to QEMU Net1)"
echo ""
echo "  QEMU Guest IPs:"
echo "    Net0: ${QEMU_NET0_IP}/24"
echo "    Net1: ${QEMU_NET1_IP}/24"
echo ""
echo "  User: ${USER}"
echo ""

# Step 1: Configure Physical NICs
echo -e "${YELLOW}[1/6] Configuring physical network interfaces...${NC}"

# Check if eth0 exists
if ! ip link show ${ETH0_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: ${ETH0_NAME} not found!${NC}"
    echo ""
    echo "Available interfaces:"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    echo ""
    echo "Please set the correct interface name:"
    echo "  ETH0_NAME=<your_interface> ETH1_NAME=<your_interface> sudo -E $0"
    echo ""
    echo "Example for your server:"
    echo "  ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E $0"
    exit 1
fi

# Check if eth1 exists
if ! ip link show ${ETH1_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: ${ETH1_NAME} not found!${NC}"
    echo ""
    echo "Available interfaces:"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    echo ""
    echo "Please set the correct interface name:"
    echo "  ETH0_NAME=<your_interface> ETH1_NAME=<your_interface> sudo -E $0"
    echo ""
    echo "Example for your server:"
    echo "  ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E $0"
    exit 1
fi

# Show current configuration
echo "  Current configuration:"
ip addr show ${ETH0_NAME} | grep "inet " | awk -v name="${ETH0_NAME}" '{print "    " name ": " $2}' || echo "    ${ETH0_NAME}: (no IP)"
ip addr show ${ETH1_NAME} | grep "inet " | awk -v name="${ETH1_NAME}" '{print "    " name ": " $2}' || echo "    ${ETH1_NAME}: (no IP)"
echo ""

# Warning about flushing IPs
echo -e "  ${YELLOW}WARNING: This will flush existing IP configuration on these interfaces!${NC}"
read -p "  Continue? (y/N) " -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo -e "${RED}Aborted by user${NC}"
    exit 1
fi
echo ""

# Bring down interfaces
echo "  Bringing down interfaces..."
ip link set ${ETH0_NAME} down 2>/dev/null || true
ip link set ${ETH1_NAME} down 2>/dev/null || true

# Flush existing IPs and configure eth0
echo "  Configuring ${ETH0_NAME}..."
ip addr flush dev ${ETH0_NAME}
ip addr add ${ETH0_IP}/24 dev ${ETH0_NAME}
ip link set ${ETH0_NAME} up
echo "  ✓ ${ETH0_NAME}: ${ETH0_IP}/24 (impersonates PLC)"

# Flush existing IPs and configure eth1
echo "  Configuring ${ETH1_NAME}..."
ip addr flush dev ${ETH1_NAME}
ip addr add ${ETH1_IP}/24 dev ${ETH1_NAME}
ip link set ${ETH1_NAME} up
echo "  ✓ ${ETH1_NAME}: ${ETH1_IP}/24 (impersonates router)"

# Verify configuration
echo ""
echo "  Verifying configuration:"
ip addr show ${ETH0_NAME} | grep "inet " | awk -v name="${ETH0_NAME}" '{print "    " name ": " $2}' || echo "    ${ETH0_NAME}: ERROR - no IP!"
ip addr show ${ETH1_NAME} | grep "inet " | awk -v name="${ETH1_NAME}" '{print "    " name ": " $2}' || echo "    ${ETH1_NAME}: ERROR - no IP!"
echo ""

# Step 2: Cleanup existing TAP interfaces
echo -e "${YELLOW}[2/6] Cleaning up existing TAP interfaces...${NC}"
if ip link show ${TAP0_NAME} &>/dev/null; then
    echo "  - Deleting existing ${TAP0_NAME}..."
    ip link set ${TAP0_NAME} down 2>/dev/null || true
    ip addr flush dev ${TAP0_NAME} 2>/dev/null || true
    ip link delete ${TAP0_NAME} 2>/dev/null || true
fi
if ip link show ${TAP1_NAME} &>/dev/null; then
    echo "  - Deleting existing ${TAP1_NAME}..."
    ip link set ${TAP1_NAME} down 2>/dev/null || true
    ip addr flush dev ${TAP1_NAME} 2>/dev/null || true
    ip link delete ${TAP1_NAME} 2>/dev/null || true
fi
echo "  ✓ Cleanup complete"
echo ""

# Step 3: Create TAP interfaces
echo -e "${YELLOW}[3/6] Creating TAP interfaces...${NC}"
ip tuntap add dev ${TAP0_NAME} mode tap user ${USER}
ip tuntap add dev ${TAP1_NAME} mode tap user ${USER}
echo "  ✓ Created ${TAP0_NAME} (owned by ${USER})"
echo "  ✓ Created ${TAP1_NAME} (owned by ${USER})"
echo ""

# Step 4: Configure TAP interfaces (private networks)
echo -e "${YELLOW}[4/6] Configuring TAP interfaces...${NC}"
ip addr add ${TAP0_IP}/24 dev ${TAP0_NAME}
ip link set ${TAP0_NAME} up
echo "  ✓ ${TAP0_NAME}: ${TAP0_IP}/24"

ip addr add ${TAP1_IP}/24 dev ${TAP1_NAME}
ip link set ${TAP1_NAME} up
echo "  ✓ ${TAP1_NAME}: ${TAP1_IP}/24"
echo ""

# Step 5: Enable IP forwarding
echo -e "${YELLOW}[5/6] Enabling IP forwarding and routing...${NC}"
sysctl -w net.ipv4.ip_forward=1 >/dev/null
echo "  ✓ IP forwarding enabled"
echo ""

# Step 6: Configure iptables NAT rules
echo -e "${YELLOW}[6/6] Configuring iptables NAT rules...${NC}"

# Check if iptables is installed
if ! command -v iptables &> /dev/null; then
    echo -e "${RED}ERROR: iptables not found!${NC}"
    echo ""
    echo "Please install iptables first:"
    echo "  sudo apt install iptables          # Ubuntu/Debian"
    echo "  sudo yum install iptables          # RHEL/CentOS"
    echo ""
    echo "Network interfaces configured successfully, but iptables NAT rules skipped."
    echo "After installing iptables, run:"
    echo "  sudo ./setup-iptables.sh"
    echo ""
    exit 1
fi

# Flush existing NAT rules
iptables -t nat -F
echo "  ✓ Flushed existing NAT rules"

# INBOUND PATH: SCADA → PLC
# Traffic arrives on eth0 (192.168.95.2:502) → DNAT to QEMU Net0 (10.2.0.2:502)
iptables -t nat -A PREROUTING -i ${ETH0_NAME} -d ${ETH0_IP} -p tcp --dport 502 \
  -j DNAT --to-destination ${QEMU_NET0_IP}:502
echo "  ✓ PREROUTING: ${ETH0_NAME} (${ETH0_IP}:502) → QEMU Net0 (${QEMU_NET0_IP}:502)"

# Traffic from QEMU Net0 → tap0 → needs SNAT to appear from eth0
iptables -t nat -A POSTROUTING -o ${ETH0_NAME} -s 10.2.0.0/24 \
  -j SNAT --to-source ${ETH0_IP}
echo "  ✓ POSTROUTING: tap0 (10.2.0.0/24) → ${ETH0_NAME} SNAT to ${ETH0_IP}"

# OUTBOUND PATH: PLC → SCADA (via QEMU Net1 → tap1 → eth1)
# Traffic from QEMU Net1 (10.3.0.2) → tap1 → eth1 → real PLC
iptables -t nat -A POSTROUTING -o ${ETH1_NAME} -s 10.3.0.0/24 \
  -j SNAT --to-source ${ETH1_IP}
echo "  ✓ POSTROUTING: tap1 (10.3.0.0/24) → ${ETH1_NAME} SNAT to ${ETH1_IP}"

# Traffic arriving on eth1 (192.168.90.1:502) → DNAT to QEMU Net1 (10.3.0.2:502)
iptables -t nat -A PREROUTING -i ${ETH1_NAME} -d ${ETH1_IP} -p tcp --dport 502 \
  -j DNAT --to-destination ${QEMU_NET1_IP}:502
echo "  ✓ PREROUTING: ${ETH1_NAME} (${ETH1_IP}:502) → QEMU Net1 (${QEMU_NET1_IP}:502)"

echo ""
echo -e "${GREEN}=======================================================${NC}"
echo -e "${GREEN}Network Setup Complete!${NC}"
echo -e "${GREEN}=======================================================${NC}"
echo ""

# Display configuration summary
echo -e "${BLUE}Network Configuration Summary:${NC}"
echo ""
echo "Physical Network Interfaces:"
ip addr show ${ETH0_NAME} | grep "inet " | awk '{print "  " $0}'
ip addr show ${ETH1_NAME} | grep "inet " | awk '{print "  " $0}'
echo ""
echo "TAP Interfaces (Private Networks):"
ip addr show ${TAP0_NAME} | grep "inet " | awk '{print "  " $0}'
ip addr show ${TAP1_NAME} | grep "inet " | awk '{print "  " $0}'
echo ""

echo -e "${BLUE}iptables NAT Rules:${NC}"
iptables -t nat -L -n -v | grep -A 2 "Chain PREROUTING\|Chain POSTROUTING" | grep -v "^--"
echo ""

echo -e "${BLUE}Traffic Flow:${NC}"
echo ""
echo "INBOUND (SCADA → PLC):"
echo "  1. SCADA (192.168.90.5) sends to PLC (192.168.95.2:502)"
echo "  2. Router (192.168.95.1) routes to 192.168.95.2"
echo "  3. Arrives at ${ETH0_NAME} (192.168.95.2)"
echo "  4. DNAT → ${QEMU_NET0_IP}:502 (QEMU Net0 via tap0)"
echo "  5. QEMU validates and forwards to Net1 (${QEMU_NET1_IP})"
echo "  6. Exits tap1 → SNAT to ${ETH1_IP}"
echo "  7. Exits ${ETH1_NAME} to real PLC (192.168.95.2 on secure VLAN)"
echo ""
echo "OUTBOUND (PLC → SCADA):"
echo "  1. Real PLC (192.168.95.2) sends to SCADA (192.168.90.5:502)"
echo "  2. PLC routes via default gateway (192.168.90.1 = your ${ETH1_NAME})"
echo "  3. Arrives at ${ETH1_NAME} (192.168.90.1)"
echo "  4. DNAT → ${QEMU_NET1_IP}:502 (QEMU Net1 via tap1)"
echo "  5. QEMU validates and forwards to Net0 (${QEMU_NET0_IP})"
echo "  6. Exits tap0 → SNAT to ${ETH0_IP}"
echo "  7. Exits ${ETH0_NAME} to router → SCADA"
echo ""

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}✓ Network Configuration Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

echo -e "${BLUE}IP Configuration Summary:${NC}"
echo ""
echo "Physical NICs:"
ip -4 addr show ${ETH0_NAME} | grep inet | awk -v name="${ETH0_NAME}" '{print "  " name ": " $2}'
ip -4 addr show ${ETH1_NAME} | grep inet | awk -v name="${ETH1_NAME}" '{print "  " name ": " $2}'
echo ""
echo "TAP Interfaces:"
ip -4 addr show ${TAP0_NAME} | grep inet | awk '{print "  tap0: " $2}'
ip -4 addr show ${TAP1_NAME} | grep inet | awk '{print "  tap1: " $2}'
echo ""
echo "IP Forwarding:"
FORWARDING=$(cat /proc/sys/net/ipv4/ip_forward)
if [ "$FORWARDING" = "1" ]; then
    echo "  ✓ Enabled"
else
    echo "  ✗ Disabled (ERROR)"
fi
echo ""

echo -e "${BLUE}NAT Rules Active:${NC}"
NAT_COUNT=$(iptables -t nat -L -n | grep -c "10.2.0.2\|10.3.0.2" || echo "0")
echo "  $NAT_COUNT iptables NAT rules configured"
echo ""

echo -e "${BLUE}Next Steps:${NC}"
echo ""
echo "1. Configure Real PLC:"
echo "   - Set PLC's default gateway to: ${ETH1_IP}"
echo "   - Test: ping ${ETH1_IP} (from PLC)"
echo ""
echo "2. Verify connectivity:"
echo "   - From router: ping ${ETH0_IP}"
echo "   - From PLC: ping ${ETH1_IP}"
echo ""
echo "3. Start security gateway:"
echo "   ./run-remote.sh"
echo ""
echo "4. Test after QEMU starts:"
echo "   ping 10.2.0.2  # QEMU Net0"
echo "   ping 10.3.0.2  # QEMU Net1"
echo ""

echo -e "${YELLOW}Note: Configuration is NOT persistent across reboots!${NC}"
echo "To make persistent, see REMOTE_SERVER_SETUP.md"
echo ""
