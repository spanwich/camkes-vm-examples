/*
 * kernelcall.c -- KernelcallHandler dispatch loop + cap_grant / cap_revoke
 *
 * One handler per kernel. Polls inbound control ring, dispatches
 * messages, mutates CapTable, wakes waiters. Local VPE threads invoke
 * the cap_grant() / cap_revoke() helpers from cooperative threads;
 * those helpers send a request, block on a reply slot, and return when
 * the handler observes the matching ACK / FINISH.
 *
 * Routing convention for the 2-kernel layout:
 *   kid=0: outbound = pool frame 0, inbound = pool frame 1
 *   kid=1: outbound = pool frame 1, inbound = pool frame 0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multikernel/kernelcall.h"
#include "multikernel/revocations.h"

/* This module learns its own kid at runtime from the MHT (mht->self_kid).
 * No compile-time MULTIKERNEL_KID dependency here — ComponentC carries it. */

#define POOL_FRAME_SIZE     0x1000u
#define POOL_FRAME(idx)     ((uint8_t *)g_pool_base + (size_t)(idx) * POOL_FRAME_SIZE)

static void *g_pool_base;
static struct mht *g_mht;
static uint16_t g_self_kid;

static struct vdtu_ring g_outbound_ring;
static struct vdtu_ring g_inbound_ring;

static struct mk_kc_stats g_stats;
static uint32_t g_next_req_id = 1;

/* ----- pending-reply slots ------------------------------------------ */

#define MK_KC_PENDING_MAX 32

struct mk_kc_reply_slot {
    uint32_t        req_id;        /* 0 = unused */
    int             ready;
    int32_t         status;
    mk_cap_id_t     dst_cap_id;    /* EXCHANGE_ACK payload                */
    uint32_t        revoked_count; /* REVOKE_FINISH payload               */
};

static struct mk_kc_reply_slot g_pending[MK_KC_PENDING_MAX];

static struct mk_kc_reply_slot *alloc_pending(uint32_t req_id)
{
    for (int i = 0; i < MK_KC_PENDING_MAX; ++i) {
        if (g_pending[i].req_id == 0) {
            memset(&g_pending[i], 0, sizeof(g_pending[i]));
            g_pending[i].req_id = req_id;
            return &g_pending[i];
        }
    }
    return NULL;
}

static struct mk_kc_reply_slot *find_pending(uint32_t req_id)
{
    if (req_id == 0) return NULL;
    for (int i = 0; i < MK_KC_PENDING_MAX; ++i) {
        if (g_pending[i].req_id == req_id) return &g_pending[i];
    }
    return NULL;
}

static void free_pending(struct mk_kc_reply_slot *s)
{
    if (s) s->req_id = 0;
}

/* ----- outbound queue ------------------------------------------------ */

static int send_msg(uint16_t dst_kid, const void *body, uint16_t body_len)
{
    /* Pad body to ensure we don't exceed slot size. */
    int rc = vdtu_ring_send(&g_outbound_ring,
                            g_self_kid, /* sender_pe (kid)              */
                            0,          /* sender_ep                    */
                            0,          /* sender_vpe                   */
                            0,          /* reply_ep                     */
                            0,          /* label                        */
                            0,          /* replylabel                   */
                            VDTU_FLAGS_WITH_DEST(0, dst_kid),
                            body, body_len);
    if (rc == 0) g_stats.ring_msgs_sent++;
    return rc;
}

uint32_t mk_kc_next_req_id(void)
{
    return ++g_next_req_id;
}

/* ----- dispatch handlers -------------------------------------------- */

static void handle_ping(const struct mk_msg_hdr *h)
{
    struct mk_msg_hdr reply;
    memset(&reply, 0, sizeof(reply));
    reply.msg_type = MK_MSG_PONG;
    reply.src_kid  = (uint8_t)g_self_kid;
    reply.dst_kid  = h->src_kid;
    reply.req_id   = h->req_id;
    send_msg(h->src_kid, &reply, sizeof(reply));
}

