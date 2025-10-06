#
# Copyright 2023, PhD Research Project
# ICS One-Way Pipeline V3 with Linux VM
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.8.2)

project(camkes-ics-oneway-norm-v3 C)

include(${CAMKES_ARM_VM_HELPERS_PATH})

# Include Linux VM support
find_package(camkes-vm-linux REQUIRED)
include(${CAMKES_VM_LINUX_HELPERS_PATH})

set(cpp_includes "${CAMKES_VM_DIR}/components/VM_Arm")

# Set Linux kernel and rootfs paths
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

AddCamkesCPPFlag(cpp_flags CONFIG_VARS VmEmmc2NoDMA)

# Define file server for VM
DefineCAmkESVMFileServer()

CAmkESAddImportPath(${KernelARMPlatform})

# Add Linux kernel to file server
AddToFileServer("linux" ${CAMKES_VM_IMAGES_DIR}/${KernelARMPlatform}/linux)

# Add initrd to file server
AddToFileServer("linux-initrd" ${rootfs_file} DEPENDS rootfs_install)

# Dynamically update DTB with initrd address (this fixes memory range!)
UpdateDtbFromInitrd(
    ${dtb_file}
    ${rootfs_file}
    ${rootfs_address}
    dtb_gen_target
    output_dtb_location
    DEPENDS rootfs_install
)
AddToFileServer("linux-dtb" "${output_dtb_location}" DEPENDS dtb_gen_target)

# Declare all our ICS pipeline components
DeclareCAmkESComponent(TestTrafficGen
    SOURCES components/TestTrafficGen/TestTrafficGen.c
    INCLUDES components/include)

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
    CPP_FLAGS ${cpp_flags}
    CPP_INCLUDES ${cpp_includes}
)
