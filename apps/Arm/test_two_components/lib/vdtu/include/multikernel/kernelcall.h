/*
 * kernelcall.h -- KernelcallHandler API (Multikernel-AMP architecture §5)
 *
 * Each kernel runs one KernelcallHandler that:
 *   - polls its inbound control ring,
 *   - dispatches incoming EXCHANGE_OFFER / EXCHANGE_ACK / REVOKE /
 *     REVOKE_BATCH / REVOKE_FINISH messages,
 *   - mutates the local CapTable accordingly, and
 *   - notifies any local thread waiting on a req_id when its reply arrives.
 *
 * Local VPE threads call the cap_grant() / cap_revoke() helpers from any
 * cooperative thread (mk_thread_*); these block via mk_thread_wait_for
 * until the handler observes the matching ACK / REVOKE_FINISH.
 *
 * Routing rule for outbound messages:
 *     dst_kid == self_kid → loopback (purely local, fast path)
 *     dst_kid != self_kid → enqueue on outbound control ring
 */

#ifndef MULTIKERNEL_KERNELCALL_H
#define MULTIKERNEL_KERNELCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "multikernel/protocol.h"
#include "multikernel/captable.h"
#include "multikernel/threadmgr.h"
#include "multikernel/mht.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostic counters — used by Test 4 (local fast path proves count==0
 * for intra-kernel revoke) and Test 7 (ping-pong throughput). */
struct mk_kc_stats {
    uint64_t ring_msgs_sent;
    uint64_t ring_msgs_recv;
    uint64_t exchange_count;
    uint64_t revoke_count;
    uint64_t local_fastpath_count;
};

/* One-shot init. Must be called after mht_init + mk_threadmgr_init.
 * `pool_vaddr` must be the same pointer ComponentC obtained from CAmkES.
 * Spawns the handler poll thread and returns. */
int  mk_kc_init(void *pool_vaddr, struct mht *mht);

/* Snapshot of stats (caller copies). */
void mk_kc_get_stats(struct mk_kc_stats *out);

/* ----- VPE-facing API -----
 *
 * cap_grant: hand a memory cap (frame_idx in our pool) to (dst_kid,
 * dst_vpe). Blocks until the receiving kernel sends EXCHANGE_ACK with
 * its locally-allocated cap_id. The donor's local CapTable is updated
 * with the remote child for revocation tracking.
 *
 * Returns 0 on success and writes the receiving kernel's allocated
 * cap_id to *out_dst_cap_id; negative on error. */
int mk_cap_grant(uint16_t dst_kid, uint16_t dst_vpe,
                 uint16_t type, uint16_t perms,
                 uint16_t frame_idx, uint64_t label,
                 mk_cap_id_t parent_cap_id,   /* MK_CAP_NONE for root grant */
                 mk_cap_id_t *out_dst_cap_id,
                 mk_cap_id_t *out_src_cap_id);

/* cap_revoke: walk subtree rooted at cap_id, batch remote children by
 * responsible_kid, send REVOKE/REVOKE_BATCH, block on awaitedResp until
 * all REVOKE_FINISHes arrive. The cap and its descendants are removed
 * from the local CapTable. */
int mk_cap_revoke(mk_cap_id_t cap_id);

/* Test 7 helper: send a PING and block for the matching PONG. */
int mk_kc_ping(uint16_t dst_kid);

/* Allocate a unique req_id (32-bit, monotonic). */
uint32_t mk_kc_next_req_id(void);

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_KERNELCALL_H */
