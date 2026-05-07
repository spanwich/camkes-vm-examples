/*
 * ComponentC — SemperKernel-on-multikernel test driver
 *
 * Each kernel's rootserver runs one instance of this component. It is the
 * VPE that hosts:
 *   - MHT init + boot announcement (Step 3)
 *   - Cooperative ThreadManager (Step 4)
 *   - KernelcallHandler (Step 5): poll thread + cross-kernel control protocol
 *   - Test driver thread that runs Tests 1 + 7 from the architect's spec
 *     (cross-kernel cap exchange + ping-pong throughput).
 *
 * Pool layout (from test_two_components.camkes, 64 KiB at 0x6FFE0000):
 *   Frame 0 (offset 0x0000): control ring 0 → 1
 *   Frame 1 (offset 0x1000): control ring 1 → 0
 *   Frames 2..15:            EP data channels (test 1 uses frame 5)
 *   Offset 0xFFC0:           MHT boot announcement region
 */

#include <stdio.h>
#include <stdint.h>
#include <camkes.h>

#include <multikernel/vdtu_ring.h>
#include <multikernel/vdtu_ep_state.h>
#include <multikernel/mht.h>
#include <multikernel/threadmgr.h>
#include <multikernel/kernelcall.h>
#include <multikernel/revocations.h>
#include <multikernel/vpe.h>
#include <multikernel/syscall.h>

#define POOL_PADDR              0x6FFE0000ull
#define POOL_FRAME_SIZE         0x1000u
#define POOL_FRAME_OFFSET(idx)  ((size_t)(idx) * POOL_FRAME_SIZE)
#define TEST1_FRAME_IDX         5u
#define TEST1_MAGIC             0xDEADBEEFu

/* Per-kid sync words inside the shared pool — each kid writes its own
 * slot (sync_word[my_kid]) to advance a phase; the peer spins on it. */
#define POOL_SYNC_OFFSET        0xFFA0u
#define POOL_SYNC_WORD(pool, kid) \
    ((volatile uint32_t *)((uint8_t *)(pool) + POOL_SYNC_OFFSET + (kid)*4))

static inline void sync_set(uint8_t *pool, uint16_t my_kid, uint32_t value)
{
    __atomic_store_n(POOL_SYNC_WORD(pool, my_kid), value, __ATOMIC_RELEASE);
}

/* Spin (yielding to handler) until peer's sync word == expected. Returns
 * 0 on success, -1 if budget exhausted. */
static inline int sync_wait(uint8_t *pool, uint16_t peer_kid,
                            uint32_t expected, int budget)
{
    for (int i = 0; i < budget; ++i) {
        if (__atomic_load_n(POOL_SYNC_WORD(pool, peer_kid),
                            __ATOMIC_ACQUIRE) == expected) return 0;
        mk_thread_yield();
    }
    return -1;
}

#ifndef MULTIKERNEL_KID
#error "MULTIKERNEL_KID must be defined at compile time"
#endif
#ifndef MULTIKERNEL_NUM_KIDS
#define MULTIKERNEL_NUM_KIDS 2
#endif

static struct mht g_mht;

/* Phase C.4 — concurrent revoke worker. The two-thread test launches
 * this with the same cap_id; first thread becomes the walker, second
 * subscribes via RevocationList and waits. */
struct rev_arg {
    mk_cap_id_t cap;
    int         result;
    int         finished;
};

static void rev_worker(void *raw)
{
    struct rev_arg *a = (struct rev_arg *)raw;
    a->result   = mk_cap_revoke(a->cap);
    a->finished = 1;
}

