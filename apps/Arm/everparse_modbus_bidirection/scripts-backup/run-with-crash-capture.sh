#!/bin/bash
# Comprehensive crash capture for week-long stability testing
# Features:
#   - Automatic memory dump on crash
#   - Complete log capture
#   - Optional GDB debugging
# Version: v1.0 (2025-10-20)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="/home/iamfo470/phd/camkes-vm-examples/build_modbus"
LOG_DIR="/home/iamfo470/phd/logs/modbus-gateway"
mkdir -p "$LOG_DIR"

# Parse arguments
USE_GDB=false
if [[ "$1" == "--gdb" ]]; then
    USE_GDB=true
    echo "GDB debugging enabled"
fi

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="$LOG_DIR/gateway-${TIMESTAMP}.log"
MONITOR_SOCK="/tmp/qemu-monitor-${TIMESTAMP}.sock"

cat << "EOF"
╔══════════════════════════════════════════════════════════╗
║  Modbus Gateway - Crash Capture Mode                    ║
║  Long-running stability test with crash diagnostics     ║
╚══════════════════════════════════════════════════════════╝
EOF

echo ""
echo "Configuration:"
echo "  Mode:           $([ "$USE_GDB" = true ] && echo "GDB debugging" || echo "Production monitoring")"
echo "  Monitor socket: $MONITOR_SOCK"
echo "  Log file:       $LOG_FILE"
echo "  Build dir:      $BUILD_DIR"
echo ""

# Check prerequisites
if ! ip link show br0 &>/dev/null || ! ip link show br1 &>/dev/null; then
    echo "ERROR: Network bridges not configured!"
    echo "Run: sudo bash scripts/setup-tap-networking.sh"
    exit 1
fi

if [ ! -f "$BUILD_DIR/images/capdl-loader-image-arm-qemu-arm-virt" ]; then
    echo "ERROR: QEMU image not found!"
    echo "Run: cd $BUILD_DIR && ninja"
    exit 1
fi

echo "Network Status:"
bridge link show br0 | head -2
bridge link show br1 | head -2
echo ""

# Start crash monitor in background
echo "Starting crash detection monitor..."

