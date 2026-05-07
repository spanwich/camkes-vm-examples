/*
 * syscall.h — VPE → SemperKernel syscall interface (v3.0 architecture §5).
 *
 * VPEs reach the kernel exclusively through these calls. Each function
 * takes a `struct mk_vpe *` (the calling VPE's identity) plus the
 * arguments listed in v3.0 §5. Return is 0 on success, negative errno
 * on failure.
 *
 * Implementation in Phase A: option (b) from the Step 1 feasibility
 * report — VPEs and SemperKernel share the address space, so the
 * syscall is a direct C call into SemperKernel code. The function's
 * VPE parameter is the privilege boundary: SemperKernel validates
 * authority against vpe->cap_table before performing kernel actions.
 *
 * In a future seL4-isolated split (Phase A.3 follow-on), these functions
 * will be replaced by a thin VPE-side stub that marshalls into a
 * seL4_Call message; the SemperKernel-side dispatcher will unmarshall
 * and call the same kernel logic. The signature is shaped to support
 * that transition cleanly — no opaque pointers cross between caller
 * and kernel that aren't selector-translatable.
 */

#ifndef MULTIKERNEL_SYSCALL_H
#define MULTIKERNEL_SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include "multikernel/protocol.h"
#include "multikernel/vpe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OBTAIN — discover a service by name and create a session capability.
 * Phase A: stub — returns -ENOSYS until Phase B's SERVICE directory.
 * Will allocate a fresh selector in vpe->cap_table and write it to
 * *out_sel; the new cap is in GRANTED state (bound but not yet
 * mintable into a CNode slot — that happens at ACTIVATE).
 */
int mk_syscall_obtain(struct mk_vpe *vpe,
                      const char *service_name, uint16_t perms,
                      uint16_t *out_sel);

/* ACTIVATE — endpoint state machine GRANTED → ACTIVE. Phase A: marks
 * the entry as active in tracking state. Phase B fills in the actual
 * seL4_CNode_Mint when VPEs hold their own CNodes. */
int mk_syscall_activate(struct mk_vpe *vpe, uint16_t sel,
                        uint16_t ep_id);

/* DEACTIVATE — endpoint state machine ACTIVE → GRANTED. */
int mk_syscall_deactivate(struct mk_vpe *vpe, uint16_t sel);

/* REVOKE — revoke the cap bound to vpe's selector and cascade through
 * descendants (uses RevocationList for concurrent-walker convergence).
 * Returns the count of caps removed (>= 0) on success. */
int mk_syscall_revoke(struct mk_vpe *vpe, uint16_t sel);

/* DELEGATE — derive a child cap from vpe's selector, bind it to a fresh
 * selector on target_vpe (intra- OR cross-kernel). Monotonic perms (C.1)
 * applies: the resulting child has at most parent ∩ requested perms.
 * Writes the target's new selector to *out_target_sel. */
int mk_syscall_delegate(struct mk_vpe *vpe, uint16_t sel,
                        uint16_t target_kid, uint16_t target_vpe_id,
                        uint16_t perms,
                        uint16_t *out_target_sel);

/* SemperKernel-only convenience used during VPE bring-up: install a
 * fresh root cap into vpe's selector table without going through a
 * parent (the kernel IS the resource origin). Returns the bound
 * selector, or -1 on failure. Not exposed to VPEs in Phase B — they
 * obtain caps via OBTAIN/DELEGATE only. */
int mk_kernel_grant_root(struct mk_vpe *vpe, uint16_t type, uint16_t perms,
                         uint16_t frame_idx, uint64_t label);

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_SYSCALL_H */
