#
# Copyright 2018, Data61, CSIRO (ABN 41 687 119 230)
# Copyright 2025, Kry10 Pty. Ltd.
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.8.2)

project(camkes-arm-vm-multikernel-minimal C)


# Helper function for adding images to the elfloader instead of file server
function(AddLoadImage filename_pref file_dest)
    # Get any existing dependencies when adding the image into the file server archive
    cmake_parse_arguments(PARSE_ARGV 2 LOAD_IMAGES "" "" "DEPENDS")
    if(NOT "${LOAD_IMAGES_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to AddLoadImage")
    endif()
    # Create a copy of the file in the binary directory to the callers
    # preferred name
    add_custom_command(
        OUTPUT load_images/${filename_pref}
        COMMAND
            ${CMAKE_COMMAND} -E copy "${file_dest}"
            "${CMAKE_CURRENT_BINARY_DIR}/load_images/${filename_pref}"
        VERBATIM
        DEPENDS ${file_dest} ${LOAD_IMAGES_DEPENDS}
    )
    file(SIZE ${file_dest} file_size)
    LIST(APPEND load_images load_images/${filename_pref})

    LIST(APPEND size_includes "#define FILE_${filename_pref}_SIZE ${file_size}")

    set(load_images ${load_images} PARENT_SCOPE)
    set(size_includes ${size_includes} PARENT_SCOPE)

    set_property(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${file_dest}"
    )

endfunction(AddLoadImage)


include(${CAMKES_ARM_VM_HELPERS_PATH})

set(cpp_includes "${CAMKES_VM_DIR}/components/VM_Arm")

# Create our CPP Flags based on ARM VM config variables
if("${KernelARMPlatform}" STREQUAL "qemu-arm-virt")

    # QEMU supports ARMv7/aarch32 and ARMv8/aarch32, but currently there are
    # Linux images for aarch64 only. However, it is unclear if that is the only
    # blocker to get this example run in QEMU on aarch32.
    if(NOT KernelSel4ArchAarch64)
        message(FATAL_ERROR "Only AARCH64 is supported")
    endif()

    find_package(camkes-vm-linux REQUIRED)
    include(${CAMKES_VM_LINUX_HELPERS_PATH})
    set(cpp_flags "-DKERNELARMPLATFORM_QEMU-ARM-VIRT")

    AddLoadImage("linux_kernel" "/tmp/tmp.3uICRi1C9y/tools/linux/arch/arm64/boot/Image")
    set(rootfs_file "${CAMKES_VM_IMAGES_DIR}/qemu-arm-virt/rootfs.cpio.gz")
    AddLoadImage("linux_initrd" ${rootfs_file})
    # Updated dtb based on initrd
    UpdateDtbFromInitrd(
        "${CAMKES_VM_IMAGES_DIR}/qemu-arm-virt/linux-dtb"
        ${rootfs_file}
        "0x4d700000"
        dtb_gen_target
        output_dtb_location
    )
    add_custom_command(
        OUTPUT linux/linux_dtb
        COMMAND
            ${CMAKE_COMMAND} -E copy "${output_dtb_location}"
            linux/linux_dtb 
        VERBATIM
        DEPENDS "${output_dtb_location}"
    )
    add_custom_target(set_dtb ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/linux/linux_dtb")

    set(vm_mapping_dir ${KernelARMPlatform}/)
    include(simulation)
    set(SIMULATION ON CACHE BOOL "Generate simulation script to run qemu with the proper arguments")
    if(SIMULATION)
        set(NUM_NODES 4)
        GenerateSimulateScript()
        unset(NUM_NODES)
    endif()

elseif("${KernelARMPlatform}" STREQUAL "zcu102")

    if("${VmZynqmpPetalinuxVersion}" STREQUAL "")
        message(
            FATAL_ERROR
                "VmZynqmpPetalinuxVersion is not set. Please set to include proper VM artifacts"
        )
    endif()

    AddLoadImage("linux_kernel" "${CAMKES_VM_IMAGES_DIR}/zynqmp/${VmZynqmpPetalinuxVersion}/linux")
    AddLoadImage(
        "linux_initrd"
        "${CAMKES_VM_IMAGES_DIR}/zynqmp/${VmZynqmpPetalinuxVersion}/rootfs-minimal.cpio.gz"
    )

    # This dts file is compiled into each of the VM images, so we need to add it's
    # path to the include search path of all vm instances
    set(dts_file "${CAMKES_VM_IMAGES_DIR}/zynqmp/${VmZynqmpPetalinuxVersion}/linux.dts")

    add_custom_command(
        OUTPUT linux/linux_dtb
        COMMAND
            dtc -q -I dts -O dtb -o linux/linux_dtb ${dts_file}
        VERBATIM
        DEPENDS ${dts_file}
    )
    add_custom_target(set_dtb ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/linux/linux_dtb")
    list(APPEND cpp_includes "${CAMKES_VM_DIR}/components/VM_Arm/plat_include/zynqmp")

    CAmkESAddImportPath(${KernelARMPlatform}/${VmZynqmpPetalinuxVersion})
    set(vm_mapping_dir ${KernelARMPlatform}/${VmZynqmpPetalinuxVersion}/)
endif()
ExtendCAmkESComponentInstance(VM vm0 INCLUDES "${CMAKE_CURRENT_BINARY_DIR}/linux")
ExtendCAmkESComponentInstance(VM vm0_1 INCLUDES "${CMAKE_CURRENT_BINARY_DIR}/linux")
ExtendCAmkESComponentInstance(VM vm0_2 INCLUDES "${CMAKE_CURRENT_BINARY_DIR}/linux")
ExtendCAmkESComponentInstance(VM vm0_3 INCLUDES "${CMAKE_CURRENT_BINARY_DIR}/linux")


add_custom_target(load_images DEPENDS ${load_images})
string(REPLACE ";" "\n" size_header "${size_includes}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/vm_file_sizes.h ${size_header})
list(APPEND cpp_includes "${CMAKE_CURRENT_BINARY_DIR}")

AddCamkesCPPFlag(cpp_flags CONFIG_VARS VmEmmc2NoDMA VmVUSB Tk1DeviceFwd Tk1Insecure)


CAmkESAddImportPath(${KernelARMPlatform})

# Declare root server
DeclareCAmkESRootserver(vm_multikernel_minimal.camkes CPP_FLAGS ${cpp_flags} CPP_INCLUDES ${cpp_includes})
target_include_directories(elfloader PRIVATE ${vm_mapping_dir} ${CMAKE_CURRENT_BINARY_DIR}/load_images ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(elfloader load_images)
