#
# Copyright 2025, PhD Research Project
#
# SPDX-License-Identifier: BSD-2-Clause
#

# ICS Bidirectional Cross-Domain Security Gateway - x86_64 Port
# EverParse formally verified Modbus TCP parsing with dual NIC architecture
#
# QEMU simulation only - dual Intel e1000 NICs for external/internal network separation

cmake_minimum_required(VERSION 3.8.2)

# x86_64 architecture (64-bit)
set(KernelSel4Arch x86_64 CACHE STRING "" FORCE)

# Single processor configuration
set(KernelMaxNumNodes 1 CACHE STRING "" FORCE)

# Enable lwIP TCP/IP stack (required for network drivers)
set(LibLwip ON CACHE BOOL "" FORCE)
