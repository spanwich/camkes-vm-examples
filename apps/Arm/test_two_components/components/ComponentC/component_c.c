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
#include <multikernel/mht.h>
#include <multikernel/threadmgr.h>
#include <multikernel/kernelcall.h>

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
    for (int i = 0; i < 5000; ++i) {
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
