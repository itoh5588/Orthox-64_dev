#include <stddef.h>
#include <stdint.h>
#include "sys_internal.h"
#include "task.h"

int sys_sigaltstack(const struct linux_stack_t_k* ss, struct linux_stack_t_k* old_ss) {
    if (old_ss) {
        old_ss->ss_sp = 0;
        old_ss->ss_flags = 2;
        old_ss->ss_size = 0;
    }
    if (ss && ss->ss_flags != 0 && ss->ss_flags != 2) {
        return -1;
    }
    return 0;
}

int sys_sigprocmask(int how, const uint64_t* set, uint64_t* oldset) {
    struct task* current = get_current_task();
    uint64_t newmask;
    if (!current) return -1;
    if (oldset) *oldset = current->sig_mask;
    if (!set) return 0;
    newmask = *set;
    switch (how) {
        case 0: // SIG_BLOCK on Linux/musl.
            current->sig_mask |= newmask;
            break;
        case 1: // SIG_UNBLOCK on Linux/musl.
            current->sig_mask &= ~newmask;
            break;
        case 2: // SIG_SETMASK on Linux/musl.
            current->sig_mask = newmask;
            break;
        default:
            return -1;
    }
    return 0;
}

int sys_rt_sigprocmask(int how, const uint64_t* set, uint64_t* oldset, size_t sigsetsize) {
    if (sigsetsize < sizeof(uint64_t)) return -1;
    return sys_sigprocmask(how, set, oldset);
}

int sys_sigpending(uint64_t* set) {
    struct task* current = get_current_task();
    if (!current || !set) return -1;
    *set = current->sig_pending & current->sig_mask;
    return 0;
}

int sys_sigaction(int sig, const struct orth_sigaction* act, struct orth_sigaction* oldact) {
    struct task* current = get_current_task();
    if (!current || sig <= 0 || sig >= 32) return -1;
    if (oldact) {
        oldact->sa_handler = current->sig_handlers[sig];
        oldact->sa_mask = current->sig_action_masks[sig];
        oldact->sa_flags = current->sig_action_flags[sig];
        oldact->reserved = 0;
    }
    if (act) {
        current->sig_handlers[sig] = act->sa_handler;
        current->sig_action_masks[sig] = act->sa_mask;
        current->sig_action_flags[sig] = act->sa_flags;
        if (act->sa_handler == 1ULL) {
            current->sig_pending &= ~(1ULL << sig);
        }
    }
    return 0;
}

int sys_rt_sigaction(int sig, const struct linux_rt_sigaction_k* act,
                     struct linux_rt_sigaction_k* oldact, size_t sigsetsize) {
    struct orth_sigaction in_act;
    struct orth_sigaction out_act;
    int ret;

    if (sigsetsize < sizeof(uint64_t)) return -1;
    if (act) {
        in_act.sa_handler = act->handler;
        in_act.sa_mask = ((uint64_t)act->mask[1] << 32) | act->mask[0];
        in_act.sa_flags = (uint32_t)act->flags;
        in_act.reserved = 0;
    }
    ret = sys_sigaction(sig, act ? &in_act : 0, oldact ? &out_act : 0);
    if (ret < 0) return ret;
    if (oldact) {
        oldact->handler = out_act.sa_handler;
        oldact->flags = out_act.sa_flags;
        oldact->restorer = 0;
        oldact->mask[0] = (uint32_t)(out_act.sa_mask & 0xffffffffU);
        oldact->mask[1] = (uint32_t)(out_act.sa_mask >> 32);
    }
    return 0;
}
