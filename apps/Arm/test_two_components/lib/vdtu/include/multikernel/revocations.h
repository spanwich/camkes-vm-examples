/*
 * revocations.h -- RevocationList: in-progress revocation tracker with
 * subscriber pattern. Adapted from SemperOS Revocations.{h,cc}.
 *
 * Two callers attempting to revoke the same cap_id (or overlapping
 * subtrees) must converge to one walker — the second caller subscribes
 * and waits for the first walker's REVOKE_FINISH. Without this, two
 * concurrent revokers fan out duplicate REVOKE_BATCH messages and
 * potentially double-free local entries.
 *
 * On QEMU TCG (single-threaded) the race window is essentially zero,
 * but on real ARM with two cores running concurrently the duplicate-
 * walk bug is real. RevocationList eliminates it.
 */

#ifndef MULTIKERNEL_REVOCATIONS_H
#define MULTIKERNEL_REVOCATIONS_H

#include <stdint.h>
#include <stdbool.h>
#include "multikernel/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MK_REV_HASH_CAP    64    /* power of 2 */
#define MK_REV_MAX_SUBS    8     /* subscribers per in-progress revocation */

struct mk_revocation {
    mk_cap_id_t  cap_id;          /* MK_CAP_NONE = empty bucket */
    mk_cap_id_t  parent;
    mk_cap_id_t  origin;          /* cap which initiated the chain */
    int32_t      result;          /* set by walker on completion */
    bool         done;
    bool         in_progress;
    /* Subscribers — opaque keys for mk_thread_notify. The walker of this
     * revocation calls mk_thread_notify(subscribers[i]) for each. The
     * waiting thread loops mk_thread_wait_for(its_key) until done=true. */
    void        *subscribers[MK_REV_MAX_SUBS];
    int          n_subscribers;
};

void mk_rev_init(void);

/* Insert or attach.
 *   - If no entry exists for cap_id: install a new entry with origin =
 *     cap_id and in_progress = true. Caller becomes the walker. Returns
 *     the new entry; *out_is_walker = true.
 *   - If an entry already exists: do not modify it. Returns the existing
 *     entry; *out_is_walker = false. Caller should subscribe + wait.
 *
 * Returns NULL on table overflow. */
struct mk_revocation *mk_rev_attach(mk_cap_id_t cap_id, mk_cap_id_t parent,
                                    bool *out_is_walker);

/* Find an in-progress revocation. NULL if none. */
struct mk_revocation *mk_rev_find(mk_cap_id_t cap_id);

/* Append `key` to subscribers. The walker will mk_thread_notify(key) when
 * the revocation completes. Returns 0 on success, -1 if list full. */
int mk_rev_subscribe(struct mk_revocation *rev, void *key);

/* Mark the revocation done with the given result, notify all subscribers
 * (via mk_thread_notify), and remove the entry from the table. */
void mk_rev_complete(struct mk_revocation *rev, int32_t result);

/* Diagnostic. */
size_t mk_rev_size(void);

#ifdef __cplusplus
}
#endif

#endif /* MULTIKERNEL_REVOCATIONS_H */
