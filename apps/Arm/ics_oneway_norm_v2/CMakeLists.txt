#
# Copyright 2023, PhD Research Project
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.8.2)

project(camkes-ics-oneway-norm-v2 C)

include(${CAMKES_ARM_VM_HELPERS_PATH})

find_package(camkes-vm-linux REQUIRED)
include(${CAMKES_VM_LINUX_HELPERS_PATH})

# Set up VM includes for configurations/vm.h
set(cpp_includes "${CAMKES_VM_DIR}/components/VM_Arm")

# Set up DTB and rootfs file paths for VM
set(dtb_file "${CAMKES_VM_IMAGES_DIR}/${KernelARMPlatform}/linux-dtb")
set(rootfs_file "${CAMKES_VM_IMAGES_DIR}/${KernelARMPlatform}/rootfs.cpio.gz")

# Create our CPP Flags based on ARM VM config variables
if("${KernelARMPlatform}" STREQUAL "qemu-arm-virt")
    set(cpp_flags "-DKERNELARMPLATFORM_QEMU-ARM-VIRT")
    set(rootfs_address "0x4d700000")
    include(simulation)
    set(SIMULATION ON CACHE BOOL "Generate simulation script to run qemu with the proper arguments")
    if(SIMULATION)
        GenerateSimulateScript()
    endif()
endif()

AddCamkesCPPFlag(cpp_flags CONFIG_VARS)

# Generate overlayed rootfs with V2 configuration
AddFileToOverlayDir(
    "interfaces"
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay_files/network_interfaces/interfaces
    "etc/network"
    overlay
)

AddFileToOverlayDir(
    "vm-bridge-script.sh"
    ${CMAKE_CURRENT_SOURCE_DIR}/vm-bridge-script.sh
    "root"
    overlay
)

AddFileToOverlayDir(
    "S90ics_bridge_start"
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay_files/init_scripts/ics_bridge_start
    "etc/init.d"
    overlay
)

AddFileToOverlayDir(
    "test_virtio_magic.sh"
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay_files/test_virtio_magic.sh
    "root"
    overlay
)

AddFileToOverlayDir(
    "S99test_virtio"
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay_files/init_scripts/S99test_virtio
    "etc/init.d"
    overlay
)

AddOverlayDirToRootfs(
    overlay
    ${rootfs_file}
    "buildroot"
    "rootfs_install"
    output_overlayed_rootfs_location
    rootfs_target
    GZIP
)

AddToFileServer("linux-initrd" ${output_overlayed_rootfs_location} DEPENDS rootfs_target)

# Updated dtb based on generated initrd
UpdateDtbFromInitrd(
    ${dtb_file}
    ${output_overlayed_rootfs_location}
    ${rootfs_address}
    dtb_gen_target
    output_dtb_location
    DEPENDS
    rootfs_target
)

AddToFileServer("linux" "${CAMKES_VM_IMAGES_DIR}/${KernelARMPlatform}/linux")
AddToFileServer("linux-dtb" "${output_dtb_location}" DEPENDS dtb_gen_target)

# Define VM file server for VM components
DefineCAmkESVMFileServer()

CAmkESAddImportPath(${KernelARMPlatform})

# Declare all our ICS pipeline components
DeclareCAmkESComponent(NetworkNicDrv
    SOURCES components/NetworkNicDrv/NetworkNicDrv.c
            components/lib/ringbuf.c
    INCLUDES components/include
             components/lib
    LIBS virtqueue
         sel4_autoconf)

DeclareCAmkESComponent(ExtFrontend
    SOURCES components/ExtFrontend/ExtFrontend.c
            components/lib/ringbuf.c
    INCLUDES components/include
             components/lib)

DeclareCAmkESComponent(ParserNorm
    SOURCES components/ParserNorm/ParserNorm.c
            components/lib/ringbuf.c
    INCLUDES components/include
             components/lib)

DeclareCAmkESComponent(PolicyEmit
    SOURCES components/PolicyEmit/PolicyEmit.c
            components/lib/ringbuf.c
    INCLUDES components/include
             components/lib)

DeclareCAmkESComponent(IntNicDrv
    SOURCES components/IntNicDrv/IntNicDrv.c
            components/lib/ringbuf.c
    INCLUDES components/include
             components/lib)

# Declare the root server with our main CAmkES file
DeclareCAmkESRootserver(
    icf.camkes
    CPP_FLAGS
    ${cpp_flags}
    CPP_INCLUDES
    ${cpp_includes}
)