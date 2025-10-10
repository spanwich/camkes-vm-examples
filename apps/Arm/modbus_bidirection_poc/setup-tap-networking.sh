#!/bin/bash
# TAP Network Setup for GRFICS ICS Security Gateway Deployment
#
# This script creates TAP interfaces for transparent man-in-the-middle deployment
# between GRFICS SCADA and PLC systems.
#
# Network Topology:
# [SCADA 192.168.90.5] → [Router] → [Internal Net 192.168.95.0/24] → [Our Firewall] → [Secure Subnet 192.168.95.0/24] → [PLC 192.168.95.2]

set -e  # Exit on error

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}GRFICS ICS Security Gateway TAP Setup${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
   echo -e "${RED}ERROR: This script must be run as root (use sudo)${NC}"
   exit 1
fi

# Configuration
TAP0_NAME="tap0"
TAP1_NAME="tap1"
TAP0_IP="192.168.95.2"      # Pretends to be PLC
TAP1_IP="192.168.90.5"      # Pretends to be SCADA
NETMASK="255.255.255.0"
USER="${SUDO_USER:-$USER}"

echo -e "${YELLOW}Configuration:${NC}"
echo "  TAP0 (Internal Network):  ${TAP0_NAME} → ${TAP0_IP}/24 (PLC IP)"
echo "  TAP1 (Secure Subnet):     ${TAP1_NAME} → ${TAP1_IP}/24 (SCADA IP)"
echo "  User: ${USER}"
echo ""

# Cleanup existing TAP interfaces (force delete and recreate)
echo -e "${YELLOW}[1/5] Cleaning up existing TAP interfaces...${NC}"
if ip link show ${TAP0_NAME} &>/dev/null; then
    echo "  - Found existing ${TAP0_NAME}, deleting..."
    ip link set ${TAP0_NAME} down 2>/dev/null || true
    ip addr flush dev ${TAP0_NAME} 2>/dev/null || true
    ip link delete ${TAP0_NAME} 2>/dev/null || true
    echo "    ✓ Deleted ${TAP0_NAME}"
fi
if ip link show ${TAP1_NAME} &>/dev/null; then
    echo "  - Found existing ${TAP1_NAME}, deleting..."
    ip link set ${TAP1_NAME} down 2>/dev/null || true
    ip addr flush dev ${TAP1_NAME} 2>/dev/null || true
    ip link delete ${TAP1_NAME} 2>/dev/null || true
    echo "    ✓ Deleted ${TAP1_NAME}"
fi
echo "  ✓ Cleanup complete"
echo ""

# Create TAP interfaces
echo -e "${YELLOW}[2/5] Creating TAP interfaces...${NC}"
ip tuntap add dev ${TAP0_NAME} mode tap user ${USER}
ip tuntap add dev ${TAP1_NAME} mode tap user ${USER}
echo "  ✓ Created ${TAP0_NAME} (owned by ${USER})"
echo "  ✓ Created ${TAP1_NAME} (owned by ${USER})"
echo ""

# Assign IP addresses
echo -e "${YELLOW}[3/5] Assigning IP addresses...${NC}"
ip addr add ${TAP0_IP}/24 dev ${TAP0_NAME}
ip addr add ${TAP1_IP}/24 dev ${TAP1_NAME}
echo "  ✓ ${TAP0_NAME}: ${TAP0_IP}/24"
echo "  ✓ ${TAP1_NAME}: ${TAP1_IP}/24"
echo ""

# Bring interfaces up
echo -e "${YELLOW}[4/5] Bringing interfaces UP...${NC}"
ip link set ${TAP0_NAME} up
ip link set ${TAP1_NAME} up
echo "  ✓ ${TAP0_NAME} is UP"
echo "  ✓ ${TAP1_NAME} is UP"
echo ""

# Enable IP forwarding (needed for routing between networks)
echo -e "${YELLOW}[5/5] Enabling IP forwarding...${NC}"
echo 1 > /proc/sys/net/ipv4/ip_forward
echo "  ✓ IP forwarding enabled"
echo ""

# Display interface status
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}TAP Interfaces Ready!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "Interface Status:"
ip addr show ${TAP0_NAME} | grep -E "inet |state"
ip addr show ${TAP1_NAME} | grep -E "inet |state"
echo ""

# Optional: Bridge to physical NICs (using 'ip' commands, no brctl needed)
echo -e "${YELLOW}OPTIONAL: Bridge to Physical NICs (without brctl)${NC}"
echo "If your server doesn't have 'brctl', use 'ip' commands instead:"
echo ""
echo "  # Create bridge for tap0 → internal network (e.g., eth0):"
echo "  sudo ip link add name br_internal type bridge"
echo "  sudo ip link set tap0 master br_internal"
echo "  sudo ip link set eth0 master br_internal  # Your internal NIC"
echo "  sudo ip link set br_internal up"
echo ""
echo "  # Create bridge for tap1 → secure subnet (e.g., eth1):"
echo "  sudo ip link add name br_secure type bridge"
echo "  sudo ip link set tap1 master br_secure"
echo "  sudo ip link set eth1 master br_secure  # Your secure subnet NIC"
echo "  sudo ip link set br_secure up"
echo ""

echo -e "${GREEN}Setup complete! You can now run QEMU with TAP networking.${NC}"
echo ""
echo "Next steps:"
echo "  1. Review bridge configuration above (if using physical NICs)"
echo "  2. Start QEMU: ./run-grfics-deployment.sh"
echo "  3. Test with GRFICS SCADA/PLC"
echo ""