static void handle_pong(const struct mk_msg_hdr *h)
{
    struct mk_kc_reply_slot *s = find_pending(h->req_id);
    if (!s) {
        fprintf(stderr, "kid=%u: stray PONG req_id=%u\n",
                g_self_kid, h->req_id);
        return;
    }
    s->status = 0;
    s->ready  = 1;
    mk_thread_notify(s, NULL, 0);
}

static void handle_exchange_offer(const struct mk_msg_exchange_offer *m)
{
    /* Allocate a fresh local cap_id (we use the offer's frame_idx as
     * the selector — sufficient for the QEMU 2-kernel scope). */
    mk_cap_id_t dst_cap_id = MK_CAP_ID(g_self_kid, m->dst_vpe, m->type, m->frame_idx);

    int rc = mk_captable_install(dst_cap_id, m->src_cap_id,
                                 m->type, m->perms, m->frame_idx,
                                 g_self_kid, m->label);
    int32_t status = (rc < 0) ? -1 : 0;

    struct mk_msg_exchange_ack ack;
    memset(&ack, 0, sizeof(ack));
    ack.hdr.msg_type = MK_MSG_EXCHANGE_ACK;
    ack.hdr.src_kid  = (uint8_t)g_self_kid;
    ack.hdr.dst_kid  = m->hdr.src_kid;
    ack.hdr.req_id   = m->hdr.req_id;
    ack.dst_cap_id   = (status == 0) ? dst_cap_id : MK_CAP_NONE;
    ack.status       = status;

    send_msg(m->hdr.src_kid, &ack, sizeof(ack));
    g_stats.exchange_count++;
}

static void handle_exchange_ack(const struct mk_msg_exchange_ack *m)
{
    struct mk_kc_reply_slot *s = find_pending(m->hdr.req_id);
    if (!s) {
        fprintf(stderr, "kid=%u: stray EXCHANGE_ACK req_id=%u\n",
                g_self_kid, m->hdr.req_id);
        return;
    }
    s->status     = m->status;
    s->dst_cap_id = m->dst_cap_id;
    s->ready      = 1;
    mk_thread_notify(s, NULL, 0);
}

/* ----- proper subtree revocation -------------------------------------
 *
 * `do_revoke_subtree(root_idx)` walks the local subtree rooted at
 * `root_idx`, batches remote children by responsible_kid, sends one
 * REVOKE_BATCH per remote kid with a fresh req_id (so cascading
 * FINISHes are matched correctly on the wire), waits on a per-kid
 * pending slot for the corresponding FINISH, then post-order removes
 * the local subtree. Caller MUST be a non-handler thread (it blocks
 * inside mk_thread_wait_for).
 *
 * Architecture §7.1 awaitedResp pattern: each outstanding remote batch
 * is one "awaited" reply; the per-kid slot is the realisation of the
 * counter for the leaf-to-root path in our flat 2-kernel topology. For
 * cascading depth >2 (Test 5: K0→K1→K0), the recursion happens on the
 * *receiving* side: handler spawns a `revoke_batch_worker` that calls
 * do_revoke_subtree itself, which sends its own REVOKE_BATCH back to
 * K0 with a fresh req_id, and so on. Total awaitedResp tracking is
 * implicit through the per-call slot and pending-table machinery. */

static int do_revoke_subtree(int root_idx);

struct revoke_batch_work {
    uint32_t    reply_req_id;
    uint16_t    initiator_kid;
    uint32_t    count;
    mk_cap_id_t cap_ids[4];
};

static void revoke_batch_worker(void *arg)
{
    struct revoke_batch_work *w = (struct revoke_batch_work *)arg;
    uint32_t total = 0;
    for (uint32_t i = 0; i < w->count && i < 4; ++i) {
        int idx = mk_captable_find(w->cap_ids[i]);
        if (idx >= 0) total += do_revoke_subtree(idx);
    }
    struct mk_msg_revoke_finish f;
    memset(&f, 0, sizeof(f));
    f.hdr.msg_type  = MK_MSG_REVOKE_FINISH;
    f.hdr.src_kid   = (uint8_t)g_self_kid;
    f.hdr.dst_kid   = (uint8_t)w->initiator_kid;
    f.hdr.req_id    = w->reply_req_id;
    f.status        = 0;
    f.revoked_count = total;
    send_msg(w->initiator_kid, &f, sizeof(f));
    free(w);
    g_stats.revoke_count++;
}

