#!/bin/sh
#
# Test script to verify VM can access VirtIO MMIO registers
# This script will attempt to read the VirtIO magic number from device registers
#

echo "=== VirtIO Magic Number Test ==="
echo "Testing VM access to VirtIO MMIO registers"
echo ""

# VirtIO MMIO base addresses for QEMU ARM virt platform
# VirtIO devices start at 0x0a000000 (virtio-mmio@a000000 in device tree)
VIRTIO_BASE_0="0x0a000000"
VIRTIO_BASE_1="0x0a000200"
VIRTIO_BASE_2="0x0a000400"
VIRTIO_BASE_3="0x0a000600"

# VirtIO Magic Number offset = 0x000
# Expected value: 0x74726976 ("virt" in little-endian)

echo "Checking for devmem tool..."
if ! command -v devmem >/dev/null 2>&1; then
    echo "ERROR: devmem not available"
    echo "Trying manual /dev/mem read..."

    # Alternative method using dd if devmem not available
    if [ -c /dev/mem ]; then
        echo "Found /dev/mem device"
        echo "Reading magic number from ${VIRTIO_BASE_0}..."
        # This would require more complex dd usage
        echo "Manual /dev/mem access requires custom C program"
    else
        echo "ERROR: /dev/mem not accessible"
        echo "Cannot test virtio register access without devmem or /dev/mem"
        exit 1
    fi
else
    echo "Found devmem utility"
    echo ""

    # Test each potential VirtIO device location
    for i in 0 1 2 3; do
        case $i in
            0) ADDR="$VIRTIO_BASE_0" ;;
            1) ADDR="$VIRTIO_BASE_1" ;;
            2) ADDR="$VIRTIO_BASE_2" ;;
            3) ADDR="$VIRTIO_BASE_3" ;;
        esac

        echo "--- VirtIO Device $i ---"
        echo "Base Address: $ADDR"
        echo -n "Magic Number: "

        # Read 32-bit value from magic number register (offset 0x00)
        MAGIC=$(devmem "$ADDR" 32 2>/dev/null)

        if [ $? -eq 0 ]; then
            echo "$MAGIC"

            # Check if it matches expected virtio magic (0x74726976)
            if [ "$MAGIC" = "0x74726976" ] || [ "$MAGIC" = "0x74726976" ]; then
                echo "✓ VALID VIRTIO DEVICE FOUND!"
                echo ""
                echo "Reading additional VirtIO registers:"
                echo -n "  Version (offset 0x04): "
                devmem $(printf "0x%x" $((ADDR + 0x04))) 32 2>/dev/null

                echo -n "  DeviceID (offset 0x08): "
                DEVICE_ID=$(devmem $(printf "0x%x" $((ADDR + 0x08))) 32 2>/dev/null)
                echo "$DEVICE_ID"

                echo -n "  VendorID (offset 0x0C): "
                devmem $(printf "0x%x" $((ADDR + 0x0C))) 32 2>/dev/null

                # Decode device type
                case "$DEVICE_ID" in
                    "0x00000001") echo "  Device Type: Network (virtio-net)" ;;
                    "0x00000002") echo "  Device Type: Block Device" ;;
                    "0x00000003") echo "  Device Type: Console" ;;
                    "0x00000004") echo "  Device Type: Entropy Source" ;;
                    *) echo "  Device Type: Unknown ($DEVICE_ID)" ;;
                esac
                echo ""
            else
                echo "✗ Not a VirtIO device (magic mismatch)"
            fi
        else
            echo "ERROR - Cannot read from address (bus error or unmapped)"
        fi
        echo ""
    done
fi

echo "=== Test Complete ==="
echo ""
echo "If you see 'bus error' or cannot read, the VirtIO MMIO region"
echo "is not properly mapped in the seL4 VM configuration."
echo ""
echo "Expected to see magic number 0x74726976 for valid VirtIO devices."
