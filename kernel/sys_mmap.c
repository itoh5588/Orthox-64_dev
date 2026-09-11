/* ---- mmap / munmap (3 アーキ共通) ----------------------------------------
 *
 * **別実装 29 組のうちの 1 組を畳んだもの (2026-09-11)。**もとは
 *
 *     x86              kernel/x86_64/sys_vm.c   の sys_mmap
 *     aarch64/riscv64  kernel/linux_syscall.c   の linux_bootstrap_sys_mmap
 *
 * の 2 つがあり、**重複ではなく動作が違っていた。**
 *
 * | | x86 | linux 側 |
 * |---|---|---|
 * | MAP_SHARED     | 受け付ける | EINVAL で断る |
 * | 失敗時の後始末 | 貼った分を剥がす | 無い (途中まで貼って返る) |
 * | PIPE の fd     | ENODEV | 検査なし |
 * | 匿名で offset  | EINVAL | 検査なし |
 *
 * **x86 側に寄せた。**musl が MAP_SHARED を使う (ports/musl/src/time/
 * __map_file.c と src/thread/sem_open.c) ので、linux 側に寄せると x86 が
 * 壊れる。逆向きなら aarch64 / riscv64 が MAP_SHARED を得る。
 *
 * ページテーブルの操作は arch_vm_* だけを通す。x86 は pml4 を直に触って
 * いたが、arch_vm_unmap_page を埋めた (2026-09-10) ので共通で書ける。
 *
 * **MAP_SHARED は受け付けるが private として扱う。**書き戻しもプロセス間の
 * 共有もしない。もとの x86 の作りをそのまま引き継いでいる。
 * ファイルを貼る場合は **貼るときに 1 度だけ写す** (遅延読みではない)。 */

#include <stdint.h>
#include <stddef.h>
#include "task.h"
#include "fs.h"
#include "pmm.h"
#include "vmm.h"   /* PHYS_TO_VIRT。3 アーキとも g_hhdm_offset を持つ */
#include "syscall.h"
#include "sys_internal.h"
#include "linux_errno.h"
#include "arch_vm.h"
#include "arch_syscall.h"
#include "string.h"

void puts(const char* s);

/* ユーザーのページテーブルを書き替えた後。宣言は include/linux_syscall.h に
 * あるが、あちらは linux 側の口をまとめたヘッダなのでここでは引かない */
void arch_syscall_flush_tlb(void);

/* kernel/linux_syscall.c。riscv64 では弱い既定 (常に -1) が選ばれる */
int64_t sys_pread64(int fd, void* buf, size_t count, int64_t offset);

/* **返り値は (void*)-errno。**musl は -4096 < ret < 0 を errno へ写すので、
 * (void*)-1 を返すと MAP_FAILED ではなく EPERM を返したことになる
 * (もとの linux_syscall.c:1317 のコメント) */
static void* mmap_err(int err) {
    return (void*)(intptr_t)(-err);
}

