#!/bin/bash
# Create deployment package for remote server
#
# This script packages the built image and all necessary deployment scripts
# into a tarball ready for remote deployment.

set -e

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Creating Remote Deployment Package${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Configuration
PROJECT_ROOT="/home/iamfo470/phd/camkes-vm-examples"
BUILD_DIR="${PROJECT_ROOT}/build_modbus"
APP_DIR="${PROJECT_ROOT}/projects/vm-examples/apps/Arm/modbus_bidirection_poc"
DEPLOY_DIR="modbus_gateway_deploy"
TARBALL="modbus_gateway_deploy.tar.gz"

# Check if built image exists
if [ ! -f "${BUILD_DIR}/images/capdl-loader-image-arm-qemu-arm-virt" ]; then
    echo -e "${RED}ERROR: Built image not found!${NC}"
    echo ""
    echo "Expected location:"
    echo "  ${BUILD_DIR}/images/capdl-loader-image-arm-qemu-arm-virt"
    echo ""
    echo "Please build the project first:"
    echo "  cd ${PROJECT_ROOT}"
    echo "  mkdir -p build_modbus && cd build_modbus"
    echo "  ../init-build.sh -DPLATFORM=qemu-arm-virt -DAARCH32=TRUE -DCAMKES_APP=modbus_bidirection_poc"
    echo "  ninja"
    exit 1
fi

# Create deployment directory
echo -e "${YELLOW}[1/5] Creating deployment directory...${NC}"
rm -rf "${DEPLOY_DIR}"
mkdir -p "${DEPLOY_DIR}"
echo "  ✓ Created ${DEPLOY_DIR}/"
echo ""

# Copy built image
echo -e "${YELLOW}[2/5] Copying built image...${NC}"
cp "${BUILD_DIR}/images/capdl-loader-image-arm-qemu-arm-virt" "${DEPLOY_DIR}/"
IMAGE_SIZE=$(ls -lh "${DEPLOY_DIR}/capdl-loader-image-arm-qemu-arm-virt" | awk '{print $5}')
echo "  ✓ Copied kernel image (${IMAGE_SIZE})"
echo ""

# Copy deployment scripts
echo -e "${YELLOW}[3/5] Copying deployment scripts...${NC}"
cp "${APP_DIR}/setup-vlan-networking.sh" "${DEPLOY_DIR}/"
cp "${APP_DIR}/setup-iptables.sh" "${DEPLOY_DIR}/"
cp "${APP_DIR}/run-remote.sh" "${DEPLOY_DIR}/"
echo "  ✓ setup-vlan-networking.sh"
echo "  ✓ setup-iptables.sh"
echo "  ✓ run-remote.sh"
echo ""

# Copy documentation
echo -e "${YELLOW}[4/5] Copying documentation...${NC}"
cp "${APP_DIR}/README.md" "${DEPLOY_DIR}/"
cp "${APP_DIR}/VLAN_ARCHITECTURE.md" "${DEPLOY_DIR}/"
cp "${APP_DIR}/REMOTE_SERVER_SETUP.md" "${DEPLOY_DIR}/"
cp "${APP_DIR}/INSTALL_DEPENDENCIES.txt" "${DEPLOY_DIR}/"
cp "${APP_DIR}/FIX_IPTABLES.txt" "${DEPLOY_DIR}/"
cp "${APP_DIR}/QUICK_START_ens224_ens256.txt" "${DEPLOY_DIR}/" 2>/dev/null || true
cp "${APP_DIR}/VIRTIO_RACE_CONDITION_ANALYSIS.md" "${DEPLOY_DIR}/" 2>/dev/null || true
echo "  ✓ README.md"
echo "  ✓ VLAN_ARCHITECTURE.md"
echo "  ✓ REMOTE_SERVER_SETUP.md"
echo "  ✓ INSTALL_DEPENDENCIES.txt"
echo "  ✓ FIX_IPTABLES.txt"
echo "  ✓ QUICK_START_ens224_ens256.txt (if available)"
echo ""

# Create deployment instructions
echo -e "${YELLOW}[5/5] Creating deployment instructions...${NC}"
cat > "${DEPLOY_DIR}/DEPLOYMENT_INSTRUCTIONS.txt" << 'EOF'
========================================
ICS Security Gateway - Remote Deployment
========================================

DEPLOYMENT STEPS:

1. EXTRACT PACKAGE
   ----------------
   tar xzf modbus_gateway_deploy.tar.gz
   cd modbus_gateway_deploy

2. VERIFY QEMU INSTALLATION
   -------------------------
   qemu-system-arm --version

   If not installed:
     sudo apt update
     sudo apt install qemu-system-arm

3. CONFIGURE NETWORK (One-time setup)
   -----------------------------------
   sudo ./setup-vlan-networking.sh

   This configures:
   - Physical NICs: eth0 (192.168.95.2), eth1 (192.168.90.1)
   - TAP interfaces: tap0 (10.2.0.1), tap1 (10.3.0.1)
   - iptables NAT rules (DNAT/SNAT)
   - IP forwarding

   IMPORTANT: Real PLC MUST be moved to isolated VLAN!

4. START SECURITY GATEWAY
   -----------------------
   ./run-remote.sh

   Expected output:
   - Network configuration summary
   - QEMU starts with dual VirtIO networks
   - Both drivers show "TCP Echo Server listening on port 502"

5. TEST CONNECTIVITY
   ------------------
   From host:
     ping 10.2.0.2  # Should reach QEMU Net0
     ping 10.3.0.2  # Should reach QEMU Net1

   From SCADA:
     modbus_client -h 192.168.95.2 -p 502

TROUBLESHOOTING:

  TAP interfaces not found:
    sudo ./setup-vlan-networking.sh

  iptables rules not configured:
    sudo ./setup-iptables.sh

  No connectivity to QEMU:
    - Check TAP interfaces are UP: ip addr show tap0 tap1
    - Check iptables: sudo iptables -t nat -L -n -v
    - Check IP forwarding: cat /proc/sys/net/ipv4/ip_forward (should be 1)

  Capture traffic:
    sudo tcpdump -i tap0 -n port 502
    sudo tcpdump -i tap1 -n port 502

FILES INCLUDED:

  capdl-loader-image-arm-qemu-arm-virt  - QEMU kernel image
  setup-vlan-networking.sh              - Network configuration
  setup-iptables.sh                     - iptables NAT setup
  run-remote.sh                         - QEMU startup script
  README.md                             - Complete documentation
  VLAN_ARCHITECTURE.md                  - Architecture details
  DEPLOYMENT_INSTRUCTIONS.txt           - This file

MAKING PERSISTENT (Optional):

  To survive reboots, install iptables-persistent:
    sudo apt install iptables-persistent
    sudo netfilter-persistent save

  Or create systemd service (see README.md)

DOCUMENTATION:

  See README.md for:
  - Complete architecture explanation
  - Traffic flow diagrams
  - Troubleshooting guide
  - Performance considerations

  See VLAN_ARCHITECTURE.md for:
  - Detailed VLAN design
  - IP address mapping
  - iptables NAT rules explained

SUPPORT:

  All scripts are self-documenting with --help
  Check QEMU console for detailed diagnostic messages
  Use tcpdump to verify traffic flow

========================================
EOF

echo "  ✓ DEPLOYMENT_INSTRUCTIONS.txt"
echo ""

# Create tarball
echo -e "${YELLOW}Creating tarball...${NC}"
tar czf "${TARBALL}" "${DEPLOY_DIR}"
TARBALL_SIZE=$(ls -lh "${TARBALL}" | awk '{print $5}')
echo "  ✓ Created ${TARBALL} (${TARBALL_SIZE})"
echo ""

# Display package contents
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Deployment Package Created!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

echo -e "${BLUE}Package Contents:${NC}"
tar tzf "${TARBALL}" | sed 's/^/  /'
echo ""

echo -e "${BLUE}Package Location:${NC}"
echo "  $(pwd)/${TARBALL}"
echo ""

echo -e "${BLUE}Next Steps:${NC}"
echo "  1. Copy to remote server:"
echo "     scp ${TARBALL} user@remote-server:~/"
echo ""
echo "  2. On remote server:"
echo "     tar xzf ${TARBALL}"
echo "     cd ${DEPLOY_DIR}"
echo "     cat DEPLOYMENT_INSTRUCTIONS.txt"
echo ""

echo -e "${GREEN}✓ Ready for deployment!${NC}"
echo ""
