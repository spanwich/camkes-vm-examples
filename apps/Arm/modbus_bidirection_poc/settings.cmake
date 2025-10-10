#
# Copyright 2025, PhD Research Project
#
# SPDX-License-Identifier: BSD-2-Clause
#

# ICS One-Way Normalizer V3 Dual-NIC Settings
# Platform-specific configuration for qemu-arm-virt with dual VirtIO NICs

set(supported_platforms qemu-arm-virt)
if(NOT ${PLATFORM} IN_LIST supported_platforms)
    message(FATAL_ERROR "ics_oneway_norm_v3_dual_nic only supports qemu-arm-virt platform. Current platform: ${PLATFORM}")
endif()

# CRITICAL: Use 32-bit ARM (ARMv7) instead of 64-bit AArch64
# AArch64 has memory attribute issues with VirtIO MMIO (maps as cacheable normal memory)
set(AARCH64 OFF CACHE BOOL "" FORCE)
set(KernelArmHypervisorSupport ON CACHE BOOL "" FORCE)

# Enable lwIP TCP/IP stack (required for network drivers)
set(LibLwip ON CACHE BOOL "" FORCE)

# VM memory configuration (not used in this pure CAmkES app, but kept for compatibility)
set(LINUX_RAM_BASE 0x40000000)
set(LINUX_RAM_SIZE 0x20000000)  # 512MB
set(LINUX_DTB_ADDR 0x4F000000)
set(LINUX_INITRD_ADDR 0x4D700000)

# Dual VirtIO network device support
set(VirtioNetSupport ON CACHE BOOL "Enable VirtIO network support (dual NICs)")

# Configure for pure CAmkES application (no VM guests)
if(NOT CAMKES_VM_APP)
    set(CAMKES_VM_APP ics_oneway_norm_v3_dual_nic)
endif()

# Enable simulation script generation
set(SIMULATION ON CACHE BOOL "Generate QEMU simulation script")
