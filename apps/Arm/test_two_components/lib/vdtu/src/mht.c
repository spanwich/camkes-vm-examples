/*
 * mht.c -- Membership Hash Table init + boot announcement
 */

#include <string.h>
#include "multikernel/mht.h"
#include "multikernel/vdtu_ring.h"

/* Pool layout offsets — must match Multikernel-AMP architecture §8.1. */
#define POOL_FRAME_SIZE         0x1000u
#define POOL_CONTROL_RING_0     0x0000u   /* kid → kid+1 ring */
#define POOL_CONTROL_RING_1     0x1000u   /* kid+1 → kid ring */
#define POOL_EP_BASE            0x2000u
#define POOL_EP_SIZE            (14u * POOL_FRAME_SIZE)

int mht_init(struct mht *mht,
             void *pool_vaddr,
             uint64_t pool_paddr,
             uint16_t self_kid,
             uint32_t n_kids)
{
    if (!mht || !pool_vaddr) return -1;
    if (n_kids == 0 || n_kids > MHT_MAX_KIDS) return -1;
    if (self_kid >= n_kids) return -1;

    memset(mht, 0, sizeof(*mht));
    mht->n_kids   = n_kids;
    mht->self_kid = self_kid;

    /* For the QEMU 2-kernel scope every kid sees the same pool, so every
     * MHT entry points at the same paddr. The architecture supports
     * per-kid pools (one pair of rings per kernel pair, §5.1) — extend
     * this initialization once N≥3 kernels arrive. */
    for (uint32_t i = 0; i < n_kids; ++i) {
        struct mht_entry *e = &mht->entries[i];
        e->control_ring_paddr = pool_paddr + POOL_CONTROL_RING_0;
        e->ep_pool_base       = pool_paddr + POOL_EP_BASE;
        e->ep_pool_size       = POOL_EP_SIZE;
        e->status             = VDTU_EP_ACTIVE;
        e->kid                = (uint16_t)i;
    }

    /* Announce this kid is alive — write magic to my slot. */
    volatile uint32_t *boot_status =
        (volatile uint32_t *)((uint8_t *)pool_vaddr + MHT_BOOT_STATUS_OFFSET);
    __atomic_store_n(&boot_status[self_kid],
                     MHT_ALIVE_VALUE(self_kid),
                     __ATOMIC_RELEASE);

    return 0;
}

int mht_wait_all_alive(struct mht *mht,
                       void *pool_vaddr,
                       uint64_t spin_budget)
{
    if (!mht || !pool_vaddr) return -1;

    volatile uint32_t *boot_status =
        (volatile uint32_t *)((uint8_t *)pool_vaddr + MHT_BOOT_STATUS_OFFSET);

    uint64_t iter = 0;
    for (;;) {
        bool all_up = true;
        for (uint32_t i = 0; i < mht->n_kids; ++i) {
            uint32_t v = __atomic_load_n(&boot_status[i], __ATOMIC_ACQUIRE);
            if (v != MHT_ALIVE_VALUE((uint16_t)i)) {
                all_up = false;
                break;
            }
        }
        if (all_up) return 0;
        if (spin_budget && ++iter >= spin_budget) return -2;
        /* QEMU TCG yields voluntarily on tight loops; on real HW
         * insert a YIELD/WFE here. */
        __asm__ volatile ("yield" ::: "memory");
    }
}
