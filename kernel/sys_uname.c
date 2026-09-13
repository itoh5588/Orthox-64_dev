/* ---- uname (3 アーキ共通) --------------------------------------------------
 *
 * **別実装 29 組のうちの 1 組を畳んだもの (2026-09-13)。**もとは
 *
 *     x86              kernel/x86_64/sys_time.c の sys_uname
 *     aarch64/riscv64  kernel/linux_syscall.c   の linux_sys_uname
 *
 * の 2 つ。**名乗る値そのものは 2026-09-09 に揃えてある** (それまで linux 側
 * だけが sysname "Linux" / release "5.0.0-orthox" を返しており、同じ OS が
 * アーキによって違う名前を名乗っていた。include/version.h を参照)。
 *
 * 畳んだ時点で残っていた差は**文字列の写し方だけ**だった:
 *
 * | | x86 (copy_cstr_fixed) | linux 側 (linux_utsname_set) |
 * |---|---|---|
 * | 上限     | sizeof(フィールド) | **64 の決め打ち** |
 * | 残り     | 終端を置くだけ     | **フィールド全体を 0 埋め** |
 *
 * **両方を取った。**長さは sizeof で縛り (フィールドの大きさが変わっても
 * 追従する)、残りは 0 埋めする (呼び手のバッファに前の中身が残らない)。 */

#include <stddef.h>
#include "linux_abi.h"       /* struct linux_utsname */
#include "linux_errno.h"
#include "linux_syscall.h"   /* arch_uname_machine / arch_uname_version */
#include "sys_internal.h"
#include "version.h"

/* dst_size のぶんだけ写し、**余りは 0 で埋める。**src が NULL なら全部 0 */
static void utsname_set(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;
    if (!dst || dst_size == 0) return;
    if (src) {
        while (i + 1 < dst_size && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    while (i < dst_size) dst[i++] = '\0';
}

int sys_uname(struct linux_utsname* buf) {
    if (!buf) return -LINUX_EFAULT;
    utsname_set(buf->sysname,    sizeof(buf->sysname),    ORTHOX_UNAME_SYSNAME);
    utsname_set(buf->nodename,   sizeof(buf->nodename),   ORTHOX_UNAME_NODENAME);
    utsname_set(buf->release,    sizeof(buf->release),    ORTHOX_KERNEL_RELEASE);
    utsname_set(buf->version,    sizeof(buf->version),    arch_uname_version());
    utsname_set(buf->machine,    sizeof(buf->machine),    arch_uname_machine());
    utsname_set(buf->domainname, sizeof(buf->domainname), ORTHOX_UNAME_DOMAINNAME);
    return 0;
}
