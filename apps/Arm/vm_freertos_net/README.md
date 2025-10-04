# CAmkES VM FreeRTOS Example - Comprehensive Technical Guide

This directory contains a minimal example of running FreeRTOS as a guest operating system within a CAmkES VM on ARM platforms, demonstrating the integration of formal verification, capability-based security, and hardware virtualization.

## Overview

This example demonstrates how to:
- Run FreeRTOS as a guest OS in a CAmkES VM using seL4 microkernel
- Build and deploy a minimal FreeRTOS application with direct hardware access
- Configure virtual hardware for the guest OS using ARM virtualization extensions
- Use UART for basic I/O from the guest through virtualized devices
- Leverage seL4's formal verification guarantees for secure virtualization

## System Architecture

### Technology Stack
- **seL4 Microkernel**: L4 family microkernel with formal verification for functional correctness
- **CAmkES Framework**: Component Architecture for Microkernel-based Embedded Systems
- **ARM Virtualization**: ARMv7-A/ARMv8-A hypervisor extensions with Stage 2 translation
- **Hardware Platform**: QEMU ARM virt machine with Cortex-A53 CPU

### Component Architecture
```
┌─────────────────────────────────────────────────────┐
│                 seL4 Microkernel                    │
├─────────────────────────────────────────────────────┤
│               CAmkES Components                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  │
│  │    Init     │  │  VM_Arm     │  │FileServer   │  │
│  │ Component   │  │ Component   │  │ Component   │  │
│  └─────────────┘  └─────────────┘  └─────────────┘  │
├─────────────────────────────────────────────────────┤
│             ARM Virtualization Layer                │
│  ┌─────────────────────────────────────────────────┐ │
│  │              Guest VM                           │ │
│  │  ┌─────────────────────────────────────────────┐ │ │
│  │  │           FreeRTOS Guest                    │ │ │
│  │  │  - Minimal kernel implementation           │ │ │
│  │  │  - Direct UART hardware access             │ │ │
│  │  │  - Memory at 0x40000000-0x48000000         │ │ │
│  │  └─────────────────────────────────────────────┘ │ │
│  └─────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘
```

## ELF Loading Mechanism

### Loading Process Flow
1. **Build Phase**: FreeRTOS compiled to ELF format (`minimal_uart_virt.elf`)
2. **Conversion**: ELF converted to raw binary using `arm-none-eabi-objcopy`
3. **Integration**: Binary packaged as "linux" kernel image in CAmkES file server
4. **Loading**: VM component loads binary into guest physical memory at runtime

### Memory Layout
The FreeRTOS guest uses a simple memory layout defined in `minimal_virt.ld`:
```
Virtual/Physical Address    Size        Purpose
0x40000000                 128MB       Guest RAM base
0x40000000                 <4KB        .text (code section)
0x40000000+text_size       <4KB        .rodata (read-only data)
0x40000000+ro_size         <4KB        .data (initialized data)
0x40000000+data_size       <4KB        .bss (uninitialized data)
0x48000000                 -           Stack pointer (top of RAM)
```

### ELF to Binary Conversion
```bash
# Build process from CMakeLists.txt
make -C freertos_build -f minimal_virt.mk
arm-none-eabi-objcopy -O binary minimal_uart_virt.elf freertos.bin
```

## Hardware Virtualization Features

### ARM Virtualization Extensions
- **Hypervisor Mode**: Uses ARM Hypervisor (HYP) mode for VM management
- **Stage 2 Translation**: Hardware-assisted memory virtualization
  - IPA (Intermediate Physical Address) to PA (Physical Address) mapping
  - 1:1 memory mapping for direct hardware access
- **Exception Handling**: Trap and forward guest exceptions to VMM
- **Timer Virtualization**: Virtual timer management and injection

### seL4 Capabilities Used
- **VCPU Objects**: Hardware virtualization contexts (`seL4_VCPU`)
- **Frame Capabilities**: Physical memory frame access for guest RAM
- **Page Directory/Table**: Memory management for guest address space
- **IRQ Handler Capabilities**: Interrupt management and injection
- **Untyped Capabilities**: Raw memory allocation for guest resources

### Virtual Hardware Configuration
From `devices.camkes`:
```c
// Memory Configuration
VM_RAM_BASE:      0x40000000    // 1GB base address
VM_RAM_SIZE:      0x20000000    // 512MB allocated
VM_DTB_ADDR:      0x4F000000    // Device tree blob location
VM_INITRD_ADDR:   0x4D700000    // Initial RAM disk location

// Device Mappings
untyped_mmios = [
    "0x8040000:12",  // Interrupt Controller Virtual CPU interface
    "0x40000000:29", // Guest physical memory region
];

// Device Tree Configuration
vm0.dtb = dtb([
    {"path": "/pl011@9000000"}, // UART device
]);
```

## Files and Implementation Details

### Build Configuration
- **`CMakeLists.txt`**: Main build configuration that:
  - Compiles FreeRTOS using cross-compilation toolchain
  - Converts ELF to binary format for VM loading
  - Integrates with CAmkES file server for runtime access
  - Configures simulation environment for QEMU

- **`settings.cmake`**: Platform-specific settings:
  - Forces ARM Cortex-A53 CPU configuration
  - Sets QEMU memory to 2GB
  - Validates supported platforms (qemu-arm-virt only)

- **`vm_minimal.camkes`**: CAmkES assembly defining:
  - VM component composition and configuration
  - Memory allocation (23-bit CNode, 24-bit untyped pool)
  - Device tree passthrough connection

