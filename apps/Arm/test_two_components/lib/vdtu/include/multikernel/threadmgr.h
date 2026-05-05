/*
 * threadmgr.h -- Cooperative thread manager for SemperKernel-on-multikernel
 *
 * Port of SemperOS m3::ThreadManager (components/SemperKernel/src/include/thread/)
 * minus the C++ class hierarchy. Provides setjmp/longjmp-based cooperative
 * context switching with wait/notify semantics.
 *
 * Threads are full callable closures with their own 16 KiB stack, NOT thin
 * continuations (Multikernel-AMP architecture §4 — cooperative scheduler is
 * the chosen model precisely so revocation code stays sequential and nested
 * protocol operations compose with natural function-call semantics).
 *
 * The dispatch model is single-CPU cooperative: only one thread runs at a
 * time, switches happen at explicit yield/wait_for/notify points. This is
 * fine since the rootserver runs on a single seL4 thread per kernel.
 *
 * Usage:
 *   mk_threadmgr_init();
 *   mk_thread_spawn(handler, &handler_arg);
 *   ...
 *   mk_threadmgr_run();   // blocks until all threads end or sleep
 */

#ifndef MULTIKERNEL_THREADMGR_H
#define MULTIKERNEL_THREADMGR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MK_THREAD_MAX           32
#define MK_THREAD_STACK_SIZE    (16 * 1024)

typedef void (*mk_thread_fn)(void *arg);

enum mk_thread_state {
    MK_THREAD_FREE = 0,
    MK_THREAD_READY,
    MK_THREAD_BLOCKED,
    MK_THREAD_DEAD,
};

struct mk_thread {
    int                  id;
    enum mk_thread_state state;
    void                *event;       /* event currently waited on (key for notify) */
    jmp_buf              ctx;
    uint8_t             *stack;       /* 16 KiB heap-allocated, owned                */
    mk_thread_fn         fn;          /* entry point (NULL once first-started)       */
    void                *arg;
    /* Optional payload delivered via notify; NULL if event-only wakeup. */
    void                *notify_msg;
    size_t               notify_len;
    /* Singly-linked list pointer (for ready / blocked queues). */
    struct mk_thread    *next;
};

/* One-shot init. Establishes the "main" thread (caller of init) so that
 * thread switches always have a valid old context to setjmp into. */
void mk_threadmgr_init(void);

/* Spawn a new ready thread that begins at fn(arg). Returns NULL on
 * out-of-thread-slots or stack alloc failure. The thread's stack is
 * malloc()'d and freed on thread death. */
struct mk_thread *mk_thread_spawn(mk_thread_fn fn, void *arg);

/* Yield to the next ready thread. Returns when scheduled again. */
void mk_thread_yield(void);

/* Block the current thread until something calls mk_thread_notify(event).
 * If notify carries a payload (notify with msg+len), it is copied into
 * static notify_msg/notify_len fields on the thread; pass NULL to ignore. */
void mk_thread_wait_for(const void *event);

/* Wake every thread blocked on this event key. msg/size are optional;
 * pass {NULL,0} for event-only wakeups. The waker does NOT yield —
 * waiters land on the ready queue and run on the next yield/wait_for. */
void mk_thread_notify(const void *event, const void *msg, size_t size);

/* Run threads until none are ready and none are blocked. Used by the
 * KernelcallHandler dispatch loop to drive worker threads to completion
 * after enqueuing wakeups. */
void mk_threadmgr_run_until_idle(void);

/* Currently running thread (NULL only before mk_threadmgr_init).        */
struct mk_thread *mk_thread_current(void);

/* Diagnostics. */
size_t mk_thread_count_ready(void);
size_t mk_thread_count_blocked(void);

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_THREADMGR_H */
