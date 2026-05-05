#
# CMakeLists.txt for test_two_components - Minimal two-component test
#

cmake_minimum_required(VERSION 3.8.2)

project(test_two_components C)

# Include CAmkES ARM VM helpers
include(${CAMKES_ARM_VM_HELPERS_PATH})

# Multikernel-AMP per-kernel ID (architecture §1.2). Set on the cmake line:
#   build-ttc-k0:  cmake ... -DMULTIKERNEL_KID=0
#   build-ttc-k1:  cmake ... -DMULTIKERNEL_KID=1
# Defaults to 0 for non-multikernel single-image builds.
if(NOT DEFINED MULTIKERNEL_KID)
    set(MULTIKERNEL_KID 0 CACHE STRING "This kernel's ID (0..N-1)")
endif()
if(NOT DEFINED MULTIKERNEL_NUM_KIDS)
    set(MULTIKERNEL_NUM_KIDS 2 CACHE STRING "Total number of kernels in the multikernel system")
endif()
set(MULTIKERNEL_C_FLAGS
    "-DMULTIKERNEL_KID=${MULTIKERNEL_KID}"
    "-DMULTIKERNEL_NUM_KIDS=${MULTIKERNEL_NUM_KIDS}"
)
message(STATUS "test_two_components: MULTIKERNEL_KID=${MULTIKERNEL_KID} of ${MULTIKERNEL_NUM_KIDS} kernels")

# No VM needed - this is pure CAmkES
# No file server needed
# No device tree needed

# Multikernel-AMP Layer 2: vdtu_ring data-plane primitive.
# Vendored from SemperOS-on-seL4 (see lib/vdtu/CMakeLists.txt).
add_subdirectory(lib/vdtu)

# Create simulation script if configured
if("${KernelARMPlatform}" STREQUAL "qemu-arm-virt")
    set(SIMULATION ON CACHE BOOL "Generate simulation script to run qemu with the proper arguments")
    if(SIMULATION)
        include(simulation)
        GenerateSimulateScript()
    endif()
endif()

# Define ComponentA
DeclareCAmkESComponent(
    ComponentA
    SOURCES components/ComponentA/component_a.c
    LIBS sel4_autoconf utils
)

# Define ComponentB
DeclareCAmkESComponent(
    ComponentB
    SOURCES components/ComponentB/component_b.c
    LIBS sel4_autoconf utils
)

# Define ComponentC — cross-kernel user-space mailbox via paddr-anchored
# MMIO dataport at 0x6FFE0000 (16 frames × 4 KiB). Same paddr in both K0
# and K1 builds, so both ComponentC instances map the same 64 KiB region.
# Frame 0 of the pool hosts a vdtu_ring (4 KiB ctrl + slots).
DeclareCAmkESComponent(
    ComponentC
    SOURCES components/ComponentC/component_c.c
    C_FLAGS ${MULTIKERNEL_C_FLAGS}
    LIBS sel4_autoconf utils multikernel_vdtu
)

# Declare root server with our simple assembly
DeclareCAmkESRootserver(
    test_two_components.camkes
)
