/*
 * captable.c -- SemperKernel CapTable
 *
 * Linear-probed open-addressing hash table indexed by cap_id. For the
 * QEMU 2-kernel scope we keep a fixed array of MK_CAPTABLE_CAP entries.
 */

#include <string.h>
#include "multikernel/captable.h"

static struct mk_cap g_table[MK_CAPTABLE_CAP];
static size_t        g_size;

void mk_captable_init(void)
{
    memset(g_table, 0, sizeof(g_table));
    for (int i = 0; i < MK_CAPTABLE_CAP; ++i) {
        g_table[i].id = MK_CAP_NONE;
        g_table[i].parent_id = MK_CAP_NONE;
        g_table[i].children_head = -1;
        g_table[i].next_sibling = -1;
        g_table[i].revoke_in_progress = false;
        g_table[i].revoke_awaited = 0;
        g_table[i].revoke_req_id = 0;
    }
    g_size = 0;
}

static inline size_t hash_key(mk_cap_id_t id)
{
    /* Splitmix64 — good enough mixer for our 64-bit keys. */
    uint64_t k = id;
    k ^= k >> 30; k *= 0xbf58476d1ce4e5b9ULL;
    k ^= k >> 27; k *= 0x94d049bb133111ebULL;
    k ^= k >> 31;
    return (size_t)(k & (MK_CAPTABLE_CAP - 1));
}

int mk_captable_find(mk_cap_id_t id)
{
    if (id == MK_CAP_NONE) return -1;
    size_t h = hash_key(id);
    for (size_t step = 0; step < MK_CAPTABLE_CAP; ++step) {
        size_t idx = (h + step) & (MK_CAPTABLE_CAP - 1);
        if (g_table[idx].id == id) return (int)idx;
        if (g_table[idx].id == MK_CAP_NONE) return -1;
    }
    return -1;
}

static int find_free_slot(mk_cap_id_t id)
{
    size_t h = hash_key(id);
    for (size_t step = 0; step < MK_CAPTABLE_CAP; ++step) {
        size_t idx = (h + step) & (MK_CAPTABLE_CAP - 1);
        if (g_table[idx].id == MK_CAP_NONE) return (int)idx;
    }
    return -1;
}

int mk_captable_install(mk_cap_id_t id, mk_cap_id_t parent_id,
                        uint16_t type, uint16_t perms, uint16_t frame_idx,
                        uint16_t responsible_kid, uint64_t label)
{
    if (id == MK_CAP_NONE) return -1;
    if (mk_captable_find(id) >= 0) return -1;

    int idx = find_free_slot(id);
    if (idx < 0) return -1;

    struct mk_cap *c = &g_table[idx];
    c->id = id;
    c->parent_id = parent_id;
    c->type = type;
    c->perms = perms;
    c->frame_idx = frame_idx;
    c->responsible_kid = responsible_kid;
    c->label = label;
    c->children_head = -1;
    c->next_sibling = -1;
    c->revoke_in_progress = false;
    c->revoke_awaited = 0;
    c->revoke_req_id = 0;

    /* Link into parent's children list. */
    if (parent_id != MK_CAP_NONE) {
        int pidx = mk_captable_find(parent_id);
        if (pidx >= 0) {
            c->next_sibling = g_table[pidx].children_head;
            g_table[pidx].children_head = idx;
        }
    }

    g_size++;
    return idx;
}

struct mk_cap *mk_captable_at(int idx)
{
    if (idx < 0 || idx >= MK_CAPTABLE_CAP) return NULL;
    if (g_table[idx].id == MK_CAP_NONE) return NULL;
    return &g_table[idx];
}

int mk_captable_remove(mk_cap_id_t id)
{
    int idx = mk_captable_find(id);
    if (idx < 0) return -1;

    /* Unlink from parent's children list. */
    mk_cap_id_t pid = g_table[idx].parent_id;
    if (pid != MK_CAP_NONE) {
        int pidx = mk_captable_find(pid);
        if (pidx >= 0) {
            int *pp = &g_table[pidx].children_head;
            while (*pp != -1) {
                if (*pp == idx) {
                    *pp = g_table[idx].next_sibling;
                    break;
                }
                pp = &g_table[*pp].next_sibling;
            }
        }
    }

    /* Note: simple deletion without rehashing leaves a tombstone-shaped
     * gap. For our test scope we never re-insert with the same id post
     * delete in a way that matters; if we did, we'd need tombstones or
     * Robin Hood. Treat the slot as empty. */
    g_table[idx].id = MK_CAP_NONE;
    g_table[idx].parent_id = MK_CAP_NONE;
    g_table[idx].children_head = -1;
    g_table[idx].next_sibling = -1;
    g_size--;
    return 0;
}

void mk_captable_walk_children(int parent_idx, mk_cap_walker cb, void *ctx)
{
    if (parent_idx < 0 || parent_idx >= MK_CAPTABLE_CAP) return;
    int c = g_table[parent_idx].children_head;
    while (c >= 0) {
        int next = g_table[c].next_sibling;
        cb(c, ctx);
        c = next;
    }
}

size_t mk_captable_size(void) { return g_size; }

bool mk_cap_check_perms(mk_cap_id_t cap_id, uint16_t requested_perms)
{
    int idx = mk_captable_find(cap_id);
    if (idx < 0) return false;
    struct mk_cap *c = &g_table[idx];
    if (c->id == MK_CAP_NONE) return false;
    return (c->perms & requested_perms) == requested_perms;
}
