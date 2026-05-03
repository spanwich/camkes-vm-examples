/*
 * Single Producer Single Consumer (SPSC) Ring Buffer Implementation
 *
 * Lock-free ring buffer for message passing between CAmkES components
 * Uses memory barriers for correct ordering on ARM platforms
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ringbuf.h"
#include <string.h>
#include <stdint.h>

/* Memory barrier macros for ARM */
#ifdef __arm__
    #define MEMORY_BARRIER() __asm__ volatile ("dmb" ::: "memory")
    #define READ_BARRIER() __asm__ volatile ("dmb" ::: "memory")
    #define WRITE_BARRIER() __asm__ volatile ("dmb" ::: "memory")
#else
    #define MEMORY_BARRIER() __asm__ volatile ("" ::: "memory")
    #define READ_BARRIER() __asm__ volatile ("" ::: "memory")
    #define WRITE_BARRIER() __asm__ volatile ("" ::: "memory")
#endif

/* Find next power of 2 >= n */
static uint32_t next_power_of_2(uint32_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

RingBuffer* rb_init(void* dataport_mem, size_t dataport_size) {
    if (!dataport_mem || dataport_size < sizeof(RingBuffer) + 64) {
        return NULL;
    }

    RingBuffer* rb = (RingBuffer*)dataport_mem;

    /* Calculate data area size and round down to power of 2 */
    size_t data_size = dataport_size - sizeof(RingBuffer);
    uint32_t buffer_size = 1;
    while (buffer_size * 2 <= data_size) {
        buffer_size *= 2;
    }

    /* Initialize ring buffer structure */
    rb->head = 0;
    rb->tail = 0;
    rb->size = buffer_size;
    rb->mask = buffer_size - 1;

    /* Clear data area */
    memset(rb->data, 0, buffer_size);

    WRITE_BARRIER();
    return rb;
}

size_t rb_write_available(const RingBuffer* rb) {
    if (!rb) return 0;

    READ_BARRIER();
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;

    /* Available space = size - used - 1 (leave one byte to distinguish full/empty) */
    uint32_t used = (head - tail) & rb->mask;
    return rb->size - used - 1;
}

size_t rb_read_available(const RingBuffer* rb) {
    if (!rb) return 0;

    READ_BARRIER();
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;

    return (head - tail) & rb->mask;
}

bool rb_write(RingBuffer* rb, const MsgHeader* header, const uint8_t* payload) {
    if (!rb || !header) {
        return false;
    }

    /* Validate header */
    if (!IS_VALID_PAYLOAD_SIZE(header->len)) {
        return false;
    }

    size_t total_size = sizeof(MsgHeader) + header->len;

    /* Check if we have enough space */
    if (rb_write_available(rb) < total_size) {
        return false;
    }

    uint32_t head = rb->head;

    /* Write header */
    for (size_t i = 0; i < sizeof(MsgHeader); i++) {
        rb->data[(head + i) & rb->mask] = ((const uint8_t*)header)[i];
    }
    head += sizeof(MsgHeader);

    /* Write payload if present */
    if (header->len > 0 && payload) {
        for (uint16_t i = 0; i < header->len; i++) {
            rb->data[(head + i) & rb->mask] = payload[i];
        }
        head += header->len;
    }

    /* Commit write by updating head pointer */
    WRITE_BARRIER();
    rb->head = head & rb->mask;

    return true;
}

int rb_peek_header(const RingBuffer* rb, MsgHeader* header) {
    if (!rb || !header) {
        return -1;
    }

    /* Check if we have at least a header */
    size_t available = rb_read_available(rb);
    if (available < sizeof(MsgHeader)) {
        return -1;
    }

    /* Read header without advancing tail */
    uint32_t tail = rb->tail;
    for (size_t i = 0; i < sizeof(MsgHeader); i++) {
        ((uint8_t*)header)[i] = rb->data[(tail + i) & rb->mask];
    }

    /* Validate header and check if complete message is available */
    if (!IS_VALID_PAYLOAD_SIZE(header->len)) {
        return -1;
    }

    size_t total_message_size = sizeof(MsgHeader) + header->len;
    if (available < total_message_size) {
        return -1;  /* Message incomplete */
    }

    return (int)total_message_size;
}

size_t rb_peek_payload(const RingBuffer* rb, uint8_t* dst, size_t max_len) {
    if (!rb || !dst) {
        return 0;
    }

    /* We need to have peeked the header first to know payload size */
    MsgHeader temp_header;
    int total_available = rb_peek_header(rb, &temp_header);
    if (total_available <= 0) {
        return 0;
    }

    size_t payload_len = MIN(temp_header.len, max_len);
    if (payload_len == 0) {
        return 0;
    }

    /* Read payload data starting after header */
    uint32_t payload_start = (rb->tail + sizeof(MsgHeader)) & rb->mask;
    for (size_t i = 0; i < payload_len; i++) {
        dst[i] = rb->data[(payload_start + i) & rb->mask];
    }

    return payload_len;
}

void rb_drop(RingBuffer* rb) {
    if (!rb) return;

    /* Peek header to get message size */
    MsgHeader header;
    int total_size = rb_peek_header(rb, &header);
    if (total_size <= 0) {
        return;  /* No message to drop */
    }

    /* Advance tail to drop the message */
    rb->tail = (rb->tail + total_size) & rb->mask;
    WRITE_BARRIER();
}

void rb_consume(RingBuffer* rb) {
    /* Same implementation as rb_drop for SPSC buffer */
    rb_drop(rb);
}

void rb_get_stats(const RingBuffer* rb, uint32_t* total_writes, uint32_t* total_reads, uint32_t* current_usage) {
    if (!rb) return;

    READ_BARRIER();
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    uint32_t used = (head - tail) & rb->mask;

    if (total_writes) {
        *total_writes = head;  /* Approximate - wraps around */
    }

    if (total_reads) {
        *total_reads = tail;   /* Approximate - wraps around */
    }

    if (current_usage) {
        *current_usage = (used * 100) / rb->size;
    }
}

void rb_reset(RingBuffer* rb) {
    if (!rb) return;

    rb->head = 0;
    rb->tail = 0;
    WRITE_BARRIER();
}

bool rb_is_valid(const RingBuffer* rb) {
    if (!rb) {
        return false;
    }

    /* Check that size is power of 2 */
    if ((rb->size & (rb->size - 1)) != 0) {
        return false;
    }

    /* Check that mask is size - 1 */
    if (rb->mask != rb->size - 1) {
        return false;
    }

    /* Check that head and tail are within bounds */
    if ((rb->head & rb->mask) != rb->head || (rb->tail & rb->mask) != rb->tail) {
        return false;
    }

    return true;
}