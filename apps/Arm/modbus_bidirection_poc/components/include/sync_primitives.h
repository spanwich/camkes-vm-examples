/*
 * sync_primitives.h
 *
 * Synchronization primitives for seL4/CAmkES VirtIO network drivers
 * Provides lock-free and spinlock-based synchronization for single-core systems
 *
 * Usage:
 *   - Spinlock: For protecting shared data structures (connection tables, pools)
 *   - Seqlock: For single-writer, multiple-reader scenarios (shared state between Net0/Net1)
 *   - Atomic reentrancy guard: For preventing callback reentrancy
 */

#ifndef SYNC_PRIMITIVES_H
#define SYNC_PRIMITIVES_H

/* Use GCC built-in atomics instead of stdatomic.h (not available with -nostdinc) */
#include <stdbool.h>
#include <stdint.h>
#include <sel4/sel4.h>

/* Define _Atomic as GCC extension */
#ifndef _Atomic
#define _Atomic volatile
#endif

/*
 * Spinlock - For critical sections in single-core seL4
 *
 * Uses cooperative yield since seL4 doesn't preempt within a component.
 * Suitable for protecting short critical sections like connection table access.
 *
 * Example usage:
 *   static spinlock_t my_lock = SPINLOCK_INIT;
 *
 *   spin_lock(&my_lock);
 *   // ... critical section ...
 *   spin_unlock(&my_lock);
 */
typedef struct {
    _Atomic int locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

static inline void spin_lock(spinlock_t *lock) {
    int expected = 0;
    while (!__atomic_compare_exchange_n(
            &lock->locked, &expected, 1, 1,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 0;
        seL4_Yield();  /* Cooperative yield to avoid busy-waiting */
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

static inline bool spin_trylock(spinlock_t *lock) {
    int expected = 0;
    return __atomic_compare_exchange_n(
        &lock->locked, &expected, 1, 0,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/*
 * Seqlock - For single-writer, multiple-reader scenarios
 *
 * Perfect for shared state between Net0 and Net1 where one driver updates
 * and the other reads. Provides lock-free reads with retry on write conflict.
 *
 * Writer example:
 *   seqlock_write_begin(&lock);
 *   // ... update shared data ...
 *   seqlock_write_end(&lock);
 *
 * Reader example:
 *   uint32_t seq;
 *   do {
 *       seq = seqlock_read_begin(&lock);
 *       // ... read shared data ...
 *   } while (seqlock_read_retry(&lock, seq));
 */
typedef struct {
    _Atomic uint32_t sequence;
} seqlock_t;

#define SEQLOCK_INIT {0}

static inline void seqlock_write_begin(seqlock_t *sl) {
    /* Increment to odd (write in progress) */
    __atomic_fetch_add(&sl->sequence, 1, __ATOMIC_RELEASE);
}

static inline void seqlock_write_end(seqlock_t *sl) {
    /* Increment to even (write complete, data consistent) */
    __atomic_fetch_add(&sl->sequence, 1, __ATOMIC_RELEASE);
}

static inline uint32_t seqlock_read_begin(seqlock_t *sl) {
    uint32_t seq;
    do {
        seq = __atomic_load_n(&sl->sequence, __ATOMIC_ACQUIRE);
    } while (seq & 1);  /* Wait for even (no write in progress) */
    return seq;
}

static inline bool seqlock_read_retry(seqlock_t *sl, uint32_t start_seq) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&sl->sequence, __ATOMIC_RELAXED) != start_seq;
}

/*
 * Atomic reentrancy guard - For IRQ/callback safety
 *
 * Prevents multiple invocations of a callback or IRQ handler from executing
 * concurrently. Uses atomic test-and-set for lock-free mutual exclusion.
 *
 * Example usage:
 *   static bool in_callback = false;
 *
 *   if (!try_enter_critical(&in_callback)) {
 *       return;  // Already processing
 *   }
 *   // ... do work ...
 *   exit_critical(&in_callback);
 */
static inline bool try_enter_critical(bool *flag) {
    return !__atomic_test_and_set(flag, __ATOMIC_ACQUIRE);
}

static inline void exit_critical(bool *flag) {
    __atomic_clear(flag, __ATOMIC_RELEASE);
}

/*
 * Atomic counter helpers - For thread-safe counters
 *
 * Provides safe increment/decrement with underflow protection.
 *
 * Example usage:
 *   static _Atomic uint32_t connection_count = 0;
 *
 *   atomic_fetch_add(&connection_count, 1);
 *
 *   uint32_t old = atomic_fetch_sub(&connection_count, 1);
 *   if (old == 0) {
 *       atomic_fetch_add(&connection_count, 1);  // Restore
 *   }
 */
static inline uint32_t atomic_increment_safe(volatile _Atomic uint32_t *counter) {
    return __atomic_fetch_add(counter, 1, __ATOMIC_ACQ_REL);
}

static inline uint32_t atomic_decrement_safe(volatile _Atomic uint32_t *counter) {
    uint32_t old = __atomic_fetch_sub(counter, 1, __ATOMIC_ACQ_REL);
    if (old == 0) {
        /* Underflow detected - restore and return error */
        __atomic_fetch_add(counter, 1, __ATOMIC_ACQ_REL);
        return UINT32_MAX;  /* Error indicator */
    }
    return old - 1;  /* New value */
}

static inline uint32_t atomic_load_safe(volatile _Atomic uint32_t *counter) {
    return __atomic_load_n(counter, __ATOMIC_ACQUIRE);
}

static inline void atomic_store_safe(volatile _Atomic uint32_t *counter, uint32_t value) {
    __atomic_store_n(counter, value, __ATOMIC_RELEASE);
}

#endif /* SYNC_PRIMITIVES_H */
