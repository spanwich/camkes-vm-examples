# GDB Script for Automated Crash Monitoring
# Usage: arm-none-eabi-gdb -x crash-gdb-monitor.gdb
#
# This script:
# 1. Connects to QEMU GDB server
# 2. Loads symbol files for Net0 and Net1
# 3. Sets breakpoints at known crash locations
# 4. Auto-dumps state when crash occurs
# 5. Saves full backtrace and memory dumps

# Disable pagination for automated logging
set pagination off
set confirm off

# Enable logging
set logging file /home/iamfo470/phd/logs/modbus-gateway/gdb-crash-$(date +%s).log
set logging overwrite on
set logging on

# Print timestamp
shell echo "GDB session started at $(date)"

# Connect to QEMU GDB server
echo \nConnecting to QEMU GDB server on localhost:1234...\n
target remote localhost:1234

# Load symbol files
echo \nLoading symbol files...\n
symbol-file /home/iamfo470/phd/camkes-vm-examples/build_modbus/CMakeFiles/net0_drv.instance.bin.dir/net0_drv.instance.bin

# Add additional symbol files for other components
add-symbol-file /home/iamfo470/phd/camkes-vm-examples/build_modbus/CMakeFiles/net1_drv.instance.bin.dir/net1_drv.instance.bin

echo Symbol files loaded\n

# Set breakpoints at known crash locations
echo \nSetting breakpoints at known crash locations...\n

# Known crash PCs from logs
break *0x38308
break *0x38300
break *0x383a4
break *0x382b8

# Breakpoint on any data fault (if we can catch it)
# This may not work depending on seL4 fault handling

echo Breakpoints set\n

# Define commands to execute when breakpoint hits
commands 1-4
    echo \n
    echo ╔══════════════════════════════════════════════════════════╗\n
    echo ║  CRASH DETECTED IN GDB                                   ║\n
    echo ╚══════════════════════════════════════════════════════════╝\n
    echo \n

    echo ========== CRASH TIMESTAMP ==========\n
    shell date
    echo \n

    echo ========== REGISTERS ==========\n
    info registers
    echo \n

    echo ========== PROGRAM COUNTER CONTEXT ==========\n
    x/32i $pc-32
    echo \n

    echo ========== FAULTING ADDRESS CONTEXT ==========\n
    # Try to read memory at r0 (often holds faulting address)
    echo Memory at R0:\n
    x/32xw $r0
    echo \n

    echo ========== STACK DUMP ==========\n
    x/64xw $sp
    echo \n

    echo ========== BACKTRACE ==========\n
    backtrace 30
    echo \n

    echo ========== THREAD INFO ==========\n
    info threads
    thread apply all backtrace
    echo \n

    echo ========== LOCAL VARIABLES ==========\n
    info locals
    echo \n

    echo ========== ARGUMENTS ==========\n
    info args
    echo \n

    echo ========== FRAME INFO ==========\n
    info frame
    echo \n

    echo ========== DISASSEMBLY AROUND PC ==========\n
    disassemble $pc-64,$pc+64
    echo \n

    echo ╔══════════════════════════════════════════════════════════╗\n
    echo ║  CRASH DUMP COMPLETE - Detaching GDB                     ║\n
    echo ╚══════════════════════════════════════════════════════════╝\n

    # Save core dump if supported
    # generate-core-file /tmp/crash-core.dump

    # Quit GDB (crash analysis complete)
    quit
end

echo \n
echo ╔══════════════════════════════════════════════════════════╗\n
echo ║  GDB Crash Monitor Active                                ║\n
echo ║  Waiting for crash...                                    ║\n
echo ║  This may take hours/days - monitor is passive           ║\n
echo ╚══════════════════════════════════════════════════════════╝\n
echo \n

# Continue execution - will break on crash
continue
