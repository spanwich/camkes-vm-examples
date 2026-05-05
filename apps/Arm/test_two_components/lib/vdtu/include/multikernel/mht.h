/*
 * mht.h -- Membership Hash Table (Multikernel-AMP §5.1, §8.1)
 *
 * Routing table mapping kernel_id (kid) → physical layout of the cross-kernel
 * shared pool that connects this kernel to peer kernels. For 2 kernels the
 * MHT is trivial (every kid → same pool); we still go through the array so
 * the same code works for N kernels (architecture §1.2).
 *
 * Boot discovery protocol (Phase 1, polling-only):
 *   1. Each kid writes MHT_ALIVE_MAGIC | kid to mht_boot_status[kid] in the
 *      pool's announcement region.
 *   2. Each kid spins on mht_boot_status[other_kid] until it sees the magic.
 *   3. Once all expected kids have reported, MHT is "ready" and the
 *      KernelcallHandler may begin polling the control rings.
 *
 * Phase 3 (future): replace boot polling with IPI-cap notification.
 */

#ifndef MULTIKERNEL_MHT_H
#define MULTIKERNEL_MHT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DTU header dest_kid field is 4 bits → 16 kids max. Plenty for QEMU MVP. */
#define MHT_MAX_KIDS            16

/* Announcement region inside the shared pool — offset within the 64 KiB
 * pool. Sits at the tail of frame 15 so it's far from frame 0/1 (control
 * rings). 16 × 4 B = 64 B aligned to a cache line. */
#define MHT_BOOT_STATUS_OFFSET  0xFFC0

/* Magic value bookkeeping. When a kid is alive it writes
 *     MHT_ALIVE_MAGIC | kid
 * to its own boot-status slot. The lower 4 bits double as a sanity check
 * that the writer's kid matches the slot index. */
#define MHT_ALIVE_MAGIC         0xCAFE0000u
#define MHT_ALIVE_VALUE(kid)    (MHT_ALIVE_MAGIC | ((kid) & 0xF))

/* MHT entry — see Multikernel-AMP §5.1.
 *   control_ring_paddr: physical address of frame 0 in this kid's pool
 *                       region. Frame 1 lives at + 0x1000.
 *   ep_pool_base:       physical address of frames 2..15.
 *   ep_pool_size:       byte size of EP data channel area (14 × 4 KiB).
 *   status:             VDTU_EP_* lifecycle for the kid as a whole. */
struct mht_entry {
    uint64_t control_ring_paddr;
    uint64_t ep_pool_base;
    uint32_t ep_pool_size;
    uint32_t status;
    uint16_t kid;
    uint16_t _pad[3];
};

/* Static MHT: indexed by kid. mht_init() fills entries [0..n_kids). */
struct mht {
    uint32_t           n_kids;
    uint16_t           self_kid;
    uint16_t           _pad;
    struct mht_entry   entries[MHT_MAX_KIDS];
};

/*
 * Initialize the MHT and announce this kernel's kid in the shared pool.
 * After this call, mht->entries[0..n_kids) is filled with each kid's
 * pool layout, and a magic word has been written to
 *     pool_base + MHT_BOOT_STATUS_OFFSET + self_kid*4.
 *
 * Pool layout assumed:
 *     pool_base + 0x0000:  control ring frame 0  (kid → kid+1 mod n)
 *     pool_base + 0x1000:  control ring frame 1  (kid+1 → kid mod n)
 *     pool_base + 0x2000:  EP data channels (frames 2..15, 56 KiB)
 *     pool_base + MHT_BOOT_STATUS_OFFSET: boot announcement region
 *
 * Returns 0 on success, -1 on bad arguments.
 */
int mht_init(struct mht *mht,
             void *pool_vaddr,
             uint64_t pool_paddr,
             uint16_t self_kid,
             uint32_t n_kids);

/*
 * Spin until every kid in [0, n_kids) has written its alive magic to the
 * announcement region. Returns 0 on success, -1 on bad arguments.
 *
 * spin_budget caps the number of iterations before giving up (returns -2);
 * pass 0 for "spin forever".
 */
int mht_wait_all_alive(struct mht *mht,
                       void *pool_vaddr,
                       uint64_t spin_budget);

/* Lookup helpers — pure functions over a populated MHT. */
static inline const struct mht_entry *
mht_lookup(const struct mht *mht, uint16_t kid)
{
    if (kid >= mht->n_kids) return NULL;
    return &mht->entries[kid];
}

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_MHT_H */
