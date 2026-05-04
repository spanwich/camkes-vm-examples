/*
 * ComponentC — cross-kernel user-space mailbox endpoint
 *
 * Each kernel's rootserver runs one instance of this component. Both
 * instances share the same 4 KiB physical page (0x6FFFE000) via a
 * paddr-anchored MMIO dataport configured in test_two_components.camkes.
 *
 * Demonstrates the cap-mediated alternative to the kernel-side shared
 * mailbox: instead of modifying kernel_putDebugChar to do cross-kernel
 * I/O, the seL4 capability system explicitly authorises a shared device
 * frame, and user-space code reads/writes it directly through a vspace
 * mapping derived from a cap in the rootserver's bootinfo.
 *
 * Protocol (lockless, observation-only):
 *   - On each iteration, atomically increment a shared counter.
 *   - Print "before" (read prior to increment) and "after" (= before + 1).
 *   - When BOTH ComponentCs are running, "before" values seen by each
 *     instance will skip — proving the OTHER instance is also writing.
 */

#include <stdio.h>
#include <stdint.h>
#include <camkes.h>

/* CAmkES auto-declares `extern Buf *cross_kernel_pool;` in the
 * generated header — no manual extern needed. We just cast it. */

int run(void)
{
    volatile uint32_t *counter = (volatile uint32_t *)(void *)cross_kernel_pool;

    printf("ComponentC: cross_kernel_pool mapped at vaddr=%p; initial value=%u\n",
           (void *)counter,
           __atomic_load_n(counter, __ATOMIC_ACQUIRE));
    fflush(stdout);

    uint32_t i = 0;
    while (1) {
        uint32_t before = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
        uint32_t mine = __atomic_fetch_add(counter, 1, __ATOMIC_ACQ_REL);
        uint32_t after = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
        printf("ComponentC[%u]: before=%u my_seq=%u after_self_inc=%u (gap=%u)\n",
               i++, before, mine, after, after - mine - 1);
        fflush(stdout);
        /* ~1 second on QEMU TCG */
        for (volatile int j = 0; j < 200000000; j++);
    }
    return 0;
}
