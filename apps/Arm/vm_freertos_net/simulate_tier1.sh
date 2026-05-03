#!/bin/bash
#
# simulate_tier1.sh - QEMU Simulation Script for Tier 1 VirtIO Test
#
# This script runs the EthernetDriver test with the CRITICAL sDDF flags
# that enable modern virtio (version 2).
#
# IMPORTANT: The flag "-global virtio-mmio.force-legacy=false" is
# required to make QEMU use virtio-mmio version 2 instead of legacy
# version 1. This was discovered from sDDF CI configuration.
#

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
BUILD_DIR="${BUILD_DIR:-build}"
IMAGE="${BUILD_DIR}/images/capdl-loader-image-arm-qemu-arm-virt"

echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║     Tier 1 VirtIO Device Discovery Test                 ║${NC}"
echo -e "${BLUE}║     CAmkES EthernetDriver + QEMU virtio-net              ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Check if image exists
if [ ! -f "$IMAGE" ]; then
    echo -e "${RED}ERROR: Build image not found at: $IMAGE${NC}"
    echo -e "${YELLOW}Please build the project first:${NC}"
    echo "  cd $BUILD_DIR"
    echo "  ninja"
    exit 1
fi

echo -e "${GREEN}Image found:${NC} $IMAGE"
echo ""

echo -e "${YELLOW}QEMU Configuration:${NC}"
echo "  Machine:       virt (ARM)"
echo "  CPU:           cortex-a53"
echo "  Memory:        2GB"
echo "  Serial:        mon:stdio"
echo ""

echo -e "${YELLOW}VirtIO Network Device:${NC}"
echo "  Device:        virtio-net-device (MMIO transport)"
echo "  Version:       2 (modern virtio 1.0+)"
echo "  CRITICAL FLAG: -global virtio-mmio.force-legacy=false"
echo "  Network:       User-mode (SLIRP)"
echo "  Port forward:  tcp::5555->:1237 (echo server future)"
echo ""

echo -e "${YELLOW}Expected Physical Addresses:${NC}"
echo "  VirtIO MMIO:   0xa003000 (IRQ 79)"
echo "  HW Ring Buf:   0x5fff0000"
echo ""

echo -e "${GREEN}Starting QEMU...${NC}"
echo -e "${BLUE}Press Ctrl-A then X to exit QEMU${NC}"
echo ""
sleep 2

# QEMU command with sDDF-compatible flags
qemu-system-aarch64 \
    -machine virt,virtualization=on,highmem=off,secure=off \
    -cpu cortex-a53 \
    -m 2G \
    -nographic \
    -serial mon:stdio \
    -global virtio-mmio.force-legacy=false \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::5555-:1237,hostfwd=tcp::8080-:80 \
    -kernel "$IMAGE"

echo ""
echo -e "${GREEN}QEMU exited${NC}"
