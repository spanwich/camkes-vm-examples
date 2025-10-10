#!/bin/bash
# Install QEMU and Dependencies for ICS Security Gateway Deployment
#
# This script installs all required packages to run the seL4-based
# ICS security gateway on the remote server.

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Installing QEMU and Dependencies${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Detect OS
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
    VERSION=$VERSION_ID
else
    echo -e "${RED}Cannot detect OS. This script supports Ubuntu/Debian.${NC}"
    exit 1
fi

echo -e "${YELLOW}Detected OS: $OS $VERSION${NC}"
echo ""

# Update package list
echo -e "${YELLOW}[1/3] Updating package list...${NC}"
sudo apt-get update -qq
echo "  ✓ Package list updated"
echo ""

# Install QEMU ARM system emulator
echo -e "${YELLOW}[2/3] Installing QEMU ARM system emulator...${NC}"
sudo apt-get install -y \
    qemu-system-arm \
    qemu-system-misc \
    qemu-utils
echo "  ✓ QEMU installed"
echo ""

# Install networking tools
echo -e "${YELLOW}[3/3] Installing networking tools...${NC}"
sudo apt-get install -y \
    iproute2 \
    iputils-ping \
    netcat-openbsd \
    tcpdump
echo "  ✓ Networking tools installed"
echo ""

# Verify installation
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Verifying Installation${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Check QEMU version
if command -v qemu-system-arm &> /dev/null; then
    QEMU_VERSION=$(qemu-system-arm --version | head -1)
    echo -e "${GREEN}✓ QEMU installed: $QEMU_VERSION${NC}"
else
    echo -e "${RED}✗ QEMU installation failed${NC}"
    exit 1
fi

# Check networking tools
if command -v ip &> /dev/null; then
    echo -e "${GREEN}✓ iproute2 (ip command) installed${NC}"
else
    echo -e "${RED}✗ iproute2 not found${NC}"
fi

if command -v nc &> /dev/null; then
    echo -e "${GREEN}✓ netcat installed${NC}"
else
    echo -e "${RED}✗ netcat not found${NC}"
fi

if command -v tcpdump &> /dev/null; then
    echo -e "${GREEN}✓ tcpdump installed${NC}"
else
    echo -e "${RED}✗ tcpdump not found${NC}"
fi

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Installation Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "Next steps:"
echo "  1. Copy deployment files to this server"
echo "  2. Run: sudo ./setup-tap-networking.sh"
echo "  3. Run: qemu-system-arm ... (see deployment guide)"
echo ""
