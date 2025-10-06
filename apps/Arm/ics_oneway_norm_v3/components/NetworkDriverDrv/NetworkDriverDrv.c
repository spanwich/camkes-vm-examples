/*
 * Copyright 2024, PhD Research Project
 * Network Driver Component - VirtIO-Net Driver Implementation
 *
 * Based on VirtIO 1.2 specification and sDDF patterns
 * Provides real VirtIO-Net MMIO device driver for external packet injection
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <camkes.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <virtio/virtio_ring.h>
#include <virtio/virtio_config.h>

#include "common.h"
#include "ring_buffer.h"

// VirtIO MMIO register offsets (from VirtIO spec section 4.2.2)
#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc

// VirtIO status bits
#define VIRTIO_STATUS_ACKNOWLEDGE       1
#define VIRTIO_STATUS_DRIVER            2
#define VIRTIO_STATUS_DRIVER_OK         4
#define VIRTIO_STATUS_FEATURES_OK       8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED            128

// VirtIO device IDs
#define VIRTIO_ID_NET                   1

// VirtIO-Net specific
#define VIRTIO_NET_RX_QUEUE             0
#define VIRTIO_NET_TX_QUEUE             1
#define VIRTIO_NET_HDR_SIZE             12

// Hardware addresses (from QEMU qemu-arm-virt configuration)
#define VIRTIO_MMIO_SLOT_SIZE           0x200      // 512 bytes per slot
#define VIRTIO_MMIO_BASE_ADDR           0xa000000  // First VirtIO MMIO slot
#define VIRTIO_NET_BASE_IRQ             16         // IRQ for first slot

// Queue sizes
#define RX_QUEUE_SIZE                   256
#define TX_QUEUE_SIZE                   256

// Memory layout for virtqueues
#define VRING_ALIGN                     4096
#define MAX_FRAME_SIZE                  1518  // Standard Ethernet MTU + headers

/* VirtIO-Net device state */
typedef struct {
    volatile uint32_t *mmio_regs;

    // RX virtqueue
    struct vring rx_vring;
    uint16_t rx_last_seen_used;

    // Memory regions for virtqueue
    void *rx_ring_memory;

    // VirtIO-Net headers (separate from packet data)
    uint8_t *virtio_net_headers;

    // Packet buffers
    uint8_t *rx_buffers[RX_QUEUE_SIZE];

    // Descriptor free list
    uint16_t free_desc_head;
    uint16_t free_desc[RX_QUEUE_SIZE];

    bool initialized;
} virtio_net_device_t;

static virtio_net_device_t net_dev;

// Output to ICS pipeline (existing interface)
static RingBuffer *output_buffer;
static uint32_t message_counter = 0;

// Forward declarations
static bool virtio_net_init_device(void);
static void process_ethernet_frame(uint8_t *frame_data, size_t frame_len);

/* Helper macros for MMIO register access */
#define VIRTIO_REG(offset) (*(volatile uint32_t *)((uintptr_t)net_dev.mmio_regs + (offset)))

static inline uint32_t virtio_read32(uint32_t offset) {
    return VIRTIO_REG(offset);
}

static inline void virtio_write32(uint32_t offset, uint32_t value) {
    VIRTIO_REG(offset) = value;
}

/* Memory barrier helpers (ARM-specific) */
static inline void memory_barrier(void) {
    __asm__ __volatile__("dmb sy" : : : "memory");
}

/* Simple descriptor allocator for virtqueue management */
static void init_desc_allocator(void) {
    for (int i = 0; i < RX_QUEUE_SIZE - 1; i++) {
        net_dev.free_desc[i] = i + 1;
    }
    net_dev.free_desc[RX_QUEUE_SIZE - 1] = 0xFFFF;  // End marker
    net_dev.free_desc_head = 0;
}

static int alloc_desc(uint16_t *desc_idx) {
    if (net_dev.free_desc_head == 0xFFFF) {
        return -1;  // No free descriptors
    }

    *desc_idx = net_dev.free_desc_head;
    net_dev.free_desc_head = net_dev.free_desc[*desc_idx];
    return 0;
}

static void free_desc(uint16_t desc_idx) {
    net_dev.free_desc[desc_idx] = net_dev.free_desc_head;
    net_dev.free_desc_head = desc_idx;
}

void pre_init(void) {
    printf("NetworkDriverDrv: Pre-initialization\n");
    memset(&net_dev, 0, sizeof(net_dev));
}

static bool virtio_net_reset_device(void) {
    printf("NetworkDriverDrv: Resetting VirtIO device\n");

    // Reset device
    virtio_write32(VIRTIO_MMIO_STATUS, 0);

    // Wait for reset to complete
    for (volatile int i = 0; i < 1000; i++);

    return true;
}

