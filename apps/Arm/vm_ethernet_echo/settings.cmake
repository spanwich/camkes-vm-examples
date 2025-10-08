#
# Copyright 2024, PhD Research
#
# SPDX-License-Identifier: BSD-2-Clause
#

# vm_ethernet_echo requires lwIP for TCP/IP stack
set(LibLwip ON CACHE BOOL "" FORCE)

# Disable USB support to avoid build complexity
set(VmVUSB OFF CACHE BOOL "" FORCE)
set(VmVchan OFF CACHE BOOL "" FORCE)

# Standard ARM VM settings
set(AARCH64 OFF CACHE BOOL "" FORCE)
set(KernelArmHypervisorSupport ON CACHE BOOL "" FORCE)
