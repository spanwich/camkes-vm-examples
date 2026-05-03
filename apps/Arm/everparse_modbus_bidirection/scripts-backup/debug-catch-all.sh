#!/bin/bash
# Launch GDB with comprehensive fault catching for seL4

BUILD_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$BUILD_DIR"

echo "╔════════════════════════════════════════════════════════════╗"
echo "║  seL4 Comprehensive Fault Debugger                         ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo
echo "This will:"
echo "  1. Launch GDB with catch-all fault configuration"
echo "  2. Connect to QEMU GDB server (localhost:1234)"
echo "  3. Catch ALL memory faults at multiple levels:"
echo "     - Kernel: c_handle_data_fault (seL4's data abort handler)"
echo "     - App: PC 0x39868 (known tcp_output crash)"
echo "     - Memory: NULL region watchpoints (0x0, 0x10, 0x20)"
echo
echo "PREREQUISITE: QEMU must be running with GDB server!"
echo
echo "To start QEMU in another terminal:"
echo "  cd $BUILD_DIR"
echo "  ./run-remote-gdb.sh"
echo
echo "Press Enter to launch GDB, or Ctrl+C to cancel..."
read

# Check if QEMU is listening on port 1234
if ! nc -z localhost 1234 2>/dev/null; then
    echo
    echo "⚠️  WARNING: No GDB server detected on localhost:1234"
    echo
    echo "Please start QEMU with GDB server first:"
    echo "  ./run-remote-gdb.sh"
    echo
    echo "Press Enter to try connecting anyway, or Ctrl+C to abort..."
    read
fi

echo
echo "Launching GDB with comprehensive fault catching..."
echo "Log will be saved to: gdb-fault-log.txt"
echo

# Launch GDB with both kernel symbols and application image
gdb-multiarch \
    -ex "add-symbol-file kernel/kernel.elf" \
    -x gdb-catch-all-faults.txt \
    images/capdl-loader-image-arm-qemu-arm-virt
