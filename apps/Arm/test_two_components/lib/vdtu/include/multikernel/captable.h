/*
 * captable.h -- SemperKernel CapTable (Multikernel-AMP architecture §6.1)
 *
 * Per-kernel user-space capability table indexed by mht_key_t cap_id.
 * Maintains parent/children links so revocation can walk the subtree.
 * Each entry records whether the responsible kernel is local or remote
 * (the "remote cap representation" of architecture §7.4 — for remote
 * children we only keep {cap_id, responsible_kid} for batching).
 *
 * Storage: open-addressing hash table over a fixed-size array. For the
 * QEMU 2-kernel scope we don't need treaps or full indexed structures.
 */

#ifndef MULTIKERNEL_CAPTABLE_H
#define MULTIKERNEL_CAPTABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "multikernel/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MK_CAPTABLE_CAP    256
#define MK_CAP_MAX_KIDS    16

struct mk_cap {
    mk_cap_id_t   id;             /* MK_CAP_NONE => slot empty           */
    mk_cap_id_t   parent_id;      /* MK_CAP_NONE => root (no parent)     */
    uint16_t      type;           /* enum mk_cap_type                    */
    uint16_t      perms;
    uint16_t      frame_idx;      /* meaningful for MK_CAP_MEM           */
    uint16_t      responsible_kid;
    uint64_t      label;
    /* Children stored as a linked-list of cap_id values via the table.
     * children_head is an index into mk_cap_table (or -1). */
    int32_t       children_head;  /* index into table; -1 if no children */
    int32_t       next_sibling;   /* index of next child of parent       */
    /* Revocation bookkeeping — set when this cap is the root of an
     * in-flight revoke and we need to count outstanding remote acks. */
    uint32_t      revoke_req_id;
    int32_t       revoke_awaited; /* awaitedResp from arch §7.1           */
    bool          revoke_in_progress;
    uint8_t       _pad[3];
};

void mk_captable_init(void);

/* Insert a fresh capability. Returns table index, or -1 on overflow. */
int  mk_captable_install(mk_cap_id_t id, mk_cap_id_t parent_id,
                         uint16_t type, uint16_t perms, uint16_t frame_idx,
                         uint16_t responsible_kid, uint64_t label);

/* Lookup by cap_id. Returns table index, or -1 if not present. */
int  mk_captable_find(mk_cap_id_t id);

/* Get pointer to entry. NULL if invalid index. */
struct mk_cap *mk_captable_at(int idx);

/* Remove entry by cap_id (used during local revoke). Detaches from
 * parent's children list. Returns 0 on success, -1 if not found. */
int  mk_captable_remove(mk_cap_id_t id);

/* Walk the children of `parent_idx`, calling cb(child_idx, ctx) for each.
 * cb may NOT mutate the table; defer removals. */
typedef void (*mk_cap_walker)(int child_idx, void *ctx);
void mk_captable_walk_children(int parent_idx, mk_cap_walker cb, void *ctx);

/* Diagnostic: count live entries. */
size_t mk_captable_size(void);

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_CAPTABLE_H */
