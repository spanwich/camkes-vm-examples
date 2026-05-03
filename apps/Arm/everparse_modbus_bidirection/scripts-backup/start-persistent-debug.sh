#!/bin/bash
# Persistent Debug Session - GDB + Console Monitoring
#
# This script sets up a persistent debugging environment that survives SSH disconnection
# and can run for extended periods (days/weeks) to catch VM crashes.
#
# Combines GDB debugging with live console log monitoring.
#
# Usage:
#   ./start-persistent-debug.sh
#
# After starting, you can safely disconnect SSH and reconnect later to check status.
# To reconnect: tmux attach -t modbus-debug

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}Persistent Debug Session - GDB + Console Monitoring${NC}"
echo ""

# Check if tmux is installed
if ! command -v tmux &> /dev/null; then
    echo -e "${RED}ERROR: tmux is not installed${NC}"
    echo "Install with: sudo apt-get install -y tmux"
    exit 1
fi

# Check TAP interfaces
if ! ip link show tap0 &>/dev/null || ! ip link show tap1 &>/dev/null; then
    echo -e "${RED}ERROR: TAP interfaces not found${NC}"
    echo "Run: sudo ../projects/vm-examples/apps/Arm/modbus_bidirection_poc/scripts/setup-policy-routing-gateway.sh"
    exit 1
fi

# Create logs directory
mkdir -p logs

# Generate timestamp for this debug session
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
LOGFILE="logs/console-${TIMESTAMP}.log"
GDBLOG="logs/gdb-${TIMESTAMP}.log"

# Kernel image path
KERNEL_IMAGE="images/capdl-loader-image-arm-qemu-arm-virt"

if [ ! -f "$KERNEL_IMAGE" ]; then
    echo -e "${RED}ERROR: Kernel image not found: $KERNEL_IMAGE${NC}"
    exit 1
fi

echo -e "${BLUE}Session Configuration:${NC}"
echo "  Session name: modbus-debug"
echo "  Console log: ${LOGFILE}"
echo "  GDB log: ${GDBLOG}"
echo "  Start time: $(date)"
echo ""

# Check if session already exists
if tmux has-session -t modbus-debug 2>/dev/null; then
    echo -e "${YELLOW}WARNING: tmux session 'modbus-debug' already exists${NC}"
    echo ""
    echo "Options:"
    echo "  1) Attach to existing session: tmux attach -t modbus-debug"
    echo "  2) Kill and restart: tmux kill-session -t modbus-debug && $0"
    exit 1
fi

echo -e "${GREEN}Creating persistent tmux session...${NC}"

# Start tmux session (detached) with 3 panes
tmux new-session -d -s modbus-debug -n "VM-Debug"

# Split window into 3 panes
# Top-left: QEMU with GDB server
# Top-right: GDB client
# Bottom: Live console log tail
tmux split-window -h -t modbus-debug:0
tmux split-window -v -t modbus-debug:0

# Pane 0 (top-left): QEMU with GDB server + console logging
tmux send-keys -t modbus-debug:0.0 "cd /home/qemu/phd/camkes-vm-examples/build_modbus" C-m
tmux send-keys -t modbus-debug:0.0 "./run-remote-gdb.sh 2>&1 | tee ${LOGFILE}" C-m

# Give QEMU a moment to start
sleep 2