static bool virtio_net_check_device(void) {
    // Check magic value (should be 0x74726976 = "virt")
    uint32_t magic = virtio_read32(VIRTIO_MMIO_MAGIC_VALUE);
    if (magic != 0x74726976) {
        printf("NetworkDriverDrv: ERROR - Invalid magic value: 0x%x (expected 0x74726976)\n", magic);
        return false;
    }
    printf("NetworkDriverDrv: VirtIO magic value correct: 0x%x\n", magic);

    // Check version (should be 2 for VirtIO 1.0+)
    uint32_t version = virtio_read32(VIRTIO_MMIO_VERSION);
    if (version != 2 && version != 1) {
        printf("NetworkDriverDrv: WARNING - VirtIO version %u (expected 1 or 2)\n", version);
    }
    printf("NetworkDriverDrv: VirtIO version: %u\n", version);

    // Check device ID (should be 1 for network device)
    uint32_t device_id = virtio_read32(VIRTIO_MMIO_DEVICE_ID);

    // DEBUG: Show what device is actually here
    const char *device_type = "unknown";
    if (device_id == 0) device_type = "none/invalid";
    else if (device_id == 1) device_type = "network";
    else if (device_id == 2) device_type = "block";
    else if (device_id == 3) device_type = "console";
    else if (device_id == 4) device_type = "rng";
    else if (device_id == 5) device_type = "balloon";
    else if (device_id == 9) device_type = "9p";

    printf("NetworkDriverDrv: Device ID: %u (%s)\n", device_id, device_type);

    if (device_id != VIRTIO_ID_NET) {
        printf("NetworkDriverDrv: ERROR - Wrong device type (expected network device)\n");
        printf("NetworkDriverDrv: HINT - Network device might be at a different MMIO address\n");
        return false;
    }
    printf("NetworkDriverDrv: VirtIO network device detected!\n");

    // Check vendor ID
    uint32_t vendor_id = virtio_read32(VIRTIO_MMIO_VENDOR_ID);
    printf("NetworkDriverDrv: Vendor ID: 0x%x\n", vendor_id);

    return true;
}

static bool virtio_net_negotiate_features(void) {
    printf("NetworkDriverDrv: Negotiating features\n");

    // Read device features (page 0)
    virtio_write32(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint32_t features_low = virtio_read32(VIRTIO_MMIO_DEVICE_FEATURES);
    printf("NetworkDriverDrv: Device features (low): 0x%x\n", features_low);

    // For minimal driver, we don't need any special features
    // Just accept basic operation
    virtio_write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    virtio_write32(VIRTIO_MMIO_DRIVER_FEATURES, 0);  // No special features

    // Set FEATURES_OK status
    uint32_t status = virtio_read32(VIRTIO_MMIO_STATUS);
    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_write32(VIRTIO_MMIO_STATUS, status);

    // Verify FEATURES_OK was accepted
    status = virtio_read32(VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        printf("NetworkDriverDrv: ERROR - Device rejected features\n");
        return false;
    }

    printf("NetworkDriverDrv: Feature negotiation complete\n");
    return true;
}

static bool virtio_net_setup_rx_queue(void) {
    printf("NetworkDriverDrv: Setting up RX virtqueue\n");

    // Select RX queue (queue 0)
    virtio_write32(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_RX_QUEUE);

    // Check maximum queue size
    uint32_t queue_num_max = virtio_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("NetworkDriverDrv: RX queue max size: %u\n", queue_num_max);

    if (queue_num_max < RX_QUEUE_SIZE) {
        printf("NetworkDriverDrv: ERROR - Queue too small (max: %u, need: %u)\n",
               queue_num_max, RX_QUEUE_SIZE);
        return false;
    }

    // Set queue size
    virtio_write32(VIRTIO_MMIO_QUEUE_NUM, RX_QUEUE_SIZE);

    // Allocate memory for virtqueue structures
    // Align to page boundary
    static uint8_t rx_vring_memory[16384] __attribute__((aligned(4096)));
    net_dev.rx_ring_memory = rx_vring_memory;

    // Initialize vring structure
    vring_init(&net_dev.rx_vring, RX_QUEUE_SIZE, net_dev.rx_ring_memory, VRING_ALIGN);

    printf("NetworkDriverDrv: RX vring initialized at %p\n", net_dev.rx_ring_memory);
    printf("NetworkDriverDrv:   Descriptor table: %p\n", net_dev.rx_vring.desc);
    printf("NetworkDriverDrv:   Available ring: %p\n", net_dev.rx_vring.avail);
    printf("NetworkDriverDrv:   Used ring: %p\n", net_dev.rx_vring.used);

    // Get physical addresses (critical for DMA)
    // NOTE: In full implementation, need to use seL4_ARM_Page_GetAddress()
    // For QEMU simulation, virtual addresses may work if IOMMU is disabled
    uintptr_t desc_paddr = (uintptr_t)net_dev.rx_vring.desc;
    uintptr_t avail_paddr = (uintptr_t)net_dev.rx_vring.avail;
    uintptr_t used_paddr = (uintptr_t)net_dev.rx_vring.used;

    // Write queue addresses to device
    virtio_write32(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)(desc_paddr & 0xFFFFFFFF));
    virtio_write32(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_paddr >> 32));

    virtio_write32(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)(avail_paddr & 0xFFFFFFFF));
    virtio_write32(VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail_paddr >> 32));

    virtio_write32(VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)(used_paddr & 0xFFFFFFFF));
    virtio_write32(VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used_paddr >> 32));

    // Mark queue as ready
    virtio_write32(VIRTIO_MMIO_QUEUE_READY, 1);

    printf("NetworkDriverDrv: RX queue configured and ready\n");
    return true;
}

