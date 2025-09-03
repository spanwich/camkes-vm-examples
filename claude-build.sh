#!/bin/bash
#
# Claude Code compatible build script for CAmkES VM examples
# Implements working solution discovered through systematic debugging
#

# Function to show usage
usage() {
    echo "Usage: $0 [cmake options]"
    echo "Example: $0 -DCAMKES_VM_APP=vm_minimal -DAARCH64=1 -DPLATFORM=qemu-arm-virt -DSIMULATION=1 -DLibUSB=OFF"
    exit 1
}

if [ $# -eq 0 ]; then
    usage
fi

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Claude Code CAmkES Build Pipeline ==="
echo "Setting up Python environment..."
export PYTHONPATH="$SCRIPT_DIR/projects/camkes-tool:$SCRIPT_DIR/projects/capdl/python-capdl-tool"

echo "Running initial CMake to generate build dependencies..."
set +e  # Disable immediate exit on error for cmake
cmake -G Ninja "$@" -DSEL4_CACHE_DIR="$SCRIPT_DIR/.sel4_cache" \
    -C "$SCRIPT_DIR/projects/vm-examples/settings.cmake" \
    "$SCRIPT_DIR/projects/vm-examples"
CMAKE_RESULT=$?
set -e  # Re-enable immediate exit on error

# Check if cmake failed due to missing AST files
if [ $CMAKE_RESULT -ne 0 ] && [ ! -f "ast.pickle.d" ]; then
    echo "⚠️  CMake failed due to missing AST files. Running manual AST generation..."
    
    # Ensure kernel.dtb exists
    if [ ! -f "kernel/kernel.dtb" ]; then
        echo "❌ kernel.dtb not found. CMake must create it first."
        exit 1
    fi
    
    echo "Generating AST files manually with comprehensive include paths..."
    
    # Extract CAMKES_VM_APP from arguments to determine correct paths
    VM_APP="vm_minimal"  # default
    for arg in "$@"; do
        if [[ $arg == -DCAMKES_VM_APP=* ]]; then
            VM_APP="${arg#-DCAMKES_VM_APP=}"
            break
        fi
    done
    
    echo "Detected VM application: $VM_APP"
    
    env PYTHONPATH="$PYTHONPATH" python3 -m camkes.parser \
        --import-path="$SCRIPT_DIR/projects/camkes-tool/include/builtin" \
        --dtb="$BUILD_DIR/kernel/kernel.dtb" \
        --cpp --cpp-bin /usr/bin/cpp \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/remote-drivers/picotcp-ethernet-async/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/remote-drivers/picotcp-socket-sync/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/modules/fdt-bind-driver/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/modules/dynamic-untyped-allocators/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/modules/single-threaded/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/modules/x86-iospace-dma/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/modules/picotcp-base/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/ClockServer/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/GPIOMUXServer/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/ResetServer/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/SerialServer/include/plat/arm_common" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/SerialServer/camkes-putchar-client/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/TimeServer/include/plat/qemu-arm-virt" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/BenchUtiliz/camkes-include" \
        --cpp-flag=-I"$SCRIPT_DIR/projects/global-components/components/Ethdriver/include/plat/qemu-arm-virt" \
        --cpp-flag=-DKERNELARMPLATFORM_QEMU-ARM-VIRT \
        --cpp-flag=-DVMEMMC2NODMA=0 \
        --cpp-flag=-I"$SCRIPT_DIR/projects/vm/components/VM_Arm" \
        --cpp-flag=-DCAMKES_TOOL_PROCESSING \
        --import-path="$SCRIPT_DIR/projects/camkes-tool/components" \
        --import-path="$SCRIPT_DIR/projects/camkes-tool/components/arch/arm" \
        --import-path="$SCRIPT_DIR/projects/global-components/interfaces" \
        --import-path="$BUILD_DIR/plat_interfaces/qemu-arm-virt" \
        --import-path="$SCRIPT_DIR/projects/global-components/components" \
        --import-path="$BUILD_DIR/plat_components/qemu-arm-virt" \
        --import-path="$SCRIPT_DIR/projects/vm/components" \
        --import-path="$BUILD_DIR/camkes-arm-vm/components" \
        --import-path="$SCRIPT_DIR/projects/vm/interfaces" \
        --import-path="$BUILD_DIR/camkes-arm-vm/interfaces" \
        --import-path="$SCRIPT_DIR/projects/vm-examples/apps/Arm/$VM_APP/qemu-arm-virt" \
        --makefile-dependencies "$BUILD_DIR/ast.pickle.d" \
        --file "$SCRIPT_DIR/projects/vm-examples/apps/Arm/$VM_APP/vm_minimal.camkes" \
        --save-ast "$BUILD_DIR/ast.pickle"
    
    if [ $? -eq 0 ]; then
        echo "✅ AST files generated successfully. Retrying CMake..."
        cmake -G Ninja "$@" -DSEL4_CACHE_DIR="$SCRIPT_DIR/.sel4_cache" \
            -C "$SCRIPT_DIR/projects/vm-examples/settings.cmake" \
            "$SCRIPT_DIR/projects/vm-examples"
    else
        echo "❌ Manual AST generation failed."
        exit 1
    fi
fi

echo "✅ CMake configuration completed successfully!"

echo "Running ninja build..."
ninja

echo "🎉 Build completed successfully!"
echo ""
echo "Build artifacts ready:"
echo "  - QEMU simulation: ./simulate"
echo "  - Images directory: ./images/"
echo "  - Kernel image: Available for deployment"