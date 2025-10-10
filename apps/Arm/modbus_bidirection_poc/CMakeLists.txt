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

# Enable lwIP
set(LibLwip ON CACHE BOOL "Enable lwIP" FORCE)

set(projects_dir "${CMAKE_CURRENT_LIST_DIR}/../../../..")
find_file(LWIP_PATH lwip PATHS ${projects_dir} CMAKE_FIND_ROOT_PATH_BOTH)
if("${LWIP_PATH}" STREQUAL "LWIP_PATH-NOTFOUND")
    message(FATAL_ERROR "Failed to find lwIP")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/lwip_helpers.cmake)
# Only call AddLWIPConfiguration once - both drivers will use the same lwIP config
AddLWIPConfiguration(${CMAKE_CURRENT_LIST_DIR}/components/VirtIO_Net0_Driver)

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
        lwip
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
DeclareCAmkESComponent(
    ICS_Outbound
    SOURCES
        components/ICS_Outbound/ICS_Outbound.c
    INCLUDES
        components/include
)

# Declare root server
DeclareCAmkESRootserver(
    ics_dual_nic.camkes
    CPP_FLAGS ${cpp_flags}
    CPP_INCLUDES ${cpp_includes}
)
