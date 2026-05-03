#
# CMakeLists.txt for test_two_components - Minimal two-component test
#

cmake_minimum_required(VERSION 3.8.2)

project(test_two_components C)

# Include CAmkES ARM VM helpers
include(${CAMKES_ARM_VM_HELPERS_PATH})

# No VM needed - this is pure CAmkES
# No file server needed
# No device tree needed

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

# Declare root server with our simple assembly
DeclareCAmkESRootserver(
    test_two_components.camkes
)
