/*
 * vdtu_ep_state.h -- Verified ep_state transition function (vendored from SemperOS)
 *
 * Source: projects/semperos-sel4-ref/components/include/vdtu_ep_state.h
 * Branch: legacy/hille-baseline @ 97f38b9
 *
 * Extracted from EpState.Low.fst (F-star / Low-star verified implementation).
 *
 * Proven properties:
 *   - Terminated is absorbing (no escape from Terminated)
 *   - Transition to Terminated requires blocked = true (Raft cache gate)
 *   - No backward transitions (state monotonically increases)
 *   - ring_send returns -3 on Terminated state
 */

#ifndef MULTIKERNEL_VDTU_EP_STATE_H
#define MULTIKERNEL_VDTU_EP_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t vdtu_ep_state_t;

#define VDTU_EP_UNCONFIGURED 0
#define VDTU_EP_CONFIGURED   1
#define VDTU_EP_ACTIVE       2
#define VDTU_EP_TERMINATED   3

static inline bool
vdtu_ep_state_transition(vdtu_ep_state_t *s, vdtu_ep_state_t next, bool blocked)
{
    vdtu_ep_state_t current = *s;

    if (current == next)
        return true;

    bool valid =
        (current == VDTU_EP_UNCONFIGURED && next == VDTU_EP_CONFIGURED) ||
        (current == VDTU_EP_CONFIGURED   && next == VDTU_EP_ACTIVE)     ||
        (current == VDTU_EP_ACTIVE       && next == VDTU_EP_TERMINATED);

    if (!valid)
        return false;

    if (next == VDTU_EP_TERMINATED && !blocked)
        return false;

    *s = next;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_VDTU_EP_STATE_H */
