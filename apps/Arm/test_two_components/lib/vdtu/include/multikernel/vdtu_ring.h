/*
 * vdtu_ring.h -- SPSC ring buffer matching the M3/SemperOS DTU message format
 *
 * Vendored from SemperOS for multikernel-AMP Layer 2.
 * Source: projects/semperos-sel4-ref/components/include/vdtu_ring.h
 * Branch: legacy/hille-baseline @ 97f38b9
 *
 * The ring buffer implements a circular message buffer matching the gem5
 * DTU's receive-buffer semantics: power-of-2 number of slots, fixed slot
 * size, producer fills DTU header + payload and advances head; consumer
 * fetches a slot pointer and acks (advances tail) when done.
 *
 * Multikernel adaptation:
 *   - Memory model: ARMv8 inner-shareable + acquire/release atomics
 *     (vs original x86 + compiler barrier)
 *   - Layout in a 4 KiB shared dataport: 64 B ctrl + N x slot
 *   - sender_core_id semantics: kernel ID (kid), 0..N-1
 */

#ifndef MULTIKERNEL_VDTU_RING_H
#define MULTIKERNEL_VDTU_RING_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * --------------------------------------------------------------------------
 *  DTU Message Header (gem5-compatible, 25 bytes packed)
 *  Layout matches gem5 src/include/base/arch/gem5/DTU.h:150-161 verbatim.
 * --------------------------------------------------------------------------
 */

#define VDTU_HEADER_SIZE    25

struct __attribute__((packed)) vdtu_msg_header {
    uint8_t  flags;            /* low nibble: REPLY/GRANT_CREDITS; high: dest_kid */
    uint16_t sender_core_id;   /* kernel ID of sender                            */
    uint8_t  sender_ep_id;     /* endpoint ID of sender's send EP                */
    uint8_t  reply_ep_id;      /* for msg: reply EP; for reply: credit EP        */
    uint16_t length;           /* payload length in bytes                         */
    uint16_t sender_vpe_id;    /* VPE ID of sender                               */
    uint64_t label;            /* routing label                                   */
    uint64_t replylabel;       /* reply label                                     */
};

struct __attribute__((packed)) vdtu_message {
    struct vdtu_msg_header hdr;
    unsigned char data[];
};

/*
 * --------------------------------------------------------------------------
 *  DTU Constants
 * --------------------------------------------------------------------------
 */

#define VDTU_EP_COUNT           32
#define VDTU_MAX_MSG_SLOTS      32
#define VDTU_DTU_PKG_SIZE       8

#define VDTU_SYSC_MSG_SIZE      512
#define VDTU_KRNLC_MSG_SIZE     2048
#define VDTU_SRV_MSG_SIZE       256

#define VDTU_DEFAULT_SLOT_COUNT 4
#define VDTU_DEFAULT_SLOT_SIZE  VDTU_SYSC_MSG_SIZE

/* Header flag bits — low nibble: real flags; high nibble: destination kid */
#define VDTU_FLAG_REPLY         (1 << 0)
#define VDTU_FLAG_GRANT_CREDITS (1 << 1)
#define VDTU_FLAG_MASK          0x0F
#define VDTU_FLAG_DEST_SHIFT    4
#define VDTU_FLAG_DEST_MASK     0xF0
#define VDTU_FLAGS_WITH_DEST(flags, dest_kid) \
    (((uint8_t)((flags) & VDTU_FLAG_MASK)) | \
     (((uint8_t)((dest_kid) & 0x0F)) << VDTU_FLAG_DEST_SHIFT))
#define VDTU_FLAGS_GET_DEST(flags) (((uint8_t)(flags) >> VDTU_FLAG_DEST_SHIFT) & 0x0F)

#define VDTU_CREDITS_UNLIM      0xFFFF

#include "vdtu_ep_state.h"

/*
 * --------------------------------------------------------------------------
 *  Ring Buffer Control Structure (64 B, cache-line aligned)
 * --------------------------------------------------------------------------
 */

#define VDTU_RING_CTRL_SIZE     64

