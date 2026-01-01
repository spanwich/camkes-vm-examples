#!/bin/bash
#
# convert_to_industry_standard_debug.sh
#
# Converts all C files in modbus_bidirection_poc to industry-standard 5-level debug system
# Levels: NONE (0), ERROR (1), WARN (2), INFO (3), DEBUG (4)
#
# Usage: ./convert_to_industry_standard_debug.sh [DEBUG_LEVEL_INFO|DEBUG_LEVEL_DEBUG|DEBUG_LEVEL_ERROR]
#

set -e

# Default debug level
DEFAULT_LEVEL="${1:-DEBUG_LEVEL_INFO}"

echo "========================================================================"
echo " Converting to Industry Standard 5-Level Debug System"
echo " Default level: $DEFAULT_LEVEL"
echo " Levels: NONE(0) < ERROR(1) < WARN(2) < INFO(3) < DEBUG(4)"
echo "========================================================================"
echo ""

# Component directories
NET0_FILE="components/VirtIO_Net0_Driver/virtio_net0_driver.c"
NET1_FILE="components/VirtIO_Net1_Driver/virtio_net1_driver.c"
ICS_IN_FILE="components/ICS_Inbound/ICS_Inbound.c"
ICS_OUT_FILE="components/ICS_Outbound/ICS_Outbound.c"

# Backup files
echo "[Step 1/5] Creating backups..."
for file in "$NET0_FILE" "$NET1_FILE" "$ICS_IN_FILE" "$ICS_OUT_FILE"; do
    if [ -f "$file" ]; then
        if [ ! -f "${file}.pre-v2.207" ]; then
            cp "$file" "${file}.pre-v2.207"
            echo "  ✓ Backed up: $file"
        else
            echo "  ⚠ Backup exists: ${file}.pre-v2.207"
        fi
    fi
done

echo ""
echo "[Step 2/5] Converting printf → DEBUG_ERROR (Level 1: Critical failures)"...
echo

for file in "$NET0_FILE" "$NET1_FILE" "$ICS_IN_FILE" "$ICS_OUT_FILE"; do
    if [ ! -f "$file" ]; then continue; fi

    before=$(grep -c 'printf.*\[ERR' "$file" 2>/dev/null || echo "0")

    # Convert [ERR] messages to DEBUG_ERROR
    sed -i 's/printf("\([^"]*\)\[ERR\]/DEBUG_ERROR("\1[ERR]/g' "$file"
    sed -i "s/printf('%s: \[ERR/DEBUG_ERROR('%s: [ERR/g" "$file"

    after=$(grep -c 'DEBUG_ERROR.*\[ERR' "$file" 2>/dev/null || echo "0")
    echo "  $file: $after ERROR messages converted"
done

echo ""
echo "[Step 3/5] Converting printf → DEBUG_WARN (Level 2: Warnings)..."
echo ""

for file in "$NET0_FILE" "$NET1_FILE" "$ICS_IN_FILE" "$ICS_OUT_FILE"; do
    if [ ! -f "$file" ]; then continue; fi

    before=$(grep -c 'printf.*\[WARN' "$file" 2>/dev/null || echo "0")

    # Convert [WARN] messages to DEBUG_WARN
    sed -i 's/printf("\([^"]*\)\[WARN\]/DEBUG_WARN("\1[WARN]/g' "$file"
    sed -i "s/printf('%s: \[WARN/DEBUG_WARN('%s: [WARN/g" "$file"

    after=$(grep -c 'DEBUG_WARN.*\[WARN' "$file" 2>/dev/null || echo "0")
    echo "  $file: $after WARN messages converted"
done

echo ""
echo "[Step 4/5] Converting printf → DEBUG_INFO (Level 3: Operational info)..."
echo ""

for file in "$NET0_FILE" "$NET1_FILE" "$ICS_IN_FILE" "$ICS_OUT_FILE"; do
    if [ ! -f "$file" ]; then continue; fi

    converted=0

    # Statistics messages
    sed -i 's/printf("\([^"]*\)\[PBUF-STATS\]/DEBUG_INFO("\1[PBUF-STATS]/g' "$file" && ((converted++)) || true
    sed -i 's/printf("\([^"]*\)\[PBUF-TYPE\]/DEBUG_INFO("\1[PBUF-TYPE]/g' "$file" && ((converted++)) || true
    sed -i 's/printf("\([^"]*\)\[PBUF-LEAK\]/DEBUG_INFO("\1[PBUF-LEAK]/g' "$file" && ((converted++)) || true
    sed -i 's/printf("\([^"]*\)\[OOSEQ\]/DEBUG_INFO("\1[OOSEQ]/g' "$file" && ((converted++)) || true
    sed -i 's/printf("\([^"]*\)\[PCB-STATE\]/DEBUG_INFO("\1[PCB-STATE]/g' "$file" && ((converted++)) || true
    sed -i 's/printf("\([^"]*\)\[CONN-MATCH\]/DEBUG_INFO("\1[CONN-MATCH]/g' "$file" && ((converted++)) || true
    sed -i 's/printf("\([^"]*\)\[MEM-DIAG\]/DEBUG_INFO("\1[MEM-DIAG]/g' "$file" && ((converted++)) || true
    sed -i 's/printf("\([^"]*\)\[ORPHAN-/DEBUG_INFO("\1[ORPHAN-/g' "$file" && ((converted++)) || true

    # Connection lifecycle
    sed -i 's/printf("\([^"]*\)\[OK\]/DEBUG_INFO("\1[OK]/g' "$file" && ((converted++)) || true

    # Initialization messages
    sed -i 's/printf("%s: Initializing/DEBUG_INFO("%s: Initializing/g' "$file" && ((converted++)) || true
    sed -i 's/printf("%s: Starting/DEBUG_INFO("%s: Starting/g' "$file" && ((converted++)) || true
    sed -i 's/printf("%s: Listening/DEBUG_INFO("%s: Listening/g' "$file" && ((converted++)) || true
    sed -i 's/printf("%s: TCP server/DEBUG_INFO("%s: TCP server/g' "$file" && ((converted++)) || true

    info_count=$(grep -c 'DEBUG_INFO' "$file" 2>/dev/null || echo "0")
    echo "  $file: ~$info_count INFO messages"
