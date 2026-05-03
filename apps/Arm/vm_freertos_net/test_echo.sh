#!/bin/bash
# test_echo.sh - Simple test script for Tier 2 echo server
#
# This script provides three testing methods:
# 1. TAP networking (requires root)
# 2. Socket backend (easier, no root)
# 3. Monitor commands (manual testing)

set -e

IMAGE="../../../../../../build/images/capdl-loader-image-arm-qemu-arm-virt"
if [ ! -f "$IMAGE" ]; then
    echo "Error: Image not found at $IMAGE"
    echo "Please build first: cd build && ninja"
    exit 1
fi

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║    Tier 2 Echo Server - Testing Options                 ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Check if running as root
if [ "$EUID" -eq 0 ]; then
    CAN_TAP=true
else
    CAN_TAP=false
fi

echo "Available testing methods:"
echo ""
echo "1) TAP Networking (requires root) - BEST for real packet testing"
if $CAN_TAP; then
    echo -e "   ${GREEN}✓ Available (running as root)${NC}"
else
    echo -e "   ${YELLOW}⚠ Not available (run with sudo for this option)${NC}"
fi
echo ""
echo "2) Socket Backend - Easy testing without root"
echo -e "   ${GREEN}✓ Always available${NC}"
echo ""
echo "3) User Networking - Default QEMU networking"
echo -e "   ${YELLOW}⚠ Layer 3 only (won't see raw Ethernet)${NC}"
echo ""

read -p "Select test method (1-3): " method

case $method in
    1)
        if ! $CAN_TAP; then
            echo -e "${YELLOW}Error: TAP networking requires root. Run with sudo.${NC}"
            exit 1
        fi

        echo -e "${GREEN}Setting up TAP networking...${NC}"

        # Create TAP device if it doesn't exist
        if ! ip link show tap0 >/dev/null 2>&1; then
            ip tuntap add dev tap0 mode tap user $(whoami)
            ip link set tap0 up
            ip addr add 192.168.100.1/24 dev tap0
            echo -e "${GREEN}✓ Created tap0 interface${NC}"
        else
            echo -e "${GREEN}✓ tap0 interface already exists${NC}"
        fi

        echo ""
        echo -e "${BLUE}Starting QEMU with TAP networking...${NC}"
        echo "MAC address: 52:54:00:12:34:56"
        echo "To test from another terminal:"
        echo "  sudo arping -I tap0 192.168.100.2"
        echo "  sudo ping -I tap0 192.168.100.2"
        echo ""
        echo "Press Ctrl-C to exit QEMU"
        echo ""

        qemu-system-aarch64 \
            -machine virt,virtualization=on,highmem=off,secure=off \
            -cpu cortex-a53 \
            -m 2G \
            -nographic \
            -serial mon:stdio \
            -global virtio-mmio.force-legacy=false \
            -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
            -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
            -kernel "$IMAGE"
        ;;

    2)
        echo -e "${GREEN}Starting QEMU with socket backend...${NC}"
        echo ""
        echo "This creates a Unix socket for packet injection."
        echo "Socket: /tmp/qemu-net.sock"
        echo ""
        echo "To send test packets from another terminal:"
        echo "  python3 test_packet_sender.py"
        echo ""
        echo "Press Ctrl-C to exit QEMU"
        echo ""

        # Remove old socket if exists
        rm -f /tmp/qemu-net.sock

        qemu-system-aarch64 \
            -machine virt,virtualization=on,highmem=off,secure=off \
            -cpu cortex-a53 \
            -m 2G \
            -nographic \
            -serial mon:stdio \
            -global virtio-mmio.force-legacy=false \
            -netdev socket,id=net0,listen=:1234 \
            -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
            -kernel "$IMAGE"
        ;;

    3)
        echo -e "${YELLOW}Starting QEMU with user networking...${NC}"
        echo ""
        echo -e "${YELLOW}NOTE: This won't show packet echoes because user networking${NC}"
        echo -e "${YELLOW}      operates at Layer 3 (IP), not Layer 2 (Ethernet).${NC}"
        echo ""
        echo "The echo server will initialize but won't receive packets."
        echo "This is useful to verify initialization only."
        echo ""
        echo "Press Ctrl-C to exit QEMU"
        echo ""

        qemu-system-aarch64 \
            -machine virt,virtualization=on,highmem=off,secure=off \
            -cpu cortex-a53 \
            -m 2G \
            -nographic \
            -serial mon:stdio \
            -global virtio-mmio.force-legacy=false \
            -netdev user,id=net0 \
            -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
            -kernel "$IMAGE"
        ;;

    *)
        echo "Invalid option"
        exit 1
        ;;
esac
