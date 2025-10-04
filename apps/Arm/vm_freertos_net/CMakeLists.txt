#
# Copyright 2018, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#
# vm_freertos_net - FreeRTOS VM with Network Filtering
#
# This CMakeLists.txt builds:
# 1. FreeRTOS guest image
# 2. NetworkFilter component (sDDF-inspired filtering)
# 3. VM system with virtqueue-based networking
#

cmake_minimum_required(VERSION 3.8.2)

project(camkes-arm-virt-vm-freertos-net C)

include(${CAMKES_ARM_VM_HELPERS_PATH})

set(cpp_includes "${CAMKES_VM_DIR}/components/VM_Arm")

# Create our CPP Flags based on ARM VM config variables
if("${KernelARMPlatform}" STREQUAL "qemu-arm-virt")
    # Use the pre-built working FreeRTOS binary directly
    set(FREERTOS_BINARY_PATH "${CMAKE_CURRENT_SOURCE_DIR}/qemu-arm-virt/freertos_virtio_test_image.bin")

    # Copy working FreeRTOS binary to build directory
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/freertos_image.bin"
        COMMAND ${CMAKE_COMMAND} -E copy "${FREERTOS_BINARY_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/freertos_image.bin"
        DEPENDS "${FREERTOS_BINARY_PATH}"
        COMMENT "Copying working FreeRTOS binary to build directory"
        VERBATIM
    )

    # Create custom target for FreeRTOS binary
    add_custom_target(freertos_binary DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/freertos_image.bin")

    # Add FreeRTOS binary to file server as "linux" (default name expected by VM component)
    AddToFileServer("linux" "${CMAKE_CURRENT_BINARY_DIR}/freertos_image.bin" DEPENDS freertos_binary)

    set(cpp_flags "-DKERNELARMPLATFORM_QEMU-ARM-VIRT")

    include(simulation)
    set(SIMULATION ON CACHE BOOL "Generate simulation script to run qemu with the proper arguments")
    if(SIMULATION)
        GenerateSimulateScript()
    endif()

endif()

AddCamkesCPPFlag(cpp_flags CONFIG_VARS VmEmmc2NoDMA)

DefineCAmkESVMFileServer()

CAmkESAddImportPath(${KernelARMPlatform})

# Add import path for EthernetDriver component
CAmkESAddImportPath(components/EthernetDriver)

#
# NetworkFilter Component Build
#
# This builds the network filtering component that sits between
# external network and the FreeRTOS VM.
# Based on the PingClient pattern from vm_virtio_net.
#

DeclareCAmkESComponent(
    NetworkFilter
    SOURCES
    components/NetworkFilter/network_filter.c
    LIBS
    virtqueue
    sel4_autoconf
)

#
# Enable lwIP TCP/IP stack (required for Tier 4)
#
set(LibLwip ON CACHE BOOL "Enable lwIP" FORCE)

# Find lwIP source directory
set(projects_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
find_file(LWIP_PATH lwip PATHS ${projects_dir} CMAKE_FIND_ROOT_PATH_BOTH)
if("${LWIP_PATH}" STREQUAL "LWIP_PATH-NOTFOUND")
    message(FATAL_ERROR "Failed to find lwIP. Expected at ${projects_dir}/lwip")
endif()

# Include lwIP helpers
include(${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/lwip_helpers.cmake)

# Add lwIP configuration with our lwipopts.h location
AddLWIPConfiguration(${CMAKE_CURRENT_LIST_DIR}/components/EthernetDriver)

#
# EthernetDriver Component Build (Tier 4: TCP Echo Server)
#
# This builds the virtio-net driver component with full TCP/IP stack.
# Tier 4 goal: TCP echo server that can be tested with telnet.
#

DeclareCAmkESComponent(
    EthernetDriver
    SOURCES
        components/EthernetDriver/ethernet_driver.c
    INCLUDES
        components/EthernetDriver
        ${LWIP_PATH}/src/include
        ${CMAKE_CURRENT_LIST_DIR}/../../util_libs/liblwip/include
    LIBS
        sel4_autoconf
        utils
        lwip
)

#
# Declare root servers
#

# Tier 1 Test: EthernetDriver standalone (no VM)
# This proves virtio device discovery works
DeclareCAmkESRootserver(
    vm_ethernet_test.camkes
    CPP_FLAGS ${cpp_flags}
    CPP_INCLUDES ${cpp_includes}
)

# Full system: NetworkFilter + VM (original)
# DeclareCAmkESRootserver(
#     vm_freertos_net.camkes
#     CPP_FLAGS ${cpp_flags}
#     CPP_INCLUDES ${cpp_includes}
# )
