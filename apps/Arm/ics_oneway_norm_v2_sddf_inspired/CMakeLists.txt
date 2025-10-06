#
# Copyright 2023, PhD Research Project
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.8.2)

project(camkes-ics-oneway-norm C)

include(${CAMKES_ARM_VM_HELPERS_PATH})
include(ExternalProject)
include(external-project-helpers)

# Create our CPP Flags based on ARM VM config variables
if("${KernelARMPlatform}" STREQUAL "qemu-arm-virt")
    set(cpp_flags "-DKERNELARMPLATFORM_QEMU-ARM-VIRT")
    include(simulation)
    set(SIMULATION ON CACHE BOOL "Generate simulation script to run qemu with the proper arguments")
    if(SIMULATION)
        GenerateSimulateScript()
    endif()
endif()

AddCamkesCPPFlag(cpp_flags CONFIG_VARS)

# Since this is a standalone CAmkES app, we don't need VM file server
# DefineCAmkESVMFileServer()

CAmkESAddImportPath(${KernelARMPlatform})

# Declare all our ICS pipeline components
# sDDF-inspired Network Driver (replaces ExtNicDrv)
DeclareCAmkESComponent(NetworkDriverDrv
    SOURCES components/NetworkDriverDrv/NetworkDriverDrv.c
            components/NetworkDriverDrv/ringbuf.c
    INCLUDES components/include
             components/NetworkDriverDrv
    LIBS virtqueue platsupport)

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
)