static bool virtio_net_allocate_buffers(void) {
    printf("NetworkDriverDrv: Allocating RX buffers\n");

    // Allocate static buffers for packet reception
    static uint8_t rx_buffer_pool[RX_QUEUE_SIZE][MAX_FRAME_SIZE] __attribute__((aligned(64)));

    // Allocate VirtIO-Net headers (separate from packet data)
    static uint8_t virtio_headers[RX_QUEUE_SIZE][VIRTIO_NET_HDR_SIZE] __attribute__((aligned(64)));
    net_dev.virtio_net_headers = (uint8_t *)virtio_headers;

    // Setup buffer pointers
    for (int i = 0; i < RX_QUEUE_SIZE; i++) {
        net_dev.rx_buffers[i] = rx_buffer_pool[i];
    }

    printf("NetworkDriverDrv: Allocated %u RX buffers (%u bytes each)\n",
           RX_QUEUE_SIZE, MAX_FRAME_SIZE);

    return true;
}

static bool virtio_net_refill_rx_queue(void) {
    printf("NetworkDriverDrv: Refilling RX queue with buffers\n");

    int refilled = 0;

    for (int i = 0; i < RX_QUEUE_SIZE; i++) {
        // Allocate two descriptors: one for header, one for packet
        uint16_t hdr_desc_idx, pkt_desc_idx;

        if (alloc_desc(&hdr_desc_idx) < 0) {
            printf("NetworkDriverDrv: ERROR - Failed to allocate header descriptor\n");
            break;
        }

        if (alloc_desc(&pkt_desc_idx) < 0) {
            printf("NetworkDriverDrv: ERROR - Failed to allocate packet descriptor\n");
            free_desc(hdr_desc_idx);
            break;
        }

        // Setup header descriptor (write-only for device)
        uintptr_t hdr_addr = (uintptr_t)&net_dev.virtio_net_headers[hdr_desc_idx * VIRTIO_NET_HDR_SIZE];
        net_dev.rx_vring.desc[hdr_desc_idx].addr = hdr_addr;
        net_dev.rx_vring.desc[hdr_desc_idx].len = VIRTIO_NET_HDR_SIZE;
        net_dev.rx_vring.desc[hdr_desc_idx].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
        net_dev.rx_vring.desc[hdr_desc_idx].next = pkt_desc_idx;

        // Setup packet descriptor (write-only for device)
        uintptr_t pkt_addr = (uintptr_t)net_dev.rx_buffers[i];
        net_dev.rx_vring.desc[pkt_desc_idx].addr = pkt_addr;
        net_dev.rx_vring.desc[pkt_desc_idx].len = MAX_FRAME_SIZE;
        net_dev.rx_vring.desc[pkt_desc_idx].flags = VRING_DESC_F_WRITE;
        net_dev.rx_vring.desc[pkt_desc_idx].next = 0;  // Last in chain

        // Add to available ring
        net_dev.rx_vring.avail->ring[net_dev.rx_vring.avail->idx % RX_QUEUE_SIZE] = hdr_desc_idx;
        memory_barrier();
        net_dev.rx_vring.avail->idx++;

        refilled++;
    }

    // Notify device that buffers are available
    virtio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_NET_RX_QUEUE);

    printf("NetworkDriverDrv: Refilled %d RX buffers\n", refilled);
    return refilled > 0;
}

void post_init(void) {
    printf("NetworkDriverDrv: Post-initialization\n");

    // Initialize VirtIO device
    if (!virtio_net_init_device()) {
        printf("NetworkDriverDrv: FATAL - Device initialization failed\n");
        return;
    }

    // Initialize output ring buffer (to ICS pipeline)
    output_buffer = rb_init(out_dp, 65536);
    if (!output_buffer || !rb_is_valid(output_buffer)) {
        printf("NetworkDriverDrv: FATAL - Output buffer initialization failed\n");
        return;
    }

    printf("NetworkDriverDrv: Output buffer initialized (size=%u bytes)\n",
           output_buffer->size);
    printf("NetworkDriverDrv: Initialization complete - waiting for network traffic\n");
}

