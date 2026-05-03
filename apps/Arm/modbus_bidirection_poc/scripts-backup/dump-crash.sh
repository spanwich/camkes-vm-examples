#!/bin/bash
# Crash Dump Script - Capture console output from tmux debug session
# Usage: ./dump-crash.sh <version-tag>
# Example: ./dump-crash.sh v2.118-test
#
# This script captures output from the running tmux modbus-debug session
# and analyzes it for crashes.

set -e

# Configuration
CRASH_DUMPS_DIR="crash-dumps"
mkdir -p "$CRASH_DUMPS_DIR"

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

# Parse version tag
VERSION_TAG="${1:-unknown}"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
QEMU_CONSOLE_LOG="${CRASH_DUMPS_DIR}/${VERSION_TAG}-qemu-console-${TIMESTAMP}.log"
GDB_CONSOLE_LOG="${CRASH_DUMPS_DIR}/${VERSION_TAG}-gdb-console-${TIMESTAMP}.log"
CRASH_SUMMARY="${CRASH_DUMPS_DIR}/${VERSION_TAG}-crash-summary-${TIMESTAMP}.txt"

echo -e "${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  ICS Gateway Crash Dump - ${VERSION_TAG}${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Configuration:"
echo "  Version:         $VERSION_TAG"
echo "  QEMU console:    $QEMU_CONSOLE_LOG"
echo "  GDB console:     $GDB_CONSOLE_LOG"
echo "  Crash summary:   $CRASH_SUMMARY"
echo ""

# Check if tmux session exists
if ! tmux has-session -t modbus-debug 2>/dev/null; then
    echo -e "${RED}ERROR: No tmux session 'modbus-debug' found${NC}"
    echo ""
    echo "Start the debug session first:"
    echo "  ./start-persistent-debug.sh"
    exit 1
fi

echo -e "${BLUE}Capturing console output from tmux session...${NC}"
echo ""

# Capture QEMU console (pane 0)
echo -e "${BLUE}[1/2] Capturing QEMU console (pane 0)...${NC}"
tmux capture-pane -t modbus-debug:0.0 -p -S -32000 > "$QEMU_CONSOLE_LOG"
QEMU_LINES=$(wc -l < "$QEMU_CONSOLE_LOG")
echo "  ✓ Captured $QEMU_LINES lines from QEMU console"

# Capture GDB console (pane 1)
echo -e "${BLUE}[2/2] Capturing GDB console (pane 1)...${NC}"
tmux capture-pane -t modbus-debug:0.1 -p -S -32000 > "$GDB_CONSOLE_LOG"
GDB_LINES=$(wc -l < "$GDB_CONSOLE_LOG")
echo "  ✓ Captured $GDB_LINES lines from GDB console"

echo ""
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo "Analyzing captured output..."
echo ""

# Analyze for crashes
CRASH_DETECTED=false
CRASH_TYPE="none"

# Check QEMU console for faults
if grep -q "FAULT HANDLER\|Data abort\|Prefetch abort\|Undefined instruction" "$QEMU_CONSOLE_LOG" 2>/dev/null; then
    CRASH_DETECTED=true
    CRASH_TYPE="seL4 Fault Handler"
    echo -e "${RED}🚨 CRASH DETECTED in QEMU console${NC}"
fi

# Check GDB console for breakpoint hits
if grep -q "CAUGHT FAULT AT PC\|KERNEL: c_handle_data_fault\|Breakpoint.*hit" "$GDB_CONSOLE_LOG" 2>/dev/null; then
    CRASH_DETECTED=true
    if [ "$CRASH_TYPE" = "none" ]; then
        CRASH_TYPE="GDB Breakpoint"
    else
        CRASH_TYPE="$CRASH_TYPE + GDB Breakpoint"
    fi
    echo -e "${RED}🚨 GDB BREAKPOINT HIT${NC}"
fi

if [ "$CRASH_DETECTED" = false ]; then
    echo -e "${GREEN}✓ No crash detected${NC}"
fi

# Create crash summary
cat > "$CRASH_SUMMARY" << EOF
╔══════════════════════════════════════════════════════════╗
║  CRASH DUMP SUMMARY - $VERSION_TAG
╚══════════════════════════════════════════════════════════╝

Timestamp:   $TIMESTAMP
Version:     $VERSION_TAG
Crash Type:  $CRASH_TYPE

Files:
  QEMU Console: $QEMU_CONSOLE_LOG ($QEMU_LINES lines)
  GDB Console:  $GDB_CONSOLE_LOG ($GDB_LINES lines)
  This Summary: $CRASH_SUMMARY