static void handle_revoke(const struct mk_msg_revoke *m)
{
    struct revoke_batch_work *w = (struct revoke_batch_work *)malloc(sizeof(*w));
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->reply_req_id  = m->hdr.req_id;
    w->initiator_kid = m->hdr.src_kid;
    w->count         = 1;
    w->cap_ids[0]    = m->cap_id;
    if (!mk_thread_spawn(revoke_batch_worker, w)) free(w);
}

static void handle_revoke_batch(const struct mk_msg_revoke_batch *m)
{
    struct revoke_batch_work *w = (struct revoke_batch_work *)malloc(sizeof(*w));
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->reply_req_id  = m->hdr.req_id;
    w->initiator_kid = m->hdr.src_kid;
    w->count         = m->count > 4 ? 4 : m->count;
    for (uint32_t i = 0; i < w->count; ++i) w->cap_ids[i] = m->cap_ids[i];
    if (!mk_thread_spawn(revoke_batch_worker, w)) free(w);
}

static int do_revoke_subtree(int root_idx)
{
    if (root_idx < 0) return 0;

    /* DFS: gather descendant cap-ids per remote kid; we walk only into
     * local children so `local descendants` includes everything we'll
     * post-order-remove ourselves. Remote children's IDs are batched
     * for REVOKE_BATCH; their own descendants are the remote kid's
     * problem (it'll do another do_revoke_subtree there). */
    struct {
        mk_cap_id_t ids[16];
        uint32_t    count;
    } per_kid[MK_CAP_MAX_KIDS];
    memset(per_kid, 0, sizeof(per_kid));

    int stack[MK_CAPTABLE_CAP];
    int sp = 0;
    stack[sp++] = root_idx;
    while (sp > 0) {
        int cur = stack[--sp];
        struct mk_cap *c = mk_captable_at(cur);
        if (!c) continue;
        for (int ch = c->children_head; ch >= 0; ) {
            struct mk_cap *cc = mk_captable_at(ch);
            int next = cc ? cc->next_sibling : -1;
            if (cc) {
                if (cc->responsible_kid == g_self_kid) {
                    if (sp < MK_CAPTABLE_CAP) stack[sp++] = ch;
                } else {
                    uint16_t k = cc->responsible_kid;
                    if (k < MK_CAP_MAX_KIDS && per_kid[k].count < 16)
                        per_kid[k].ids[per_kid[k].count++] = cc->id;
                }
            }
            ch = next;
        }
    }

    /* Send one REVOKE_BATCH per remote kid, each with its own req_id and
     * pending slot. Wait on each slot in turn — by the time we wait,
     * the handler thread is the only thing that can advance us, and
     * the slot pattern is safe because each slot is only ever notified
     * once (no overwrite race like the previous shared-slot impl). */
    struct mk_kc_reply_slot *slots[MK_CAP_MAX_KIDS] = { NULL };
    for (uint16_t k = 0; k < MK_CAP_MAX_KIDS; ++k) {
        if (per_kid[k].count == 0) continue;
        uint32_t req = mk_kc_next_req_id();
        struct mk_kc_reply_slot *s = alloc_pending(req);
        if (!s) continue;
        slots[k] = s;
        struct mk_msg_revoke_batch m;
        memset(&m, 0, sizeof(m));
        m.hdr.msg_type = MK_MSG_REVOKE_BATCH;
        m.hdr.src_kid  = (uint8_t)g_self_kid;
        m.hdr.dst_kid  = (uint8_t)k;
        m.hdr.req_id   = req;
        uint32_t cnt = per_kid[k].count > 4 ? 4 : per_kid[k].count;
        m.count = cnt;
        for (uint32_t i = 0; i < cnt; ++i) m.cap_ids[i] = per_kid[k].ids[i];
        if (send_msg(k, &m, sizeof(m)) != 0) {
            free_pending(s);
            slots[k] = NULL;
        }
    }
    for (uint16_t k = 0; k < MK_CAP_MAX_KIDS; ++k) {
        if (!slots[k]) continue;
        while (!slots[k]->ready) mk_thread_wait_for(slots[k]);
        free_pending(slots[k]);
    }

    /* Post-order remove the local subtree. Pre-order DFS into a
     * collected[] array, then walk it back to front (leaves first).
     * Push ALL children — including remote placeholders — so their
     * local entries get cleaned up. Cascading FINISHes have already
     * removed the local descendants under those remote placeholders
     * by the time we reach this point. */
    int rstack[MK_CAPTABLE_CAP];
    int rsp = 0;
    rstack[rsp++] = root_idx;
    int collected[MK_CAPTABLE_CAP];
    int csp = 0;
    while (rsp > 0) {
        int cur = rstack[--rsp];
        struct mk_cap *c = mk_captable_at(cur);
        if (!c) continue;
        if (csp < MK_CAPTABLE_CAP) collected[csp++] = cur;
        for (int ch = c->children_head; ch >= 0; ) {
            struct mk_cap *cc = mk_captable_at(ch);
            int next = cc ? cc->next_sibling : -1;
            if (cc) {
                if (rsp < MK_CAPTABLE_CAP) rstack[rsp++] = ch;
            }
            ch = next;
        }
    }
    int removed = 0;
    for (int i = csp - 1; i >= 0; --i) {
        struct mk_cap *c = mk_captable_at(collected[i]);
        if (c) { mk_captable_remove(c->id); removed++; }
    }
    return removed;
}

