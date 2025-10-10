#!/bin/bash
# QEMU Run Script for GRFICS ICS Security Gateway Deployment
#
# This script starts the Modbus bidirectional POC firewall with TAP networking
# for transparent deployment between GRFICS SCADA and PLC.

set -e

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Starting GRFICS ICS Security Gateway${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Check if TAP interfaces exist
if ! ip link show tap0 &>/dev/null || ! ip link show tap1 &>/dev/null; then
    echo -e "${RED}ERROR: TAP interfaces not found!${NC}"
    echo ""
    echo "Please run the setup script first:"
    echo "  sudo ./setup-tap-networking.sh"
    echo ""
    exit 1
fi

# Display TAP interface status
echo -e "${YELLOW}TAP Interface Status:${NC}"
ip addr show tap0 | grep -E "inet " | awk '{print "  tap0: " $2}'
ip addr show tap1 | grep -E "inet " | awk '{print "  tap1: " $2}'
echo ""

# Find the build directory
BUILD_DIR="../../../../../../build_modbus"
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}ERROR: Build directory not found: ${BUILD_DIR}${NC}"
    echo ""
    echo "Please build the project first:"
    echo "  cd /home/iamfo470/phd/camkes-vm-examples"
    echo "  mkdir -p build_modbus && cd build_modbus"
    echo "  ../init-build.sh -DPLATFORM=qemu-arm-virt -DAARCH32=TRUE -DCAMKES_APP=modbus_bidirection_poc"
    echo "  ninja"
    echo ""
    exit 1
fi

# QEMU configuration for TAP networking
QEMU_ARGS="-global virtio-mmio.force-legacy=false \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
  -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -netdev tap,id=net1,ifname=tap1,script=no,downscript=no \
  -device virtio-net-device,netdev=net1,mac=52:54:00:12:34:57"

echo -e "${YELLOW}QEMU Configuration:${NC}"
echo "  Network 0 (Internal):"
echo "    - TAP: tap0"
echo "    - MAC: 52:54:00:12:34:56"
echo "    - Guest IP: 192.168.95.2 (pretends to be PLC)"
echo "    - Listens on: Modbus port 502"
echo ""
echo "  Network 1 (Secure Subnet):"
echo "    - TAP: tap1"
echo "    - MAC: 52:54:00:12:34:57"
echo "    - Guest IP: 192.168.90.5 (pretends to be SCADA)"
echo "    - Listens on: Modbus port 502"
echo ""

echo -e "${GREEN}Starting QEMU...${NC}"
echo ""
echo "Expected traffic flow:"
echo "  SCADA (192.168.90.5) → Net0:502 → ICS_Inbound → Net1 → PLC (192.168.95.2)"
echo "  PLC (192.168.95.2) → Net1:502 → ICS_Outbound → Net0 → SCADA (192.168.90.5)"
echo ""
echo "Press Ctrl-C to stop"
echo ""
echo "=========================================="
echo ""

# Run QEMU
cd ${BUILD_DIR}
./simulate --extra-qemu-args="${QEMU_ARGS}"
