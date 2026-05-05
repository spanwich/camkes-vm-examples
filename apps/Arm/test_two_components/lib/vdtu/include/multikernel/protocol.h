/*
 * protocol.h -- Cross-kernel SemperKernel control-plane message protocol
 *
 * Multikernel-AMP architecture §5.1 / §6 / §7. Fixed-size messages travel
 * over per-kernel-pair vdtu_ring instances on frame 0 / 1 of the shared
 * pool. Every header carries {msg_type, req_id, src_kid, dst_kid}.
 *
 * Message size budget: 64 bytes (vdtu_ring slot fits 25-byte vdtu header
 * + payload; we use a 32-byte payload that trivially fits the 512-byte
 * default slot — well within the 64-byte budget the architecture asks
 * for).
 */

#ifndef MULTIKERNEL_PROTOCOL_H
#define MULTIKERNEL_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "multikernel/vdtu_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SemperOS mht_key_t cap-ID encoding (Confluence design Q5).
 * Layout: [63:48] PE/kid | [47:32] VPE | [31:16] type | [15:0] sel */
typedef uint64_t mk_cap_id_t;

#define MK_CAP_ID(kid, vpe, type, sel) \
    (((uint64_t)(kid)  & 0xFFFFu) << 48 | \
     ((uint64_t)(vpe)  & 0xFFFFu) << 32 | \
     ((uint64_t)(type) & 0xFFFFu) << 16 | \
     ((uint64_t)(sel)  & 0xFFFFu))

#define MK_CAP_KID(id)  ((uint16_t)(((id) >> 48) & 0xFFFFu))
#define MK_CAP_VPE(id)  ((uint16_t)(((id) >> 32) & 0xFFFFu))
#define MK_CAP_TYPE(id) ((uint16_t)(((id) >> 16) & 0xFFFFu))
#define MK_CAP_SEL(id)  ((uint16_t)( (id)        & 0xFFFFu))

/* Capability types. */
enum mk_cap_type {
    MK_CAP_NONE  = 0x0000,
    MK_CAP_MEM   = 0x0008,   /* shared frame                 */
    MK_CAP_MSG   = 0x0004,   /* DTU message endpoint          */
    MK_CAP_NULL_TYPE = 0xFFFF,
};

/* Control-plane message types. */
enum mk_msg_type {
    MK_MSG_NONE          = 0x00,
    MK_MSG_PING          = 0x01,   /* round-trip latency probe (Test 7) */
    MK_MSG_PONG          = 0x02,
    MK_MSG_EXCHANGE_OFFER= 0x10,
    MK_MSG_EXCHANGE_ACK  = 0x11,
    MK_MSG_REVOKE        = 0x20,
    MK_MSG_REVOKE_BATCH  = 0x21,
    MK_MSG_REVOKE_FINISH = 0x22,
};

/* Wire-format header for every cross-kernel control message. Total 16 B
 * — comfortably inside the 64-byte budget. */
struct __attribute__((packed)) mk_msg_hdr {
    uint8_t  msg_type;       /* enum mk_msg_type                     */
    uint8_t  src_kid;
    uint8_t  dst_kid;
    uint8_t  _r0;
    uint32_t req_id;         /* matches OFFER↔ACK, REVOKE↔FINISH    */
    uint64_t _r1;
};

/* EXCHANGE_OFFER body — donor → acquirer. */
struct __attribute__((packed)) mk_msg_exchange_offer {
    struct mk_msg_hdr hdr;
    mk_cap_id_t       src_cap_id;     /* donor's cap (parent on grant) */
    uint16_t          type;           /* MK_CAP_MEM, etc.              */
    uint16_t          dst_vpe;
    uint16_t          frame_idx;      /* index into EP pool (2..15)    */
    uint16_t          perms;          /* 1=R, 2=W, 3=RW                */
    uint64_t          label;
};

/* EXCHANGE_ACK body — acquirer → donor. */
struct __attribute__((packed)) mk_msg_exchange_ack {
    struct mk_msg_hdr hdr;
    mk_cap_id_t       dst_cap_id;     /* acquirer's locally-allocated cap */
    int32_t           status;         /* 0 = OK, negative = error     */
    uint32_t          _pad;
};

/* REVOKE body — revoker → target. Asks target to revoke cap_id and
 * everything beneath it that lives on the target. */
struct __attribute__((packed)) mk_msg_revoke {
    struct mk_msg_hdr hdr;
    mk_cap_id_t       cap_id;
    uint8_t           _pad[8];
};

/* REVOKE_BATCH body — revoker sends up to 4 cap IDs at once. */
struct __attribute__((packed)) mk_msg_revoke_batch {
    struct mk_msg_hdr hdr;
    uint32_t          count;          /* number of valid cap IDs (≤4)  */
    uint32_t          _pad;
    mk_cap_id_t       cap_ids[4];
};

/* REVOKE_FINISH body — target → revoker. Sent when the target has
 * finished revoking everything it owes for this req_id. */
struct __attribute__((packed)) mk_msg_revoke_finish {
    struct mk_msg_hdr hdr;
    int32_t           status;         /* 0 = OK                       */
    uint32_t          revoked_count;
    uint64_t          _pad;
};

/* Helper: fixed slot size for the control ring on frame 0/1 of the pool.
 * 4 KiB per ring frame minus 64 B ctrl = 4032 B, divided by 64 B/slot
 * gives 63 slots; round down to a power of two = 32. Each slot must
 * hold the 25-byte vdtu header + our control message body (≤56 B). 64
 * is comfortably enough. */
#define MK_CTRL_SLOT_SIZE   128u
#define MK_CTRL_SLOT_COUNT  16u

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_PROTOCOL_H */
