#
# vm_echo_net settings.cmake
# Based on vm_minimal with additional network configuration
#

set(supported "tk1;tx1;tx2;exynos5422;qemu-arm-virt;odroidc2;zcu102")
if(NOT "${PLATFORM}" IN_LIST supported)
    message(FATAL_ERROR "PLATFORM: ${PLATFORM} not supported.
         Supported: ${supported}")
endif()
if(${PLATFORM} STREQUAL "tk1")
    set(KernelTk1SMMU ON CACHE BOOL "" FORCE)
    set(KernelTk1SMMUInterruptEnable ON CACHE BOOL "" FORCE)
endif()

set(LibUSB OFF CACHE BOOL "" FORCE)
if(${PLATFORM} STREQUAL "exynos5422")
    set(VmPCISupport ON CACHE BOOL "" FORCE)
    set(VmVirtioNet ON CACHE BOOL "" FORCE)
    set(VmInitRdFile ON CACHE BOOL "" FORCE)
endif()
if(${PLATFORM} STREQUAL "tx2")
    set(VmPCISupport ON CACHE BOOL "" FORCE)
    set(VmVirtioNet ON CACHE BOOL "" FORCE)
    set(VmInitRdFile ON CACHE BOOL "" FORCE)
    set(VmDtbFile ON CACHE BOOL "" FORCE)
endif()
if(${PLATFORM} STREQUAL "odroidc2")
    set(VmInitRdFile ON CACHE BOOL "" FORCE)
    set(VmDtbFile ON CACHE BOOL "" FORCE)
endif()
if(${PLATFORM} STREQUAL "qemu-arm-virt")
    # force cpu
    set(QEMU_MEMORY "2048")
    set(KernelArmCPU cortex-a53 CACHE STRING "" FORCE)
    set(VmInitRdFile ON CACHE BOOL "" FORCE)
    # Enable PCI support for virtio-net
    set(VmPCISupport ON CACHE BOOL "" FORCE)
    set(VmVirtioNet ON CACHE BOOL "" FORCE)
endif()
if(${PLATFORM} STREQUAL "zcu102")
    set(AARCH64 ON CACHE BOOL "" FORCE)
    set(KernelAllowSMCCalls ON CACHE BOOL "" FORCE)
endif()
