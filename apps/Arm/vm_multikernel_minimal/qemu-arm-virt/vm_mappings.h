/*
 * Copyright 2025, Kry10 Pty. Ltd.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Generated file with filesizes in it
#include <vm_file_sizes.h>

// #define VM_INITRD_MAX_SIZE 0x08000000 //128 MB
#define VM_INITRD_MAX_SIZE 0x1900000 //25 MB
#define VM_KERNEL_OFFSET 0x00000

#define VM0_RAM_BASE 0x40000000
#define VM0_RAM_SIZE 0x20000000
#define VM0_RAM_OFFSET 0x00000000

#define VM0_DTB_ADDR 0x4F000000
#define VM0_LINUX_ADDR (VM0_RAM_BASE + VM_KERNEL_OFFSET)
#define VM0_INITRD_ADDR 0x4D700000 //VM0_DTB_ADDR - VM_INITRD_MAX_SIZE
#define VM0_LINUX_NAME linux_kernel
#define VM0_LINUX_SIZE _EXPAND_FILESIZE(VM0_LINUX_NAME)
#define VM0_INITRD_NAME linux_initrd
#define VM0_INITRD_SIZE _EXPAND_FILESIZE(VM0_INITRD_NAME)


#define INLINE_BINARY(sym_name, filename)   \
  extern const void* sym_name;       \
  extern const void* sym_name##_end; \
  asm(                                 \
    ".section ._archive_cpio_apps,\"a\" \n \
.globl " #sym_name ", " #sym_name      \
    "_end \n \
.balign 0x1000 \n \
" #sym_name ": \n \
.incbin \"" #filename "\" \n \
" #sym_name "_end: \n");

#define CALL_INLINE_BINARY(sym_name, filename) INLINE_BINARY(sym_name, filename)


#define __EXPAND_FILESIZE(filename) FILE_##filename##_SIZE
#define _EXPAND_FILESIZE(filename) __EXPAND_FILESIZE(filename)