done

echo ""
echo "[Step 5/5] Updating #if DEBUG_PACKET_DETAIL → #if DEBUG_ENABLED_DEBUG..."
echo ""

for file in "$NET0_FILE" "$NET1_FILE" "$ICS_IN_FILE" "$ICS_OUT_FILE"; do
    if [ ! -f "$file" ]; then continue; fi

    before=$(grep -c '#if DEBUG_PACKET_DETAIL' "$file" 2>/dev/null || echo "0")

    # Convert DEBUG_PACKET_DETAIL blocks
    sed -i 's/#if DEBUG_PACKET_DETAIL/#if DEBUG_ENABLED_DEBUG/g' "$file"
    sed -i 's/#endif \/\* DEBUG_PACKET_DETAIL \*\//#endif \/\* DEBUG_ENABLED_DEBUG \*\//g' "$file"

    after=$(grep -c '#if DEBUG_ENABLED_DEBUG' "$file" 2>/dev/null || echo "0")
    echo "  $file: $after DEBUG blocks converted"
done

echo ""
echo "========================================================================"
echo " Conversion Summary"
echo "========================================================================"
echo ""

total_error=0
total_warn=0
total_info=0
total_plain=0

for file in "$NET0_FILE" "$NET1_FILE" "$ICS_IN_FILE" "$ICS_OUT_FILE"; do
    if [ ! -f "$file" ]; then continue; fi

    error_count=$(grep -c 'DEBUG_ERROR' "$file" 2>/dev/null || echo "0")
    warn_count=$(grep -c 'DEBUG_WARN' "$file" 2>/dev/null || echo "0")
    info_count=$(grep -c 'DEBUG_INFO' "$file" 2>/dev/null || echo "0")
    plain_count=$(grep -c 'printf(' "$file" 2>/dev/null || echo "0")

    total_error=$((total_error + error_count))
    total_warn=$((total_warn + warn_count))
    total_info=$((total_info + info_count))
    total_plain=$((total_plain + plain_count))

    echo "$(basename $file):"
    echo "  DEBUG_ERROR: $error_count"
    echo "  DEBUG_WARN:  $warn_count"
    echo "  DEBUG_INFO:  $info_count"
    echo "  plain printf: $plain_count (needs manual review)"
    echo ""
done

echo "TOTALS:"
echo "  DEBUG_ERROR: $total_error (Level 1: Critical failures)"
echo "  DEBUG_WARN:  $total_warn (Level 2: Warnings)"
echo "  DEBUG_INFO:  $total_info (Level 3: Operational info)"
echo "  plain printf: $total_plain (Level 4: DEBUG - needs manual conversion)"
echo ""

echo "========================================================================"
echo " Next Steps - MANUAL CONVERSION REQUIRED"
echo "========================================================================"
echo ""
echo "The remaining ~$total_plain plain printf statements need manual review:"
echo ""
echo "1. Open each C file in your editor"
echo "2. Search for 'printf(' (not DEBUG_ERROR/WARN/INFO)"
echo "3. For each printf, decide:"
echo "   - Is it inside #if DEBUG_ENABLED_DEBUG? → Keep as printf or use DEBUG()"
echo "   - Is it packet detail/hex dump? → Change to DEBUG()"
echo "   - Is it operational info? → Change to DEBUG_INFO()"
echo "   - Is it a warning you missed? → Change to DEBUG_WARN()"
echo ""
echo "Common patterns:"
echo "  - printf(\"  Ethernet: ...\")  → DEBUG(\"  Ethernet: ...\")"
echo "  - printf(\"  IP: ...\")         → DEBUG(\"  IP: ...\")"
echo "  - printf(\"╔═══...\")          → DEBUG(\"╔═══...\")"
echo "  - printf(\"\\n\")               → DEBUG(\"\\n\") or DEBUG_INFO(\"\\n\")"
echo ""
echo "4. Add debug header to each file:"
echo "   At top of file, before first #include:"
echo "   #define DEBUG_LEVEL $DEFAULT_LEVEL"
echo "   #include \"debug_levels.h\""
echo ""
echo "5. Remove old DEBUG_LEVEL_SILENT/QUIET/NORMAL/VERBOSE defines"
echo ""
echo "6. Test build at each level:"
echo "   - DEBUG_LEVEL_NONE  (should produce no output)"
echo "   - DEBUG_LEVEL_ERROR (only [ERR] messages)"
echo "   - DEBUG_LEVEL_WARN  ([ERR] + [WARN])"
echo "   - DEBUG_LEVEL_INFO  (+ stats + connections)"
echo "   - DEBUG_LEVEL_DEBUG (everything)"
echo ""
echo "📁 Backups saved with .pre-v2.207 extension"
echo ""
echo "========================================================================"
