cmake_minimum_required(VERSION 3.8.2)

project(camkes-arm-virt-vm-ethernet-echo C)

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
CAmkESAddImportPath(components/EthernetDriver)
CAmkESAddImportPath(components/EchoComponent)

# Enable lwIP
set(LibLwip ON CACHE BOOL "Enable lwIP" FORCE)

set(projects_dir "${CMAKE_CURRENT_LIST_DIR}/../../../..")
find_file(LWIP_PATH lwip PATHS ${projects_dir} CMAKE_FIND_ROOT_PATH_BOTH)
if("${LWIP_PATH}" STREQUAL "LWIP_PATH-NOTFOUND")
    message(FATAL_ERROR "Failed to find lwIP")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/lwip_helpers.cmake)
AddLWIPConfiguration(${CMAKE_CURRENT_LIST_DIR}/components/EthernetDriver)

# EthernetDriver Component
DeclareCAmkESComponent(
    EthernetDriver
    SOURCES
        components/EthernetDriver/ethernet_driver.c
    INCLUDES
        components/EthernetDriver
        ${LWIP_PATH}/src/include
        ${CMAKE_CURRENT_LIST_DIR}/../../../../util_libs/liblwip/include
    LIBS
        sel4_autoconf
        utils
        lwip
)

# EchoComponent
DeclareCAmkESComponent(
    EchoComponent
    SOURCES
        components/EchoComponent/echo_component.c
    LIBS
        sel4_autoconf
        utils
)

# Declare root server
DeclareCAmkESRootserver(
    vm_ethernet_echo.camkes
    CPP_FLAGS ${cpp_flags}
    CPP_INCLUDES ${cpp_includes}
)
