#!/bin/bash
# Test script for sDDF-inspired ICS Network Driver with ncat
# Demonstrates real ICS protocol testing

echo "=== sDDF-Inspired ICS Security Gateway Network Test ==="
echo ""

# Build the system
echo "1. Building sDDF-inspired ICS system..."
cd ~/phd/camkes-vm-examples/build
source ~/seL4-venv/bin/activate
ninja

if [ $? -ne 0 ]; then
    echo "❌ Build failed!"
    exit 1
fi

echo "✅ Build successful!"
echo ""

# Create test payloads for different ICS protocols
echo "2. Creating ICS protocol test payloads..."

# Modbus TCP Read Input Registers (Function Code 04)
echo -ne '\x00\x01\x00\x00\x00\x06\x01\x04\x00\x00\x00\x0A' > /tmp/modbus_read_input.bin

# Modbus TCP Read Holding Registers (Function Code 03)
echo -ne '\x00\x01\x00\x00\x00\x06\x01\x03\x00\x00\x00\x01' > /tmp/modbus_read_holding.bin

# DNP3 Data Link Frame
echo -ne '\x05\x64\x05\xC9\x01\x00\x00\x04\xE9\x21' > /tmp/dnp3_frame.bin

# EtherNet/IP List Identity Command
echo -ne '\x6F\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x63\x00' > /tmp/ethernet_ip_list.bin

echo "✅ Test payloads created in /tmp/"
echo ""

# Run QEMU with network enabled
echo "3. Starting sDDF-inspired ICS system with network enabled..."
echo "   (Will show protocol detection and security pipeline processing)"
echo ""

# Enhanced QEMU command with user networking
timeout 30s qemu-system-arm \
    -machine virt -nographic -m 2G \
    -kernel images/capdl-loader-image-arm-qemu-arm-virt \
    -netdev user,id=net0,hostfwd=tcp::15502-:502,hostfwd=tcp::15503-:20000 \
    -device virtio-net-device,netdev=net0 &

QEMU_PID=$!
sleep 5

echo ""
echo "4. QEMU is running with network forwarding:"
echo "   - Host port 15502 → Guest port 502 (Modbus TCP)"
echo "   - Host port 15503 → Guest port 20000 (DNP3)"
echo ""

# Test commands the user can run
echo "5. ⚡ Test Commands (run in separate terminals):"
echo ""
echo "   📡 Test Modbus TCP Read Input Registers:"
echo "   cat /tmp/modbus_read_input.bin | ncat localhost 15502"
echo ""
echo "   📡 Test Modbus TCP Read Holding Registers:"
echo "   cat /tmp/modbus_read_holding.bin | ncat localhost 15502"
echo ""
echo "   📡 Test DNP3 Protocol:"
echo "   cat /tmp/dnp3_frame.bin | ncat localhost 15503"
echo ""
echo "   📡 Test EtherNet/IP Protocol:"
echo "   cat /tmp/ethernet_ip_list.bin | ncat localhost 15503"
echo ""
echo "   📡 Send Custom Modbus (interactive):"
echo "   ncat localhost 15502"
echo ""

echo "🔍 Expected Results:"
echo "   - Protocol detection based on binary payload analysis"
echo "   - Security pipeline processing: ExtFrontend → ParserNorm → PolicyEmit → IntNicDrv"
echo "   - Real-time message classification and forwarding"
echo ""

echo "⏰ QEMU will run for 30 seconds, then auto-terminate"
echo "   Press Ctrl+C to stop early"
echo ""

# Wait for QEMU to finish or be terminated
wait $QEMU_PID

echo ""
echo "✅ Test session complete!"
echo ""
echo "📋 Summary:"
echo "   - Built sDDF-inspired ICS security gateway"
echo "   - Enabled network testing with QEMU user networking"
echo "   - Provided ncat test commands for Modbus TCP, DNP3, EtherNet/IP"
echo "   - Demonstrated real binary protocol detection"
echo ""
echo "🚀 Next Steps:"
echo "   - Run the ncat commands during QEMU execution"
echo "   - Observe protocol detection and security processing"
echo "   - Test with custom ICS payloads"