struct vdtu_ring_ctrl {
    /* Producer-written */
    volatile uint32_t head;

    /* Consumer-written */
    volatile uint32_t tail;

    /* Immutable after init */
    uint32_t slot_count;
    uint32_t slot_size;
    uint32_t slot_mask;

    /* EP lifecycle (control plane writes; data plane reads) */
    volatile uint32_t ep_state;

    uint8_t  _pad[VDTU_RING_CTRL_SIZE - 6 * sizeof(uint32_t)];
};

#ifdef __cplusplus
static_assert(sizeof(struct vdtu_ring_ctrl) == VDTU_RING_CTRL_SIZE,
              "ring ctrl must be 64 bytes");
#else
_Static_assert(sizeof(struct vdtu_ring_ctrl) == VDTU_RING_CTRL_SIZE,
               "ring ctrl must be 64 bytes");
#endif

/*
 * --------------------------------------------------------------------------
 *  Ring Buffer Handle
 * --------------------------------------------------------------------------
 */

struct vdtu_ring {
    struct vdtu_ring_ctrl *ctrl;
    uint8_t *slots;
};

/*
 * --------------------------------------------------------------------------
 *  API
 * --------------------------------------------------------------------------
 */

int vdtu_ring_init(struct vdtu_ring *ring, void *mem,
                   uint32_t slot_count, uint32_t slot_size);

int vdtu_ring_attach(struct vdtu_ring *ring, void *mem);

/* Phase C.3: explicit lifecycle transitions via vdtu_ep_state_transition
 * (the F*-verified function). Any out-of-order transition is rejected.
 *
 * vdtu_ring_init drives UNCONFIGURED → CONFIGURED → ACTIVE internally,
 * so existing call sites work unchanged. vdtu_ring_detach drives
 * ACTIVE → TERMINATED (requires blocked=true). After TERMINATED, send
 * and fetch must fail. */
int vdtu_ring_detach(struct vdtu_ring *ring);

/* Direct access to the EP state for tests + state-aware code paths. */
static inline vdtu_ep_state_t vdtu_ring_get_state(const struct vdtu_ring *ring)
{
    if (!ring || !ring->ctrl) return VDTU_EP_UNCONFIGURED;
    return __atomic_load_n(&ring->ctrl->ep_state, __ATOMIC_ACQUIRE);
}

static inline size_t vdtu_ring_total_size(uint32_t slot_count, uint32_t slot_size) {
    return VDTU_RING_CTRL_SIZE + (size_t)slot_count * slot_size;
}

static inline int vdtu_ring_is_full(const struct vdtu_ring *ring) {
    uint32_t head = __atomic_load_n(&ring->ctrl->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&ring->ctrl->tail, __ATOMIC_ACQUIRE);
    uint32_t next_head = (head + 1) & ring->ctrl->slot_mask;
    return next_head == tail;
}

static inline int vdtu_ring_is_empty(const struct vdtu_ring *ring) {
    uint32_t head = __atomic_load_n(&ring->ctrl->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&ring->ctrl->tail, __ATOMIC_RELAXED);
    return head == tail;
}

static inline uint32_t vdtu_ring_available(const struct vdtu_ring *ring) {
    uint32_t head = __atomic_load_n(&ring->ctrl->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&ring->ctrl->tail, __ATOMIC_RELAXED);
    return (head - tail) & ring->ctrl->slot_mask;
}

int vdtu_ring_send(struct vdtu_ring *ring,
                   uint16_t sender_pe, uint8_t sender_ep,
                   uint16_t sender_vpe, uint8_t reply_ep,
                   uint64_t label, uint64_t replylabel, uint8_t flags,
                   const void *payload, uint16_t payload_len);

const struct vdtu_message *vdtu_ring_fetch(const struct vdtu_ring *ring);

void vdtu_ring_ack(struct vdtu_ring *ring);

static inline size_t vdtu_ring_msg_offset(const struct vdtu_ring *ring,
                                          const struct vdtu_message *msg) {
    return (const uint8_t *)msg - ring->slots;
}

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_VDTU_RING_H */
