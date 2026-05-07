/*
 * service.h — SERVICE cap type + service directory (v3.0 §2.1, §6.2;
 * Concern 5 cross-kernel announcement).
 *
 * Each kernel maintains a local registry of services (name → recv_ep
 * selector + owning VPE). On register, the kernel broadcasts a
 * SERVICE_ANNOUNCE message via the control ring so every peer kernel
 * adds the service to its remote registry. mk_syscall_obtain walks the
 * local registry first, then the remote registry — both indexed by
 * service name.
 *
 * For Phase B the registry is fixed-size and uses a flat name->entry
 * scan. With ≤ MK_SERVICE_MAX entries (32) this is fine for the test
 * scope; a hash table is a Phase E optimization.
 */

#ifndef MULTIKERNEL_SERVICE_H
#define MULTIKERNEL_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "multikernel/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MK_SERVICE_MAX        32
#define MK_SERVICE_NAME_MAX   32

struct mk_service_entry {
    char        name[MK_SERVICE_NAME_MAX];
    uint16_t    kid;            /* owning kernel                      */
    uint16_t    vpe_id;         /* owning VPE on that kernel           */
    uint16_t    recv_ep_sel;    /* VPE-local sel of the RECV_EP       */
    uint16_t    perms;          /* default perms for OBTAIN sessions  */
    bool        in_use;
};

void mk_service_init(void);

/* Register a service NAME bound to (kid, vpe_id, recv_ep_sel) with
 * default perms. If kid == self_kid, also broadcasts SERVICE_ANNOUNCE
 * to all peer kernels (Concern 5). Returns 0 on success, -1 on full
 * table or duplicate name. */
int mk_service_register(const char *name, uint16_t kid, uint16_t vpe_id,
                        uint16_t recv_ep_sel, uint16_t perms);

/* Withdraw a service (remove from local registry; broadcast WITHDRAW). */
int mk_service_withdraw(const char *name);

/* Find a service by name — local registry first, then any remote
 * registry entry installed via SERVICE_ANNOUNCE. Returns the entry
 * pointer or NULL. */
const struct mk_service_entry *mk_service_find(const char *name);

/* Called by the KernelcallHandler when a SERVICE_ANNOUNCE arrives. */
int mk_service_handle_announce(const char *name, uint16_t kid,
                               uint16_t vpe_id, uint16_t recv_ep_sel,
                               uint16_t perms);

/* Called by the handler on SERVICE_WITHDRAW. */
int mk_service_handle_withdraw(const char *name);

/* Diagnostics. */
size_t mk_service_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_SERVICE_H */