static void handle_revoke_finish(const struct mk_msg_revoke_finish *m)
{
    struct mk_kc_reply_slot *s = find_pending(m->hdr.req_id);
    if (!s) {
        fprintf(stderr, "kid=%u: stray REVOKE_FINISH req_id=%u\n",
                g_self_kid, m->hdr.req_id);
        return;
    }
    s->status        = m->status;
    s->revoked_count = m->revoked_count;
    s->ready         = 1;
    mk_thread_notify(s, NULL, 0);
}

/* ----- handler thread ----------------------------------------------- */

static void handler_thread(void *arg)
{
    (void)arg;
    for (;;) {
        const struct vdtu_message *vm = vdtu_ring_fetch(&g_inbound_ring);
        if (!vm) {
            mk_thread_yield();
            continue;
        }
        const struct mk_msg_hdr *h = (const struct mk_msg_hdr *)vm->data;
        g_stats.ring_msgs_recv++;

        switch (h->msg_type) {
        case MK_MSG_PING:
            handle_ping(h);
            break;
        case MK_MSG_PONG:
            handle_pong(h);
            break;
        case MK_MSG_EXCHANGE_OFFER:
            handle_exchange_offer((const struct mk_msg_exchange_offer *)vm->data);
            break;
        case MK_MSG_EXCHANGE_ACK:
            handle_exchange_ack((const struct mk_msg_exchange_ack *)vm->data);
            break;
        case MK_MSG_REVOKE:
            handle_revoke((const struct mk_msg_revoke *)vm->data);
            break;
        case MK_MSG_REVOKE_BATCH:
            handle_revoke_batch((const struct mk_msg_revoke_batch *)vm->data);
            break;
        case MK_MSG_REVOKE_FINISH:
            handle_revoke_finish((const struct mk_msg_revoke_finish *)vm->data);
            break;
        default:
            fprintf(stderr, "kid=%u: unknown msg_type=0x%x req_id=%u\n",
                    g_self_kid, h->msg_type, h->req_id);
            break;
        }
        vdtu_ring_ack(&g_inbound_ring);
    }
}

/* ----- public init -------------------------------------------------- */