void eth_irq_handle(void) {
    // Placeholder for Phase 3
    printf("NetworkDriverDrv: IRQ handler called (not yet implemented)\n");
}

static bool virtio_net_init_device(void) {
    printf("NetworkDriverDrv: Initializing VirtIO-Net device\n");

    // Step 1: Scan VirtIO MMIO slots to find the network device
    // We have 8KB mapped starting at 0xa000000, covering slots 0-15
    printf("NetworkDriverDrv: Scanning VirtIO MMIO slots for network device...\n");

    volatile uint32_t *base_regs = (volatile uint32_t *)virtio_mmio;
    bool found = false;
    int slot_found = -1;

    for (int slot = 0; slot < 16; slot++) {
        volatile uint32_t *slot_regs = (volatile uint32_t *)((uintptr_t)base_regs + (slot * VIRTIO_MMIO_SLOT_SIZE));

        // Check magic value
        uint32_t magic = slot_regs[VIRTIO_MMIO_MAGIC_VALUE / 4];
        if (magic != 0x74726976) {
            continue;  // Not a valid VirtIO device
        }

        // Check device ID
        uint32_t device_id = slot_regs[VIRTIO_MMIO_DEVICE_ID / 4];

        printf("NetworkDriverDrv: Slot %d (0x%lx): Device ID %u\n",
               slot, VIRTIO_MMIO_BASE_ADDR + (slot * VIRTIO_MMIO_SLOT_SIZE), device_id);

        if (device_id == VIRTIO_ID_NET) {
            printf("NetworkDriverDrv: Found network device at slot %d!\n", slot);
            net_dev.mmio_regs = slot_regs;
            slot_found = slot;
            found = true;
            break;
        }
    }

    if (!found) {
        printf("NetworkDriverDrv: ERROR - No network device found in scanned slots\n");
        return false;
    }

    printf("NetworkDriverDrv: MMIO base at 0x%lx (slot %d, IRQ %d)\n",
           (uintptr_t)net_dev.mmio_regs, slot_found, VIRTIO_NET_BASE_IRQ + slot_found);

    // Step 2: Reset device
    if (!virtio_net_reset_device()) {
        return false;
    }

    // Step 3: Set ACKNOWLEDGE status
    virtio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    // Step 4: Check device identity
    if (!virtio_net_check_device()) {
        return false;
    }

    // Step 5: Set DRIVER status
    uint32_t status = virtio_read32(VIRTIO_MMIO_STATUS);
    status |= VIRTIO_STATUS_DRIVER;
    virtio_write32(VIRTIO_MMIO_STATUS, status);

    // Step 6: Negotiate features
    if (!virtio_net_negotiate_features()) {
        return false;
    }

    // Step 7: Initialize descriptor allocator
    init_desc_allocator();

    // Step 8: Allocate packet buffers
    if (!virtio_net_allocate_buffers()) {
        return false;
    }

    // Step 9: Setup RX virtqueue
    if (!virtio_net_setup_rx_queue()) {
        return false;
    }

    // Step 10: Refill RX queue with buffers
    if (!virtio_net_refill_rx_queue()) {
        return false;
    }

    // Step 11: Set DRIVER_OK status
    status = virtio_read32(VIRTIO_MMIO_STATUS);
    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_write32(VIRTIO_MMIO_STATUS, status);

    net_dev.rx_last_seen_used = 0;
    net_dev.initialized = true;

    printf("NetworkDriverDrv: VirtIO-Net device initialization complete\n");
    printf("NetworkDriverDrv: Ready to receive network packets\n");

    return true;
}

static void process_ethernet_frame(uint8_t *frame_data, size_t frame_len) {
    // Placeholder for Phase 2
    // Will implement Ethernet/IP/TCP parsing and forwarding to ICS pipeline
    printf("NetworkDriverDrv: Frame processing (Phase 2 - not yet implemented)\n");
}

// Main entry point for CAmkES control component
int run(void) {
    printf("NetworkDriverDrv: Starting (Phase 1 - initialization only)\n");

    if (!net_dev.initialized) {
        printf("NetworkDriverDrv: ERROR - Device not initialized\n");
        return -1;
    }

    printf("NetworkDriverDrv: Phase 1 complete - device ready\n");
    printf("NetworkDriverDrv: Waiting for Phase 2 implementation (packet reception)\n");

    // No polling loop in Phase 1 - just wait
    return 0;
}