{
    CRASH_DETECTED=false

    while IFS= read -r line; do
        # Pass through all output
        echo "$line"

        # Detect crash
        if echo "$line" | grep -q "FAULT HANDLER"; then
            if [ "$CRASH_DETECTED" = false ]; then
                CRASH_DETECTED=true

                echo ""
                echo "╔══════════════════════════════════════════════════════════╗"
                echo "║  🚨 CRASH DETECTED - Capturing diagnostics...           ║"
                echo "╚══════════════════════════════════════════════════════════╝"
                echo ""

                CRASH_TIME=$(date +"%Y-%m-%d %H:%M:%S")
                CRASH_DUMP="$LOG_DIR/crash-memory-${TIMESTAMP}.dump"
                CRASH_REGS="$LOG_DIR/crash-registers-${TIMESTAMP}.txt"

                # Give fault handler time to print full dump
                sleep 2

                # Dump QEMU memory via monitor
                echo "Dumping QEMU memory (this may take 30-60 seconds)..."
                if echo "pmemsave 0x0 0x80000000 $CRASH_DUMP" | \
                   socat - UNIX-CONNECT:$MONITOR_SOCK 2>/dev/null; then
                    echo "✓ Memory dump saved: $CRASH_DUMP"
                else
                    echo "✗ Warning: Failed to dump memory"
                fi

                # Dump registers
                if echo "info registers" | \
                   socat - UNIX-CONNECT:$MONITOR_SOCK 2>/dev/null > "$CRASH_REGS"; then
                    echo "✓ Register dump saved: $CRASH_REGS"
                else
                    echo "✗ Warning: Failed to dump registers"
                fi

                # Create crash summary
                CRASH_SUMMARY="$LOG_DIR/CRASH-SUMMARY-${TIMESTAMP}.txt"
                cat > "$CRASH_SUMMARY" << SUMMARY_EOF
╔══════════════════════════════════════════════════════════╗
║  CRASH SUMMARY                                           ║
╚══════════════════════════════════════════════════════════╝

Crash Time: $CRASH_TIME
Session Start: $TIMESTAMP
Log File: $LOG_FILE

Files Generated:
  - Full log:     $LOG_FILE
  - Memory dump:  $CRASH_DUMP
  - Registers:    $CRASH_REGS
  - This summary: $CRASH_SUMMARY

To analyze crash:
  1. grep "FAULT HANDLER" $LOG_FILE
  2. Check PC address and fault address
  3. Use arm-none-eabi-objdump to disassemble:
     arm-none-eabi-objdump -d $BUILD_DIR/CMakeFiles/net0_drv.instance.bin.dir/*.o | grep -A 20 "PC_ADDRESS"

Last 50 lines before crash:
$(tail -50 "$LOG_FILE" | grep -B 50 "FAULT HANDLER" | tail -50)

╚══════════════════════════════════════════════════════════╝
SUMMARY_EOF

                echo "✓ Crash summary saved: $CRASH_SUMMARY"
                echo ""
                echo "Crash analysis files ready:"
                ls -lh "$CRASH_DUMP" "$CRASH_REGS" "$CRASH_SUMMARY" 2>/dev/null || true

                # Continue monitoring for final output
                sleep 5
            fi
        fi
    done
} | tee -a "$LOG_FILE" &

MONITOR_PID=$!

# Cleanup
cleanup() {
    echo ""
    echo "═══════════════════════════════════════════════════════════"
    echo "Session ended at $(date)"
    echo ""

    # Kill monitor
    kill $MONITOR_PID 2>/dev/null || true

    # Show session summary
    DURATION=$SECONDS
    HOURS=$((DURATION / 3600))
    MINS=$(((DURATION % 3600) / 60))
    SECS=$((DURATION % 60))

    echo "Runtime: ${HOURS}h ${MINS}m ${SECS}s"
    echo "Log file: $LOG_FILE"
    echo "Log size: $(du -h "$LOG_FILE" | cut -f1)"
    echo ""

    # Check if crash occurred
    if grep -q "FAULT HANDLER" "$LOG_FILE" 2>/dev/null; then
        echo "🚨 CRASH DETECTED - See crash summary:"
        ls "$LOG_DIR"/CRASH-SUMMARY-*.txt 2>/dev/null | tail -1
    else
        echo "✓ No crashes detected"
    fi

    echo "═══════════════════════════════════════════════════════════"
}
trap cleanup EXIT INT TERM

echo "Starting QEMU..."
echo "Press Ctrl+C to stop"
echo ""
echo "Monitor output will be logged to: $LOG_FILE"
echo ""

cd "$BUILD_DIR"

# Build QEMU command
QEMU_ARGS=(
    -machine virt,virtualization=on,highmem=off,secure=off
    -cpu cortex-a53
    -nographic
    -m 2048
    -kernel images/capdl-loader-image-arm-qemu-arm-virt
    -netdev bridge,id=mynet0,br=br0
    -device virtio-net-device,netdev=mynet0,mac=52:54:00:12:34:56
    -netdev bridge,id=mynet1,br=br1
    -device virtio-net-device,netdev=mynet1,mac=52:54:00:12:34:57
    -monitor unix:$MONITOR_SOCK,server,nowait
    -serial stdio
)

# Add GDB server if requested
if [ "$USE_GDB" = true ]; then
    QEMU_ARGS+=(-s)  # GDB server on port 1234
    echo "GDB server enabled on port 1234"
    echo "To attach: arm-none-eabi-gdb -x scripts/crash-gdb-monitor.gdb"
    echo ""
fi

# Run QEMU
qemu-system-arm "${QEMU_ARGS[@]}"

echo ""
echo "QEMU exited normally"
