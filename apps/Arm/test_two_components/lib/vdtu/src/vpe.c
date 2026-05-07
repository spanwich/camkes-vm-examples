/*
 * vpe.c — VPE registry + per-VPE selector tables.
 */

#include <string.h>
#include "multikernel/vpe.h"
#include "multikernel/captable.h"
#include "multikernel/threadmgr.h"

static struct mk_vpe g_vpes[MK_VPE_MAX];
static size_t        g_count;
static bool          g_inited;

void mk_vpe_init(void)
{
    memset(g_vpes, 0, sizeof(g_vpes));
    for (size_t i = 0; i < MK_VPE_MAX; ++i) {
        g_vpes[i].vpe_id = (uint16_t)0xFFFF;       /* sentinel "free" */
        g_vpes[i].state  = MK_VPE_STATE_IDLE;
        for (size_t s = 0; s < MK_VPE_MAX_SELECTORS; ++s)
            g_vpes[i].cap_table.entries[s] = MK_CAP_NONE;
    }
    g_count  = 0;
    g_inited = true;
}

struct mk_vpe *mk_vpe_create(uint16_t vpe_id, uint16_t kid,
                             enum mk_vpe_type type)
{
    if (!g_inited) mk_vpe_init();
    /* Reject duplicate id. */
    for (size_t i = 0; i < MK_VPE_MAX; ++i) {
        if (g_vpes[i].vpe_id == vpe_id) return &g_vpes[i];  /* idempotent */
    }
    /* Find free slot. */
    for (size_t i = 0; i < MK_VPE_MAX; ++i) {
        if (g_vpes[i].vpe_id == (uint16_t)0xFFFF) {
            memset(&g_vpes[i], 0, sizeof(g_vpes[i]));
            g_vpes[i].vpe_id = vpe_id;
            g_vpes[i].kid    = kid;
            g_vpes[i].type   = type;
            g_vpes[i].state  = MK_VPE_STATE_IDLE;
            for (size_t s = 0; s < MK_VPE_MAX_SELECTORS; ++s)
                g_vpes[i].cap_table.entries[s] = MK_CAP_NONE;
            g_count++;
            return &g_vpes[i];
        }
    }
    return NULL;
}

struct mk_vpe *mk_vpe_find(uint16_t vpe_id)
{
    if (!g_inited) return NULL;
    for (size_t i = 0; i < MK_VPE_MAX; ++i) {
        if (g_vpes[i].vpe_id == vpe_id && g_vpes[i].vpe_id != (uint16_t)0xFFFF)
            return &g_vpes[i];
    }
    return NULL;
}

void mk_vpe_clear(struct mk_vpe *vpe)
{
    if (!vpe) return;
    vpe->state = MK_VPE_STATE_CLEARING;
    /* Walk selectors; for each non-NONE entry, attempt revoke. The
     * order doesn't matter — RevocationList serializes overlapping
     * walks and each revoke is idempotent. */
    extern int mk_cap_revoke(mk_cap_id_t);
    for (size_t s = 0; s < MK_VPE_MAX_SELECTORS; ++s) {
        if (vpe->cap_table.entries[s] != MK_CAP_NONE) {
            (void)mk_cap_revoke(vpe->cap_table.entries[s]);
            vpe->cap_table.entries[s] = MK_CAP_NONE;
        }
    }
    vpe->thread = NULL;
    vpe->state  = MK_VPE_STATE_IDLE;
}

void mk_vpe_destroy(struct mk_vpe *vpe)
{
    if (!vpe) return;
    mk_vpe_clear(vpe);
    vpe->vpe_id = (uint16_t)0xFFFF;
    if (g_count > 0) g_count--;
}

/* Concern 4 — selector reservation. A slot in mk_vpe_alloc_sel is set to
 * this sentinel to claim the index without yet binding a real cap; the
 * subsequent mk_vpe_bind_sel writes the real cap_id. mk_vpe_lookup_sel
 * treats the sentinel as "not yet bound" so concurrent OBTAIN/DELEGATE
 * cannot pick the same slot. */
#define MK_VPE_SEL_RESERVED ((mk_cap_id_t)~(uint64_t)0)

int mk_vpe_bind_sel(struct mk_vpe *vpe, uint16_t sel, mk_cap_id_t cap)
{
    if (!vpe || sel >= MK_VPE_MAX_SELECTORS) return -1;
    vpe->cap_table.entries[sel] = cap;
    return 0;
}

mk_cap_id_t mk_vpe_lookup_sel(struct mk_vpe *vpe, uint16_t sel)
{
    if (!vpe || sel >= MK_VPE_MAX_SELECTORS) return MK_CAP_NONE;
    mk_cap_id_t v = vpe->cap_table.entries[sel];
    if (v == MK_VPE_SEL_RESERVED) return MK_CAP_NONE;
    return v;
}

int mk_vpe_clear_sel(struct mk_vpe *vpe, uint16_t sel)
{
    if (!vpe || sel >= MK_VPE_MAX_SELECTORS) return -1;
    vpe->cap_table.entries[sel] = MK_CAP_NONE;
    return 0;
}

int mk_vpe_alloc_sel(struct mk_vpe *vpe)
{
    if (!vpe) return -1;
    /* Reserve sel=0 as "null selector" — same convention as fd 0 = stdin.
     * Phase B will use this for default-bound caps if needed. */
    for (size_t s = 1; s < MK_VPE_MAX_SELECTORS; ++s) {
        if (vpe->cap_table.entries[s] == MK_CAP_NONE) {
            vpe->cap_table.entries[s] = MK_VPE_SEL_RESERVED;
            return (int)s;
        }
    }
    return -1;
}

size_t mk_vpe_count(void) { return g_count; }
