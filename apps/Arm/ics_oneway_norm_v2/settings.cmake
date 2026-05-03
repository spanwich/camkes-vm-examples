#
# Copyright 2023, PhD Research Project
#
# SPDX-License-Identifier: BSD-2-Clause
#

# ICS One-Way Normalizer V2 Settings
# Platform-specific configuration for qemu-arm-virt

set(supported_platforms qemu-arm-virt)
if(NOT ${PLATFORM} IN_LIST supported_platforms)
    message(FATAL_ERROR "ics_oneway_norm_v2 only supports qemu-arm-virt platform. Current platform: ${PLATFORM}")
endif()

# CRITICAL: Kernel configuration (copied from vm_freertos)
if(${PLATFORM} STREQUAL "qemu-arm-virt")
    set(QEMU_MEMORY "2048")
    set(KernelArmCPU cortex-a15 CACHE STRING "" FORCE)
    set(KernelSel4Arch arm_hyp CACHE STRING "" FORCE)
endif()

# Default Linux configuration for QEMU ARM virt platform
set(linux_rootfs_file "${CAMKES_VM_IMAGES_DIR}/${KernelARMPlatform}/rootfs.cpio.gz")

# VM memory configuration
set(LINUX_RAM_BASE 0x40000000)
set(LINUX_RAM_SIZE 0x20000000)  # 512MB
set(LINUX_DTB_ADDR 0x4F000000)
set(LINUX_INITRD_ADDR 0x4D700000)

# Virtio device support configuration
set(VirtioNetSupport ON CACHE BOOL "Enable VirtIO network support")
set(VirtioConsoleSupport ON CACHE BOOL "Enable VirtIO console support")

# Configure for VM-based application
if(NOT CAMKES_VM_APP)
    set(CAMKES_VM_APP ics_oneway_norm_v2)
endif()

# Enable simulation script generation
set(SIMULATION ON CACHE BOOL "Generate QEMU simulation script")
