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
CAmkESAddImportPath(components/EthernetDriver_RX)
CAmkESAddImportPath(components/EthernetDriver_TX)

# Enable lwIP
set(LibLwip ON CACHE BOOL "Enable lwIP" FORCE)

set(projects_dir "${CMAKE_CURRENT_LIST_DIR}/../../../..")
find_file(LWIP_PATH lwip PATHS ${projects_dir} CMAKE_FIND_ROOT_PATH_BOTH)
if("${LWIP_PATH}" STREQUAL "LWIP_PATH-NOTFOUND")
    message(FATAL_ERROR "Failed to find lwIP")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/lwip_helpers.cmake)
# Only call AddLWIPConfiguration once - both RX and TX will use the same lwIP config
AddLWIPConfiguration(${CMAKE_CURRENT_LIST_DIR}/components/EthernetDriver_RX)

# EthernetDriver_RX Component
DeclareCAmkESComponent(
    EthernetDriver_RX
    SOURCES
        components/EthernetDriver_RX/ethernet_driver_rx.c
    INCLUDES
        components/EthernetDriver_RX
        ${LWIP_PATH}/src/include
        ${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/include
    LIBS
        sel4_autoconf
        utils
        lwip
)

# EthernetDriver_TX Component
DeclareCAmkESComponent(
    EthernetDriver_TX
    SOURCES
        components/EthernetDriver_TX/ethernet_driver_tx.c
    INCLUDES
        components/EthernetDriver_TX
        components/include
        ${LWIP_PATH}/src/include
        ${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/include
    LIBS
        sel4_autoconf
        utils
        lwip
)

# ExtFrontend Component
DeclareCAmkESComponent(
    ExtFrontend
    SOURCES
        components/ExtFrontend/ExtFrontend.c
        components/lib/ringbuf.c
    INCLUDES
        components/include
        components/lib
)

# ParserNorm Component
DeclareCAmkESComponent(
    ParserNorm
    SOURCES
        components/ParserNorm/ParserNorm.c
        components/lib/ringbuf.c
    INCLUDES
        components/include
        components/lib
)

# PolicyEmit Component
DeclareCAmkESComponent(
    PolicyEmit
    SOURCES
        components/PolicyEmit/PolicyEmit.c
        components/lib/ringbuf.c
    INCLUDES
        components/include
        components/lib
)

# IntNicDrv Component (now TX-enabled)
DeclareCAmkESComponent(
    IntNicDrv
    SOURCES
        components/IntNicDrv/IntNicDrv.c
        components/lib/ringbuf.c
    INCLUDES
        components/include
        components/lib
)

# Declare root server
DeclareCAmkESRootserver(
    ics_dual_nic.camkes
    CPP_FLAGS ${cpp_flags}
    CPP_INCLUDES ${cpp_includes}
)
