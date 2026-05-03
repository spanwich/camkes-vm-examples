#!/bin/bash
# Install Bridge Networking with Persistence
#
# This script:
# 1. Applies bridge network configuration
# 2. Installs systemd service for persistence across reboots
# 3. Enables the service to start automatically

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}╔═══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  ICS Security Gateway - Persistent Bridge Setup          ║${NC}"
echo -e "${GREEN}╚═══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
   echo -e "${RED}ERROR: This script must be run as root (use sudo)${NC}"
   exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo -e "${BLUE}Project Directory:${NC} $PROJECT_DIR"
echo -e "${BLUE}Scripts Directory:${NC} $SCRIPT_DIR"
echo ""

# Step 1: Apply bridge configuration
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}Step 1/3: Apply Bridge Network Configuration${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

if [ -f "$SCRIPT_DIR/setup-bridge-networking.sh" ]; then
    bash "$SCRIPT_DIR/setup-bridge-networking.sh"
else
    echo -e "${RED}ERROR: setup-bridge-networking.sh not found${NC}"
    exit 1
fi

echo ""
echo -e "${GREEN}✓ Bridge configuration applied successfully${NC}"
echo ""

# Step 2: Install systemd service
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}Step 2/3: Install Systemd Service${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

SYSTEMD_DIR="/etc/systemd/system"
SERVICE_FILE="qemu-bridges.service"

if [ -f "$SCRIPT_DIR/$SERVICE_FILE" ]; then
    echo "Installing $SERVICE_FILE to $SYSTEMD_DIR..."
    cp "$SCRIPT_DIR/$SERVICE_FILE" "$SYSTEMD_DIR/"
    chmod 644 "$SYSTEMD_DIR/$SERVICE_FILE"
    echo -e "${GREEN}✓ Service file installed${NC}"
else
    echo -e "${RED}ERROR: $SERVICE_FILE not found${NC}"
    exit 1
fi

# Reload systemd daemon
echo "Reloading systemd daemon..."
systemctl daemon-reload
echo -e "${GREEN}✓ Systemd daemon reloaded${NC}"
echo ""

# Step 3: Enable and verify service
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo -e "${YELLOW}Step 3/3: Enable Service for Auto-Start${NC}"
echo -e "${YELLOW}═══════════════════════════════════════════════════════════${NC}"
echo ""

echo "Enabling qemu-bridges.service..."
systemctl enable qemu-bridges.service
echo -e "${GREEN}✓ Service enabled (will start automatically on boot)${NC}"
echo ""

# Verify service status
echo -e "${BLUE}Service Status:${NC}"
systemctl status qemu-bridges.service --no-pager || true
echo ""

# Summary
echo ""
echo -e "${GREEN}╔═══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  Installation Complete!                                   ║${NC}"
echo -e "${GREEN}╚═══════════════════════════════════════════════════════════╝${NC}"
echo ""

echo -e "${BLUE}What was configured:${NC}"
echo "  ✓ Bridge br0: ens224 ↔ tap0 (External network)"
echo "  ✓ Bridge br1: ens256 ↔ tap1 (Internal network)"
echo "  ✓ Systemd service installed and enabled"
echo "  ✓ Configuration will persist across reboots"
echo ""

echo -e "${BLUE}Verify configuration:${NC}"
echo "  $ bridge link show"
echo "  $ ip link show br0 br1"
echo "  $ systemctl status qemu-bridges.service"
echo ""

echo -e "${BLUE}Manual service control:${NC}"
echo "  Start:   sudo systemctl start qemu-bridges.service"
echo "  Stop:    sudo systemctl stop qemu-bridges.service"
echo "  Restart: sudo systemctl restart qemu-bridges.service"
echo "  Disable: sudo systemctl disable qemu-bridges.service"
echo ""

echo -e "${BLUE}Test after reboot:${NC}"
echo "  1. Reboot system: sudo reboot"
echo "  2. After reboot, check: bridge link show"
echo "  3. Bridges should be automatically created"
echo ""

echo -e "${BLUE}Next steps:${NC}"
echo "  1. Start QEMU gateway: ./scripts/run-remote.sh"
echo "  2. QEMU will use IPs: 192.168.96.2 (Net0), 192.168.95.1 (Net1)"
echo "  3. Test connectivity from SCADA network: ping 192.168.96.2"
echo "  4. Test connectivity from PLC network: ping 192.168.95.1"
echo ""

echo -e "${GREEN}Installation successful!${NC}"
echo ""
