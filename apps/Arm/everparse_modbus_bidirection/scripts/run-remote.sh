#!/bin/bash
# QEMU Run Script for ICS Security Gateway

set -e

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}ICS Security Gateway - Starting QEMU${NC}"
echo ""

# Check TAP interfaces
if ! ip link show tap0 &>/dev/null || ! ip link show tap1 &>/dev/null; then
    echo -e "${RED}ERROR: TAP interfaces not found${NC}"
    echo "Run: sudo ./scripts/setup-policy-routing-gateway.sh"
    exit 1
fi

# Check iptables NAT rules (optional verification)
if command -v sudo &> /dev/null && sudo -n true 2>/dev/null; then
    NAT_RULES_COUNT=$(sudo iptables -t nat -L PREROUTING -n 2>/dev/null | grep -c "10.2.0.2\|10.3.0.2" || echo "0")
    if [ "$NAT_RULES_COUNT" -lt 2 ]; then
        echo -e "${YELLOW}WARNING: iptables NAT rules not detected${NC}"
        read -p "Continue anyway? (y/N) " -n 1 -r
        echo ""
        [[ ! $REPLY =~ ^[Yy]$ ]] && exit 1
    fi
fi

# Display network status
echo -e "${BLUE}Network Status:${NC}"
ip addr show tap0 tap1 | grep -E "^[0-9]+:|inet " | sed 's/^/  /'
echo ""

# Find kernel image
KERNEL_IMAGE=""
for path in "capdl-loader-image-arm-qemu-arm-virt" \
            "images/capdl-loader-image-arm-qemu-arm-virt" \
            "../capdl-loader-image-arm-qemu-arm-virt"; do
    if [ -f "$path" ]; then
        KERNEL_IMAGE="$path"
        break
    fi
done

if [ -z "$KERNEL_IMAGE" ]; then
    echo -e "${RED}ERROR: Kernel image not found${NC}"
    echo "Looking for: capdl-loader-image-arm-qemu-arm-virt"
    exit 1
fi

echo "Kernel: ${KERNEL_IMAGE}"
echo ""

# QEMU configuration
QEMU_ARGS="-global virtio-mmio.force-legacy=false \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
  -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -netdev tap,id=net1,ifname=tap1,script=no,downscript=no \
  -device virtio-net-device,netdev=net1,mac=52:54:00:12:34:57"

echo -e "${GREEN}Starting QEMU (Press Ctrl-A X to exit)${NC}"
echo "=========================================="
echo ""

# Run QEMU
qemu-system-arm \
  -machine virt,virtualization=on,highmem=off,secure=off \
  -cpu cortex-a15 \
  -nographic \
  -m size=1024 \
  ${QEMU_ARGS} \
  -kernel "${KERNEL_IMAGE}"
