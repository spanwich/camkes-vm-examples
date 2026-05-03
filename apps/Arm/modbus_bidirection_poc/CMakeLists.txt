cmake_minimum_required(VERSION 3.8.2)

project(camkes-ics-oneway-dual-nic C)

include(${CAMKES_ARM_VM_HELPERS_PATH})

set(cpp_includes "${CAMKES_VM_DIR}/components/VM_Arm")

# Platform configuration
if("${KernelARMPlatform}" STREQUAL "qemu-arm-virt")
    set(cpp_flags "-DKERNELARMPLATFORM_QEMU-ARM-VIRT")

    include(simulation)
    set(SIMULATION ON CACHE BOOL "Generate simulation script")
    if(SIMULATION)
        GenerateSimulateScript()
    endif()
endif()

AddCamkesCPPFlag(cpp_flags CONFIG_VARS VmEmmc2NoDMA)

DefineCAmkESVMFileServer()

CAmkESAddImportPath(${KernelARMPlatform})
CAmkESAddImportPath(components/VirtIO_Net0_Driver)
CAmkESAddImportPath(components/VirtIO_Net1_Driver)
CAmkESAddImportPath(components/interfaces)

# Enable lwIP
set(LibLwip ON CACHE BOOL "Enable lwIP" FORCE)

set(projects_dir "${CMAKE_CURRENT_LIST_DIR}/../../../..")
find_file(LWIP_PATH lwip PATHS ${projects_dir} CMAKE_FIND_ROOT_PATH_BOTH)
if("${LWIP_PATH}" STREQUAL "LWIP_PATH-NOTFOUND")
    message(FATAL_ERROR "Failed to find lwIP")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/lwip_helpers.cmake)

# v2.79: Create SEPARATE lwIP libraries for Net0 and Net1 to prevent shared state corruption
# Each component gets its own lwIP instance with isolated memory pools

# lwIP for Net0 (SCADA-facing driver)
AddLWIPConfiguration(${CMAKE_CURRENT_LIST_DIR}/components/VirtIO_Net0_Driver)

# lwIP for Net1 (PLC-facing driver) - Create separate library with its own config
add_library(liblwip_config_net1 INTERFACE)
target_include_directories(liblwip_config_net1 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/components/VirtIO_Net1_Driver)

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

# VirtIO_Net0_Driver Component (External Network - port 6000)
DeclareCAmkESComponent(
    VirtIO_Net0_Driver
    SOURCES
        components/VirtIO_Net0_Driver/virtio_net0_driver.c
    INCLUDES
        components/VirtIO_Net0_Driver
        components/include
        ${LWIP_PATH}/src/include
        ${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/include
    LIBS
        sel4_autoconf
        utils
        lwip
)

# VirtIO_Net1_Driver Component (Internal Network - port 7000)
# v2.79: Uses separate lwip_net1 library for isolation
DeclareCAmkESComponent(
    VirtIO_Net1_Driver
    SOURCES
        components/VirtIO_Net1_Driver/virtio_net1_driver.c
    INCLUDES
        components/VirtIO_Net1_Driver
        components/include
        ${LWIP_PATH}/src/include
        ${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/include
    LIBS
        sel4_autoconf
        utils
        lwip_net1
)

# ICS_Inbound Component (External → Internal validation)
DeclareCAmkESComponent(
    ICS_Inbound
    SOURCES
        components/ICS_Inbound/ICS_Inbound.c
    INCLUDES
        components/include
)

# ICS_Outbound Component (Internal → External validation)
# v2.300: Symmetric F*-verified response validation (Zero Trust ICS)
# Response validators prevent data exfiltration, address confusion, and exception abuse.
DeclareCAmkESComponent(
    ICS_Outbound
    SOURCES
        components/ICS_Outbound/ICS_Outbound.c
        components/lib/modbus_response_policy_fstar.c
        components/include/ModbusTCP_v5_ResponsePolicy.c
        components/include/ModbusTCP_v5_ResponsePolicyWrapper.c
        components/lib/everparse_error_handler.c
    INCLUDES
        components/include
)

# ModbusParser Component (Isolated EverParse validator - separate address space)
DeclareCAmkESComponent(
    ModbusParser
    SOURCES
        components/ModbusParser/ModbusParser.c
        components/include/ModbusTCP_v3_Simple.c
        components/include/ModbusTCP_v3_SimpleWrapper.c
        components/lib/everparse_error_handler.c
    INCLUDES
        components/include
)

# Declare root server
DeclareCAmkESRootserver(
    ics_dual_nic.camkes
    CPP_FLAGS ${cpp_flags}
    CPP_INCLUDES ${cpp_includes}
)
