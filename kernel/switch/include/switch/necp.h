// SPDX-License-Identifier: GPL-2.0-only
/*
 * A local-network drop NECP reports for a process (diag/necp.c, debug kernels): one "SwitchNECP:" line with the
 * process, the process NECP judged its platform conditions on, and the rule that matched. 24B119's Local Network rules
 * exempt a process only through a skip on delegate-is-platform-binary ahead of the drops, so the line carries what NECP
 * judged.
 */
#ifndef SWITCH_NECP_H
#define SWITCH_NECP_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

struct proc;

struct switch_necp_drop {
    uint32_t site;
    int32_t pid;
    uint32_t uid;
    struct proc *process;
    struct proc *caller;
    struct proc *responsible;
    int32_t last_pid;
    int32_t responsible_pid;
    bool delegated;
    bool used_responsible;
    bool necp_platform;
    bool necp_real_platform;
    bool skip_installed;
    uint32_t policy_id;
    uint32_t session_order;
    uint32_t order;
    uint64_t conditions;
    uint64_t negated;
    uint32_t drop_flags;
    uint32_t skip_id;
};

#define SWITCH_NECP_PROBE_PIDS 256u

/* True the first time `pid` is seen while one of `slots` (zero when free) is still free. */
static inline bool switch_necp_probe_claim(int32_t *slots, unsigned count, int32_t pid)
{
    if (pid <= 0) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        int32_t seen = __atomic_load_n(&slots[i], __ATOMIC_RELAXED);
        if (seen == 0 &&
            __atomic_compare_exchange_n(&slots[i], &seen, pid, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            return true;
        }
        if (seen == pid) {
            return false;
        }
    }
    return false;
}

#if DEVELOPMENT || DEBUG
/* Runs under NECP's policy lock and the caller's socket or client locks, where NECP itself logs and reads the same
 * process fields. */
void switch_necp_local_drop(const struct switch_necp_drop *drop);

#define SWITCH_NECP_DROP_FIELDS(info, policy, skip, mask)                                                           \
    .pid = (info).pid, .delegated = (info).is_delegated,                                                            \
    .used_responsible = (info).used_responsible_pid, .necp_platform = (info).is_platform_binary,                    \
    .necp_real_platform = (info).real_is_platform_binary,                                                           \
    .skip_installed = ((mask) & NECP_KERNEL_CONDITION_DELEGATE_IS_PLATFORM_BINARY) != 0, .policy_id = (policy)->id, \
    .session_order = (policy)->session_order, .order = (policy)->order, .conditions = (policy)->condition_mask,     \
    .negated = (policy)->condition_negated_mask, .drop_flags = (policy)->result_parameter.drop_flags,               \
    .skip_id = (skip)

#define SWITCH_NECP_CLIENT_LOCAL_DROP(info, policy, skip, caller_proc, effective, responsible_proc, mask) \
    switch_necp_local_drop(&(const struct switch_necp_drop){.site = 1,                                    \
                                                            .uid = (info).uid,                            \
                                                            .process = (effective),                       \
                                                            .caller = (caller_proc),                      \
                                                            .responsible = (responsible_proc),            \
                                                            SWITCH_NECP_DROP_FIELDS(info, policy, skip, mask)})

/* info.uid is filled only when some policy has a uid condition; the socket's credential is what NECP reads then. */
#define SWITCH_NECP_SOCKET_LOCAL_DROP(site_number, info, policy, skip, so, effective, mask)                 \
    switch_necp_local_drop(&(const struct switch_necp_drop){.site = (site_number),                          \
                                                            .uid = kauth_cred_getuid((so)->so_cred),        \
                                                            .process = (effective),                         \
                                                            .last_pid = (so)->last_pid,                     \
                                                            .responsible_pid = (so)->so_rpid,               \
                                                            SWITCH_NECP_DROP_FIELDS(info, policy, skip, mask)})
#else
#define SWITCH_NECP_CLIENT_LOCAL_DROP(info, policy, skip, caller_proc, effective, responsible_proc, mask) \
    do {                                                                                                  \
    } while (0)
#define SWITCH_NECP_SOCKET_LOCAL_DROP(site_number, info, policy, skip, so, effective, mask) \
    do {                                                                                    \
    } while (0)
#endif

__END_DECLS

#endif /* SWITCH_NECP_H */
