#!/bin/bash
#
# Test script for ICS One-Way Normalizer V2 Pipeline
# Tests external → VM → seL4 → internal flow
#

EXTERNAL_IP="192.168.1.10"
EXTERNAL_PORT="8502"
INTERNAL_IP="192.168.10.10"
INTERNAL_PORT="8503"

echo "=== ICS One-Way Normalizer V2 Pipeline Test ==="
echo "Testing complete end-to-end flow"

# Test messages
TEST_MESSAGES=(
    "MODBUS_READ_HOLDING_REGISTERS"
    "DNP3_DATA_REQUEST"
    "ETHERNET_IP_CLASS_REQUEST"
    "GENERIC_SENSOR_DATA_12345"
    "MALFORMED_MESSAGE_XXXXXX"
)

echo "Starting internal listener for testing..."
{
    echo "Internal target ready on port $INTERNAL_PORT"
    while true; do
        echo "[$(date)] Waiting for processed messages..."
        nc -l -p $INTERNAL_PORT | while read line; do
            echo "[$(date)] INTERNAL RECEIVED: $line"
        done
        sleep 1
    done
} &
INTERNAL_PID=$!

echo "Waiting for VM bridge to be ready..."
sleep 5

echo "Testing pipeline with various ICS messages..."
echo "============================================="

for i in "${!TEST_MESSAGES[@]}"; do
    message="${TEST_MESSAGES[$i]}"
    echo ""
    echo "Test $((i+1)): $message"
    echo "  → Sending to ${EXTERNAL_IP}:${EXTERNAL_PORT}"

    # Send message and capture response
    response=$(echo "$message" | nc -w 5 $EXTERNAL_IP $EXTERNAL_PORT 2>/dev/null || echo "NO_RESPONSE")

    if [[ "$response" != "NO_RESPONSE" ]]; then
        echo "  ✓ External response: $response"
    else
        echo "  ✗ No response from external interface"
    fi

    sleep 2
done

echo ""
echo "=== Pipeline Test Summary ==="
echo "Check seL4 console output for:"
echo "  1. NetworkNicDrv: Received messages from VM"
echo "  2. ExtFrontend: Processing messages"
echo "  3. ParserNorm: Validating messages"
echo "  4. PolicyEmit: Applying policies"
echo "  5. IntNicDrv: Forwarding processed messages"

echo ""
echo "Expected seL4 console trace for each message:"
echo "  NetworkNicDrv: Received X bytes from VM: [message]"
echo "  NetworkNicDrv: Converted to protocol tag=0xXXXX, len=XX"
echo "  NetworkNicDrv: Forwarded message #X to pipeline"
echo "  ExtFrontend: Processing message..."
echo "  ParserNorm: Validating message..."
echo "  PolicyEmit: Applying policy..."
echo "  IntNicDrv: Message processed successfully"

# Cleanup
echo ""
echo "Cleaning up test..."
kill $INTERNAL_PID 2>/dev/null || true

echo "V2 Pipeline test complete!"
echo "If messages appear in seL4 console, the external→VM→seL4 flow is working!"