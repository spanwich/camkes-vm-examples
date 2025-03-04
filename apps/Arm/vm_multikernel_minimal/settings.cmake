#
# Copyright 2018, Data61, CSIRO (ABN 41 687 119 230)
# Copyright 2025, Kry10 Pty. Ltd.
#
# SPDX-License-Identifier: BSD-2-Clause
#

set(supported "qemu-arm-virt;zcu102")
if(NOT "${PLATFORM}" IN_LIST supported)
    message(FATAL_ERROR "PLATFORM: ${PLATFORM} not supported.
         Supported: ${supported}")
endif()
set(LibUSB OFF CACHE BOOL "" FORCE)

if(${PLATFORM} STREQUAL "qemu-arm-virt")
    # force cpu
    set(QEMU_MEMORY "2048")
    set(ElfLoaderNumNodes 4 CACHE INTERNAL "" FORCE)
    set(AARCH64 ON CACHE BOOL "" FORCE)
    set(VmInitRdFile ON CACHE BOOL "" FORCE)
endif()
if(${PLATFORM} STREQUAL "zcu102")
    set(AARCH64 ON CACHE BOOL "" FORCE)
    set(ElfLoaderNumNodes 4 CACHE INTERNAL "" FORCE)
    set(KernelAllowSMCCalls ON CACHE BOOL "" FORCE)
    list(APPEND KernelCustomDTSOverlay ${CMAKE_CURRENT_LIST_DIR}/${PLATFORM}/overlay.dts)
endif()