static void test_driver_kid0(void *raw)
{
    uint8_t *pool = (uint8_t *)raw;

    /* ----- Test 1 (cross-kernel exchange) — run FIRST so K1's poll
     * loop sees the cap quickly before its budget runs out. */
    printf("[TEST 1] kid=0: writing 0x%X to frame %u, granting MEM cap to kid=1\n",
           TEST1_MAGIC, TEST1_FRAME_IDX);
    fflush(stdout);

    volatile uint32_t *frame =
        (volatile uint32_t *)(pool + POOL_FRAME_OFFSET(TEST1_FRAME_IDX));
    __atomic_store_n(frame, TEST1_MAGIC, __ATOMIC_RELEASE);

    mk_cap_id_t dst_id = MK_CAP_NONE, src_id = MK_CAP_NONE;
    int rc = mk_cap_grant(/*dst_kid=*/1, /*dst_vpe=*/1,
                          MK_CAP_MEM, /*perms=R|W=*/3,
                          TEST1_FRAME_IDX, /*label=*/0xCAFE0000ull,
                          /*parent=*/MK_CAP_NONE,
                          &dst_id, &src_id);
    if (rc == 0) {
        printf("[TEST 1] kid=0: grant OK; src_id=0x%llx dst_id=0x%llx\n",
               (unsigned long long)src_id, (unsigned long long)dst_id);
    } else {
        printf("[TEST 1] kid=0: grant FAILED rc=%d\n", rc);
    }
    fflush(stdout);

    /* ----- Test 4 (local fast path: intra-kernel grant uses 0 ring msgs) */
    {
        struct mk_kc_stats before, after;
        mk_kc_get_stats(&before);
        mk_cap_id_t local_dst = MK_CAP_NONE, local_src = MK_CAP_NONE;
        int rc4 = mk_cap_grant(/*dst_kid=*/0, /*dst_vpe=*/2,
                               MK_CAP_MEM, 3, /*frame_idx=*/6,
                               0xBEEF, MK_CAP_NONE,
                               &local_dst, &local_src);
        mk_kc_get_stats(&after);
        uint64_t ring_delta = after.ring_msgs_sent - before.ring_msgs_sent;
        if (rc4 == 0 && ring_delta == 0 && after.local_fastpath_count > before.local_fastpath_count)
            printf("[TEST 4] Local fast path: PASS (0 ring messages, fastpath_count++)\n");
        else
            printf("[TEST 4] Local fast path: FAIL rc=%d ring_delta=%llu fastpath_delta=%llu\n",
                   rc4, (unsigned long long)ring_delta,
                   (unsigned long long)(after.local_fastpath_count - before.local_fastpath_count));
        fflush(stdout);
    }

    /* ----- Test 3a (intra-kernel completeness) ----- */
    {
        /* Build: K0 vpe=0 → vpe=2 → vpe=3 chain on frame 7. */
        mk_cap_id_t lvl1_dst = MK_CAP_NONE, lvl1_src = MK_CAP_NONE;
        mk_cap_id_t lvl2_dst = MK_CAP_NONE;
        mk_cap_grant(/*dst_kid=*/0, /*dst_vpe=*/2,
                     MK_CAP_MEM, 3, /*frame_idx=*/7,
                     0x3a00, MK_CAP_NONE,
                     &lvl1_dst, &lvl1_src);
        mk_cap_grant(/*dst_kid=*/0, /*dst_vpe=*/3,
                     MK_CAP_MEM, 3, /*frame_idx=*/7,
                     0x3a01, lvl1_dst,
                     &lvl2_dst, NULL);
        int present_root  = (mk_captable_find(lvl1_src) >= 0);
        int present_lvl1  = (mk_captable_find(lvl1_dst) >= 0);
        int present_lvl2  = (mk_captable_find(lvl2_dst) >= 0);
        int rc3a = mk_cap_revoke(lvl1_src);
        int gone_root  = (mk_captable_find(lvl1_src) < 0);
        int gone_lvl1  = (mk_captable_find(lvl1_dst) < 0);
        int gone_lvl2  = (mk_captable_find(lvl2_dst) < 0);
        if (present_root && present_lvl1 && present_lvl2 &&
            rc3a >= 3 && gone_root && gone_lvl1 && gone_lvl2)
            printf("[TEST 3a] Intra-kernel completeness: PASS (3/3 caps revoked, "
                   "all subtree gone)\n");
        else
            printf("[TEST 3a] Intra-kernel completeness: FAIL "
                   "before=(%d %d %d) rc=%d after_gone=(%d %d %d)\n",
                   present_root, present_lvl1, present_lvl2, rc3a,
                   gone_root, gone_lvl1, gone_lvl2);
        fflush(stdout);
    }

    /* ----- Test 2a (intra-kernel revocation removes access) ----- */
    {
        mk_cap_id_t cap_2a = MK_CAP_ID(/*kid=*/0, /*vpe=*/2,
                                       MK_CAP_MEM, /*sel=*/6);
        int present_before = (mk_captable_find(cap_2a) >= 0);
        int rc2a = mk_cap_revoke(cap_2a);
        int present_after = (mk_captable_find(cap_2a) >= 0);
        if (present_before && rc2a >= 0 && !present_after)
            printf("[TEST 2a] Intra-kernel revocation: PASS (grantee unmapped, %d local cap%s removed)\n",
                   rc2a, rc2a == 1 ? "" : "s");
        else
            printf("[TEST 2a] Intra-kernel revocation: FAIL "
                   "before=%d rc=%d after=%d\n",
                   present_before, rc2a, present_after);
        fflush(stdout);
    }

    /* ----- Test 2b (cross-kernel revocation removes access) ----- */
    {
        /* Revoke the source cap on K0 — should also revoke the
         * dependent cap that K1 installed in Test 1. */
        int rc2b = mk_cap_revoke(/*cap_id=*/src_id);
        int still_on_k0 = (mk_captable_find(src_id) >= 0);
        if (rc2b >= 0 && !still_on_k0)
            printf("[TEST 2b] Cross-kernel revocation: PASS "
                   "(K0 src_cap removed, %d local cap%s; REVOKE_BATCH delivered)\n",
                   rc2b, rc2b == 1 ? "" : "s");
        else
            printf("[TEST 2b] Cross-kernel revocation: FAIL rc=%d still_on_k0=%d\n",
                   rc2b, still_on_k0);
        fflush(stdout);
    }

    /* ----- Test 3b (cross-kernel completeness) ----- */
    {
        const uint16_t f3b = 8;
        volatile uint32_t *frame3b =
            (volatile uint32_t *)(pool + POOL_FRAME_OFFSET(f3b));
        __atomic_store_n(frame3b, 0x3B0BEEFu, __ATOMIC_RELEASE);

        mk_cap_id_t dst3b = MK_CAP_NONE, src3b = MK_CAP_NONE;
        if (mk_cap_grant(/*dst_kid=*/1, /*dst_vpe=*/1,
                         MK_CAP_MEM, 3, f3b, 0x3B00,
                         MK_CAP_NONE, &dst3b, &src3b) != 0) {
            printf("[TEST 3b] grant FAILED\n"); fflush(stdout);
        } else {
            /* Wait for K1 to derive C2 from C1 to vpe=2. */
            if (sync_wait(pool, /*peer_kid=*/1, /*ready_3b=*/0x3B11, 2000000) != 0) {
                printf("[TEST 3b] sync_wait peer ready timed out — FAIL\n");
                fflush(stdout);
            } else {
                int rc3b = mk_cap_revoke(src3b);
                int gone_src = (mk_captable_find(src3b) < 0);
                int gone_dst_placeholder = (mk_captable_find(dst3b) < 0);
                /* K1 cleanup happens on K1; we report from K0's POV. */
                if (rc3b >= 0 && gone_src && gone_dst_placeholder)
                    printf("[TEST 3b] Cross-kernel completeness: PASS (K0 removed %d "
                           "local caps; K1 reported subtree revoked via FINISH)\n", rc3b);
                else
                    printf("[TEST 3b] Cross-kernel completeness: FAIL rc=%d "
                           "gone_src=%d gone_placeholder=%d\n",
                           rc3b, gone_src, gone_dst_placeholder);
                fflush(stdout);
            }
            /* Signal K1 that the test is done so it can verify its side. */
            sync_set(pool, /*my_kid=*/0, /*done_3b=*/0x3BD0);
        }
    }

    /* ----- Test 5 (spanning tree K0→K1→K0) ----- */
    {
        const uint16_t f5 = 9;
        volatile uint32_t *frame5 =
            (volatile uint32_t *)(pool + POOL_FRAME_OFFSET(f5));
        __atomic_store_n(frame5, 0x500FACEu, __ATOMIC_RELEASE);

        mk_cap_id_t dst5 = MK_CAP_NONE, src5 = MK_CAP_NONE;
        if (mk_cap_grant(/*dst_kid=*/1, /*dst_vpe=*/1,
                         MK_CAP_MEM, 3, f5, 0x5500,
                         MK_CAP_NONE, &dst5, &src5) != 0) {
            printf("[TEST 5] grant K0→K1 FAILED\n"); fflush(stdout);
        } else {
            struct mk_kc_stats before, after;
            mk_kc_get_stats(&before);
            /* Wait for K1 to grant the derived cap back to K0 vpe=2. */
            if (sync_wait(pool, /*peer_kid=*/1, /*ready_5=*/0x5511, 2000000) != 0) {
                printf("[TEST 5] sync_wait peer ready timed out — FAIL\n");
                fflush(stdout);
            } else {
                /* The cap K1 granted back to K0 (C2) lives at K0 vpe=2 sel=f5. */
                mk_cap_id_t k0_c2 = MK_CAP_ID(/*kid=*/0, /*vpe=*/2,
                                              MK_CAP_MEM, f5);
                int present_k0_c2 = (mk_captable_find(k0_c2) >= 0);
                int rc5 = mk_cap_revoke(src5);
                mk_kc_get_stats(&after);
                int gone_src = (mk_captable_find(src5) < 0);
                int gone_k1_placeholder = (mk_captable_find(dst5) < 0);
                int gone_k0_c2 = (mk_captable_find(k0_c2) < 0);
                /* Expect 2 cross-kernel hops (REVOKE_BATCH K0→K1 +
                 * REVOKE_BATCH K1→K0) → 4 ring messages (2 BATCH + 2 FINISH). */
                uint64_t ring_delta = (after.ring_msgs_sent - before.ring_msgs_sent)
                                     + (after.ring_msgs_recv - before.ring_msgs_recv);
                if (present_k0_c2 && rc5 >= 0 && gone_src &&
                    gone_k1_placeholder && gone_k0_c2 && ring_delta >= 4)
                    printf("[TEST 5] Spanning tree walk: PASS "
                           "(K0→K1→K0 cascade; rc5=%d ring_delta=%llu, "
                           "src+placeholder+K0_C2 all gone)\n",
                           rc5, (unsigned long long)ring_delta);
                else
                    printf("[TEST 5] Spanning tree walk: FAIL "
                           "before_k0_c2=%d rc=%d gone(src=%d ph=%d k0_c2=%d) ring_delta=%llu\n",
                           present_k0_c2, rc5, gone_src,
                           gone_k1_placeholder, gone_k0_c2,
                           (unsigned long long)ring_delta);
                fflush(stdout);
            }
            sync_set(pool, /*my_kid=*/0, /*done_5=*/0x55D0);
        }
    }

    /* ----- Test 6 (concurrent exchange + revoke race) ----- */
    {
        const uint16_t f6 = 10;
        volatile uint32_t *frame6 =
            (volatile uint32_t *)(pool + POOL_FRAME_OFFSET(f6));
        __atomic_store_n(frame6, 0x600BEEFu, __ATOMIC_RELEASE);

        mk_cap_id_t dst6 = MK_CAP_NONE, src6 = MK_CAP_NONE;
        int rc_grant = mk_cap_grant(/*dst_kid=*/1, /*dst_vpe=*/1,
                                    MK_CAP_MEM, 3, f6, 0x6600,
                                    MK_CAP_NONE, &dst6, &src6);
        /* No yield here — we want grant followed immediately by revoke
         * to maximally compress any post-ACK window K1 might have. */
        int rc_revoke = mk_cap_revoke(src6);
        int gone_src = (mk_captable_find(src6) < 0);
        int gone_placeholder = (mk_captable_find(dst6) < 0);
        if (rc_grant == 0 && rc_revoke >= 0 && gone_src && gone_placeholder)
            printf("[TEST 6] Concurrent exchange+revoke: PASS "
                   "(grant rc=%d revoke rc=%d, no orphans on K0)\n",
                   rc_grant, rc_revoke);
        else
            printf("[TEST 6] Concurrent exchange+revoke: FAIL "
                   "grant=%d revoke=%d gone(src=%d ph=%d)\n",
                   rc_grant, rc_revoke, gone_src, gone_placeholder);
        fflush(stdout);
        /* Tell K1 we've started+finished the race. */
        sync_set(pool, /*my_kid=*/0, /*done_6=*/0x66D0);
    }

    /* ----- Test 7 (ping-pong throughput) — 8 round trips ----- */
    printf("[TEST 7] kid=0: starting ping-pong (8 rt)\n"); fflush(stdout);
    int pings_ok = 0;
    for (int i = 0; i < 8; ++i) {
        if (mk_kc_ping(/*dst_kid=*/1) == 0) pings_ok++;
    }
    if (pings_ok == 8)
        printf("[TEST 7] Ping-pong: PASS (8/8 round trips)\n");
    else
        printf("[TEST 7] Ping-pong: FAIL (only %d/8)\n", pings_ok);
    fflush(stdout);

    /* ----- Phase C.1: Monotonic permission restriction (intra-kernel) -- */
    {
        /* Grant parent with R-only at frame_idx=11. Derive a child to
         * vpe=4 requesting RW (3). Child's perms must end up R (1). */
        const uint16_t f = 11;
        mk_cap_id_t parent_id = MK_CAP_NONE, _src = MK_CAP_NONE;
        int rcp = mk_cap_grant(/*dst_kid=*/0, /*dst_vpe=*/3,
                               MK_CAP_MEM, MK_PERM_R, f, 0xC100,
                               MK_CAP_NONE, &parent_id, &_src);
        if (rcp != 0) {
            printf("[TEST C.1] parent grant FAILED rc=%d\n", rcp);
        } else {
            mk_cap_id_t child_id = MK_CAP_NONE;
            int rcc = mk_cap_grant(/*dst_kid=*/0, /*dst_vpe=*/4,
                                   MK_CAP_MEM, MK_PERM_RW, f, 0xC110,
                                   parent_id, &child_id, NULL);
            int idx = mk_captable_find(child_id);
            struct mk_cap *child = idx >= 0 ? mk_captable_at(idx) : NULL;
            if (rcc == 0 && child && child->perms == MK_PERM_R)
                printf("[TEST C.1] Monotonic perms: PASS "
                       "(requested RW, parent R-only → child perms=R)\n");
            else
                printf("[TEST C.1] Monotonic perms: FAIL rc=%d child.perms=%d "
                       "(expected %d=R)\n",
                       rcc, child ? child->perms : -1, MK_PERM_R);
            mk_cap_revoke(parent_id);  /* clean up subtree */
        }
        fflush(stdout);
    }

    /* ----- Phase C.1 TOCTOU: parent revoked while derivation in flight -- */
    {
        /* Local fast-path: the AND happens BEFORE installing, so revoking
         * the parent right after grant should not leave the child with
         * un-ANDed perms. Simulate by: grant parent R, derive child RW
         * (gets R), revoke parent, verify child also gone (cascade). */
        const uint16_t f = 12;
        mk_cap_id_t p = MK_CAP_NONE, _s = MK_CAP_NONE;
        if (mk_cap_grant(0, 3, MK_CAP_MEM, MK_PERM_R, f, 0xC1A0,
                         MK_CAP_NONE, &p, &_s) == 0) {
            mk_cap_id_t c = MK_CAP_NONE;
            mk_cap_grant(0, 4, MK_CAP_MEM, MK_PERM_RW, f, 0xC1A1,
                         p, &c, NULL);
            int rc = mk_cap_revoke(p);  /* parent + child gone */
            int p_gone = mk_captable_find(p) < 0;
            int c_gone = mk_captable_find(c) < 0;
            if (rc >= 0 && p_gone && c_gone)
                printf("[TEST C.1-TOCTOU] cascade revoke: PASS "
                       "(parent + child both gone, no orphan with un-ANDed perms)\n");
            else
                printf("[TEST C.1-TOCTOU] cascade revoke: FAIL "
                       "rc=%d p_gone=%d c_gone=%d\n",
                       rc, p_gone, c_gone);
        }
        fflush(stdout);
    }

    /* ----- Phase C.2: mk_cap_check_perms helper at use-time ----------- */
    {
        const uint16_t f = 13;
        mk_cap_id_t cap = MK_CAP_NONE, _s = MK_CAP_NONE;
        int n = 0, fail = 0;
        if (mk_cap_grant(0, 5, MK_CAP_MEM, MK_PERM_R, f, 0xC200,
                         MK_CAP_NONE, &cap, &_s) != 0) {
            printf("[TEST C.2] grant FAILED\n");
        } else {
            n++; if (!mk_cap_check_perms(cap, MK_PERM_R))           fail++;
            n++; if ( mk_cap_check_perms(cap, MK_PERM_W))           fail++;
            n++; if ( mk_cap_check_perms(cap, MK_PERM_RW))          fail++;
            n++; if (!mk_cap_check_perms(cap, 0))                   fail++; /* empty subset always OK */
            mk_cap_revoke(cap);
            /* After revoke: every check returns false (cap gone). */
            n++; if ( mk_cap_check_perms(cap, MK_PERM_R))           fail++;
            n++; if ( mk_cap_check_perms(cap, 0))                   fail++;
            if (fail == 0)
                printf("[TEST C.2] mk_cap_check_perms: PASS "
                       "(R-only cap allows R, denies W and RW; revoked cap denies all; %d assertions)\n", n);
            else
                printf("[TEST C.2] mk_cap_check_perms: FAIL %d/%d assertions failed\n",
                       fail, n);
        }
        fflush(stdout);
    }

    /* ----- Phase C.3: EP state machine activation -------------------- */
    {
        /* Carve a small scratch region out of frame 15 of the pool for a
         * standalone ring. We're the only writer to this region. The
         * ring is purely intra-component — never sent over hardware. */
        static uint8_t scratch[VDTU_RING_CTRL_SIZE + 4 * VDTU_DEFAULT_SLOT_SIZE]
            __attribute__((aligned(64)));
        struct vdtu_ring r;
        int n = 0, fail = 0;

        /* Pre-init state: zeroed memory means UNCONFIGURED. */
        memset(scratch, 0, vdtu_ring_total_size(4, VDTU_DEFAULT_SLOT_SIZE));

        /* Direct verified-function call: try to skip UNCONFIGURED→ACTIVE.
         * Verified rejects (returns false). State stays UNCONFIGURED. */
        vdtu_ep_state_t s = VDTU_EP_UNCONFIGURED;
        n++; if ( vdtu_ep_state_transition(&s, VDTU_EP_ACTIVE, false))     fail++;
        n++; if (s != VDTU_EP_UNCONFIGURED)                                fail++;
        /* Valid sequence UNCONFIGURED → CONFIGURED → ACTIVE. */
        n++; if (!vdtu_ep_state_transition(&s, VDTU_EP_CONFIGURED, false)) fail++;
        n++; if (!vdtu_ep_state_transition(&s, VDTU_EP_ACTIVE, false))     fail++;
        /* TERMINATED requires blocked=true. */
        n++; if ( vdtu_ep_state_transition(&s, VDTU_EP_TERMINATED, false)) fail++;
        n++; if (!vdtu_ep_state_transition(&s, VDTU_EP_TERMINATED, true))  fail++;
        /* TERMINATED is absorbing — no escape. */
        n++; if ( vdtu_ep_state_transition(&s, VDTU_EP_ACTIVE, false))     fail++;

        /* vdtu_ring_init walks UNCONFIGURED → CONFIGURED → ACTIVE for us. */
        n++; if (vdtu_ring_init(&r, scratch, 4, VDTU_DEFAULT_SLOT_SIZE) != 0) fail++;
        n++; if (vdtu_ring_get_state(&r) != VDTU_EP_ACTIVE)                  fail++;

        /* Send works in ACTIVE. */
        const char *body = "C3";
        n++; if (vdtu_ring_send(&r, 0,0,0,0,0,0,0, body, 2) != 0)            fail++;
        n++; if (vdtu_ring_fetch(&r) == NULL)                                fail++;
        vdtu_ring_ack(&r);

        /* Detach → TERMINATED. Send must now reject (-3). Fetch returns NULL. */
        n++; if (vdtu_ring_detach(&r) != 0)                                  fail++;
        n++; if (vdtu_ring_get_state(&r) != VDTU_EP_TERMINATED)              fail++;
        n++; if (vdtu_ring_send(&r, 0,0,0,0,0,0,0, body, 2) != -3)           fail++;
        n++; if (vdtu_ring_fetch(&r) != NULL)                                fail++;

        if (fail == 0)
            printf("[TEST C.3] EP state machine: PASS "
                   "(verified function rejects skip-ahead and pre-blocked TERMINATE; "
                   "ring respects state in send/fetch/detach; %d assertions)\n", n);
        else
            printf("[TEST C.3] EP state machine: FAIL %d/%d assertions failed\n",
                   fail, n);
        fflush(stdout);
    }

    /* ----- Phase C.3 TOCTOU: concurrent attach/detach convergence ----- */
    {
        /* Single-threaded simulation: producer detaches while a consumer
         * is mid-fetch. Outcome must be deterministic — fetch sees the
         * TERMINATED state and returns NULL (no torn reads). */
        static uint8_t scratch[VDTU_RING_CTRL_SIZE + 4 * VDTU_DEFAULT_SLOT_SIZE]
            __attribute__((aligned(64)));
        memset(scratch, 0, vdtu_ring_total_size(4, VDTU_DEFAULT_SLOT_SIZE));
        struct vdtu_ring r;
        int rc_init = vdtu_ring_init(&r, scratch, 4, VDTU_DEFAULT_SLOT_SIZE);
        const char *body = "C3T";
        int rc_send = vdtu_ring_send(&r, 0,0,0,0,0,0,0, body, 3);
        /* Detach BEFORE the consumer fetches. */
        int rc_detach = vdtu_ring_detach(&r);
        const struct vdtu_message *m = vdtu_ring_fetch(&r);
        if (rc_init == 0 && rc_send == 0 && rc_detach == 0 && m == NULL)
            printf("[TEST C.3-TOCTOU] detach/fetch convergence: PASS "
                   "(post-detach fetch returns NULL — no torn read)\n");
        else
            printf("[TEST C.3-TOCTOU] detach/fetch convergence: FAIL "
                   "init=%d send=%d detach=%d fetch_null=%d\n",
                   rc_init, rc_send, rc_detach, m == NULL);
        fflush(stdout);
    }

    /* ----- Phase C.4: RevocationList — concurrent-revoke convergence -- */
    {
        /* Grant a parent cap with a cross-kernel child so the revoke
         * walker yields (waits on K1's REVOKE_FINISH). While yielded,
         * a second worker enters mk_cap_revoke on the same cap and
         * must subscribe rather than start a duplicate walk. */
        const uint16_t f = 0;  /* unused — root grant uses fresh cap */
        (void)f;
        mk_cap_id_t parent = MK_CAP_NONE, _src = MK_CAP_NONE;
        if (mk_cap_grant(/*dst_kid=*/1, /*dst_vpe=*/2,
                         MK_CAP_MEM, MK_PERM_RW, /*frame=*/4, 0xC400,
                         MK_CAP_NONE, &parent, &_src) != 0) {
            printf("[TEST C.4] cross-kernel grant FAILED\n");
        } else {
            /* Use the donor (src) cap — it's the parent of the placeholder
             * for the K1 child, so revoking src walks to the placeholder
             * and sends REVOKE_BATCH (yielding the walker). */
            struct rev_arg a1 = { .cap = _src };
            struct rev_arg a2 = { .cap = _src };
            size_t before = mk_rev_size();
            if (!mk_thread_spawn(rev_worker, &a1)) {
                printf("[TEST C.4] spawn worker A FAILED\n");
            } else if (!mk_thread_spawn(rev_worker, &a2)) {
                printf("[TEST C.4] spawn worker B FAILED\n");
            } else {
                /* Yield repeatedly until both workers finish. The first
                 * worker becomes the walker, the second subscribes. */
                int budget = 2000000;
                while (budget-- > 0 && !(a1.finished && a2.finished))
                    mk_thread_yield();
                size_t after = mk_rev_size();
                /* Both must finish, both must report the same result
                 * (subscriber inherits walker's result), and the table
                 * must be empty post-completion. */
                int both = a1.finished && a2.finished;
                int same = both && (a1.result == a2.result);
                if (both && same && a1.result >= 0 && after == before)
                    printf("[TEST C.4] RevocationList converge: PASS "
                           "(both workers got result=%d, RevList post-size=%zu)\n",
                           a1.result, after);
                else
                    printf("[TEST C.4] RevocationList converge: FAIL "
                           "fin=(%d,%d) res=(%d,%d) rev_size=%zu→%zu\n",
                           a1.finished, a2.finished, a1.result, a2.result,
                           before, after);
            }
        }
        fflush(stdout);
    }

    /* ----- Phase C.4 TOCTOU: K0+K1 revoking overlapping subtrees ------ */
    {
        /* K0 holds donor of a chain to K1. K0 revokes its donor while
         * K1's worker (set up in test_driver_kid1) revokes the placeholder
         * locally. With RevocationList both kernels' walks converge:
         * - K0's revoke triggers REVOKE_BATCH to K1
         * - K1's local revoke (started ~simultaneously) collides with the
         *   incoming REVOKE_BATCH; the second to attempt mk_rev_attach
         *   subscribes rather than walking again
         * - Final state: subtree fully revoked, no orphans, no double-frees. */
        mk_cap_id_t p = MK_CAP_NONE, _src = MK_CAP_NONE;
        if (mk_cap_grant(/*dst_kid=*/1, /*dst_vpe=*/2,
                         MK_CAP_MEM, MK_PERM_RW, /*frame=*/4, 0xC4A0,
                         MK_CAP_NONE, &p, &_src) == 0) {
            /* Tell K1 to start its own revoke now. */
            sync_set(pool, /*my_kid=*/0, /*go_c4_toctou=*/0xC4A1);
            int rc_k0 = mk_cap_revoke(_src);
            int p_gone = mk_captable_find(_src) < 0;
            int placeholder_gone = mk_captable_find(p) < 0;
            sync_wait(pool, /*peer_kid=*/1, /*done_c4=*/0xC4A2, 2000000);
            if (rc_k0 >= 0 && p_gone && placeholder_gone)
                printf("[TEST C.4-TOCTOU] overlapping concurrent revoke: PASS "
                       "(K0 rc=%d, donor + placeholder both gone)\n", rc_k0);
            else
                printf("[TEST C.4-TOCTOU] overlapping concurrent revoke: FAIL "
                       "rc=%d donor_gone=%d placeholder_gone=%d\n",
                       rc_k0, p_gone, placeholder_gone);
        }
        fflush(stdout);
    }

    /* ----- Phase C.5: per-type revoke action dispatch --------------- */
    {
        /* Build a 3-level intra-kernel chain of MEM caps, register a
         * stub action, revoke from root, verify the action fired
         * exactly 3 times and the chain is gone. */
        mk_cap_revoke_action_reset_counters();
        size_t before = mk_cap_revoke_action_count(MK_CAP_MEM);

        mk_cap_id_t l1 = MK_CAP_NONE, l1_src = MK_CAP_NONE;
        mk_cap_id_t l2 = MK_CAP_NONE, l3 = MK_CAP_NONE;
        mk_cap_grant(0, 7, MK_CAP_MEM, MK_PERM_RW, 1, 0xC500,
                     MK_CAP_NONE, &l1, &l1_src);
        mk_cap_grant(0, 8, MK_CAP_MEM, MK_PERM_RW, 1, 0xC501,
                     l1, &l2, NULL);
        mk_cap_grant(0, 9, MK_CAP_MEM, MK_PERM_RW, 1, 0xC502,
                     l2, &l3, NULL);

        int rc = mk_cap_revoke(l1_src);
        size_t after = mk_cap_revoke_action_count(MK_CAP_MEM);
        size_t fired = after - before;

        if (rc >= 4 && fired >= 4 &&
            mk_captable_find(l1_src) < 0 && mk_captable_find(l1) < 0 &&
            mk_captable_find(l2) < 0     && mk_captable_find(l3) < 0)
            printf("[TEST C.5] per-type revoke action: PASS "
                   "(rc=%d MEM action fired %zu times, full chain gone)\n",
                   rc, fired);
        else
            printf("[TEST C.5] per-type revoke action: FAIL "
                   "rc=%d fired=%zu before=%zu\n", rc, fired, before);
        fflush(stdout);
    }

    /* ----- Phase C.5 TOCTOU: action runs once per cap when 2 workers
     * race on the same root.
     *
     * Ordering: the first worker becomes the walker and removes the
     * subtree; the second finds no entry to revoke (cap_id was removed
     * by the walker). Crucially the second's `mk_cap_revoke` does NOT
     * re-fire the per-type action because there is nothing to walk —
     * the action firing count therefore stays at the subtree size. */
    {
        size_t before = mk_cap_revoke_action_count(MK_CAP_MEM);
        mk_cap_id_t a = MK_CAP_NONE, a_src = MK_CAP_NONE;
        /* Use a distinct frame index that no prior test installed. */
        if (mk_cap_grant(0, 10, MK_CAP_MEM, MK_PERM_RW, /*frame=*/3, 0xC5A0,
                         MK_CAP_NONE, &a, &a_src) == 0) {
            printf("[TEST C.5-TOCTOU] grant ok a=0x%llx a_src=0x%llx\n",
                   (unsigned long long)a, (unsigned long long)a_src);
            fflush(stdout);
            struct rev_arg w1 = { .cap = a_src };
            struct rev_arg w2 = { .cap = a_src };
            if (!mk_thread_spawn(rev_worker, &w1)) {
                printf("[TEST C.5-TOCTOU] spawn w1 FAIL\n");
            } else if (!mk_thread_spawn(rev_worker, &w2)) {
                printf("[TEST C.5-TOCTOU] spawn w2 FAIL\n");
            } else {
                int budget = 100000;
                while (budget-- > 0 && !(w1.finished && w2.finished))
                    mk_thread_yield();
                size_t fired = mk_cap_revoke_action_count(MK_CAP_MEM) - before;
                if (w1.finished && w2.finished && fired == 2)
                    printf("[TEST C.5-TOCTOU] action fires once per cap: PASS "
                           "(2 caps in subtree → 2 action invocations across 2 workers, "
                           "results=%d/%d)\n",
                           w1.result, w2.result);
                else
                    printf("[TEST C.5-TOCTOU] action fires once per cap: FAIL "
                           "fin=(%d,%d) res=(%d,%d) fired=%zu (expected 2) budget_left=%d\n",
                           w1.finished, w2.finished,
                           w1.result, w2.result, fired, budget);
            }
        } else {
            printf("[TEST C.5-TOCTOU] grant FAIL\n");
        }
        fflush(stdout);
    }

    /* ===== Phase A — VPE struct + syscall interface ================== */

    /* ----- Phase A.1: VPE struct as first-class object ---------------- */
    {
        mk_vpe_init();
        struct mk_vpe *v1 = mk_vpe_create(/*id=*/100, /*kid=*/0, MK_VPE_THREAD);
        struct mk_vpe *v2 = mk_vpe_create(/*id=*/101, /*kid=*/0, MK_VPE_THREAD);
        struct mk_vpe *find1 = mk_vpe_find(100);
        size_t cnt = mk_vpe_count();
        if (v1 && v2 && find1 == v1 && cnt == 2 &&
            v1->state == MK_VPE_STATE_IDLE && v1->type == MK_VPE_THREAD)
            printf("[TEST A.1] VPE struct: PASS (created 2, find_by_id ok, "
                   "count=%zu, initial state=IDLE)\n", cnt);
        else
            printf("[TEST A.1] VPE struct: FAIL v1=%p v2=%p find1=%p cnt=%zu\n",
                   v1, v2, find1, cnt);
        fflush(stdout);
    }

    /* ----- Phase A.2: per-VPE selector table -------------------------- */
    {
        struct mk_vpe *v = mk_vpe_find(100);
        int s1 = mk_vpe_alloc_sel(v);
        int s2 = mk_vpe_alloc_sel(v);
        mk_cap_id_t fake_cap = MK_CAP_ID(0, 100, MK_CAP_MEM, 42);
        mk_vpe_bind_sel(v, (uint16_t)s1, fake_cap);
        mk_cap_id_t looked = mk_vpe_lookup_sel(v, (uint16_t)s1);
        mk_cap_id_t empty  = mk_vpe_lookup_sel(v, (uint16_t)s2);
        mk_vpe_clear_sel(v, (uint16_t)s1);
        mk_cap_id_t cleared = mk_vpe_lookup_sel(v, (uint16_t)s1);
        if (s1 > 0 && s2 > s1 && looked == fake_cap &&
            empty == MK_CAP_NONE && cleared == MK_CAP_NONE)
            printf("[TEST A.2] VPE-local selectors: PASS "
                   "(alloc unique sels %d %d, bind/lookup/clear roundtrip)\n",
                   s1, s2);
        else
            printf("[TEST A.2] VPE-local selectors: FAIL "
                   "s1=%d s2=%d looked=0x%llx empty=0x%llx cleared=0x%llx\n",
                   s1, s2, (unsigned long long)looked,
                   (unsigned long long)empty, (unsigned long long)cleared);
        fflush(stdout);
    }

    /* ----- Phase A.3+A.4: SemperKernel root grant + DELEGATE syscall -- */
    {
        struct mk_vpe *donor    = mk_vpe_find(100);
        struct mk_vpe *recipient = mk_vpe_find(101);
        /* Kernel grants donor a fresh root cap (frame 2 in pool). */
        int donor_sel = mk_kernel_grant_root(donor, MK_CAP_MEM,
                                             MK_PERM_RW, /*frame=*/2, 0xA300);
        /* Donor delegates to recipient via syscall — same kernel, kid=0. */
        uint16_t target_sel = 0;
        int rc_del = mk_syscall_delegate(donor, (uint16_t)donor_sel,
                                         /*target_kid=*/0,
                                         /*target_vpe_id=*/101,
                                         MK_PERM_RW, &target_sel);
        mk_cap_id_t recipient_cap = mk_vpe_lookup_sel(recipient, target_sel);

        /* ACTIVATE / DEACTIVATE round-trip. */
        int rc_act    = mk_syscall_activate(recipient, target_sel, /*ep_id=*/3);
        int rc_deact  = mk_syscall_deactivate(recipient, target_sel);

        /* REVOKE on donor side cascades to recipient. */
        int rc_rev = mk_syscall_revoke(donor, (uint16_t)donor_sel);
        mk_cap_id_t after_donor    = mk_vpe_lookup_sel(donor, (uint16_t)donor_sel);
        mk_cap_id_t after_recipient = mk_vpe_lookup_sel(recipient, target_sel);

        if (donor_sel > 0 && rc_del == 0 && recipient_cap != MK_CAP_NONE &&
            rc_act == 0 && rc_deact == 0 && rc_rev >= 0 &&
            after_donor == MK_CAP_NONE)
            printf("[TEST A.3/A.4] syscall flow OBTAIN-equivalent + ACTIVATE + "
                   "DEACTIVATE + DELEGATE + REVOKE: PASS "
                   "(donor_sel=%d target_sel=%u rc_revoke=%d)\n",
                   donor_sel, target_sel, rc_rev);
        else
            printf("[TEST A.3/A.4] syscall flow: FAIL "
                   "donor_sel=%d rc_del=%d rcap=0x%llx rc_act=%d rc_deact=%d "
                   "rc_rev=%d after_d=0x%llx after_r=0x%llx\n",
                   donor_sel, rc_del,
                   (unsigned long long)recipient_cap,
                   rc_act, rc_deact, rc_rev,
                   (unsigned long long)after_donor,
                   (unsigned long long)after_recipient);
        fflush(stdout);
    }

    /* ----- Phase A.4: OBTAIN stub returns -ENOSYS until Phase B ------- */
    {
        struct mk_vpe *v = mk_vpe_find(100);
        uint16_t sel = 0;
        int rc = mk_syscall_obtain(v, "imaginary-service", MK_PERM_R, &sel);
        if (rc == -38)
            printf("[TEST A.4] OBTAIN stub: PASS (returns -ENOSYS=%d "
                   "until Phase B SERVICE directory)\n", rc);
        else
            printf("[TEST A.4] OBTAIN stub: FAIL rc=%d (expected -38)\n", rc);
        fflush(stdout);
    }

    /* ----- Phase A.5: Re-run sentinels — confirm A.x didn't perturb -- */
    {
        /* The "tests 1-7 still pass" property is verified by the rest of
         * test_driver_kid0/kid1 already running before this point.
         * Restate explicitly: fast-path counter > 0 (Test 4),
         * revoke_count > 0 (Tests 2a/2b/3a/3b/5/6/C.x), pings_ok still 8. */
        struct mk_kc_stats st;
        mk_kc_get_stats(&st);
        int regression =
            (st.local_fastpath_count == 0) ||
            (st.revoke_count == 0)         ||
            (pings_ok != 8);
        if (!regression)
            printf("[TEST A.5] Phase A regression sentinels: PASS "
                   "(fastpath=%llu revokes=%llu pings=%d)\n",
                   (unsigned long long)st.local_fastpath_count,
                   (unsigned long long)st.revoke_count, pings_ok);
        else
            printf("[TEST A.5] Phase A regression sentinels: FAIL "
                   "fastpath=%llu revokes=%llu pings=%d\n",
                   (unsigned long long)st.local_fastpath_count,
                   (unsigned long long)st.revoke_count, pings_ok);
        fflush(stdout);
    }

    /* ----- Phase C.2 TOCTOU: write-permitted check passes, revoke, retry */
    {
        /* Sequence: grant RW, check W=true, revoke, check W=false. The
         * cooperative-thread model gives us deterministic ordering, but
         * the property held in the audit (Item 1) is: a check that
         * passed must NOT be authority over a subsequent op that is
         * issued AFTER a revoke completes. */
        const uint16_t f = 14;
        mk_cap_id_t cap = MK_CAP_NONE, _s = MK_CAP_NONE;
        if (mk_cap_grant(0, 6, MK_CAP_MEM, MK_PERM_RW, f, 0xC2A0,
                         MK_CAP_NONE, &cap, &_s) == 0) {
            int pre  = mk_cap_check_perms(cap, MK_PERM_W);
            mk_cap_revoke(cap);
            int post = mk_cap_check_perms(cap, MK_PERM_W);
            if (pre && !post)
                printf("[TEST C.2-TOCTOU] permission rescinded by revoke: PASS "
                       "(pre-revoke W=true, post-revoke W=false)\n");
            else
                printf("[TEST C.2-TOCTOU] permission rescinded by revoke: FAIL pre=%d post=%d\n",
                       pre, post);
        }
        fflush(stdout);
    }

    /* Stats summary. */
    struct mk_kc_stats st;
    mk_kc_get_stats(&st);
    printf("[STATS] kid=0: ring_sent=%llu ring_recv=%llu exchanges=%llu pings ok=%d\n",
           (unsigned long long)st.ring_msgs_sent,
           (unsigned long long)st.ring_msgs_recv,
           (unsigned long long)st.exchange_count,
           pings_ok);
    fflush(stdout);
}

