/*
 * vpe.h — VPE (Virtual Processing Element) as a first-class object
 * (v3.0 architecture §1, §3, §4; audit Item 3 + 11).
 *
 * Each PE slot in the kernel is represented by exactly one struct mk_vpe.
 * The architect's specification in the 2026-05-05 greenlight message:
 *
 *     struct mk_vpe {
 *         uint16_t                vpe_id;
 *         enum mk_vpe_type        type;     // VPE_THREAD | VPE_VCPU
 *         uint16_t                kid;       // owning kernel
 *         struct mk_vpe_cap_table cap_table; // per-VPE selector → cap_id
 *         void                   *exec;      // exec context (thread / vcpu)
 *     };
 *
 * Capability access from a VPE is via opaque integer selectors (like
 * file descriptors). The kernel translates (vpe_id, sel) → kernel-side
 * mk_cap_id_t when the VPE invokes a syscall. VPEs cannot address other
 * VPEs' selectors — the selector space is private per-VPE.
 *
 * In v3.0 §4 the PE slot itself is reusable: a VPE_THREAD-typed slot
 * can run different code at different times. SemperKernel holds the
 * underlying TCB/CNode/VSpace caps and load/clears them on workload
 * change. For the Phase A initial cut, all VPEs run cooperative threads
 * inside SemperKernel's address space (architect's option (b) from the
 * Step 1 feasibility report).
 */

#ifndef MULTIKERNEL_VPE_H
#define MULTIKERNEL_VPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "multikernel/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MK_VPE_MAX           32   /* per-kernel maximum                */
#define MK_VPE_MAX_SELECTORS 64   /* per-VPE selector table size       */

enum mk_vpe_type {
    MK_VPE_THREAD = 0,
    MK_VPE_VCPU   = 1,
};

enum mk_vpe_state {
    MK_VPE_STATE_IDLE     = 0,    /* slot exists, no workload          */
    MK_VPE_STATE_LOADED   = 1,    /* code mapped, TCB suspended        */
    MK_VPE_STATE_RUNNING  = 2,    /* TCB resumed                       */
    MK_VPE_STATE_CLEARING = 3,    /* mid-teardown                      */
};

/* A per-VPE selector table maps small integer sel ↦ kernel-side
 * mk_cap_id_t. MK_CAP_NONE in a slot means "selector not bound". */
struct mk_vpe_cap_table {
    mk_cap_id_t entries[MK_VPE_MAX_SELECTORS];
};

struct mk_thread;  /* forward */

struct mk_vpe {
    uint16_t                  vpe_id;
    uint16_t                  kid;
    enum mk_vpe_type          type;
    enum mk_vpe_state         state;
    struct mk_vpe_cap_table   cap_table;
    /* Execution context. For VPE_THREAD this is the cooperative thread
     * pointer that runs the VPE's code. For VPE_VCPU it would be the
     * AArch64 vcpu register file (deferred — not needed for Phase A). */
    struct mk_thread         *thread;
};

void mk_vpe_init(void);

/* Create a fresh VPE with given id + type, owned by `kid`. Returns the
 * VPE pointer on success or NULL on table overflow. */
struct mk_vpe *mk_vpe_create(uint16_t vpe_id, uint16_t kid,
                             enum mk_vpe_type type);

/* Find a VPE by id. Returns NULL if no such VPE on this kernel. */
struct mk_vpe *mk_vpe_find(uint16_t vpe_id);

/* Cascade-revoke all caps in the VPE's selector table, clear the
 * thread, mark IDLE. Does NOT free the mk_vpe slot — slots are
 * reusable per v3.0 §4. */
void mk_vpe_clear(struct mk_vpe *vpe);

/* Destroy a VPE entirely (slot returned to the pool). */
void mk_vpe_destroy(struct mk_vpe *vpe);

/* Selector ⇄ cap_id translation. */
int         mk_vpe_bind_sel(struct mk_vpe *vpe, uint16_t sel, mk_cap_id_t cap);
mk_cap_id_t mk_vpe_lookup_sel(struct mk_vpe *vpe, uint16_t sel);
int         mk_vpe_clear_sel(struct mk_vpe *vpe, uint16_t sel);
/* Allocate a fresh free selector. Returns sel (>= 0) or -1 if full. */
int         mk_vpe_alloc_sel(struct mk_vpe *vpe);

/* Diagnostics. */
size_t mk_vpe_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_VPE_H */
