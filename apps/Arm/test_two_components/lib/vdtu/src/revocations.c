/*
 * revocations.c -- RevocationList implementation.
 */

#include <string.h>
#include "multikernel/revocations.h"
#include "multikernel/threadmgr.h"

static struct mk_revocation g_table[MK_REV_HASH_CAP];
static size_t               g_count;

void mk_rev_init(void)
{
    memset(g_table, 0, sizeof(g_table));
    for (size_t i = 0; i < MK_REV_HASH_CAP; ++i) {
        g_table[i].cap_id = MK_CAP_NONE;
    }
    g_count = 0;
}

static inline size_t hash_idx(mk_cap_id_t key)
{
    /* Fibonacci hash, same constant as SemperOS Revocations.h. */
    uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ULL;
    return (size_t)((h >> 56) & (MK_REV_HASH_CAP - 1));
}

static int find_index(mk_cap_id_t cap_id)
{
    size_t idx = hash_idx(cap_id);
    for (size_t probes = 0; probes < MK_REV_HASH_CAP; ++probes) {
        if (g_table[idx].cap_id == MK_CAP_NONE) return -1;
        if (g_table[idx].cap_id == cap_id)      return (int)idx;
        idx = (idx + 1) & (MK_REV_HASH_CAP - 1);
    }
    return -1;
}

struct mk_revocation *mk_rev_find(mk_cap_id_t cap_id)
{
    int idx = find_index(cap_id);
    return idx < 0 ? NULL : &g_table[idx];
}

struct mk_revocation *mk_rev_attach(mk_cap_id_t cap_id, mk_cap_id_t parent,
                                    bool *out_is_walker)
{
    int existing = find_index(cap_id);
    if (existing >= 0) {
        if (out_is_walker) *out_is_walker = false;
        return &g_table[existing];
    }
    if (g_count >= MK_REV_HASH_CAP) return NULL;

    size_t idx = hash_idx(cap_id);
    while (g_table[idx].cap_id != MK_CAP_NONE)
        idx = (idx + 1) & (MK_REV_HASH_CAP - 1);

    memset(&g_table[idx], 0, sizeof(g_table[idx]));
    g_table[idx].cap_id      = cap_id;
    g_table[idx].parent      = parent;
    g_table[idx].origin      = cap_id;
    g_table[idx].in_progress = true;
    g_table[idx].done        = false;
    g_count++;

    if (out_is_walker) *out_is_walker = true;
    return &g_table[idx];
}

int mk_rev_subscribe(struct mk_revocation *rev, void *key)
{
    if (!rev) return -1;
    if (rev->n_subscribers >= MK_REV_MAX_SUBS) return -1;
    rev->subscribers[rev->n_subscribers++] = key;
    return 0;
}

void mk_rev_complete(struct mk_revocation *rev, int32_t result)
{
    if (!rev) return;
    rev->result      = result;
    rev->done        = true;
    rev->in_progress = false;

    /* Notify subscribers. Each waiter is blocked on mk_thread_wait_for
     * with its registered key. */
    for (int i = 0; i < rev->n_subscribers; ++i) {
        mk_thread_notify(rev->subscribers[i], NULL, 0);
    }
    /* Hand-off: subscribers may copy result before clearing. We free the
     * slot lazily on next attach via the cap_id == MK_CAP_NONE check
     * after subscribers run; but cleanest is to mark gone now and rely
     * on the slot's done=true to be readable by any straggler. */

    /* Find our slot and clear it. Robin-hood-style backshift: walk
     * forward shifting any entry whose ideal slot is at-or-before us. */
    size_t idx = (size_t)((uintptr_t)(rev - g_table));
    if (idx >= MK_REV_HASH_CAP) return;
    g_table[idx].cap_id = MK_CAP_NONE;
    g_count--;
    size_t next = (idx + 1) & (MK_REV_HASH_CAP - 1);
    while (g_table[next].cap_id != MK_CAP_NONE) {
        size_t ideal = hash_idx(g_table[next].cap_id);
        if (ideal == next) break;
        /* shift back */
        g_table[idx] = g_table[next];
        g_table[next].cap_id = MK_CAP_NONE;
        idx = next;
        next = (next + 1) & (MK_REV_HASH_CAP - 1);
    }
}

size_t mk_rev_size(void) { return g_count; }
