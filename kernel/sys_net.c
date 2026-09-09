/* **ソケットの転送層は畳んだ (2026-09-08)。**
 *
 * ここには sys_socket / sys_bind / ... が 11 個並んでいたが、**どれも
 * net_socket_* を呼ぶだけで、引数も型も 1:1** だった。一方 aarch64 /
 * riscv64 の linux_syscall.c は最初から net_socket_* を直に呼んでいる ——
 * **同じ実装を、片方だけ 1 段余計に通していた。**x86 の syscall.c も直呼び
 * に揃えて、この層を外した (振る舞いは 1 ビットも変わらない)。
 *
 * 残る sys_dns_lookup は Linux の syscall ではなく Orthox 独自の口
 * (fcntl 経由) で、呼び先も net_socket ではなく lwIP なのでここに残す。 */
#include <stdint.h>
#include <stddef.h>
#include "lwip_port.h"
#include "sys_internal.h"

int sys_dns_lookup(const char* hostname, uint32_t* out_addr) {
    return lwip_port_lookup_ipv4(hostname, out_addr);
}
