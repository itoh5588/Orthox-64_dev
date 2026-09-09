#ifndef ORTHOX_BOOT_MODULE_H
#define ORTHOX_BOOT_MODULE_H

#include <stdint.h>

/* **ブートローダが載せた初期ファイル。**
 *
 * x86 は Limine の module (limine_file) がこれに当たる。**受け取り方は
 * ブートローダの規約なので ISA ごとに違う** —— aarch64 (Pi 4 / QEMU virt)
 * には Limine が無く、初期ファイルは xv6fs のイメージから来る。
 *
 * 共有層 (fs.c) はこの 3 つしか見ていないので、契約もこの 3 つに絞る。
 * 中身の寿命はブートローダが確保した領域そのもの —— **呼び手は解放しない**
 * (fs.c の fs_free_exec_buffer が address を見て素通しするのはこのため)。 */
struct boot_module {
    const char* path;
    void*       address;
    uint64_t    size;
};

#endif
