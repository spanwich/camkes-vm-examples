/*
 * Single Producer Single Consumer (SPSC) Ring Buffer
 *
 * Lock-free ring buffer implementation for one-way message passing
 * between CAmkES components using shared dataports.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "common.h"

/*
 * Ring buffer structure stored at the beginning of the shared dataport
 * This must be consistent between producer and consumer components
 */
typedef struct {
    volatile uint32_t head;     /* Write position (producer updates) */
    volatile uint32_t tail;     /* Read position (consumer updates) */
    uint32_t size;              /* Total buffer size (must be power of 2) */
    uint32_t mask;              /* Size - 1, for fast modulo operations */
    uint8_t data[];             /* Variable length data area */
} RingBuffer;

/*
 * Initialize ring buffer in shared dataport
 * Should be called by both producer and consumer during initialization
 *
 * @param dataport_mem: Pointer to shared dataport memory
 * @param dataport_size: Total size of dataport
 * @return: Pointer to initialized ring buffer, or NULL on error
 */
RingBuffer* rb_init(void* dataport_mem, size_t dataport_size);

/*
 * Get available space for writing (producer only)
 *
 * @param rb: Ring buffer instance
 * @return: Number of bytes available for writing
 */
size_t rb_write_available(const RingBuffer* rb);

/*
 * Get available data for reading (consumer only)
 *
 * @param rb: Ring buffer instance
 * @return: Number of bytes available for reading
 */
size_t rb_read_available(const RingBuffer* rb);

/*
 * Write a message to ring buffer (producer only)
 * Writes header + payload as atomic operation
 *
 * @param rb: Ring buffer instance
 * @param header: Message header to write
 * @param payload: Payload data to write (can be NULL if header->len == 0)
 * @return: true on success, false if insufficient space
 */
bool rb_write(RingBuffer* rb, const MsgHeader* header, const uint8_t* payload);

/*
 * Peek at next message header without consuming it (consumer only)
 * Allows consumer to check message size before committing to read
 *
 * @param rb: Ring buffer instance
 * @param header: Output buffer for header
 * @return: Number of bytes available including header, or -1 if no complete message
 */
int rb_peek_header(const RingBuffer* rb, MsgHeader* header);

/*
 * Read payload data without consuming the message (consumer only)
 * Must be called after rb_peek_header() to get payload size
 *
 * @param rb: Ring buffer instance
 * @param dst: Destination buffer for payload
 * @param max_len: Maximum bytes to read
 * @return: Number of bytes actually read
 */
size_t rb_peek_payload(const RingBuffer* rb, uint8_t* dst, size_t max_len);

/*
 * Drop the current message without reading payload (consumer only)
 * Used when message fails validation and should be discarded
 */
void rb_drop(RingBuffer* rb);

/*
 * Consume the current message (consumer only)
 * Commits the read operation and advances tail pointer
 */
void rb_consume(RingBuffer* rb);

/*
 * Get ring buffer statistics for debugging
 *
 * @param rb: Ring buffer instance
 * @param total_writes: Output for total write operations (can be NULL)
 * @param total_reads: Output for total read operations (can be NULL)
 * @param current_usage: Output for current buffer usage percentage (can be NULL)
 */
void rb_get_stats(const RingBuffer* rb, uint32_t* total_writes, uint32_t* total_reads, uint32_t* current_usage);

/*
 * Reset ring buffer to empty state
 * Should only be used during initialization or error recovery
 */
void rb_reset(RingBuffer* rb);

/*
 * Check if ring buffer is in a consistent state
 * Used for debugging and error detection
 *
 * @param rb: Ring buffer instance
 * @return: true if buffer state is valid
 */
bool rb_is_valid(const RingBuffer* rb);

/*
 * Calculate required dataport size for given ring buffer capacity
 *
 * @param data_capacity: Desired data capacity in bytes
 * @return: Total dataport size needed (includes ring buffer header)
 */
static inline size_t rb_required_dataport_size(size_t data_capacity) {
    return sizeof(RingBuffer) + data_capacity;
}

/*
 * Get maximum message size that can fit in ring buffer
 *
 * @param rb: Ring buffer instance
 * @return: Maximum message size (header + payload)
 */
static inline size_t rb_max_message_size(const RingBuffer* rb) {
    /* Leave some space to distinguish full from empty buffer */
    return (rb->size - sizeof(MsgHeader) - 1);
}

#endif /* RINGBUF_H */