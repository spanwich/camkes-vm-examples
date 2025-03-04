#
# Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.8.2)
include(settings.cmake)
project(camkes-vm C CXX ASM)

# These two config variables are used in the camkes-multikernel build
# for defining the configuration passed through to the individual seL4 builds.
# The config file is evaluated via -C <file> and each variable listed is
# also passed in via -D${var}=${${var}}.
set(SEL4_CONFIG_FILE ${CMAKE_CURRENT_LIST_DIR}/settings.cmake)
set(SEL4_CONFIG_VARIABLES CAMKES_VM_APP PLATFORM RELEASE)

if(AppArch STREQUAL "Arm")
    find_package(camkes-arm-vm REQUIRED)
    camkes_arm_vm_setup_arm_vm_environment()
elseif(AppArch STREQUAL "x86")
    camkes_x86_vm_setup_x86_vm_environment()
else()
    message(FATAL_ERROR "Unsupported")
endif()
# Add VM application
add_subdirectory("apps/${AppArch}/${CAMKES_VM_APP}")
# Should be done adding targets, can now generate the root server and the global configuration
GenerateCAmkESRootserver()
