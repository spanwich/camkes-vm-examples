/*
 * service.c — service directory + cross-kernel announcement.
 */

#include <string.h>
#include "multikernel/service.h"

static struct mk_service_entry g_services[MK_SERVICE_MAX];
static size_t                  g_count;

void mk_service_init(void)
{
    memset(g_services, 0, sizeof(g_services));
    g_count = 0;
}

static int find_slot(const char *name)
{
    for (int i = 0; i < MK_SERVICE_MAX; ++i) {
        if (g_services[i].in_use &&
            strncmp(g_services[i].name, name, MK_SERVICE_NAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

static int alloc_slot(void)
{
    for (int i = 0; i < MK_SERVICE_MAX; ++i) {
        if (!g_services[i].in_use) return i;
    }
    return -1;
}

/* Forward decl from kernelcall — sends a SERVICE_ANNOUNCE/WITHDRAW to
 * every peer kernel via the control ring. */
extern void mk_kc_broadcast_service_announce(const char *name,
                                             uint16_t kid, uint16_t vpe_id,
                                             uint16_t recv_ep_sel,
                                             uint16_t perms);
extern void mk_kc_broadcast_service_withdraw(const char *name);

int mk_service_register(const char *name, uint16_t kid, uint16_t vpe_id,
                        uint16_t recv_ep_sel, uint16_t perms)
{
    if (!name) return -1;
    if (find_slot(name) >= 0) return -1;
    int slot = alloc_slot();
    if (slot < 0) return -1;
    strncpy(g_services[slot].name, name, MK_SERVICE_NAME_MAX - 1);
    g_services[slot].name[MK_SERVICE_NAME_MAX - 1] = '\0';
    g_services[slot].kid         = kid;
    g_services[slot].vpe_id      = vpe_id;
    g_services[slot].recv_ep_sel = recv_ep_sel;
    g_services[slot].perms       = perms;
    g_services[slot].in_use      = true;
    g_count++;
    /* Concern 5: broadcast announcement so peer kernels populate their
     * remote registry. */
    mk_kc_broadcast_service_announce(name, kid, vpe_id, recv_ep_sel, perms);
    return 0;
}

int mk_service_withdraw(const char *name)
{
    int slot = find_slot(name);
    if (slot < 0) return -1;
    g_services[slot].in_use = false;
    if (g_count > 0) g_count--;
    mk_kc_broadcast_service_withdraw(name);
    return 0;
}

const struct mk_service_entry *mk_service_find(const char *name)
{
    int slot = find_slot(name);
    return slot < 0 ? NULL : &g_services[slot];
}

int mk_service_handle_announce(const char *name, uint16_t kid,
                               uint16_t vpe_id, uint16_t recv_ep_sel,
                               uint16_t perms)
{
    if (!name) return -1;
    /* Idempotent: re-announce of the same service is a no-op (keeps
     * the existing entry). */
    if (find_slot(name) >= 0) return 0;
    int slot = alloc_slot();
    if (slot < 0) return -1;
    strncpy(g_services[slot].name, name, MK_SERVICE_NAME_MAX - 1);
    g_services[slot].name[MK_SERVICE_NAME_MAX - 1] = '\0';
    g_services[slot].kid         = kid;
    g_services[slot].vpe_id      = vpe_id;
    g_services[slot].recv_ep_sel = recv_ep_sel;
    g_services[slot].perms       = perms;
    g_services[slot].in_use      = true;
    g_count++;
    return 0;
}

int mk_service_handle_withdraw(const char *name)
{
    int slot = find_slot(name);
    if (slot < 0) return 0;
    g_services[slot].in_use = false;
    if (g_count > 0) g_count--;
    return 0;
}

size_t mk_service_count(void) { return g_count; }
