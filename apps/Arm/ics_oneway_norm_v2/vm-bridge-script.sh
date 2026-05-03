#!/bin/bash
#
# ICS One-Way Normalizer V2 - VM Bridge Script
# Dual NIC bridge with seL4 processing injection
#
# Network Configuration:
#   eth0: 192.168.1.10:8502  (external-facing)
#   eth1: 192.168.10.10:8503 (internal-facing)
#
# Flow: External → eth0 → seL4 processing → eth1 → Internal
#

set -e

EXTERNAL_IP="192.168.1.10"
EXTERNAL_PORT="8502"
INTERNAL_IP="192.168.10.10"
INTERNAL_PORT="8503"
VIRTIO_CONSOLE="/dev/virtio-ports/vport0p1"

echo "=== ICS One-Way Normalizer V2 Bridge Starting ==="
echo "External: ${EXTERNAL_IP}:${EXTERNAL_PORT}"
echo "Internal: ${INTERNAL_IP}:${INTERNAL_PORT}"
echo "seL4 Pipe: ${VIRTIO_CONSOLE}"

# Function to setup network interfaces
setup_network() {
    echo "Setting up network interfaces..."

    # Configure external interface
    ip addr add ${EXTERNAL_IP}/24 dev eth0
    ip link set eth0 up
    echo "✓ eth0: ${EXTERNAL_IP} (external)"

    # Configure internal interface
    ip addr add ${INTERNAL_IP}/24 dev eth1
    ip link set eth1 up
    echo "✓ eth1: ${INTERNAL_IP} (internal)"

    # Enable IP forwarding
    echo 1 > /proc/sys/net/ipv4/ip_forward
    echo "✓ IP forwarding enabled"
}

# Function to process message through seL4 pipeline
process_via_sel4() {
    local message="$1"

    echo "[BRIDGE] Sending to seL4: $message" >&2

    # Send message to seL4 NetworkNicDrv via virtio-console
    echo "$message" > "$VIRTIO_CONSOLE"

    # Wait for processed response from seL4 IntNicDrv
    # Note: This is simplified - real implementation would handle async responses
    read -r response < "$VIRTIO_CONSOLE"

    echo "[BRIDGE] Received from seL4: $response" >&2
    echo "$response"
}

# Function to handle external client connections
handle_external_client() {
    local client_fd="$1"

    echo "[BRIDGE] New external client connected"

    while read -r line <&${client_fd}; do
        echo "[BRIDGE] External input: $line"

        # Process through seL4 security pipeline
        processed_message=$(process_via_sel4 "$line")

        # Forward to internal network
        echo "[BRIDGE] Forwarding to internal: $processed_message"
        echo "$processed_message" | nc ${INTERNAL_IP} ${INTERNAL_PORT} &

        # Echo response back to external client (optional)
        echo "PROCESSED: $processed_message" >&${client_fd}
    done

    echo "[BRIDGE] External client disconnected"
}

# Function to start external listener
start_external_listener() {
    echo "Starting external listener on ${EXTERNAL_IP}:${EXTERNAL_PORT}..."

    # Use netcat to listen for external connections
    while true; do
        echo "[BRIDGE] Waiting for external connections..."
        nc -l -s ${EXTERNAL_IP} -p ${EXTERNAL_PORT} | while read -r line; do
            echo "[BRIDGE] External input: $line"

            # Process through seL4 security pipeline
            if [[ -c "$VIRTIO_CONSOLE" ]]; then
                processed_message=$(process_via_sel4 "$line")

                # Forward to internal network (fire-and-forget)
                echo "[BRIDGE] Forwarding to internal: $processed_message"
                echo "$processed_message" | nc -w 1 ${INTERNAL_IP} ${INTERNAL_PORT} 2>/dev/null &
            else
                echo "[BRIDGE] WARNING: seL4 virtio-console not available, bypassing"
                processed_message="$line"
            fi
        done

        echo "[BRIDGE] Connection closed, restarting listener..."
        sleep 1
    done
}

# Function to start internal forwarder (for testing)
start_internal_forwarder() {
    echo "Starting internal forwarder on ${INTERNAL_IP}:${INTERNAL_PORT}..."

    while true; do
        echo "[BRIDGE] Internal forwarder ready"
        nc -l -s ${INTERNAL_IP} -p ${INTERNAL_PORT} | while read -r line; do
            echo "[BRIDGE] Internal output: $line"
            # In real deployment, this would forward to actual internal systems
        done
        sleep 1
    done
}

# Main execution
main() {
    # Setup network
    setup_network

    # Wait for virtio-console (seL4 might take time to initialize)
    echo "Waiting for seL4 virtio-console..."
    timeout=30
    while [[ ! -c "$VIRTIO_CONSOLE" && $timeout -gt 0 ]]; do
        echo "  Waiting for $VIRTIO_CONSOLE ($timeout seconds remaining)"
        sleep 1
        ((timeout--))
    done

    if [[ -c "$VIRTIO_CONSOLE" ]]; then
        echo "✓ seL4 virtio-console available"
    else
        echo "⚠ seL4 virtio-console not found, bridge will bypass seL4 processing"
    fi

    # Start internal forwarder in background (for testing)
    start_internal_forwarder &
    INTERNAL_PID=$!

    echo "=== Bridge fully operational ==="
    echo "Test with: echo 'MODBUS_READ_COILS' | nc ${EXTERNAL_IP} ${EXTERNAL_PORT}"

    # Start external listener (blocks)
    start_external_listener
}

# Cleanup function
cleanup() {
    echo "=== Bridge shutting down ==="
    kill $INTERNAL_PID 2>/dev/null || true
    exit 0
}

# Handle signals
trap cleanup SIGINT SIGTERM

# Run main function
main "$@"