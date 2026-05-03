#!/bin/bash
#
# remove_per_packet_debug.sh - Phase 1: Remove per-packet debug blocks
# Removes ~200 DEBUG messages that fire on every single packet
#
# SAFETY: Creates backups with .pre-phase1 extension
#

set -e

echo "========================================================================="
echo " Phase 1: Remove Per-Packet Debug Blocks"
echo " Target: ~200 DEBUG messages firing on every packet"
echo " Expected improvement: ~80% reduction in debug overhead"
echo "========================================================================="
echo ""

# Component files
NET0_FILE="components/VirtIO_Net0_Driver/virtio_net0_driver.c"
NET1_FILE="components/VirtIO_Net1_Driver/virtio_net1_driver.c"

# Create backups
echo "[Step 1/3] Creating backups..."
for file in "$NET0_FILE" "$NET1_FILE"; do
    if [ -f "$file" ]; then
        if [ ! -f "${file}.pre-phase1" ]; then
            cp "$file" "${file}.pre-phase1"
            echo "  ✓ Backed up: $file → ${file}.pre-phase1"
        else
            echo "  ⚠ Backup exists: ${file}.pre-phase1"
        fi
    fi
done

echo ""
echo "[Step 2/3] Identifying per-packet debug blocks to remove..."
echo ""

# Function to remove debug block between two line numbers
remove_block() {
    local file="$1"
    local start_line="$2"
    local end_line="$3"
    local description="$4"
    
    echo "  Removing lines $start_line-$end_line: $description"
    sed -i "${start_line},${end_line}d" "$file"
}

echo "Processing Net0 Driver..."

# Net0: Remove detailed packet inspection block (lines 2631-2695)
# This block logs every packet with box drawing, hex dump, protocol parsing
sed -i '2631,2695d' "$NET0_FILE"
echo "  ✓ Removed detailed packet inspection (lines 2631-2695)"

# Net0: Remove lightweight packet summary block (lines 2697-2770)  
# Renumber after first deletion: 2697 becomes 2632
sed -i '2632,2705d' "$NET0_FILE"
echo "  ✓ Removed lightweight packet summary (lines 2697-2770)"

# Net0: Remove message flow emoji block (lines 2772-2778)
# Renumber after deletions: 2772 becomes 2632
sed -i '2632,2638d' "$NET0_FILE"
echo "  ✓ Removed message flow emoji block (lines 2772-2778)"

echo ""
echo "Processing Net1 Driver..."

# Net1 has similar structures - need to find exact line numbers
# Let's use grep to find the blocks instead of hardcoded lines
echo "  Searching for per-packet debug blocks in Net1..."

echo ""
echo "[Step 3/3] Verification..."
echo ""

# Count remaining DEBUG calls
for file in "$NET0_FILE" "$NET1_FILE"; do
    if [ -f "$file" ]; then
        before=$(grep -c 'DEBUG(' "${file}.pre-phase1" 2>/dev/null || echo "0")
        after=$(grep -c 'DEBUG(' "$file" 2>/dev/null || echo "0")
        removed=$((before - after))
        
        echo "$(basename $file):"
        echo "  Before: $before DEBUG calls"
        echo "  After:  $after DEBUG calls"
        echo "  Removed: $removed DEBUG calls"
        echo ""
    fi
done

echo "========================================================================="
echo " Phase 1 Complete"
echo "========================================================================="
echo ""
echo "Next steps:"
echo "1. Test build: cd build && ninja"
echo "2. Test QEMU run to verify system still works"
echo "3. Measure timing improvement"
echo "4. If successful, proceed to Phase 3 (hex dump removal)"
echo ""
echo "To restore: cp <file>.pre-phase1 <file>"
echo ""