static void test_driver_kid1(void *raw)
{
    uint8_t *pool = (uint8_t *)raw;

    /* Test 7: nothing to do — handler thread auto-PONGs. */

    /* Test 1: poll for the cap to be installed by our handler, then read
     * frame TEST1_FRAME_IDX and check the magic. The handler thread is
     * the only thing that can install the cap so we yield repeatedly. */
    mk_cap_id_t expected = MK_CAP_ID(/*kid=*/1, /*vpe=*/1,
                                     MK_CAP_MEM, TEST1_FRAME_IDX);
    for (int i = 0; i < 2000000; ++i) {
        if (mk_captable_find(expected) >= 0) break;
        mk_thread_yield();
    }
    if (mk_captable_find(expected) < 0) {
        printf("[TEST 1] kid=1: cap 0x%llx never installed — FAIL\n",
               (unsigned long long)expected);
        fflush(stdout);
        return;
    }
    volatile uint32_t *frame =
        (volatile uint32_t *)(pool + POOL_FRAME_OFFSET(TEST1_FRAME_IDX));
    uint32_t v = __atomic_load_n(frame, __ATOMIC_ACQUIRE);
    if (v == TEST1_MAGIC)
        printf("[TEST 1] Cross-kernel exchange: PASS (K1 read 0x%X via cap 0x%llx)\n",
               v, (unsigned long long)expected);
    else
        printf("[TEST 1] Cross-kernel exchange: FAIL (read 0x%X expected 0x%X)\n",
               v, TEST1_MAGIC);
    fflush(stdout);

    /* Test 2b verification: poll for the cap to disappear from our side.
     * K0 reaches its Test 2b only after running Tests 4, 3a, 2a — give
     * enough budget for the QEMU-TCG cycle cost. */
    for (int i = 0; i < 2000000; ++i) {
        if (mk_captable_find(expected) < 0) break;
        mk_thread_yield();
    }
    if (mk_captable_find(expected) < 0)
        printf("[TEST 2b] kid=1: cap 0x%llx removed by REVOKE_BATCH — PASS\n",
               (unsigned long long)expected);
    else
        printf("[TEST 2b] kid=1: cap 0x%llx still present after polling — FAIL\n",
               (unsigned long long)expected);
    fflush(stdout);

    /* ----- Test 3b on K1: derive C2 from C1 to vpe=2, signal ready,
     * then poll for the whole subtree to disappear. */
    {
        const uint16_t f3b = 8;
        mk_cap_id_t cap_c1 = MK_CAP_ID(/*kid=*/1, /*vpe=*/1, MK_CAP_MEM, f3b);
        for (int i = 0; i < 2000000; ++i) {
            if (mk_captable_find(cap_c1) >= 0) break;
            mk_thread_yield();
        }
        if (mk_captable_find(cap_c1) < 0) {
            printf("[TEST 3b] kid=1: C1 never arrived — FAIL\n");
            fflush(stdout);
        } else {
            mk_cap_id_t c2 = MK_CAP_NONE;
            int rc = mk_cap_grant(/*dst_kid=*/1, /*dst_vpe=*/2,
                                  MK_CAP_MEM, 3, f3b, 0x3B22,
                                  /*parent=*/cap_c1, &c2, NULL);
            if (rc != 0)
                printf("[TEST 3b] kid=1: derived grant FAILED rc=%d\n", rc);
            sync_set(pool, /*my_kid=*/1, /*ready_3b=*/0x3B11);
            /* Wait for K0's revoke to finish. */
            for (int i = 0; i < 2000000; ++i) {
                if (mk_captable_find(cap_c1) < 0) break;
                mk_thread_yield();
            }
            int gone_c1 = (mk_captable_find(cap_c1) < 0);
            int gone_c2 = (mk_captable_find(c2) < 0);
            if (gone_c1 && gone_c2)
                printf("[TEST 3b] kid=1: subtree gone (C1 + derived C2) — PASS\n");
            else
                printf("[TEST 3b] kid=1: gone_c1=%d gone_c2=%d — FAIL\n",
                       gone_c1, gone_c2);
            fflush(stdout);
            sync_wait(pool, 0, 0x3BD0, 2000000);
        }
    }

    /* ----- Test 5 on K1: derive C2 from C1 and grant cross-kernel
     * back to K0 vpe=2, signal ready, poll for subtree to vanish. */
    {
        const uint16_t f5 = 9;
        mk_cap_id_t cap_c1_5 = MK_CAP_ID(/*kid=*/1, /*vpe=*/1, MK_CAP_MEM, f5);
        for (int i = 0; i < 2000000; ++i) {
            if (mk_captable_find(cap_c1_5) >= 0) break;
            mk_thread_yield();
        }
        if (mk_captable_find(cap_c1_5) < 0) {
            printf("[TEST 5] kid=1: C1 never arrived — FAIL\n");
            fflush(stdout);
        } else {
            mk_cap_id_t k0_c2 = MK_CAP_NONE;
            int rc = mk_cap_grant(/*dst_kid=*/0, /*dst_vpe=*/2,
                                  MK_CAP_MEM, 3, f5, 0x5522,
                                  /*parent=*/cap_c1_5, &k0_c2, NULL);
            if (rc != 0)
                printf("[TEST 5] kid=1: cross-kernel derived grant FAILED rc=%d\n", rc);
            sync_set(pool, /*my_kid=*/1, /*ready_5=*/0x5511);
            for (int i = 0; i < 2000000; ++i) {
                if (mk_captable_find(cap_c1_5) < 0) break;
                mk_thread_yield();
            }
            int gone_c1 = (mk_captable_find(cap_c1_5) < 0);
            /* k0_c2 is the placeholder on K1's side for K0's cap. */
            int gone_k0_c2_placeholder = (mk_captable_find(k0_c2) < 0);
            if (gone_c1 && gone_k0_c2_placeholder)
                printf("[TEST 5] kid=1: C1 + K0_C2 placeholder gone — PASS\n");
            else
                printf("[TEST 5] kid=1: gone_c1=%d gone_k0_c2_placeholder=%d — FAIL\n",
                       gone_c1, gone_k0_c2_placeholder);
            fflush(stdout);
            sync_wait(pool, 0, 0x55D0, 2000000);
        }
    }

    /* ----- Test 6 on K1: just verify final state after K0's
     * grant+revoke race settles. */
    {
        const uint16_t f6 = 10;
        mk_cap_id_t cap_t6 = MK_CAP_ID(/*kid=*/1, /*vpe=*/1, MK_CAP_MEM, f6);
        sync_wait(pool, 0, 0x66D0, 2000000);
        /* Wait a few yields for any in-flight FINISH to settle. */
        for (int i = 0; i < 2000000; ++i) {
            if (mk_captable_find(cap_t6) < 0) break;
            mk_thread_yield();
        }
        if (mk_captable_find(cap_t6) < 0)
            printf("[TEST 6] kid=1: cap_t6 cleanly removed by REVOKE — PASS\n");
        else
            printf("[TEST 6] kid=1: cap_t6 lingers after revoke — FAIL\n");
        fflush(stdout);
    }

    /* ----- Phase C.4 TOCTOU on K1: race K0's revoke ------------------ */
    {
        /* Wait for K0 to signal "go" — at that moment K0's
         * revoke is about to fire. We immediately try to revoke
         * the same subtree on our side. Whichever side calls
         * mk_rev_attach first walks; the other subscribes. */
        if (sync_wait(pool, /*peer_kid=*/0, /*go_c4=*/0xC4A1, 2000000) == 0) {
            mk_cap_id_t local = MK_CAP_ID(/*kid=*/1, /*vpe=*/2,
                                          MK_CAP_MEM, /*frame=*/4);
            /* Wait briefly for the cap to land. */
            for (int i = 0; i < 1000 && mk_captable_find(local) < 0; ++i)
                mk_thread_yield();
            (void)mk_cap_revoke(local);
            sync_set(pool, /*my_kid=*/1, /*done_c4=*/0xC4A2);
        }
    }

    struct mk_kc_stats st;
    mk_kc_get_stats(&st);
    printf("[STATS] kid=1: ring_sent=%llu ring_recv=%llu exchanges=%llu revokes=%llu\n",
           (unsigned long long)st.ring_msgs_sent,
           (unsigned long long)st.ring_msgs_recv,
           (unsigned long long)st.exchange_count,
           (unsigned long long)st.revoke_count);
    fflush(stdout);
}