static uint64_t mmap_align_up(uint64_t v) {
    return (v + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static int mmap_range_valid(uint64_t vaddr, uint64_t size) {
    if (size == 0) return 0;
    if ((vaddr & (PAGE_SIZE - 1ULL)) != 0) return 0;
    if (vaddr < USER_MMAP_BASE_VADDR) return 0;
    if (vaddr >= USER_MMAP_TOP_VADDR) return 0;
    if (vaddr + size < vaddr) return 0;
    if (vaddr + size > USER_MMAP_TOP_VADDR) return 0;
    return 1;
}

static int mmap_range_unmapped(arch_address_space_t as, uint64_t base, uint64_t size) {
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        if (arch_vm_get_phys(as, base + off) != 0) return 0;
    }
    return 1;
}

/* **写像を外して物理ページを返す。**arch_vm_unmap_page は写像を外すだけで
 * 持ち主を変えない約束なので、返すのはこちら。
 *
 * 外しただけだと **そのページは誰の物でもなくなり、参照カウントが 1 の
 * まま二度と配られない。**musl の malloc は大きい塊を mmap / munmap で
 * 扱うので、cc1 のように確保と解放を繰り返すプログラムで積み上がる。
 * 実測では OS の中でカーネルを 1 本コンパイルするたびに約 18MB 漏れ、
 * 512MB を 15 本で使い切っていた (もとは linux_syscall.c の munmap に
 * 書いてあった説明。2026-09-11 に munmap が 3 アーキ共通になって移した)。
 *
 * pmm_free は参照カウントを見るので、共有されているページ (fork の COW) を
 * 返しても取り上げてしまうことはない。get_phys はページ内オフセットを
 * OR して返すアーキがあるので、**下位ビットを落としてから渡す。**
 *
 * **外れたことを確かめてから返す** —— get_phys は 2MB ページでも番地を
 * 返すが arch_vm_unmap_page は 2MB を触らない。確かめずに返すと
 * まだ写像されているページを取り上げる */
static void mmap_drop_page(arch_address_space_t as, uint64_t vaddr) {
    uint64_t phys = arch_vm_get_phys(as, vaddr);
    arch_vm_unmap_page(as, vaddr);
    if (phys && arch_vm_get_phys(as, vaddr) == 0) {
        pmm_free((void*)(uintptr_t)(phys & ~(PAGE_SIZE - 1ULL)), 1);
    }
}

/* 空きを探す。**hint から上へ、足りなければ下端から hint まで。**
 * もとの x86 の find_mmap_gap と同じ順で、こちらは arch_vm_get_phys で見る */
static uint64_t mmap_find_gap_from(arch_address_space_t as, uint64_t size,
                                   uint64_t start, uint64_t end) {
    if (start < USER_MMAP_BASE_VADDR) start = USER_MMAP_BASE_VADDR;
    start = mmap_align_up(start);
    for (uint64_t base = start; base + size <= end; base += PAGE_SIZE) {
        if (mmap_range_unmapped(as, base, size)) return base;
    }
    return 0;
}

static uint64_t mmap_find_gap(arch_address_space_t as, uint64_t size, uint64_t hint) {
    uint64_t base;
    if (size == 0 || size > (USER_MMAP_TOP_VADDR - USER_MMAP_BASE_VADDR)) return 0;
    if (hint < USER_MMAP_BASE_VADDR || hint >= USER_MMAP_TOP_VADDR) hint = USER_MMAP_BASE_VADDR;

    base = mmap_find_gap_from(as, size, hint, USER_MMAP_TOP_VADDR);
    if (base != 0) return base;
    if (hint > USER_MMAP_BASE_VADDR) return mmap_find_gap_from(as, size, USER_MMAP_BASE_VADDR, hint);
    return 0;
}

/* ページ 1 枚をファイルから読んで埋める。**pread なので fd の現在位置は
 * 動かない。**musl は同じ fd でヘッダを読みながらセグメントを貼るので、
 * 位置を動かすと壊れる (linux_syscall.c から引き継いだ理由) */
static void mmap_fill_from_file(uint8_t* dest, int fd, uint64_t file_off) {
    if (!dest || fd < 0) return;
    (void)sys_pread64(fd, dest, PAGE_SIZE, (int64_t)file_off);
}

/* この機械でファイルを貼る mmap が使えるか。
 * **riscv64 は kernel/sys_fs.c を繋いでおらず sys_pread64 が弱い既定
 * (常に -1)** なので、貼れない。黙って 0 埋めのページを返すより
 * ENOSYS で断るほうがよい —— musl は静的な道へ退くか、意味の分かる
 * 文言を出せる (linux_syscall.c から引き継いだ判断) */
static int mmap_can_map_file(void) {
#if defined(__riscv)
    return 0;
#else
    return 1;
#endif
}

void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    struct task* current = get_current_task();
    arch_address_space_t as;
    file_descriptor_t* backing_fd = 0;
    uint64_t size;
    uint64_t base;
    uint64_t map_flags;
    uint64_t mapped;
    int is_anonymous;

    if (!current) return mmap_err(LINUX_ESRCH);
    if (length == 0) return mmap_err(LINUX_EINVAL);
    /* **どちらか一方は要る。**x86 の作りをそのまま引き継ぐ */
    if (!(flags & (MAP_PRIVATE | MAP_SHARED))) return mmap_err(LINUX_EINVAL);
    if (offset < 0) return mmap_err(LINUX_EINVAL);
    if ((offset & (PAGE_SIZE - 1)) != 0) return mmap_err(LINUX_EINVAL);

    size = mmap_align_up((uint64_t)length);
    if (size == 0) return mmap_err(LINUX_EINVAL);

    as = arch_task_context_get_address_space(&current->ctx);

    is_anonymous = (flags & MAP_ANONYMOUS) != 0;
    if (is_anonymous) {
        if (fd != -1 && fd != 0) return mmap_err(LINUX_EINVAL);
        if (offset != 0) return mmap_err(LINUX_EINVAL);
    } else {
        if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return mmap_err(LINUX_EBADF);
        backing_fd = &current->fds[fd];
        if (fs_fd_type(backing_fd) == FT_PIPE) return mmap_err(LINUX_ENODEV);
        /* **貼れないなら先に断る。**途中まで貼ってから諦めると後始末が要る */
        if (!mmap_can_map_file()) return mmap_err(LINUX_ENOSYS);
    }

    base = mmap_align_up((uint64_t)(uintptr_t)addr);
    if (base == 0 || !(flags & MAP_FIXED)) {
        base = mmap_find_gap(as, size, current->mmap_end);
        if (base == 0) return mmap_err(LINUX_ENOMEM);
        current->mmap_end = mmap_align_up(base + size);
    } else {
        if ((uint64_t)(uintptr_t)addr != base) return mmap_err(LINUX_EINVAL);
        if (!mmap_range_valid(base, size)) return mmap_err(LINUX_EINVAL);
        /* Linux の MAP_FIXED は重なった写像を置き換える。
         * **musl は 1 度予約した範囲の中を指してくる**ので実際に通る道 */
        if (!mmap_range_unmapped(as, base, size)) {
            for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
                mmap_drop_page(as, base + off);
            }
        }
    }

    if (!mmap_range_valid(base, size)) return mmap_err(LINUX_EINVAL);

    map_flags = arch_vm_user_page_flags((prot & PROT_WRITE) != 0, (prot & PROT_EXEC) != 0);

    mapped = 0;
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        void* phys = pmm_alloc(1);
        if (!phys) {
            /* **貼った分を剥がしてから返す。**途中まで貼ったまま失敗すると、
             * ユーザーからは見えない写像が残る (x86 の rollback_mmap) */
            for (uint64_t back = 0; back < mapped; back += PAGE_SIZE) {
                mmap_drop_page(as, base + back);
            }
            arch_syscall_flush_tlb();
            return mmap_err(LINUX_ENOMEM);
        }
        memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        if (!is_anonymous) {
            mmap_fill_from_file((uint8_t*)PHYS_TO_VIRT(phys), fd, (uint64_t)offset + off);
        }
        arch_vm_map_page(as, base + off, (uint64_t)(uintptr_t)phys, map_flags);
        mapped += PAGE_SIZE;
    }

    if (base + size > current->mmap_end) current->mmap_end = base + size;
    arch_syscall_flush_tlb();
    return (void*)(uintptr_t)base;
}

int sys_munmap(void* addr, size_t length) {
    struct task* current = get_current_task();
    arch_address_space_t as;
    uint64_t base;
    uint64_t size;

    if (!current || length == 0) return -LINUX_EINVAL;

    base = (uint64_t)(uintptr_t)addr;
    if (base & (PAGE_SIZE - 1ULL)) return -LINUX_EINVAL;
    size = mmap_align_up((uint64_t)length);
    /* **範囲の外は断る。**mmap が貼るのはこの範囲だけなので、外せる範囲も
     * それに揃える。riscv64 はカーネルが下位半分 (0x80200000〜) に居て、
     * ユーザーの表は下の段をカーネルと共有しているため、ここを見ないと
     * **ユーザーから全プロセスのカーネルの写像を外せた** (aarch64 / riscv64
     * の linux 側 munmap がそうだった。user/riscv64_errno_probe.c の
     * munmap-kernel で、次の write でカーネルが止まるのを確かめた) */
    if (!mmap_range_valid(base, size)) return -LINUX_EINVAL;

    as = arch_task_context_get_address_space(&current->ctx);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        mmap_drop_page(as, base + off);
    }
    arch_syscall_flush_tlb();
    return 0;
}
