/*
 * syscall.c — VPE → SemperKernel syscall implementation.
 */

#include <string.h>
#include "multikernel/syscall.h"
#include "multikernel/kernelcall.h"
#include "multikernel/captable.h"
#include "multikernel/service.h"

int mk_syscall_obtain(struct mk_vpe *vpe,
                      const char *service_name, uint16_t perms,
                      uint16_t *out_sel)
{
    if (!vpe || !service_name || !out_sel) return -1;
    const struct mk_service_entry *svc = mk_service_find(service_name);
    if (!svc) return -2;       /* service not found */

    /* Concern 4 — selector reservation. alloc_sel marks the slot before
     * we install anything, so a concurrent OBTAIN/DELEGATE on the same
     * VPE cannot race for the same selector. */
    int sel = mk_vpe_alloc_sel(vpe);
    if (sel < 0) return -3;

    /* Create a SESSION cap as the result of OBTAIN. The session is the
     * holder's handle to the service; sub-caps (EP + MEM) are created
     * lazily by the application via DELEGATE. Monotonic perms applies:
     * effective = svc->perms ∩ requested. */
    uint16_t effective_perms = (uint16_t)(svc->perms & perms);
    if (effective_perms == 0) {
        mk_vpe_clear_sel(vpe, (uint16_t)sel);
        return -4;
    }
    mk_cap_id_t dst = MK_CAP_NONE, src = MK_CAP_NONE;
    int rc = mk_cap_grant(vpe->kid, vpe->vpe_id,
                          MK_CAP_SESSION, effective_perms,
                          /*frame_idx=*/(uint16_t)sel,
                          /*label=*/(uint64_t)svc->kid << 32 |
                                    (uint32_t)svc->vpe_id,
                          MK_CAP_NONE, &dst, &src);
    if (rc != 0) {
        mk_vpe_clear_sel(vpe, (uint16_t)sel);
        return rc;
    }
    mk_vpe_bind_sel(vpe, (uint16_t)sel, dst);
    *out_sel = (uint16_t)sel;
    return 0;
}

int mk_syscall_activate(struct mk_vpe *vpe, uint16_t sel, uint16_t ep_id)
{
    if (!vpe) return -1;
    mk_cap_id_t cap = mk_vpe_lookup_sel(vpe, sel);
    if (cap == MK_CAP_NONE) return -2;
    /* Phase A: state-tracking only. Phase B will mint the resource cap
     * (frame for MEM_CAP, endpoint for SEND_EP/RECV_EP) into the VPE's
     * CNode slot and update the per-EP state machine to ACTIVE. */
    (void)ep_id;
    return 0;
}

int mk_syscall_deactivate(struct mk_vpe *vpe, uint16_t sel)
{
    if (!vpe) return -1;
    mk_cap_id_t cap = mk_vpe_lookup_sel(vpe, sel);
    if (cap == MK_CAP_NONE) return -2;
    /* Phase A: state-tracking only. Phase B will delete the minted cap
     * from the VPE's CNode (back to GRANTED). */
    return 0;
}

int mk_syscall_revoke(struct mk_vpe *vpe, uint16_t sel)
{
    if (!vpe) return -1;
    mk_cap_id_t cap = mk_vpe_lookup_sel(vpe, sel);
    if (cap == MK_CAP_NONE) return -2;
    int rc = mk_cap_revoke(cap);
    mk_vpe_clear_sel(vpe, sel);
    return rc;
}

int mk_syscall_delegate(struct mk_vpe *vpe, uint16_t sel,
                        uint16_t target_kid, uint16_t target_vpe_id,
                        uint16_t perms,
                        uint16_t *out_target_sel)
{
    if (!vpe || !out_target_sel) return -1;
    mk_cap_id_t parent = mk_vpe_lookup_sel(vpe, sel);
    if (parent == MK_CAP_NONE) return -2;

    /* Allocate a fresh selector on the target. The cap_id encoding
     * uses the selector as the SEL field, so we mint a synthetic
     * frame_idx = sel for compatibility with the existing protocol. */
    int target_sel = -1;
    struct mk_vpe *target = NULL;
    if (target_kid == vpe->kid) {
        target = mk_vpe_find(target_vpe_id);
        if (!target) return -3;
        target_sel = mk_vpe_alloc_sel(target);
        if (target_sel < 0) return -4;
    } else {
        /* Cross-kernel: target VPE lives on another kernel. We still
         * need a unique frame_idx — use a small monotonically-increasing
         * counter. The target kernel's handle_exchange_offer will install
         * the cap; we (the donor side) only need the cap_id back. */
        static uint16_t g_xkern_sel = 16;
        target_sel = g_xkern_sel++;
        if (target_sel >= MK_VPE_MAX_SELECTORS) return -4;
    }

    mk_cap_id_t target_cap = MK_CAP_NONE;
    int rc = mk_cap_grant(target_kid, target_vpe_id,
                          (uint16_t)MK_CAP_TYPE(parent), perms,
                          /*frame_idx=*/(uint16_t)target_sel,
                          /*label=*/0,
                          parent, &target_cap, NULL);
    if (rc != 0) return rc;

    /* For local target, bind the selector now. For cross-kernel, the
     * remote SemperKernel maintains the binding on its side. */
    if (target) {
        mk_vpe_bind_sel(target, (uint16_t)target_sel, target_cap);
    }
    *out_target_sel = (uint16_t)target_sel;
    return 0;
}

int mk_kernel_grant_root(struct mk_vpe *vpe, uint16_t type, uint16_t perms,
                         uint16_t frame_idx, uint64_t label)
{
    if (!vpe) return -1;
    int sel = mk_vpe_alloc_sel(vpe);
    if (sel < 0) return -2;
    mk_cap_id_t dst = MK_CAP_NONE, src = MK_CAP_NONE;
    int rc = mk_cap_grant(vpe->kid, vpe->vpe_id, type, perms,
                          /*frame_idx=*/(uint16_t)sel, label,
                          /*parent=*/MK_CAP_NONE, &dst, &src);
    if (rc != 0) return rc;
    /* The grant produced a placeholder dst cap on this kernel for the
     * VPE's local view. Bind it to the selector. */
    (void)src; (void)frame_idx;
    mk_vpe_bind_sel(vpe, (uint16_t)sel, dst);
    return sel;
}
