#!/bin/bash
# Final step: Add debug headers to remaining files and verify conversion

set -e

echo "========================================================================" 
echo " Finalizing Debug System Conversion"
echo "========================================================================"
echo ""

# Check Net0 (already done manually, just verify)
echo "[1/2] Verifying virtio_net0_driver.c..."
if grep -q "debug_levels.h" components/VirtIO_Net0_Driver/virtio_net0_driver.c; then
    echo "  ✓ debug_levels.h included"
else
    echo "  ✗ Missing debug_levels.h - needs manual fix"
fi

# Update Net1
echo ""
echo "[2/2] Updating virtio_net1_driver.c..."
NET1_FILE="components/VirtIO_Net1_Driver/virtio_net1_driver.c"

# Check if already updated
if ! grep -q "debug_levels.h" "$NET1_FILE"; then
    # Find first #include line
    FIRST_INCLUDE=$(grep -n "^#include" "$NET1_FILE" | head -1 | cut -d: -f1)
    
    if [ -n "$FIRST_INCLUDE" ]; then
        # Insert before first #include
        sed -i "${FIRST_INCLUDE}i\\
/* v2.207: New industry-standard 5-level debug system */\\
#define DEBUG_LEVEL DEBUG_LEVEL_INFO\\
#include \"debug_levels.h\"\\
" "$NET1_FILE"
        echo "  ✓ Added debug header to $NET1_FILE"
    else
        echo "  ✗ Could not find #include in $NET1_FILE"
    fi
else
    echo "  ✓ debug_levels.h already included"
fi

# Comment out old debug system in Net1
if grep -q "^#define DEBUG_LEVEL_SILENT" "$NET1_FILE"; then
    # Find the block and comment it out
    sed -i '/#define DEBUG_LEVEL_SILENT/,/#endif/ {
        s/^/#OLD# /
    }' "$NET1_FILE" 2>/dev/null || echo "  ℹ Old debug defines already removed"
fi

echo ""
echo "========================================================================" 
echo " Conversion Complete!"
echo "========================================================================"
echo ""

# Statistics
for file in components/VirtIO_Net0_Driver/virtio_net0_driver.c components/VirtIO_Net1_Driver/virtio_net1_driver.c; do
    if [ -f "$file" ]; then
        errors=$(grep -c 'DEBUG_ERROR' "$file" 2>/dev/null || echo "0")
        warns=$(grep -c 'DEBUG_WARN' "$file" 2>/dev/null || echo "0")
        infos=$(grep -c 'DEBUG_INFO' "$file" 2>/dev/null || echo "0")
        debugs=$(grep -c 'DEBUG(' "$file" 2>/dev/null || echo "0")
        plain=$(grep -c ' printf(' "$file" 2>/dev/null || echo "0")
        
        echo "$(basename $file):"
        echo "  DEBUG_ERROR: $errors"
        echo "  DEBUG_WARN:  $warns"
        echo "  DEBUG_INFO:  $infos"
        echo "  DEBUG():     $debugs"
        echo "  Plain printf: $plain"
        echo ""
    fi
done

echo "Ready to test! Try building with different debug levels:"
echo ""
echo "  Edit line 21 of virtio_net0_driver.c to change debug level:"
echo "    #define DEBUG_LEVEL DEBUG_LEVEL_NONE   // Silent"
echo "    #define DEBUG_LEVEL DEBUG_LEVEL_ERROR  // Errors only"
echo "    #define DEBUG_LEVEL DEBUG_LEVEL_WARN   // Errors + Warnings"
echo "    #define DEBUG_LEVEL DEBUG_LEVEL_INFO   // + Operational info (default)"
echo "    #define DEBUG_LEVEL DEBUG_LEVEL_DEBUG  // Everything"
echo ""
echo "========================================================================"