int mk_kc_init(void *pool_vaddr, struct mht *mht)
{
    if (!pool_vaddr || !mht) return -1;
    g_pool_base = pool_vaddr;
    g_mht       = mht;
    g_self_kid  = mht->self_kid;

    /* Frame 0 is "ring 0→1" (kid=0 produces, kid=1 consumes).
     * Frame 1 is "ring 1→0" (kid=1 produces, kid=0 consumes). */
    void *outbound_frame, *inbound_frame;
    if (g_self_kid == 0) {
        outbound_frame = POOL_FRAME(0);
        inbound_frame  = POOL_FRAME(1);
    } else {
        outbound_frame = POOL_FRAME(1);
        inbound_frame  = POOL_FRAME(0);
    }

    /* Each kid initializes ITS OWN outbound ring; the remote will
     * vdtu_ring_attach to it on the other side. We attach to the peer's
     * outbound (= our inbound) which they'll initialize before we read.
     * The boot announcement in mht_init proves both kids are alive
     * before we begin polling, so the attach can safely follow. */
    if (vdtu_ring_init(&g_outbound_ring, outbound_frame,
                       MK_CTRL_SLOT_COUNT, MK_CTRL_SLOT_SIZE) != 0) {
        return -2;
    }
    if (vdtu_ring_attach(&g_inbound_ring, inbound_frame) != 0) {
        return -3;
    }

    mk_captable_init();
    mk_rev_init();
    memset(g_pending, 0, sizeof(g_pending));
    memset(&g_stats, 0, sizeof(g_stats));

    if (!mk_thread_spawn(handler_thread, NULL)) {
        return -4;
    }
    return 0;
}

void mk_kc_get_stats(struct mk_kc_stats *out)
{
    if (out) *out = g_stats;
}

/* ----- VPE-facing helpers ------------------------------------------- */

int mk_kc_ping(uint16_t dst_kid)
{
    uint32_t req = mk_kc_next_req_id();
    struct mk_kc_reply_slot *s = alloc_pending(req);
    if (!s) return -1;

    struct mk_msg_hdr h;
    memset(&h, 0, sizeof(h));
    h.msg_type = MK_MSG_PING;
    h.src_kid  = (uint8_t)g_self_kid;
    h.dst_kid  = (uint8_t)dst_kid;
    h.req_id   = req;
    if (send_msg(dst_kid, &h, sizeof(h)) != 0) {
        free_pending(s);
        return -2;
    }
    while (!s->ready) mk_thread_wait_for(s);
    int rc = s->status;
    free_pending(s);
    return rc;
}

