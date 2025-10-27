#!/bin/bash
# Enhanced QEMU run script with monitoring and crash capture
# Version: v1.0 (2025-10-20)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="/home/iamfo470/phd/camkes-vm-examples/build_modbus"

# Log file with timestamp
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="/home/iamfo470/phd/logs/modbus-gateway"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/gateway-${TIMESTAMP}.log"
MONITOR_SOCK="/tmp/qemu-monitor-${TIMESTAMP}.sock"

echo "═══════════════════════════════════════════════════════════"
echo "Starting GRFICS ICS Security Gateway"
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "Configuration:"
echo "  Log file:       $LOG_FILE"
echo "  Monitor socket: $MONITOR_SOCK"
echo "  Build dir:      $BUILD_DIR"
echo ""

# Check if bridges exist
if ! ip link show br0 &>/dev/null; then
    echo "ERROR: Bridge br0 not found! Run setup-tap-networking.sh first"
    exit 1
fi

if ! ip link show br1 &>/dev/null; then
    echo "ERROR: Bridge br1 not found! Run setup-tap-networking.sh first"
    exit 1
fi

echo "Bridge Status:"
bridge link show br0 | head -2
bridge link show br1 | head -2
echo ""

# Start crash monitor in background
echo "Starting crash monitor..."
(
    # Monitor for crash and auto-dump memory
    while IFS= read -r line; do
        echo "$line"  # Pass through to log

        if echo "$line" | grep -q "FAULT HANDLER"; then
            echo ""
            echo "╔══════════════════════════════════════════════════════════╗"
            echo "║  CRASH DETECTED - Dumping QEMU memory                   ║"
            echo "╚══════════════════════════════════════════════════════════╝"

            CRASH_DUMP="$LOG_DIR/crash-memory-${TIMESTAMP}.dump"

            # Dump entire QEMU memory via monitor
            echo "pmemsave 0x0 0x80000000 $CRASH_DUMP" | \
                socat - UNIX-CONNECT:$MONITOR_SOCK 2>/dev/null || \
                echo "Warning: Failed to connect to QEMU monitor"

            # Also save registers
            echo "info registers" | \
                socat - UNIX-CONNECT:$MONITOR_SOCK 2>/dev/null > \
                "$LOG_DIR/crash-registers-${TIMESTAMP}.txt" || true

            echo "Memory dump saved to: $CRASH_DUMP"
            echo "Register dump saved to: $LOG_DIR/crash-registers-${TIMESTAMP}.txt"
            echo ""

            # Keep logging for a bit to capture final state
            sleep 5
            break
        fi
    done
) | tee -a "$LOG_FILE" &

MONITOR_PID=$!

echo "Monitor PID: $MONITOR_PID"
echo ""

# Cleanup on exit
cleanup() {
    echo ""
    echo "Cleaning up..."
    kill $MONITOR_PID 2>/dev/null || true

    # Final log summary
    echo ""
    echo "═══════════════════════════════════════════════════════════"
    echo "Session ended at $(date)"
    echo "Log file: $LOG_FILE"

    # Show last few lines
    echo "Last 10 lines:"
    tail -10 "$LOG_FILE"
    echo "═══════════════════════════════════════════════════════════"
}
trap cleanup EXIT INT TERM

echo "Starting QEMU..."
echo ""

# Run QEMU with monitor enabled
cd "$BUILD_DIR"

qemu-system-arm \
    -machine virt,virtualization=on,highmem=off,secure=off \
    -cpu cortex-a53 \
    -nographic \
    -m 2048 \
    -kernel images/capdl-loader-image-arm-qemu-arm-virt \
    -netdev bridge,id=mynet0,br=br0 \
    -device virtio-net-device,netdev=mynet0,mac=52:54:00:12:34:56 \
    -netdev bridge,id=mynet1,br=br1 \
    -device virtio-net-device,netdev=mynet1,mac=52:54:00:12:34:57 \
    -monitor unix:$MONITOR_SOCK,server,nowait \
    -serial stdio

echo ""
echo "QEMU exited"