EOF

if [ "$CRASH_DETECTED" = true ]; then
    cat >> "$CRASH_SUMMARY" << EOF
═══════════════════════════════════════════════════════════
CRASH INFORMATION:
═══════════════════════════════════════════════════════════

EOF

    # Extract fault information from QEMU console
    if grep -q "FAULT HANDLER\|Data abort" "$QEMU_CONSOLE_LOG"; then
        echo "seL4 Fault Handler Output:" >> "$CRASH_SUMMARY"
        grep -A 30 "FAULT HANDLER\|Data abort\|Prefetch abort" "$QEMU_CONSOLE_LOG" | head -40 >> "$CRASH_SUMMARY" || true
        echo "" >> "$CRASH_SUMMARY"
    fi

    # Extract GDB breakpoint information
    if grep -q "CAUGHT FAULT AT PC\|KERNEL: c_handle_data_fault" "$GDB_CONSOLE_LOG"; then
        echo "GDB Fault Detection:" >> "$CRASH_SUMMARY"
        grep -B 5 -A 40 "CAUGHT FAULT AT PC\|KERNEL: c_handle_data_fault" "$GDB_CONSOLE_LOG" | head -50 >> "$CRASH_SUMMARY" || true
        echo "" >> "$CRASH_SUMMARY"
    fi

    # Extract PC and fault addresses
    PC_ADDR=$(grep -oP "PC:\s+0x[0-9a-f]+" "$QEMU_CONSOLE_LOG" "$GDB_CONSOLE_LOG" 2>/dev/null | head -1 | awk '{print $2}' || echo "Not found")
    FAULT_ADDR=$(grep -oP "(FAR_EL2|DFAR):\s+0x[0-9a-f]+" "$QEMU_CONSOLE_LOG" "$GDB_CONSOLE_LOG" 2>/dev/null | head -1 | awk '{print $2}' || echo "Not found")

    cat >> "$CRASH_SUMMARY" << EOF
═══════════════════════════════════════════════════════════
QUICK ANALYSIS:
═══════════════════════════════════════════════════════════

Program Counter (PC): $PC_ADDR
Fault Address:        $FAULT_ADDR

To analyze with addr2line:
  arm-none-eabi-addr2line -e net0_drv.instance.bin -f $PC_ADDR
  arm-none-eabi-addr2line -e net1_drv.instance.bin -f $PC_ADDR

To disassemble around crash:
  arm-none-eabi-objdump -d net0_drv.instance.bin | grep -A 20 "${PC_ADDR#0x}"
  arm-none-eabi-objdump -d net1_drv.instance.bin | grep -A 20 "${PC_ADDR#0x}"

EOF
fi

# Last 100 lines from QEMU console
cat >> "$CRASH_SUMMARY" << EOF
═══════════════════════════════════════════════════════════
LAST 100 LINES OF QEMU CONSOLE:
═══════════════════════════════════════════════════════════

EOF
tail -100 "$QEMU_CONSOLE_LOG" >> "$CRASH_SUMMARY"

# Last 50 lines from GDB console
cat >> "$CRASH_SUMMARY" << EOF

═══════════════════════════════════════════════════════════
LAST 50 LINES OF GDB CONSOLE:
═══════════════════════════════════════════════════════════

EOF
tail -50 "$GDB_CONSOLE_LOG" >> "$CRASH_SUMMARY"

cat >> "$CRASH_SUMMARY" << EOF

╚══════════════════════════════════════════════════════════╝
EOF

# Display summary
echo ""
echo -e "${BLUE}Summary:${NC}"
if [ "$CRASH_DETECTED" = true ]; then
    echo -e "  Status:        ${RED}CRASHED${NC}"
    echo -e "  Type:          $CRASH_TYPE"
    echo -e "  PC:            $PC_ADDR"
    echo -e "  Fault Addr:    $FAULT_ADDR"
else
    echo -e "  Status:        ${GREEN}NO CRASH${NC}"
fi
echo "  QEMU console:  $QEMU_CONSOLE_LOG ($QEMU_LINES lines)"
echo "  GDB console:   $GDB_CONSOLE_LOG ($GDB_LINES lines)"
echo "  Summary:       $CRASH_SUMMARY"
echo ""

# Show crash summary if crash occurred
if [ "$CRASH_DETECTED" = true ]; then
    echo -e "${YELLOW}Crash summary (first 100 lines):${NC}"
    head -100 "$CRASH_SUMMARY"
fi

echo ""
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