int mk_cap_grant(uint16_t dst_kid, uint16_t dst_vpe,
                 uint16_t type, uint16_t perms,
                 uint16_t frame_idx, uint64_t label,
                 mk_cap_id_t parent_cap_id,
                 mk_cap_id_t *out_dst_cap_id,
                 mk_cap_id_t *out_src_cap_id)
{
    /* The OFFER's `src_cap_id` field is what the recipient will use as
     * the parent of its newly-installed cap. For a root grant we synth
     * a fresh donor cap and install it locally; for a derived grant we
     * use the caller-supplied parent_cap_id (which must already be in
     * the donor's CapTable — typically because we received it earlier
     * via EXCHANGE_OFFER from another kernel).
     *
     * Phase C.1 — monotonic perm restriction: derived grants MUST have
     * perms ⊆ parent's perms. AND requested perms with parent's perms.
     * Enforced at grant-time, before OFFER goes out (sender side is the
     * security boundary; receiver trusts the OFFER's perms field). */
    mk_cap_id_t src_cap;
    uint16_t effective_perms = perms;
    if (parent_cap_id == MK_CAP_NONE) {
        src_cap = MK_CAP_ID(g_self_kid, /* donor vpe */ 0, type, frame_idx);
        if (mk_captable_find(src_cap) < 0) {
            mk_captable_install(src_cap, MK_CAP_NONE, type, effective_perms,
                                frame_idx, g_self_kid, label);
        }
    } else {
        src_cap = parent_cap_id;
        int pidx = mk_captable_find(parent_cap_id);
        if (pidx < 0) return -3;   /* parent must exist locally */
        struct mk_cap *p = mk_captable_at(pidx);
        if (p) effective_perms &= p->perms;
    }

    /* C.2 — at-use permission gate. Empty effective perms means every
     * bit was masked away by the AND; reject as no-op grant. The full
     * delegation (MK_PERM_D) check is enforced at the syscall boundary
     * once Phase A.4 is in place. For Phase C, callers using
     * mk_cap_check_perms directly self-gate. */
    if (effective_perms == 0) return -5;

    /* Local fast path — avoid the ring entirely. */
    if (dst_kid == g_self_kid) {
        mk_cap_id_t local_dst = MK_CAP_ID(g_self_kid, dst_vpe, type, frame_idx);
        if (mk_captable_install(local_dst, src_cap, type, effective_perms,
                                frame_idx, g_self_kid, label) < 0) {
            return -1;
        }
        g_stats.local_fastpath_count++;
        if (out_dst_cap_id) *out_dst_cap_id = local_dst;
        if (out_src_cap_id) *out_src_cap_id = src_cap;
        return 0;
    }

    uint32_t req = mk_kc_next_req_id();
    struct mk_kc_reply_slot *s = alloc_pending(req);
    if (!s) return -1;

    struct mk_msg_exchange_offer m;
    memset(&m, 0, sizeof(m));
    m.hdr.msg_type = MK_MSG_EXCHANGE_OFFER;
    m.hdr.src_kid  = (uint8_t)g_self_kid;
    m.hdr.dst_kid  = (uint8_t)dst_kid;
    m.hdr.req_id   = req;
    m.src_cap_id   = src_cap;
    m.type         = type;
    m.dst_vpe      = dst_vpe;
    m.frame_idx    = frame_idx;
    m.perms        = effective_perms;
    m.label        = label;
    if (send_msg(dst_kid, &m, sizeof(m)) != 0) {
        free_pending(s);
        return -2;
    }
    while (!s->ready) mk_thread_wait_for(s);
    int rc = s->status;
    mk_cap_id_t dst_id = s->dst_cap_id;
    free_pending(s);
    if (rc != 0) return rc;

    /* Record the remote child as a placeholder so revocation can route
     * through it. parent_id is `src_cap` — which is either our freshly
     * installed donor root, or (for derived grants) the existing parent
     * we received earlier; both are guaranteed in our CapTable. */
    mk_captable_install(dst_id, src_cap, type, effective_perms, frame_idx,
                        dst_kid, label);

    if (out_dst_cap_id) *out_dst_cap_id = dst_id;
    if (out_src_cap_id) *out_src_cap_id = src_cap;
    return 0;
}

int mk_cap_revoke(mk_cap_id_t cap_id)
{
    /* Phase C.4 — RevocationList: serialize concurrent revokers of the
     * same cap. The first caller becomes the walker; subsequent callers
     * subscribe, block on their own notify key, and inherit the walker's
     * result on completion. Without this serialization, two threads
     * (especially across kernels) could fan out duplicate REVOKE_BATCH
     * messages and double-process the local subtree. */
    bool is_walker = false;
    struct mk_revocation *rev =
        mk_rev_attach(cap_id, /*parent=*/MK_CAP_NONE, &is_walker);
    if (!rev) {
        /* Table full — fall back to direct walk; lose the convergence
         * guarantee but don't deadlock. */
        int idx = mk_captable_find(cap_id);
        if (idx < 0) return -1;
        int removed = do_revoke_subtree(idx);
        g_stats.revoke_count++;
        return removed;
    }

    if (!is_walker) {
        /* Subscribe and wait for the walker. Our notify key is the
         * address of a local stack variable — uniquely identifies this
         * waiter to mk_thread_notify. */
        volatile int my_notify_slot = 0;
        if (mk_rev_subscribe(rev, (void *)&my_notify_slot) != 0) {
            return -2;
        }
        while (!rev->done) {
            mk_thread_wait_for((void *)&my_notify_slot);
        }
        return rev->result;
    }

    /* Walker path. */
    int idx = mk_captable_find(cap_id);
    int removed;
    if (idx < 0) {
        removed = -1;
    } else {
        removed = do_revoke_subtree(idx);
        g_stats.revoke_count++;
    }
    mk_rev_complete(rev, removed);
    return removed;
}
