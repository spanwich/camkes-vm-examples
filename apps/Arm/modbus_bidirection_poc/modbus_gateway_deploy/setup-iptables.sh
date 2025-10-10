#!/bin/bash
# iptables NAT Rules for VLAN-based ICS Security Gateway
#
# This script configures iptables to translate traffic between physical NICs
# and QEMU guest private networks.
#
# Usage:
#   sudo ./setup-iptables.sh                              # Uses eth0/eth1 (auto-detects if not found)
#   ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E $0          # Custom interface names
#
# Network Architecture:
#   Physical NIC 0 (192.168.95.2) ←→ tap0 (10.2.0.1) ←→ QEMU Net0 (10.2.0.2)
#   Physical NIC 1 (192.168.90.1) ←→ tap1 (10.3.0.1) ←→ QEMU Net1 (10.3.0.2)

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}ICS Security Gateway - iptables Setup${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
   echo -e "${RED}ERROR: This script must be run as root (use sudo)${NC}"
   exit 1
fi

# Check if iptables is installed
if ! command -v iptables &> /dev/null; then
    echo -e "${RED}ERROR: iptables not found!${NC}"
    echo ""
    echo "Please install iptables first:"
    echo "  sudo apt update"
    echo "  sudo apt install iptables          # Ubuntu/Debian"
    echo ""
    echo "Or:"
    echo "  sudo yum install iptables          # RHEL/CentOS"
    echo ""
    exit 1
fi

# Configuration
ETH0_IP="192.168.95.2"
ETH1_IP="192.168.90.1"
QEMU_NET0_IP="10.2.0.2"
QEMU_NET1_IP="10.3.0.2"
MODBUS_PORT="502"

# Interface name detection (environment variable or auto-detect)
ETH0_NAME="${ETH0_NAME:-}"
ETH1_NAME="${ETH1_NAME:-}"

# Auto-detect ETH0 if not provided
if [ -z "$ETH0_NAME" ]; then
    # Try default name first
    if ip link show eth0 &>/dev/null; then
        ETH0_NAME="eth0"
    else
        # Auto-detect by IP address
        DETECTED_ETH0=$(ip addr show | grep -B2 "inet ${ETH0_IP}" | grep -oP '^\d+: \K[^:]+' | head -1)
        if [ -n "$DETECTED_ETH0" ]; then
            ETH0_NAME="$DETECTED_ETH0"
            echo -e "${BLUE}Auto-detected interface with ${ETH0_IP}: ${ETH0_NAME}${NC}"
        fi
    fi
fi

# Auto-detect ETH1 if not provided
if [ -z "$ETH1_NAME" ]; then
    # Try default name first
    if ip link show eth1 &>/dev/null; then
        ETH1_NAME="eth1"
    else
        # Auto-detect by IP address
        DETECTED_ETH1=$(ip addr show | grep -B2 "inet ${ETH1_IP}" | grep -oP '^\d+: \K[^:]+' | head -1)
        if [ -n "$DETECTED_ETH1" ]; then
            ETH1_NAME="$DETECTED_ETH1"
            echo -e "${BLUE}Auto-detected interface with ${ETH1_IP}: ${ETH1_NAME}${NC}"
        fi
    fi
fi

# Verify interfaces were found
if [ -z "$ETH0_NAME" ] || ! ip link show ${ETH0_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: Could not find interface with IP ${ETH0_IP}!${NC}"
    echo ""
    echo "Available interfaces:"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    echo ""
    echo "Current IP configuration:"
    ip addr show | grep -E "^[0-9]+:|inet " | sed 's/^/  /'
    echo ""
    echo "Please set the correct interface name:"
    echo "  ETH0_NAME=<your_interface> ETH1_NAME=<your_interface> sudo -E $0"
    echo ""
    echo "Example:"
    echo "  ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E $0"
    exit 1
fi

if [ -z "$ETH1_NAME" ] || ! ip link show ${ETH1_NAME} &>/dev/null; then
    echo -e "${RED}ERROR: Could not find interface with IP ${ETH1_IP}!${NC}"
    echo ""
    echo "Available interfaces:"
    ip link show | grep "^[0-9]" | awk '{print "  " $2}' | tr -d ':'
    echo ""
    echo "Current IP configuration:"
    ip addr show | grep -E "^[0-9]+:|inet " | sed 's/^/  /'
    echo ""
    echo "Please set the correct interface name:"
    echo "  ETH0_NAME=<your_interface> ETH1_NAME=<your_interface> sudo -E $0"
    echo ""
    echo "Example:"
    echo "  ETH0_NAME=ens224 ETH1_NAME=ens256 sudo -E $0"
    exit 1
fi

echo -e "${YELLOW}Configuration:${NC}"
echo "  Physical NICs:"
echo "    ${ETH0_NAME}: ${ETH0_IP} (impersonates PLC)"
echo "    ${ETH1_NAME}: ${ETH1_IP} (impersonates router)"
echo ""
echo "  QEMU Guest IPs:"
echo "    Net0: ${QEMU_NET0_IP} (via tap0)"
echo "    Net1: ${QEMU_NET1_IP} (via tap1)"
echo ""
echo "  Modbus Port: ${MODBUS_PORT}"
echo ""

# Enable IP forwarding
echo -e "${YELLOW}[1/3] Enabling IP forwarding...${NC}"
sysctl -w net.ipv4.ip_forward=1 >/dev/null
echo "  ✓ IP forwarding enabled"
echo ""

# Flush existing NAT rules
echo -e "${YELLOW}[2/3] Flushing existing NAT rules...${NC}"
iptables -t nat -F
echo "  ✓ NAT table flushed"
echo ""

# Configure NAT rules
echo -e "${YELLOW}[3/3] Configuring iptables NAT rules...${NC}"

echo "  Inbound Path (SCADA → PLC via ${ETH0_NAME} → QEMU Net0):"

# PREROUTING: ETH0 → tap0 (DNAT)
iptables -t nat -A PREROUTING -i ${ETH0_NAME} -d ${ETH0_IP} -p tcp --dport ${MODBUS_PORT} \
  -j DNAT --to-destination ${QEMU_NET0_IP}:${MODBUS_PORT}
echo "    ✓ DNAT: ${ETH0_NAME} (${ETH0_IP}:${MODBUS_PORT}) → QEMU Net0 (${QEMU_NET0_IP}:${MODBUS_PORT})"

# POSTROUTING: tap0 → ETH0 (SNAT)
iptables -t nat -A POSTROUTING -o ${ETH0_NAME} -s 10.2.0.0/24 \
  -j SNAT --to-source ${ETH0_IP}
echo "    ✓ SNAT: tap0 (10.2.0.0/24) → ${ETH0_NAME} (${ETH0_IP})"

echo ""
echo "  Outbound Path (PLC → SCADA via ${ETH1_NAME} → QEMU Net1):"

# PREROUTING: ETH1 → tap1 (DNAT)
iptables -t nat -A PREROUTING -i ${ETH1_NAME} -d ${ETH1_IP} -p tcp --dport ${MODBUS_PORT} \
  -j DNAT --to-destination ${QEMU_NET1_IP}:${MODBUS_PORT}
echo "    ✓ DNAT: ${ETH1_NAME} (${ETH1_IP}:${MODBUS_PORT}) → QEMU Net1 (${QEMU_NET1_IP}:${MODBUS_PORT})"

# POSTROUTING: tap1 → ETH1 (SNAT)
iptables -t nat -A POSTROUTING -o ${ETH1_NAME} -s 10.3.0.0/24 \
  -j SNAT --to-source ${ETH1_IP}
echo "    ✓ SNAT: tap1 (10.3.0.0/24) → ${ETH1_NAME} (${ETH1_IP})"

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}iptables NAT Rules Configured!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Display NAT table
echo -e "${BLUE}Active NAT Rules:${NC}"
echo ""
iptables -t nat -L -n -v --line-numbers
echo ""

echo -e "${BLUE}Traffic Flow:${NC}"
echo ""
echo "INBOUND (SCADA → PLC):"
echo "  1. SCADA sends to PLC (${ETH0_IP}:${MODBUS_PORT})"
echo "  2. Packet arrives at ${ETH0_NAME}"
echo "  3. DNAT → ${QEMU_NET0_IP}:${MODBUS_PORT}"
echo "  4. QEMU Net0 validates and forwards to Net1"
echo "  5. Packet exits tap1 → SNAT to ${ETH1_IP}"
echo "  6. Exits ${ETH1_NAME} to real PLC"
echo ""
echo "OUTBOUND (PLC → SCADA):"
echo "  1. PLC sends to SCADA (via ${ETH1_IP})"
echo "  2. Packet arrives at ${ETH1_NAME}"
echo "  3. DNAT → ${QEMU_NET1_IP}:${MODBUS_PORT}"
echo "  4. QEMU Net1 validates and forwards to Net0"
echo "  5. Packet exits tap0 → SNAT to ${ETH0_IP}"
echo "  6. Exits ${ETH0_NAME} to router → SCADA"
echo ""

echo -e "${GREEN}✓ iptables configuration complete!${NC}"
echo ""
echo -e "${YELLOW}Note: These rules are NOT persistent across reboots.${NC}"
echo "To make persistent, install and configure iptables-persistent:"
echo "  sudo apt install iptables-persistent"
echo "  sudo netfilter-persistent save"
echo ""
