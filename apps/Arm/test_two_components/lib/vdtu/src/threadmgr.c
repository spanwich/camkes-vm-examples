/*
 * threadmgr.c -- Cooperative thread manager (musl jmp_buf, AArch64)
 *
 * Layout of musl's AArch64 jmp_buf (from src/setjmp/aarch64/setjmp.s):
 *     offset  0..  15:  x19, x20      (callee-saved)
 *     offset 16..  31:  x21, x22
 *     offset 32..  47:  x23, x24
 *     offset 48..  63:  x25, x26
 *     offset 64..  79:  x27, x28
 *     offset 80..  95:  x29 (FP), x30 (LR)
 *     offset 104.. 111: sp
 *     offset 112.. 175: d8..d15
 *
 * To launch a fresh thread we hand-craft a jmp_buf with x30 (LR) set to the
 * trampoline and sp set to the top of the new stack. longjmp restores those
 * registers and `ret`s, jumping into the trampoline on the new stack.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "multikernel/threadmgr.h"

#if !defined(__aarch64__)
#error "threadmgr currently assumes aarch64 jmp_buf layout (musl)"
#endif

/* Byte offsets into musl's jmp_buf. Treat as uint64_t array indices /8. */
#define JB_LR_IDX   11   /* x30 / LR — offset 88 */
#define JB_SP_IDX   13   /* sp     — offset 104 */

#define MK_STACK_ALIGN  16ull

static struct mk_thread g_threads[MK_THREAD_MAX];
static struct mk_thread *g_current = NULL;
static struct mk_thread *g_ready_head = NULL;   /* ready queue (FIFO) */
static struct mk_thread *g_ready_tail = NULL;
static struct mk_thread *g_blocked_head = NULL; /* unordered list */

/* The thread struct that's about to be started for the first time. The
 * trampoline reads this just before running fn(arg). It's necessary
 * because longjmp can't pass arguments through registers safely. */
static struct mk_thread *g_starting = NULL;

/* Forward declaration. */
static void mk_thread_trampoline(void);

void mk_threadmgr_init(void)
{
    memset(g_threads, 0, sizeof(g_threads));
    g_ready_head = g_ready_tail = NULL;
    g_blocked_head = NULL;

    /* Slot 0 is reserved for the main thread (the caller of init). */
    g_threads[0].id = 0;
    g_threads[0].state = MK_THREAD_READY;
    g_threads[0].fn = NULL;       /* never re-entered via trampoline */
    g_threads[0].next = NULL;
    g_current = &g_threads[0];
}

struct mk_thread *mk_thread_current(void) { return g_current; }

static struct mk_thread *alloc_thread(void)
{
    /* Reap any DEAD slots first — frees their stacks and recycles the
     * mk_thread struct. */
    for (int i = 1; i < MK_THREAD_MAX; ++i) {
        if (g_threads[i].state == MK_THREAD_DEAD) {
            free(g_threads[i].stack);
            g_threads[i].stack = NULL;
            g_threads[i].state = MK_THREAD_FREE;
        }
    }
    for (int i = 1; i < MK_THREAD_MAX; ++i) {
        if (g_threads[i].state == MK_THREAD_FREE) {
            g_threads[i].id = i;
            return &g_threads[i];
        }
    }
    return NULL;
}

static void enqueue_ready(struct mk_thread *t)
{
    t->next = NULL;
    t->state = MK_THREAD_READY;
    if (g_ready_tail) {
        g_ready_tail->next = t;
        g_ready_tail = t;
    } else {
        g_ready_head = g_ready_tail = t;
    }
}

static struct mk_thread *dequeue_ready(void)
{
    struct mk_thread *t = g_ready_head;
    if (!t) return NULL;
    g_ready_head = t->next;
    if (!g_ready_head) g_ready_tail = NULL;
    t->next = NULL;
    return t;
}

static void enqueue_blocked(struct mk_thread *t)
{
    t->state = MK_THREAD_BLOCKED;
    t->next = g_blocked_head;
    g_blocked_head = t;
}

static void unblock_one(struct mk_thread *t,
                        const void *msg, size_t size)
{
    /* Remove t from blocked list. */
    struct mk_thread **pp = &g_blocked_head;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp == t) *pp = t->next;
    t->next = NULL;
    t->event = NULL;
    t->notify_msg = (void *)msg;
    t->notify_len = size;
    enqueue_ready(t);
}

