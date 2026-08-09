/*
 * aarch64 にまだ無いもの (C-1a)。
 *
 * kernel/fs.c / task_exec.c を共有層から取り込むと、x86 にしか無い周辺
 * (キーボード / USB / ネットワーク / Limine のモジュール) が芋づるで
 * 要求される。**必要なのは 11 個だけ**で、llvm-nm -u で実測して決めた。
 *
 * ここに集める方針:
 *
 *   1. **黙って成功を返さない。** 「まだ無い」と「何もしなくてよい」は
 *      別のこと。前者は失敗を返し、後者は正しく何もする必要が無い理由を書く
 *   2. **黙って固まらせない。** 待ち続ける形にすると、原因不明のハングに
 *      化ける。無いものは無いと即座に返す
 *   3. 1 か所にまとめる。散らすと「何が未実装か」が読めなくなる
 */
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "task.h"

/* ---- コンソール入力 (キーボード) ----------------------------------------
 *
 * **PL011 の受信割り込みをまだ入れていない。** riscv64 は
 * kernel/riscv64/runtime.c に riscv64_console_* 一式を持っている。
 *
 * kernel/fs.c の fs_console_read_blocking は
 *
 *     while (read_bytes == 0) { kb_read(); 寝る; 譲る; }
 *
 * という形なので、**0 を返すと誰も起こさないまま永久に寝る。**
 * 負を返してループを抜けさせ、read が失敗として返るようにする。
 * 「入力が無い」ではなく「入力の口が無い」ので、これが正しい */
int kb_read(char* buf, int count) {
    (void)buf;
    (void)count;
    return -1;
}

void kb_set_waiter(struct task* t)   { (void)t; }
void kb_clear_waiter(struct task* t) { (void)t; }

/* ---- 時刻 ----------------------------------------------------------------
 *
 * x86 の LAPIC タイマ由来の ms。**aarch64 には本物がある**ので繋ぐ
 * (ここだけはスタブではない) */
uint64_t arch_time_now_ms(void);
uint64_t lapic_get_ticks_ms(void) { return arch_time_now_ms(); }

/* ---- Limine のモジュール (initrd) ----------------------------------------
 *
 * x86 は Limine がブート時にモジュールを載せる。**aarch64 は -kernel で
 * ELF を直接渡しているので、そもそもモジュールが無い。**
 *
 * kernel/fs.c は module_request.response が 0 なら「無い」として
 * -1 を返す作りなので、0 のまま置けば正しく振る舞う。
 * **これは「何もしなくてよい」ほうの例。** */
volatile struct limine_module_request module_request;

/* ---- ネットワーク --------------------------------------------------------
 *
 * aarch64 に virtio-net を入れていない。socket の fd は作られないので
 * ここへは来ないはずだが、**来たら失敗を返す** (-ENOSYS) */
int64_t net_socket_read_fd(void* f, void* buf, size_t count) {
    (void)f; (void)buf; (void)count;
    return -38;
}

int64_t net_socket_write_fd(void* f, const void* buf, size_t count) {
    (void)f; (void)buf; (void)count;
    return -38;
}

/* ---- 乱数 ----------------------------------------------------------------
 *
 * kernel/sys_random.c は rdrand / rdtsc を直書きしていて aarch64 では
 * コンパイルできない (`invalid output constraint '=a'`)。
 *
 * **偽の乱数を返さない。** 適当な値を返すと、呼ぶ側は乱数を得たつもりで
 * 進む。-ENOSYS で「無い」と伝える。AArch64 には RNDR (ARMv8.5) や
 * CNTVCT を種にする手があるので、要るときに本物を入れること */
int64_t sys_getrandom(void* buf, size_t len, unsigned flags) {
    (void)buf; (void)len; (void)flags;
    return -38;
}

/* ---- USB -----------------------------------------------------------------
 *
 * aarch64 に USB スタックは無い。**「準備できていない」と答えるのが正しい。**
 * ready が 0 を返すので read が呼ばれることは無いが、念のため失敗を返す */
int usb_block_device_ready(void) { return 0; }

int usb_read_block(uint32_t lba, void* buf, uint32_t count) {
    (void)lba; (void)buf; (void)count;
    return -1;
}

/* ---- virtio コンソール出力 ----------------------------------------------
 *
 * x86 のログ収集用の出口。aarch64 では PL011 に直接出しているので要らない。
 * **書けたふりをしない** (書けたことにすると、出ていないログを出たと数える) */
int virtio_kout_write_raw(uint64_t byte_offset, const void* buf, size_t count) {
    (void)byte_offset; (void)buf; (void)count;
    return -1;
}
