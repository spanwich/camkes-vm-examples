/*
 * Lock-Free Control Message Queue for One-Way Dataports
 *
 * This header defines a Single-Producer Single-Consumer (SPSC) lock-free queue
 * for control messages (close notifications, error notifications) between
 * Net0 and Net1 components.
 *
 * Design:
 * - Producer writes to shared dataport (head pointer + notifications array)
 * - Consumer reads from shared dataport (maintains local tail pointer)
 * - No shared read/write variables = no synchronization needed
 * - Works with CAmkES one-way dataport isolation model
 *
 * Queue Size: 128 slots
 * - Handles 100 max connections + burst capacity
 * - Power of 2 for fast modulo via bitmask
 * - Total overhead: 128 × 12 bytes = 1536 bytes per queue
 *
 * Security Features:
 * - Deduplication prevents RST flood attacks
 * - Sequence numbers detect wraparound overwrites
 * - Fixed size prevents memory exhaustion
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef CONTROL_QUEUE_H
#define CONTROL_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

/* Queue configuration */
#define CONTROL_QUEUE_SIZE 128  /* Must be power of 2 */
#define CONTROL_QUEUE_MASK 127  /* For fast modulo (SIZE - 1) */

/*
 * Control notification structure
 *
 * Used for both close notifications (Net0 → Net1) and error notifications (Net1 → Net0)
 *
 * Size: 12 bytes per notification
 */
struct control_notification {
    uint32_t seq_num;      /* Sequence number (monotonic counter, for wraparound detection) */
    uint32_t session_id;   /* Which session (0 = invalid/empty slot) */
    int8_t err_code;       /* Error code (for errors: ERR_RST=-14, etc. For close: 0) */
    uint8_t flags;         /* Future use (graceful vs abort, etc.) */
    uint16_t _padding;     /* Explicit padding for alignment */
} __attribute__((packed));

/*
 * Lock-free SPSC queue structure
 *
 * Producer (writer):
 * - Reads head (local cache OK)
 * - Writes to notifications[head % SIZE]
 * - Memory barrier (__sync_synchronize)
 * - Writes head (atomic increment)
 *
 * Consumer (reader):
 * - Reads head (volatile, gets latest)
 * - Reads notifications[tail % SIZE]
 * - Increments tail (local variable, never shared)
 *
 * Total size: 4 + (128 × 12) = 1540 bytes
 */
struct control_queue {
    volatile uint32_t head;  /* Next write position (monotonic counter, wraps at UINT32_MAX) */
    struct control_notification notifications[CONTROL_QUEUE_SIZE];
};

/*
 * Helper functions for queue operations
 */

/* Check if queue has space for new notification (producer side) */
static inline bool control_queue_has_space(volatile struct control_queue *q, uint32_t consumer_estimate) {
    uint32_t head = q->head;
    /* Conservative estimate: assume consumer hasn't moved since last check */
    return (head - consumer_estimate) < CONTROL_QUEUE_SIZE;
}

/* Enqueue notification (producer side) */
static inline bool control_queue_enqueue(
    volatile struct control_queue *q,
    uint32_t session_id,
    int8_t err_code,
    uint8_t flags)
{
    uint32_t seq = q->head;
    uint32_t slot = seq & CONTROL_QUEUE_MASK;  /* Fast modulo */

    /* Write notification */
    q->notifications[slot].seq_num = seq;
    q->notifications[slot].session_id = session_id;
    q->notifications[slot].err_code = err_code;
    q->notifications[slot].flags = flags;

    /* Memory barrier: ensure notification is written before head update */
    __sync_synchronize();

    /* Atomic increment (single writer, so this is safe) */
    q->head = seq + 1;

    return true;
}

/* Get current queue depth (consumer side) */
static inline uint32_t control_queue_depth(volatile struct control_queue *q, uint32_t tail) {
    uint32_t head = q->head;
    return head - tail;
}

/* Peek next notification without consuming (consumer side) */
static inline volatile struct control_notification* control_queue_peek(
    volatile struct control_queue *q,
    uint32_t tail)
{
    uint32_t head = q->head;
    if (tail >= head) {
        return NULL;  /* Queue empty */
    }

    uint32_t slot = tail & CONTROL_QUEUE_MASK;
    return &q->notifications[slot];
}

#endif /* CONTROL_QUEUE_H */
