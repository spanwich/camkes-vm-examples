/*
 * vdtu_ring.c -- SPSC ring buffer implementation (vendored from SemperOS)
 *
 * Source: projects/semperos-sel4-ref/src/vdtu_ring.c
 * Branch: legacy/hille-baseline @ 97f38b9
 *
 * Multikernel adaptation: ARMv8 inner-shareable + acquire/release atomics
 * on head/tail (vs original x86 + compiler barrier). The data plane is
 * otherwise unchanged.
 */

#include "multikernel/vdtu_ring.h"

static int is_power_of_2(uint32_t n) {
    return n >= 2 && (n & (n - 1)) == 0;
}

int vdtu_ring_init(struct vdtu_ring *ring, void *mem,
                   uint32_t slot_count, uint32_t slot_size)
{
    if (!ring || !mem)
        return -1;
    if (!is_power_of_2(slot_count))
        return -1;
    if (slot_size < VDTU_HEADER_SIZE || !is_power_of_2(slot_size))
        return -1;

    struct vdtu_ring_ctrl *ctrl = (struct vdtu_ring_ctrl *)mem;
    memset(ctrl, 0, sizeof(*ctrl));

    ctrl->slot_count = slot_count;
    ctrl->slot_size  = slot_size;
    ctrl->slot_mask  = slot_count - 1;
    __atomic_store_n(&ctrl->tail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctrl->head, 0, __ATOMIC_RELEASE);

    /* Phase C.3: drive the verified state machine
     *   UNCONFIGURED → CONFIGURED → ACTIVE.
     * The verified vdtu_ep_state_transition rejects skip-ahead. memset
     * left ep_state at VDTU_EP_UNCONFIGURED (=0). */
    vdtu_ep_state_t s = VDTU_EP_UNCONFIGURED;
    if (!vdtu_ep_state_transition(&s, VDTU_EP_CONFIGURED, false)) return -5;
    if (!vdtu_ep_state_transition(&s, VDTU_EP_ACTIVE, false))     return -5;
    __atomic_store_n(&ctrl->ep_state, s, __ATOMIC_RELEASE);

    ring->ctrl = ctrl;
    ring->slots = (uint8_t *)mem + VDTU_RING_CTRL_SIZE;

    return 0;
}

int vdtu_ring_detach(struct vdtu_ring *ring)
{
    if (!ring || !ring->ctrl) return -1;
    /* Read-modify-write under sequential consistency to make the verified
     * transition observation stable in the face of a concurrent peer. */
    vdtu_ep_state_t cur = __atomic_load_n(&ring->ctrl->ep_state, __ATOMIC_ACQUIRE);
    if (!vdtu_ep_state_transition(&cur, VDTU_EP_TERMINATED, /*blocked=*/true))
        return -5;
    __atomic_store_n(&ring->ctrl->ep_state, cur, __ATOMIC_RELEASE);
    return 0;
}

int vdtu_ring_attach(struct vdtu_ring *ring, void *mem)
{
    if (!ring || !mem)
        return -1;

    ring->ctrl = (struct vdtu_ring_ctrl *)mem;
    ring->slots = (uint8_t *)mem + VDTU_RING_CTRL_SIZE;

    return 0;
}

int vdtu_ring_send(struct vdtu_ring *ring,
                   uint16_t sender_pe, uint8_t sender_ep,
                   uint16_t sender_vpe, uint8_t reply_ep,
                   uint64_t label, uint64_t replylabel, uint8_t flags,
                   const void *payload, uint16_t payload_len)
{
    if (!ring || !ring->ctrl)
        return -1;

    /* Phase C.3: send requires the EP to be in ACTIVE. UNCONFIGURED /
     * CONFIGURED / TERMINATED all reject. */
    if (__atomic_load_n(&ring->ctrl->ep_state, __ATOMIC_ACQUIRE) != VDTU_EP_ACTIVE)
        return -3;

    if ((size_t)VDTU_HEADER_SIZE + payload_len > ring->ctrl->slot_size)
        return -2;

    uint32_t head = __atomic_load_n(&ring->ctrl->head, __ATOMIC_RELAXED);
    uint32_t next_head = (head + 1) & ring->ctrl->slot_mask;
    uint32_t tail = __atomic_load_n(&ring->ctrl->tail, __ATOMIC_ACQUIRE);
    if (next_head == tail)
        return -1;  /* full */

    if (ring->ctrl->slot_size == 0 || ring->ctrl->slot_size > 4096) {
        return -4;  /* corrupt ring state */
    }

    uint8_t *slot = ring->slots + (size_t)head * ring->ctrl->slot_size;

    memset(slot, 0, ring->ctrl->slot_size);

    struct vdtu_msg_header *hdr = (struct vdtu_msg_header *)slot;
    hdr->flags          = flags;
    hdr->sender_core_id = sender_pe;
    hdr->sender_ep_id   = sender_ep;
    hdr->reply_ep_id    = reply_ep;
    hdr->length         = payload_len;
    hdr->sender_vpe_id  = sender_vpe;
    hdr->label          = label;
    hdr->replylabel     = replylabel;

    if (payload && payload_len > 0) {
        memcpy(slot + VDTU_HEADER_SIZE, payload, payload_len);
    }

    /* Release: payload writes complete before head advance is visible */
    __atomic_store_n(&ring->ctrl->head, next_head, __ATOMIC_RELEASE);

    return 0;
}

const struct vdtu_message *vdtu_ring_fetch(const struct vdtu_ring *ring)
{
    if (!ring || !ring->ctrl)
        return NULL;

    /* Phase C.3: fetch requires ACTIVE — TERMINATED returns NULL so the
     * polling consumer cleanly exits its loop on detach. */
    if (__atomic_load_n(&ring->ctrl->ep_state, __ATOMIC_ACQUIRE) != VDTU_EP_ACTIVE)
        return NULL;

    uint32_t tail = __atomic_load_n(&ring->ctrl->tail, __ATOMIC_RELAXED);
    /* Acquire on head pairs with release in vdtu_ring_send */
    uint32_t head = __atomic_load_n(&ring->ctrl->head, __ATOMIC_ACQUIRE);

    if (tail == head)
        return NULL;

    const uint8_t *slot = ring->slots + (size_t)tail * ring->ctrl->slot_size;
    return (const struct vdtu_message *)slot;
}

void vdtu_ring_ack(struct vdtu_ring *ring)
{
    if (!ring || !ring->ctrl)
        return;

    uint32_t tail = __atomic_load_n(&ring->ctrl->tail, __ATOMIC_RELAXED);
    /* Release: payload reads complete before tail advance is visible */
    __atomic_store_n(&ring->ctrl->tail,
                     (tail + 1) & ring->ctrl->slot_mask,
                     __ATOMIC_RELEASE);
}
