#!/bin/bash
# QEMU Run Script for Remote Deployment
#
# This script is designed to run on a remote server where only the
# pre-built image and scripts have been copied (no build directory).

set -e

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}VLAN-based ICS Security Gateway${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Check if TAP interfaces exist
if ! ip link show tap0 &>/dev/null || ! ip link show tap1 &>/dev/null; then
    echo -e "${RED}ERROR: TAP interfaces not found!${NC}"
    echo ""
    echo "Please run the network setup script first:"
    echo "  sudo ./setup-vlan-networking.sh"
    echo ""
    exit 1
fi

# Check if iptables NAT rules are configured (requires sudo to read)
# Try with sudo first, fall back to non-sudo if not available
if command -v sudo &> /dev/null && sudo -n true 2>/dev/null; then
    NAT_RULES_COUNT=$(sudo iptables -t nat -L PREROUTING -n 2>/dev/null | grep -c "10.2.0.2\|10.3.0.2" || echo "0")
else
    NAT_RULES_COUNT=$(iptables -t nat -L PREROUTING -n 2>/dev/null | grep -c "10.2.0.2\|10.3.0.2" || echo "0")
fi

if [ "$NAT_RULES_COUNT" -lt 2 ]; then
    echo -e "${YELLOW}WARNING: iptables NAT rules may not be configured!${NC}"
    echo ""
    echo "Cannot verify iptables rules (requires sudo)."
    echo "If you haven't configured iptables yet, run:"
    echo "  sudo ./setup-iptables.sh"
    echo ""
    echo "Or run the combined network setup:"
    echo "  sudo ./setup-vlan-networking.sh"
    echo ""
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Display network configuration
echo -e "${BLUE}Network Configuration:${NC}"
echo ""
echo "Physical NICs:"

# Auto-detect physical interfaces by their configured IPs
ETH0_NAME=$(ip addr show | grep -B2 "inet 192.168.95.2" | grep -oP '^\d+: \K[^:]+' | head -1)
ETH1_NAME=$(ip addr show | grep -B2 "inet 192.168.90.1" | grep -oP '^\d+: \K[^:]+' | head -1)

if [ -n "$ETH0_NAME" ]; then
    ip addr show "$ETH0_NAME" | grep -E "inet " | awk -v name="$ETH0_NAME" '{print "  " name ": " $2 " (impersonates PLC)"}'
else
    echo "  (No interface with 192.168.95.2 found)"
fi

if [ -n "$ETH1_NAME" ]; then
    ip addr show "$ETH1_NAME" | grep -E "inet " | awk -v name="$ETH1_NAME" '{print "  " name ": " $2 " (impersonates router)"}'
else
    echo "  (No interface with 192.168.90.1 found)"
fi
echo ""
echo "TAP Interfaces (Private Networks):"
ip addr show tap0 | grep -E "inet " | awk '{print "  tap0: " $2 " (QEMU Net0 gateway)"}'
ip addr show tap1 | grep -E "inet " | awk '{print "  tap1: " $2 " (QEMU Net1 gateway)"}'
echo ""

# Find the kernel image
KERNEL_IMAGE=""

# Check common locations
if [ -f "capdl-loader-image-arm-qemu-arm-virt" ]; then
    KERNEL_IMAGE="capdl-loader-image-arm-qemu-arm-virt"
elif [ -f "images/capdl-loader-image-arm-qemu-arm-virt" ]; then
    KERNEL_IMAGE="images/capdl-loader-image-arm-qemu-arm-virt"
elif [ -f "../capdl-loader-image-arm-qemu-arm-virt" ]; then
    KERNEL_IMAGE="../capdl-loader-image-arm-qemu-arm-virt"
else
    echo -e "${RED}ERROR: Kernel image not found!${NC}"
    echo ""
    echo "Looking for: capdl-loader-image-arm-qemu-arm-virt"
    echo "Searched locations:"
    echo "  - Current directory"
    echo "  - ./images/"
    echo "  - ../"
    echo ""
    echo "Please ensure the built image is in the same directory as this script."
    exit 1
fi

echo -e "${GREEN}Found kernel image: ${KERNEL_IMAGE}${NC}"
echo ""

# QEMU configuration for TAP networking
QEMU_ARGS="-global virtio-mmio.force-legacy=false \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
  -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -netdev tap,id=net1,ifname=tap1,script=no,downscript=no \
  -device virtio-net-device,netdev=net1,mac=52:54:00:12:34:57"

echo -e "${BLUE}QEMU Guest Configuration:${NC}"
echo ""
echo "  Network 0 (Connected to tap0):"
echo "    - Guest IP: 10.2.0.2/24"
echo "    - Gateway: 10.2.0.1 (tap0)"
echo "    - MAC: 52:54:00:12:34:56"
echo "    - Listens on: :502"
echo "    - Host NAT: eth0 (192.168.95.2) ←→ tap0 (10.2.0.2)"
echo ""
echo "  Network 1 (Connected to tap1):"
echo "    - Guest IP: 10.3.0.2/24"
echo "    - Gateway: 10.3.0.1 (tap1)"
echo "    - MAC: 52:54:00:12:34:57"
echo "    - Listens on: :502"
echo "    - Host NAT: eth1 (192.168.90.1) ←→ tap1 (10.3.0.2)"
echo ""

echo -e "${BLUE}Expected Traffic Flow:${NC}"
echo ""
echo "INBOUND (SCADA → PLC):"
echo "  SCADA (192.168.90.5) → Router (192.168.90.1/192.168.95.1)"
echo "    → eth0 (192.168.95.2:502) [DNAT]→ tap0 → Net0 (10.2.0.2:502)"
echo "    → ICS_Inbound validation → Net1 (10.3.0.2)"
echo "    → tap1 [SNAT]→ eth1 (192.168.90.1) → Real PLC (192.168.95.2)"
echo ""
echo "OUTBOUND (PLC → SCADA):"
echo "  Real PLC (192.168.95.2) → eth1 (192.168.90.1:502)"
echo "    → [DNAT]→ tap1 → Net1 (10.3.0.2:502) → ICS_Outbound validation"
echo "    → Net0 (10.2.0.2) → tap0 [SNAT]→ eth0 (192.168.95.2)"
echo "    → Router → SCADA (192.168.90.5)"
echo ""

echo -e "${GREEN}Starting QEMU...${NC}"
echo "Press Ctrl-C to stop"
echo ""
echo "=========================================="
echo ""

# Run QEMU directly with the kernel image
qemu-system-arm \
  -machine virt,virtualization=on,highmem=off,secure=off \
  -cpu cortex-a15 \
  -nographic \
  -m size=1024 \
  ${QEMU_ARGS} \
  -kernel "${KERNEL_IMAGE}"
