/*
 * Copyright 2024, PhD Research Project
 * sDDF-Inspired Virtio Network Algorithms for CAmkES
 *
 * Simplified virtio-net algorithms based on sDDF patterns
 * Adapted for CAmkES framework without VirtQueue memory overhead
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* VirtIO network device registers (simplified) */
#define VIRTIO_MMIO_MAGIC_VALUE     0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00c
#define VIRTIO_MMIO_STATUS          0x070

/* VirtIO status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8

/* VirtIO network frame header (simplified) */
typedef struct {
    uint16_t flags;
    uint16_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed)) virtio_net_hdr_t;

/* Function prototypes for sDDF-inspired algorithms */
bool virtio_net_device_init(volatile uint32_t *regs);
bool virtio_net_receive_available(volatile uint32_t *regs);
size_t virtio_net_read_frame(volatile uint32_t *regs, uint8_t *buffer, size_t max_len);
void virtio_net_frame_processed(volatile uint32_t *regs);