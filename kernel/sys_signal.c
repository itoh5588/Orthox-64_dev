/* ---- シグナル系 syscall (3 アーキ共通) ------------------------------------
 *
 * **2026-09-13 にファイルごと 3 アーキ共通にした (別実装 29 組の 2 組)。**
 * rt_sigaction / rt_sigprocmask は kernel/linux_syscall.c にも
 * linux_bootstrap_sys_* として複製があり、判断 (番号 1〜31、SIG_IGN で保留を
 * 落とす、how の 3 種) は同じだった。違いは次の 2 つだけ:
 *
 *   - rt_sigaction の sigsetsize: x86 は「8 以上」、linux 側は「8 ちょうど」
 *     -> **Linux どおり 8 ちょうどに揃えた**
 *   - 構造体: x86 は mask を uint32_t[2]、linux 側は uint64_t 1 つで持つ。
 *     並び (handler, flags, restorer, mask) と大きさ (32 バイト) は同じで、
 *     3 アーキとも little-endian なのでメモリ上は同一
 *
 * include は ISA に依らないものだけで、aarch64 / riscv64 に同名の定義は
 * 無かった (sigaltstack / sigpending / sigaction は linux 側に口が無いので、
 * 共有にしても使われないだけ)。 */

#include <stddef.h>
#include <stdint.h>
#include "linux_abi.h"
#include "linux_errno.h"
#include "sys_internal.h"
#include "task.h"

int sys_sigaltstack(const struct linux_stack_t_k* ss, struct linux_stack_t_k* old_ss) {
    if (old_ss) {
        old_ss->ss_sp = 0;
        old_ss->ss_flags = 2;
        old_ss->ss_size = 0;
    }
    if (ss && ss->ss_flags != 0 && ss->ss_flags != 2) {
        return -LINUX_EINVAL;
    }
    return 0;
}

/* **返り値と how の定数は Linux ABI に揃えてある (2026-09-08)。**以前は
 * -1 (= EPERM) の直書きで、how も生の 0/1/2 だった。同じロジックが
 * linux_syscall.c 側にもあり、そちらは正しい errno を返していた。 */
int sys_sigprocmask(int how, const uint64_t* set, uint64_t* oldset) {
    struct task* current = get_current_task();
    uint64_t newmask;
    if (!current) return -LINUX_ESRCH;
    if (oldset) *oldset = current->sig_mask;
    if (!set) return 0;
    newmask = *set;
    switch (how) {
        case LINUX_SIG_BLOCK:
            current->sig_mask |= newmask;
            break;
        case LINUX_SIG_UNBLOCK:
            current->sig_mask &= ~newmask;
            break;
        case LINUX_SIG_SETMASK:
            current->sig_mask = newmask;
            break;
        default:
            return -LINUX_EINVAL;
    }
    return 0;
}

int sys_rt_sigprocmask(int how, const uint64_t* set, uint64_t* oldset, size_t sigsetsize) {
    /* **Linux は sigsetsize が sizeof(sigset_t) と厳密に一致しないと
     * EINVAL を返す。**以前は「8 以上なら通す」だったので、8 より大きい値を
     * 受け付けていた (linux_syscall.c 側は最初から != で弾いている) */
    if (sigsetsize != sizeof(uint64_t)) return -LINUX_EINVAL;
    return sys_sigprocmask(how, set, oldset);
}

int sys_sigpending(uint64_t* set) {
    struct task* current = get_current_task();
    if (!current) return -LINUX_ESRCH;
    if (!set) return -LINUX_EFAULT;
    *set = current->sig_pending & current->sig_mask;
    return 0;
}

int sys_sigaction(int sig, const struct orth_sigaction* act, struct orth_sigaction* oldact) {
    struct task* current = get_current_task();
    if (!current) return -LINUX_ESRCH;
    if (sig <= 0 || sig >= 32) return -LINUX_EINVAL;
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

    /* **sigsetsize は厳密に一致しないと EINVAL (2026-09-13)。**ここは
     * 「8 以上なら通す」のままだった。rt_sigprocmask は 2026-09-08 に直して
     * いたが、こちらだけ同じ穴が取り残されていた (linux 側は最初から != ) */
    if (sigsetsize != sizeof(uint64_t)) return -LINUX_EINVAL;
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
