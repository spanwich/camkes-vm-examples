#!/bin/bash
# Convert all remaining plain printf() to DEBUG()
# Run this AFTER convert_to_industry_standard_debug.sh

set -e

echo "Converting remaining printf() → DEBUG()..."
echo ""

NET0_FILE="components/VirtIO_Net0_Driver/virtio_net0_driver.c"
NET1_FILE="components/VirtIO_Net1_Driver/virtio_net1_driver.c"
ICS_IN_FILE="components/ICS_Inbound/ICS_Inbound.c"
ICS_OUT_FILE="components/ICS_Outbound/ICS_Outbound.c"

for file in "$NET0_FILE" "$NET1_FILE" "$ICS_IN_FILE" "$ICS_OUT_FILE"; do
    if [ ! -f "$file" ]; then continue; fi
    
    before=$(grep -c 'printf(' "$file" 2>/dev/null || echo "0")
    
    # Convert all remaining printf to DEBUG
    # This will NOT match DEBUG_ERROR, DEBUG_WARN, DEBUG_INFO (they don't have parenthesis after printf)
    sed -i 's/\bprintf(/DEBUG(/g' "$file"
    
    after=$(grep -c 'printf(' "$file" 2>/dev/null || echo "0")
    converted=$((before - after))
    
    echo "  $file: $converted printf → DEBUG"
done

echo ""
echo "Done! All remaining printf() converted to DEBUG()"
