#
# Copyright 2025, PhD Research Project
#
# SPDX-License-Identifier: BSD-2-Clause
#
# ICS Bidirectional Cross-Domain Security Gateway - x86_64 Port
# EverParse formally verified Modbus TCP parsing with dual NIC architecture
#

cmake_minimum_required(VERSION 3.8.2)

project(everparse-modbus-bidirection-x86 C)

# Platform configuration
if("${KernelPlatform}" STREQUAL "pc99")
    set(cpp_flags "-DKERNELPLATFORM_PC99")

    include(simulation)
    set(SIMULATION ON CACHE BOOL "Generate simulation script")
    if(SIMULATION)
        GenerateSimulateScript()
    endif()
endif()

# Enable lwIP
set(LibLwip ON CACHE BOOL "Enable lwIP" FORCE)

set(projects_dir "${CMAKE_CURRENT_LIST_DIR}/../../../..")
find_file(LWIP_PATH lwip PATHS ${projects_dir} CMAKE_FIND_ROOT_PATH_BOTH)
if("${LWIP_PATH}" STREQUAL "LWIP_PATH-NOTFOUND")
    message(FATAL_ERROR "Failed to find lwIP")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/lwip_helpers.cmake)

# Create SEPARATE lwIP libraries for Net0 and Net1 to prevent shared state corruption
# Each component gets its own lwIP instance with isolated memory pools

# lwIP for Net0 (SCADA-facing driver)
AddLWIPConfiguration(${CMAKE_CURRENT_LIST_DIR}/components/Net0_Driver)

# lwIP for Net1 (PLC-facing driver) - Create separate library with its own config
add_library(liblwip_config_net1 INTERFACE)
target_include_directories(liblwip_config_net1 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/components/Net1_Driver)

file(GLOB lwip_sources_net1
    ${LWIP_PATH}/src/*/*.c
    ${LWIP_PATH}/src/core/ipv4/*.c
)
add_library(lwip_net1 STATIC EXCLUDE_FROM_ALL ${lwip_sources_net1})
target_include_directories(lwip_net1 PUBLIC
    ${LWIP_PATH}/src/include
    ${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/include
    ${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/include/lwip
)
target_link_libraries(lwip_net1 muslc liblwip_config_net1)
target_compile_definitions(lwip_net1 PRIVATE LWIP_NET1_INSTANCE=1)

# Net0_Driver Component (External Network - SCADA facing)
DeclareCAmkESComponent(
    Net0_Driver
    SOURCES
        components/Net0_Driver/net0_driver.c
    INCLUDES
        components/Net0_Driver
        components/include
        ${LWIP_PATH}/src/include
        ${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/include
    LIBS
        sel4_autoconf
        utils
        lwip
        ethdrivers
)

# Net1_Driver Component (Internal Network - PLC facing)
# Uses separate lwip_net1 library for isolation
DeclareCAmkESComponent(
    Net1_Driver
    SOURCES
        components/Net1_Driver/net1_driver.c
    INCLUDES
        components/Net1_Driver
        components/include
        ${LWIP_PATH}/src/include
        ${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/include
    LIBS
        sel4_autoconf
        utils
        lwip_net1
        ethdrivers
)

# ICS_Inbound Component (External -> Internal validation)
# EverParse v3 parser with trailing byte attack detection
DeclareCAmkESComponent(
    ICS_Inbound
    SOURCES
        components/ICS_Inbound/ICS_Inbound.c
        components/include/ModbusTCP_v3_Simple.c
        components/include/ModbusTCP_v3_SimpleWrapper.c
        components/lib/everparse_error_handler.c
    INCLUDES
        components/include
)

# ICS_Outbound Component (Internal -> External validation)
# EverParse v3 parser with trailing byte attack detection
DeclareCAmkESComponent(
    ICS_Outbound
    SOURCES
        components/ICS_Outbound/ICS_Outbound.c
        components/include/ModbusTCP_v3_Simple.c
        components/include/ModbusTCP_v3_SimpleWrapper.c
        components/lib/everparse_error_handler.c
    INCLUDES
        components/include
)

# Declare root server
DeclareCAmkESRootserver(
    everparse_modbus_bidirection_x86.camkes
    CPP_FLAGS ${cpp_flags}
)
