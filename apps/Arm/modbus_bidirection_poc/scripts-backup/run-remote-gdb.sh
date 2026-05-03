#!/bin/bash
# QEMU Run Script for ICS Security Gateway - WITH GDB SERVER
# This version includes GDB server on port 1234 for debugging

set -e

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}ICS Security Gateway - Starting QEMU with GDB Server${NC}"
echo ""

# Check TAP interfaces
if ! ip link show tap0 &>/dev/null || ! ip link show tap1 &>/dev/null; then
    echo -e "${RED}ERROR: TAP interfaces not found${NC}"
    echo "Run: sudo ../projects/vm-examples/apps/Arm/modbus_bidirection_poc/scripts/setup-policy-routing-gateway.sh"
    exit 1
fi

# Display network status
echo -e "${BLUE}Network Status:${NC}"
ip addr show tap0 tap1 | grep -E "^[0-9]+:|inet " | sed 's/^/  /'
echo ""

# Find kernel image
KERNEL_IMAGE="images/capdl-loader-image-arm-qemu-arm-virt"

if [ ! -f "$KERNEL_IMAGE" ]; then
    echo -e "${RED}ERROR: Kernel image not found: $KERNEL_IMAGE${NC}"
    exit 1
fi

echo "Kernel: ${KERNEL_IMAGE}"
echo "GDB Server: localhost:1234"
echo ""

# QEMU configuration
QEMU_ARGS="-global virtio-mmio.force-legacy=false \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
  -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -netdev tap,id=net1,ifname=tap1,script=no,downscript=no \
  -device virtio-net-device,netdev=net1,mac=52:54:00:12:34:57"

echo -e "${GREEN}Starting QEMU with GDB Server (Press Ctrl-A X to exit)${NC}"
echo "=========================================="
echo ""

# Run QEMU with GDB server
qemu-system-arm \
  -machine virt,virtualization=on,highmem=off,secure=off \
  -cpu cortex-a15 \
  -nographic \
  -m size=1024 \
  ${QEMU_ARGS} \
  -kernel "${KERNEL_IMAGE}" \
  -s -S
