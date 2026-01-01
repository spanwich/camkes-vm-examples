#!/bin/bash
# Check Debug Session Status
#
# This script allows you to check the status of a running debug session
# without attaching to it. Useful for quick checks over SSH.

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}Debug Session Status Check${NC}"
echo ""

# Check if tmux is installed
if ! command -v tmux &> /dev/null; then
    echo -e "${RED}ERROR: tmux is not installed${NC}"
    exit 1
fi

# Check if session exists
if ! tmux has-session -t modbus-debug 2>/dev/null; then
    echo -e "${RED}No debug session found (modbus-debug)${NC}"
    echo ""
    echo "Start a new session with: ./start-persistent-debug.sh"
    exit 1
fi

echo -e "${GREEN}✅ Debug session is running${NC}"
echo ""

# Get session info
echo -e "${BLUE}Session Information:${NC}"
tmux list-sessions | grep modbus-debug || true
echo ""

# Get QEMU output (top pane)
echo -e "${BLUE}QEMU Console Output (last 20 lines):${NC}"
echo "-----------------------------------"
tmux capture-pane -t modbus-debug:0.0 -p | tail -20
echo "-----------------------------------"
echo ""

# Get crash monitor status (bottom-left pane)
echo -e "${BLUE}Crash Monitor Status:${NC}"
echo "-----------------------------------"
tmux capture-pane -t modbus-debug:0.1 -p | tail -10
echo "-----------------------------------"
echo ""

# Check for recent logs
echo -e "${BLUE}Recent Log Files:${NC}"
if [ -d logs ]; then
    ls -lht logs/ | head -5
else
    echo "No logs directory found"
fi
echo ""

# Look for crash indicators in QEMU output
echo -e "${BLUE}Crash Detection:${NC}"
QEMU_OUTPUT=$(tmux capture-pane -t modbus-debug:0.0 -p)

if echo "$QEMU_OUTPUT" | grep -q "vm_fault_type\|seL4_Fault\|ASSERT"; then
    echo -e "${RED}⚠️  CRASH DETECTED in QEMU output!${NC}"
    echo ""
    echo "Crash context:"
    echo "$QEMU_OUTPUT" | grep -A 5 -B 5 "vm_fault_type\|seL4_Fault\|ASSERT" | tail -20
else
    echo -e "${GREEN}✅ No crash detected${NC}"
fi
echo ""

# Check uptime
echo -e "${BLUE}System Uptime:${NC}"
uptime
echo ""

echo -e "${BLUE}To interact with the session:${NC}"
echo "  Attach: ${YELLOW}tmux attach -t modbus-debug${NC}"
echo "  View console log: ${YELLOW}tail -f logs/console-*.log${NC}"
echo "  View crash analysis: ${YELLOW}cat logs/crash-analysis-*.txt${NC}"
echo ""