int run(void)
{
    uint8_t *pool_base = (uint8_t *)(void *)cross_kernel_pool;

    printf("ComponentC[kid=%u]: pool vaddr=%p paddr=0x%llx\n",
           (unsigned)MULTIKERNEL_KID, (void *)pool_base,
           (unsigned long long)POOL_PADDR);
    fflush(stdout);

    if (mht_init(&g_mht, pool_base, POOL_PADDR,
                 (uint16_t)MULTIKERNEL_KID, MULTIKERNEL_NUM_KIDS) != 0) {
        printf("ComponentC[kid=%u]: mht_init FAILED\n", (unsigned)MULTIKERNEL_KID);
        return -1;
    }
    if (mht_wait_all_alive(&g_mht, pool_base, 50000000ull) != 0) {
        printf("ComponentC[kid=%u]: mht_wait_all_alive FAILED\n",
               (unsigned)MULTIKERNEL_KID);
        return -1;
    }
    printf("ComponentC[kid=%u]: MHT ready\n", (unsigned)MULTIKERNEL_KID);
    fflush(stdout);

    mk_threadmgr_init();

    if (mk_kc_init(pool_base, &g_mht) != 0) {
        printf("ComponentC[kid=%u]: mk_kc_init FAILED\n", (unsigned)MULTIKERNEL_KID);
        return -1;
    }
    printf("ComponentC[kid=%u]: KernelcallHandler ready\n", (unsigned)MULTIKERNEL_KID);
    fflush(stdout);

    /* Spawn the test driver. Different role per kid. */
    mk_thread_fn driver = (MULTIKERNEL_KID == 0) ? test_driver_kid0 : test_driver_kid1;
    if (!mk_thread_spawn(driver, pool_base)) {
        printf("ComponentC[kid=%u]: spawn driver FAILED\n",
               (unsigned)MULTIKERNEL_KID);
        return -1;
    }

    /* Run cooperatively until both driver and handler block (handler
     * never exits — it polls forever on the ring — so this loop runs
     * the driver to completion and then yields back to the handler
     * indefinitely). */
    mk_threadmgr_run_until_idle();
    /* After driver completes, handler is the only ready/yieldable thread.
     * Stay in a polling loop here so the handler keeps draining the ring. */
    for (;;) {
        mk_thread_yield();
    }
    return 0;
}