/* Switch to thread `next`. The current thread's context is captured via
 * setjmp; longjmp resumes it later. For a fresh thread the longjmp lands
 * in mk_thread_trampoline on the thread's own stack. */
static void switch_to(struct mk_thread *next)
{
    if (next == g_current) return;
    struct mk_thread *prev = g_current;
    if (setjmp(prev->ctx) == 0) {
        g_current = next;
        if (next->fn) {
            /* First start: tell the trampoline which thread to run. */
            g_starting = next;
        }
        longjmp(next->ctx, 1);
        /* unreachable */
    }
    /* Resumed via longjmp from elsewhere — fall back into caller. */
}

static void mk_thread_trampoline(void)
{
    struct mk_thread *t = g_starting;
    g_starting = NULL;
    /* Once we're running on the new stack, the entry function is consumed
     * and we treat the thread like any other (fn=NULL means "resume via
     * longjmp into prior setjmp site", not "first-start trampoline"). */
    mk_thread_fn fn = t->fn;
    void *arg = t->arg;
    t->fn = NULL;
    t->arg = NULL;

    fn(arg);

    /* Thread function returned — clean up and yield permanently. */
    t->state = MK_THREAD_DEAD;
    /* Free stack on next reaping; for now leak until threadmgr teardown. */
    struct mk_thread *next = dequeue_ready();
    if (!next) {
        fprintf(stderr, "mk_thread: deadlock — last thread died with no runnable peers\n");
        for (;;) {}
    }
    g_current = next;
    longjmp(next->ctx, 1);
}

struct mk_thread *mk_thread_spawn(mk_thread_fn fn, void *arg)
{
    if (!fn) return NULL;
    struct mk_thread *t = alloc_thread();
    if (!t) return NULL;
    t->stack = (uint8_t *)malloc(MK_THREAD_STACK_SIZE);
    if (!t->stack) {
        t->state = MK_THREAD_FREE;
        return NULL;
    }
    t->fn = fn;
    t->arg = arg;
    t->event = NULL;
    t->notify_msg = NULL;
    t->notify_len = 0;
    t->next = NULL;

    /* Hand-craft the jmp_buf so the first longjmp lands in the
     * trampoline on the thread's own stack. */
    memset(&t->ctx, 0, sizeof(t->ctx));
    uint64_t *jb = (uint64_t *)&t->ctx;
    uint64_t sp_top = ((uint64_t)t->stack + MK_THREAD_STACK_SIZE) & ~(MK_STACK_ALIGN - 1);
    jb[JB_LR_IDX] = (uint64_t)&mk_thread_trampoline;
    jb[JB_SP_IDX] = sp_top;

    enqueue_ready(t);
    return t;
}

void mk_thread_yield(void)
{
    struct mk_thread *next = dequeue_ready();
    if (!next) return;       /* nothing else to run; just continue */
    enqueue_ready(g_current);
    switch_to(next);
}

void mk_thread_wait_for(const void *event)
{
    g_current->event = (void *)event;
    enqueue_blocked(g_current);
    struct mk_thread *next = dequeue_ready();
    if (!next) {
        /* No ready peer to switch to — caller must arrange events to be
         * notified before reaching this state, otherwise we'd hang. */
        fprintf(stderr,
                "mk_thread: wait_for(%p) deadlock — no ready threads\n",
                event);
        for (;;) {}
    }
    switch_to(next);
    /* Resumed — caller resumes after the wait. notify_msg/len are set. */
}

void mk_thread_notify(const void *event, const void *msg, size_t size)
{
    /* Wake every blocked thread whose event matches. */
    struct mk_thread *t = g_blocked_head;
    while (t) {
        struct mk_thread *next_t = t->next;
        if (t->event == event) {
            unblock_one(t, msg, size);
        }
        t = next_t;
    }
}

void mk_threadmgr_run_until_idle(void)
{
    while (g_ready_head) {
        struct mk_thread *next = dequeue_ready();
        enqueue_ready(g_current);
        switch_to(next);
    }
}

size_t mk_thread_count_ready(void)
{
    size_t n = 0;
    for (struct mk_thread *t = g_ready_head; t; t = t->next) ++n;
    return n;
}

size_t mk_thread_count_blocked(void)
{
    size_t n = 0;
    for (struct mk_thread *t = g_blocked_head; t; t = t->next) ++n;
    return n;
}