### Platform Configuration
- **`qemu-arm-virt/devices.camkes`**: Virtual hardware configuration:
  - Memory layout with 512MB RAM at 0x40000000
  - UART device mapping to PL011 at 0x9000000
  - Interrupt controller setup for guest interrupts
  - Device tree configuration for hardware discovery

### FreeRTOS Implementation
- **`freertos_build/minimal_main_virt.c`**: Minimal bare-metal application:
  - Direct UART register access at 0x9000000 (PL011 UART)
  - Simple character output without interrupt handling
  - Infinite loop with periodic messaging and delays
  - No actual FreeRTOS scheduler (despite the name)

- **`freertos_build/minimal_startup_virt.S`**: ARM assembly startup:
  - Sets stack pointer to 0x48000000 (128MB offset)
  - Jumps to main C function
  - Minimal startup without full ARM initialization

- **`freertos_build/minimal_virt.ld`**: Linker script:
  - Places code at 0x40000000 (guest RAM base)
  - Defines standard ELF sections (.text, .rodata, .data, .bss)
  - Discards debug and comment sections for minimal size

- **`freertos_build/minimal_virt.mk`**: Cross-compilation makefile:
  - Uses ARM GCC toolchain (`arm-none-eabi-gcc`)
  - Compiles for Cortex-A15 (compatible with A53)
  - Freestanding compilation without standard library

## Software Specifications and Requirements

### Build Dependencies
- **seL4 Microkernel**: Compiled with ARM hypervisor support
- **CAmkES Framework**: Component architecture and code generation
- **ARM Toolchain**: `arm-none-eabi-gcc` cross-compilation suite
- **CMake**: Build system (minimum version 3.8.2)
- **Python**: CAmkES code generation and build scripts

### Hardware Requirements
- **ARM Architecture**: ARMv7-A with virtualization extensions or ARMv8-A
- **Hypervisor Support**: ARM Hypervisor (HYP) mode capability
- **Memory**: Minimum 2GB RAM for host and guest allocation
- **Platform**: QEMU ARM virt machine or compatible hardware

### Runtime Environment
- **QEMU Configuration**:
  - Machine: ARM virt
  - CPU: Cortex-A53
  - Memory: 2GB
  - Devices: PL011 UART, Generic Interrupt Controller

### Security Features
- **Capability-based Security**: All access controlled through seL4 capabilities
- **Memory Isolation**: Hardware-enforced memory separation between host and guest
- **Formal Verification**: seL4 microkernel formally verified for functional correctness
- **Minimal TCB**: Trusted Computing Base reduced to seL4 microkernel

## Build Process Details

### Phase 1: FreeRTOS Compilation
```bash
# Cross-compile FreeRTOS source to ELF
arm-none-eabi-gcc -mcpu=cortex-a15 -marm -nostdlib -ffreestanding -O2 -Wall \
  -c -o minimal_startup_virt.o minimal_startup_virt.S
arm-none-eabi-gcc -mcpu=cortex-a15 -marm -nostdlib -ffreestanding -O2 -Wall \
  -c -o minimal_main_virt.o minimal_main_virt.c
arm-none-eabi-gcc -Tminimal_virt.ld -nostdlib -static \
  -o minimal_uart_virt.elf minimal_startup_virt.o minimal_main_virt.o
```

### Phase 2: Binary Conversion
```bash
# Convert ELF to raw binary for VM loading
arm-none-eabi-objcopy -O binary minimal_uart_virt.elf freertos.bin
```

### Phase 3: CAmkES Integration
- Binary registered with CAmkES file server as "linux" kernel
- VM component configured to load binary at 0x40000000
- Memory mappings established for guest execution

### Phase 4: System Boot
1. seL4 microkernel boots and initializes
2. CAmkES components start (Init, VM_Arm, FileServer)
3. VM component creates VCPU and guest memory space
4. FreeRTOS binary loaded into guest physical memory
5. VCPU configured and guest execution begins

## Usage and Execution

### Running the System
When executed, the system performs the following sequence:
1. **seL4 Boot**: Microkernel initializes and creates initial task
2. **CAmkES Initialization**: Components start and establish connections
3. **VM Creation**: VM component creates guest VCPU and memory space
4. **Guest Boot**: FreeRTOS guest begins execution at 0x40000000
5. **Output**: Guest prints "FreeRTOS starting..." followed by periodic "Hello from ARM-VIRT!" messages

### Expected Output
```
FreeRTOS starting...
Hello from ARM-VIRT!
[5 second delay]
Hello from ARM-VIRT!
[5 second delay]
...
```

### Debug and Monitoring
- UART output visible through QEMU console
- seL4 debug output for VM events and fault handling
- CAmkES component communication debugging

## Advanced Features and Extensions

### Potential Enhancements
- **Real FreeRTOS**: Integrate actual FreeRTOS kernel with task scheduling
- **Device Drivers**: Add support for network, storage, or other devices
- **Multi-VM**: Run multiple guest VMs simultaneously
- **Inter-VM Communication**: Implement secure communication channels
- **Performance Monitoring**: Add guest performance measurement capabilities

### Security Considerations
- **Isolation**: Each VM isolated through seL4 capabilities
- **Verification**: Formal verification of microkernel ensures correctness
- **Attack Surface**: Minimal hypervisor reduces potential vulnerabilities
- **Side Channels**: Hardware-based isolation prevents most side-channel attacks

This example serves as a foundation for building secure, verified virtualization systems using seL4 and CAmkES, suitable for high-assurance applications requiring strong isolation guarantees.