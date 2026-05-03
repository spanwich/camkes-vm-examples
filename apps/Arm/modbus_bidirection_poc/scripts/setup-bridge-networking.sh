#!/bin/bash
# Bridge Network Setup for ICS Security Gateway (Layer 2)
#
# Architecture: Pure Layer 2 bridge forwarding
# - QEMU owns gateway IPs: 192.168.96.2 (nic0), 192.168.95.1 (nic1)
# - No NAT - preserves original source/destination IPs
# - Bridge members have NO IP addresses (pure L2)
# - Connection tracking + IP rewriting in QEMU for transparency

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}ICS Gateway - Bridge Architecture Setup (Layer 2)${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
   echo -e "${RED}ERROR: This script must be run as root (use sudo)${NC}"
   exit 1
fi

# Configuration
ETH0_NAME="${ETH0_NAME:-ens224}"
ETH1_NAME="${ETH1_NAME:-ens256}"
BRIDGE0_NAME="br0"
BRIDGE1_NAME="br1"
TAP0_NAME="tap0"
TAP1_NAME="tap1"

# QEMU guest IPs (owned by QEMU, not host)
QEMU_NET0_IP="192.168.96.2/24"  # VirtIO_Net0_Driver
QEMU_NET1_IP="192.168.95.1/24"  # VirtIO_Net1_Driver

# Network topology
SCADA_NETWORK="192.168.90.0/24"
PLC_NETWORK="192.168.95.0/24"

USER="${SUDO_USER:-$USER}"

echo "Bridge Architecture:"
echo "  External Bridge (br0): ${ETH0_NAME} ↔ ${TAP0_NAME}"
echo "  Internal Bridge (br1): ${ETH1_NAME} ↔ ${TAP1_NAME}"
echo "  QEMU Gateway IPs: ${QEMU_NET0_IP} (Net0), ${QEMU_NET1_IP} (Net1)"
echo "  Host NICs: NO IP (pure Layer 2 bridges)"
echo ""

# Verify interfaces exist
if ! ip link show ${ETH0_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: ${ETH0_NAME} not found${NC}"
    echo "Available interfaces:"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    exit 1
fi

if ! ip link show ${ETH1_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: ${ETH1_NAME} not found${NC}"
    echo "Available interfaces:"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    exit 1
fi

# Step 1: Cleanup old configuration
echo -e "${YELLOW}[1/6] Cleanup old configuration${NC}"
# Remove old TAP interfaces
ip link delete ${TAP0_NAME} 2>/dev/null || true
ip link delete ${TAP1_NAME} 2>/dev/null || true

# Remove old bridges
ip link set ${BRIDGE0_NAME} down 2>/dev/null || true
ip link set ${BRIDGE1_NAME} down 2>/dev/null || true
ip link delete ${BRIDGE0_NAME} 2>/dev/null || true
ip link delete ${BRIDGE1_NAME} 2>/dev/null || true

# Remove any policy routing rules
sed -i '/to_qemu/d' /etc/iproute2/rt_tables 2>/dev/null || true
while ip rule del priority 100 2>/dev/null; do :; done
while ip rule del priority 101 2>/dev/null; do :; done

# Remove iptables rules
iptables -t nat -F PREROUTING 2>/dev/null || true
iptables -t nat -F POSTROUTING 2>/dev/null || true
iptables -F FORWARD 2>/dev/null || true

echo "  → Cleaned up old configuration"

# Step 2: Create TAP Interfaces (NO IP)
echo -e "${YELLOW}[2/6] Create TAP interfaces${NC}"
ip tuntap add dev ${TAP0_NAME} mode tap user ${USER}
ip link set dev ${TAP0_NAME} up

ip tuntap add dev ${TAP1_NAME} mode tap user ${USER}
ip link set dev ${TAP1_NAME} up

echo "  → TAP interfaces created: ${TAP0_NAME}, ${TAP1_NAME}"

# Step 3: Create Linux Bridges (NO IP)
echo -e "${YELLOW}[3/6] Create Linux bridges${NC}"
ip link add name ${BRIDGE0_NAME} type bridge
ip link set dev ${BRIDGE0_NAME} up

ip link add name ${BRIDGE1_NAME} type bridge
ip link set dev ${BRIDGE1_NAME} up

echo "  → Bridges created: ${BRIDGE0_NAME}, ${BRIDGE1_NAME}"

# Step 4: Attach Interfaces to Bridges
echo -e "${YELLOW}[4/6] Attach interfaces to bridges${NC}"

# External bridge (br0): ens224 ↔ tap0
ip link set dev ${ETH0_NAME} master ${BRIDGE0_NAME}
ip link set dev ${TAP0_NAME} master ${BRIDGE0_NAME}
echo "  → ${BRIDGE0_NAME}: ${ETH0_NAME} ↔ ${TAP0_NAME} (External network)"

# Internal bridge (br1): ens256 ↔ tap1
ip link set dev ${ETH1_NAME} master ${BRIDGE1_NAME}
ip link set dev ${TAP1_NAME} master ${BRIDGE1_NAME}
echo "  → ${BRIDGE1_NAME}: ${ETH1_NAME} ↔ ${TAP1_NAME} (Internal network)"

# Step 5: Remove IPs from Physical NICs (pure Layer 2)
echo -e "${YELLOW}[5/6] Configure interfaces (Layer 2 mode)${NC}"

# Bring up physical NICs
ip link set ${ETH0_NAME} up
ip link set ${ETH1_NAME} up

# Remove ALL IP addresses from bridge members
ip addr flush dev ${ETH0_NAME}
ip addr flush dev ${ETH1_NAME}
ip addr flush dev ${TAP0_NAME} 2>/dev/null || true
ip addr flush dev ${TAP1_NAME} 2>/dev/null || true

echo "  → ${ETH0_NAME}: UP, no IP (Layer 2 bridge member)"
echo "  → ${ETH1_NAME}: UP, no IP (Layer 2 bridge member)"
echo "  → ${TAP0_NAME}: UP, no IP (Layer 2 bridge member)"
echo "  → ${TAP1_NAME}: UP, no IP (Layer 2 bridge member)"

# Step 6: Enable IP forwarding (optional for bridges, but good practice)
echo -e "${YELLOW}[6/6] Enable IP forwarding${NC}"
sysctl -w net.ipv4.ip_forward=1 >/dev/null
echo "  → IP forwarding enabled"

# Verification
echo ""
echo -e "${GREEN}✓ Bridge Configuration Complete${NC}"
echo ""

echo -e "${BLUE}Bridge Status:${NC}"
bridge link show | sed 's/^/  /'
echo ""

echo -e "${BLUE}Bridge ${BRIDGE0_NAME} (External):${NC}"
ip link show ${BRIDGE0_NAME} | sed 's/^/  /'
echo ""

echo -e "${BLUE}Bridge ${BRIDGE1_NAME} (Internal):${NC}"
ip link show ${BRIDGE1_NAME} | sed 's/^/  /'
echo ""

echo -e "${BLUE}Physical NICs (should have NO IP):${NC}"
ip addr show ${ETH0_NAME} ${ETH1_NAME} | grep -E "^[0-9]+:|inet " | sed 's/^/  /'
echo ""

echo -e "${BLUE}TAP Interfaces (should have NO IP):${NC}"
ip addr show ${TAP0_NAME} ${TAP1_NAME} | grep -E "^[0-9]+:|inet " | sed 's/^/  /'
echo ""

echo -e "${YELLOW}Architecture Summary:${NC}"
echo "  ┌─────────────────────────────────────────────────────────┐"
echo "  │  External Network (SCADA side)                          │"
echo "  │    ↓                                                    │"
echo "  │  ${ETH0_NAME} → ${BRIDGE0_NAME} (bridge) → ${TAP0_NAME}                       │"
echo "  │    ↓                                                    │"
echo "  │  QEMU nic0 (VirtIO_Net0): ${QEMU_NET0_IP}               │"
echo "  │    ↓ [Connection tracking + IP rewriting]              │"
echo "  │    ↓ [ICS validation pipeline]                         │"
echo "  │    ↓                                                    │"
echo "  │  QEMU nic1 (VirtIO_Net1): ${QEMU_NET1_IP}               │"
echo "  │    ↓                                                    │"
echo "  │  ${TAP1_NAME} → ${BRIDGE1_NAME} (bridge) → ${ETH1_NAME}                       │"
echo "  │    ↓                                                    │"
echo "  │  Internal Network (PLC side)                            │"
echo "  └─────────────────────────────────────────────────────────┘"
echo ""
echo "  Key Features:"
echo "    • Pure Layer 2 forwarding - NO NAT"
echo "    • QEMU owns gateway IPs directly"
echo "    • Original IPs preserved for ICS validation"
echo "    • Bridge members have NO IP addresses"
echo "    • Protocol-break architecture in QEMU"
echo ""

echo -e "${BLUE}Traffic Flow:${NC}"
echo "  SCADA → ens224 → br0 → tap0 → nic0 → [validation] → nic1 → tap1 → br1 → ens256 → PLC"
echo ""

echo -e "${GREEN}Next Steps:${NC}"
echo "  1. Verify QEMU configuration uses correct IPs:"
echo "     • Net0: 192.168.96.2/24, Gateway: 192.168.96.1"
echo "     • Net1: 192.168.95.1/24"
echo "  2. Start QEMU: ./scripts/run-remote.sh"
echo "  3. Test connectivity from external network: ping 192.168.96.2"
echo "  4. Test connectivity from internal network: ping 192.168.95.1"
echo ""
