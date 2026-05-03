#!/bin/bash
# Analyze crash log for breadcrumbs and fault information

if [ -z "$1" ]; then
    echo "Usage: $0 <log-file>"
    echo ""
    echo "Or to analyze latest log:"
    echo "  $0 latest"
    exit 1
fi

# Find log file
if [ "$1" = "latest" ]; then
    LOG_FILE=$(ls -t logs/console-*.log 2>/dev/null | head -1)
    if [ -z "$LOG_FILE" ]; then
        echo "ERROR: No log files found in logs/"
        exit 1
    fi
    echo "Analyzing latest log: $LOG_FILE"
    echo ""
else
    LOG_FILE="$1"
fi

if [ ! -f "$LOG_FILE" ]; then
    echo "ERROR: Log file not found: $LOG_FILE"
    exit 1
fi

echo "==================================================================="
echo "Crash Log Analysis: $(basename $LOG_FILE)"
echo "==================================================================="
echo ""

# 1. Extract breadcrumb sequence
echo "📍 BREADCRUMB SEQUENCE:"
echo "-------------------------------------------------------------------"
BREADCRUMBS=$(grep -o "B[0-9]\+" "$LOG_FILE" 2>/dev/null)
if [ -n "$BREADCRUMBS" ]; then
    echo "$BREADCRUMBS" | tail -30
    LAST_BREADCRUMB=$(echo "$BREADCRUMBS" | tail -1)
    echo ""
    echo "Last breadcrumb before crash/exit: $LAST_BREADCRUMB"
else
    echo "No breadcrumbs found in log"
fi
echo ""

# 2. Check for VM faults
echo "🔍 VM FAULT DETECTION:"
echo "-------------------------------------------------------------------"
if grep -q "vm exit" "$LOG_FILE" 2>/dev/null; then
    echo "⚠️  VM FAULT DETECTED"
    echo ""
    grep -B 2 -A 10 "vm exit" "$LOG_FILE" | tail -30
else
    echo "No VM faults detected"
fi
echo ""

# 3. Check for seL4 faults
echo "🔍 seL4 FAULT DETECTION:"
echo "-------------------------------------------------------------------"
if grep -qE "(Prefetch|Data) fault" "$LOG_FILE" 2>/dev/null; then
    echo "⚠️  seL4 FAULT DETECTED"
    echo ""
    grep -B 2 -A 5 -E "(Prefetch|Data) fault" "$LOG_FILE" | tail -30
else
    echo "No seL4 faults detected"
fi
echo ""

# 4. Show version info
echo "📋 VERSION INFO:"
echo "-------------------------------------------------------------------"
grep "SOFTWARE VERSION" "$LOG_FILE" 2>/dev/null || echo "No version info found"
echo ""

# 5. Component status
echo "🔧 COMPONENT STATUS:"
echo "-------------------------------------------------------------------"
grep "Component started" "$LOG_FILE" 2>/dev/null || echo "No component startup info"
echo ""

# 6. Last 20 lines
echo "📄 LAST 20 LINES OF LOG:"
echo "-------------------------------------------------------------------"
tail -20 "$LOG_FILE"
echo ""

echo "==================================================================="
echo "Full log: $LOG_FILE"
echo "==================================================================="