# Pane 1 (top-right): GDB client with COMPREHENSIVE fault catching
tmux send-keys -t modbus-debug:0.1 "cd /home/qemu/phd/camkes-vm-examples/build_modbus" C-m
tmux send-keys -t modbus-debug:0.1 "echo '╔════════════════════════════════════════════════╗'" C-m
tmux send-keys -t modbus-debug:0.1 "echo '║  GDB CATCH-ALL FAULT DEBUGGER                  ║'" C-m
tmux send-keys -t modbus-debug:0.1 "echo '╚════════════════════════════════════════════════╝'" C-m
tmux send-keys -t modbus-debug:0.1 "echo 'GDB Log: ${GDBLOG}'" C-m
tmux send-keys -t modbus-debug:0.1 "echo ''  " C-m
tmux send-keys -t modbus-debug:0.1 "echo 'Fault catching enabled at 3 levels:'" C-m
tmux send-keys -t modbus-debug:0.1 "echo '  ✓ Kernel data abort handler (catches ANY fault)'" C-m
tmux send-keys -t modbus-debug:0.1 "echo '  ✓ Application PC 0x39868 (tcp_output)'" C-m
tmux send-keys -t modbus-debug:0.1 "echo '  ✓ NULL region watchpoints (0x0, 0x10, 0x20)'" C-m
tmux send-keys -t modbus-debug:0.1 "echo ''  " C-m
tmux send-keys -t modbus-debug:0.1 "echo 'Type \"continue\" or \"c\" to start execution'" C-m
tmux send-keys -t modbus-debug:0.1 "echo 'GDB will stop BEFORE any crash occurs'" C-m
tmux send-keys -t modbus-debug:0.1 "sleep 3" C-m
tmux send-keys -t modbus-debug:0.1 "gdb-multiarch -ex 'add-symbol-file kernel/kernel.elf' -ex 'set logging file ${GDBLOG}' -x gdb-catch-all-faults.txt ${KERNEL_IMAGE}" C-m

# Pane 2 (bottom): Live console log tail
tmux send-keys -t modbus-debug:0.2 "cd /home/qemu/phd/camkes-vm-examples/build_modbus" C-m
tmux send-keys -t modbus-debug:0.2 "echo 'Waiting for console output...'" C-m
tmux send-keys -t modbus-debug:0.2 "sleep 5" C-m
tmux send-keys -t modbus-debug:0.2 "tail -f ${LOGFILE}" C-m

# Set up tmux status bar
tmux set-option -t modbus-debug status-style "bg=green,fg=black"
tmux set-option -t modbus-debug status-left "[GDB Debug] "
tmux set-option -t modbus-debug status-right "Started: $(date +%H:%M) | Ctrl-b d to detach"

echo ""
echo -e "${GREEN}✅ Persistent debug session started successfully!${NC}"
echo ""
echo -e "${BLUE}Session Layout:${NC}"
echo "  ┌──────────────────┬──────────────────┐"
echo "  │ QEMU + GDB       │ GDB Client       │"
echo "  │ Server           │ (attached)       │"
echo "  ├──────────────────┴──────────────────┤"
echo "  │ Live Console Log (tail -f)          │"
echo "  └─────────────────────────────────────┘"
echo ""
echo -e "${BLUE}Session Management:${NC}"
echo "  Attach to session:   ${YELLOW}tmux attach -t modbus-debug${NC}"
echo "  Detach from session: ${YELLOW}Ctrl-b d${NC}"
echo "  Kill session:        ${YELLOW}tmux kill-session -t modbus-debug${NC}"
echo ""
echo -e "${BLUE}Navigating Panes:${NC}"
echo "  Switch panes:        ${YELLOW}Ctrl-b <arrow keys>${NC}"
echo "  Top-left pane:       QEMU output"
echo "  Top-right pane:      GDB (use 'bt', 'info registers', etc.)"
echo "  Bottom pane:         Live console log"
echo ""
echo -e "${BLUE}Viewing Logs:${NC}"
echo "  Console output: ${YELLOW}cat ${LOGFILE}${NC}"
echo "  GDB log:        ${YELLOW}cat ${GDBLOG}${NC}"
echo "  All logs:       ${YELLOW}ls -lh logs/${NC}"
echo ""
echo -e "${BLUE}Checking Status (while detached):${NC}"
echo "  ${YELLOW}./check-debug-status.sh${NC}"
echo ""
echo -e "${GREEN}You can now safely disconnect SSH. The debug session will continue running.${NC}"
echo -e "${GREEN}GDB is attached and will catch any faults. Console is being logged.${NC}"
echo ""
echo -e "${YELLOW}Attaching to session now...${NC}"
echo -e "${YELLOW}(Press Ctrl-b d to detach and return to shell)${NC}"
sleep 2

# Attach to the session
tmux attach -t modbus-debug
