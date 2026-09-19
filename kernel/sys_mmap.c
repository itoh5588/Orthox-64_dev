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
#include "vm_cow.h"
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

/* ---- mprotect (3 アーキ共通) ----------------------------------------------
 *
 * **別実装 29 組のうちの 1 組を畳んだもの (2026-09-12)。**もとは
 *
 *     x86              kernel/x86_64/sys_vm.c の sys_mprotect
 *     aarch64/riscv64  kernel/linux_syscall.c の linux_bootstrap_sys_mprotect
 *
 * の 2 つで、**動作が違っていた:**
 *
 * | | x86 | linux 側 |
 * |---|---|---|
 * | 境界でない addr | 切り下げて続ける | EINVAL |
 * | length == 0     | EINVAL           | 成功 (0) |
 * | COW のページ    | **書き込み可にしない** | 素通りで書き込み可になる |
 * | icache          | 揃えない | PROT_EXEC なら揃える |
 *
 * **引数の扱いは linux 側に寄せた** —— Linux がそう振る舞うため
 * (境界でない addr は EINVAL、length 0 は成功)。musl はページ境界で呼ぶので
 * 呼び手には影響しない。
 *
 * **COW は x86 側に寄せた。**fork の CoW で共有中のページを書き込み可に
 * すると**両プロセスから同じページに書けてしまう。**2026-09-19 に CoW を
 * 3 アーキ共通にしたので、判断は vm_cow_protect_page (kernel/vm_cow.c) が
 * 持つ —— 共有中なら書き込み可ではなく CoW の印に振り替える。
 *
 * icache も残した (実行可にしたのに古い命令が見えるのを防ぐ)。 */
int sys_mprotect(void* addr, size_t length, int prot) {
    struct task* current = get_current_task();
    arch_address_space_t as;
    uint64_t base = (uint64_t)(uintptr_t)addr;
    uint64_t size;

    if (!current) return -LINUX_ESRCH;
    /* Linux は addr がページ境界でないと EINVAL。length 0 は成功 */
    if (base & (PAGE_SIZE - 1ULL)) return -LINUX_EINVAL;
    if (length == 0) return 0;

    size = mmap_align_up((uint64_t)length);
    if (!size || base + size < base) return -LINUX_EINVAL;

    as = arch_task_context_get_address_space(&current->ctx);

    /* **先に全域がユーザーのページであることを確かめる。**途中まで書き換えて
     * から穴に当たると、成功した分が戻せない。Linux も穴があれば ENOMEM。
     *
     * **get_phys != 0 で見てはいけない (2026-09-11)。**riscv64 はカーネルが
     * RAM 全体を下位半分に恒等写像しており、get_phys はそのページにも番地を
     * 返す。そう見ていた頃は mprotect(0x9f000000, 4096, PROT_READ) が通り、
     * **ユーザーがカーネルの RAM を読めた** (user/riscv64_errno_probe.c の
     * mprotect-kernel-ram) */
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        if (!arch_vm_is_user_page(as, base + off)) return -LINUX_ENOMEM;
    }
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        vm_cow_protect_page(as, base + off, (prot & PROT_WRITE) != 0,
                            (prot & PROT_EXEC) != 0);
    }
    arch_syscall_flush_tlb();
    /* 実行可にしたなら、命令キャッシュを揃えないと古い中身を実行しうる */
    if (prot & PROT_EXEC) arch_sync_icache_range((void*)(uintptr_t)base, size);
    return 0;
}

/* ---- mremap (3 アーキ共通) ------------------------------------------------
 *
 * **別実装 29 組のうちの 1 組を畳んだもの (2026-09-12)。**もとは
 *
 *     x86              kernel/x86_64/sys_vm.c の sys_mremap
 *     aarch64/riscv64  kernel/linux_syscall.c の linux_bootstrap_sys_mremap
 *
 * の 2 つで、**動作が違っていた:**
 *
 * | | x86 | linux 側 |
 * |---|---|---|
 * | 古い範囲の検査 | 先頭 1 枚だけ | **全ページ** (2026-09-12 に塞いだ穴) |
 * | **保護の引き継ぎ** | **先頭 PTE から読む** | **固定で rw / 実行不可** |
 * | 移動時の写し | バイト単位 (ページ跨ぎ対応) | ページ単位 |
 *
 * **保護の引き継ぎは x86 側へ寄せた。**linux 側は移した先を必ず rw・実行不可に
 * していたので、**実行可の範囲を mremap した瞬間に実行できなくなる。**
 * 読み出しは vm_cow_get_page_prot (kernel/vm_cow.c)。CoW の印の立った
 * ページは「書けた」と読む必要がある。
 *
 * **範囲の検査は linux 側 (新しいほう) へ寄せた。**先頭 1 枚しか見ないと、
 * riscv64 ではカーネルのページを縮小・移動できた (a3abd59)。
 *
 * 写しはページ単位でよい —— ここへ来る番地はすべてページ境界に揃っている。 */
#define MREMAP_MAYMOVE_FLAG 1
#define MREMAP_FIXED_FLAG   2

void* sys_mremap(void* old_addr, size_t old_len, size_t new_len, int flags, void* new_addr) {
    struct task* current = get_current_task();
    arch_address_space_t as;
    uint64_t old_base = (uint64_t)(uintptr_t)old_addr;
    uint64_t old_size, new_size;
    int writable = 1, executable = 0;
    int prot;

    if (!current) return mmap_err(LINUX_ESRCH);
    if (!old_base || !old_len || !new_len) return mmap_err(LINUX_EINVAL);
    if ((old_base & (PAGE_SIZE - 1ULL)) != 0) return mmap_err(LINUX_EINVAL);
    if (flags & ~(MREMAP_MAYMOVE_FLAG | MREMAP_FIXED_FLAG)) return mmap_err(LINUX_EINVAL);
    if ((flags & MREMAP_FIXED_FLAG) && !(flags & MREMAP_MAYMOVE_FLAG)) return mmap_err(LINUX_EINVAL);

    old_size = mmap_align_up((uint64_t)old_len);
    new_size = mmap_align_up((uint64_t)new_len);
    as = arch_task_context_get_address_space(&current->ctx);

    /* **古い範囲が全部このプロセスのページであることを先に確かめる。**
     * 先頭 1 枚を get_phys != 0 で見るだけだった頃は、riscv64 で
     * カーネルのページを縮小・移動できた (user/riscv64_errno_probe.c の
     * mremap-kernel-shrink / mremap-kernel-ram)。
     *
     * munmap は VA の範囲で断っているが、**mremap は mprotect と同じく
     * ページごとの U ビットで見る** —— Linux は ELF の段やスタックの
     * mremap も認めるので、mmap の範囲には狭められない */
    for (uint64_t off = 0; off < old_size; off += PAGE_SIZE) {
        if (!arch_vm_is_user_page(as, old_base + off)) return mmap_err(LINUX_EINVAL);
    }

    /* **元の保護を引き継ぐ。**読めなければ「読み書き・実行不可」に倒す
     * (musl の realloc はヒープにしか使わないので、そこが既定) */
    if (vm_cow_get_page_prot(as, old_base, &writable, &executable) < 0) {
        writable = 1;
        executable = 0;
    }
    prot = PROT_READ | (writable ? PROT_WRITE : 0) | (executable ? PROT_EXEC : 0);

    if (new_size == old_size) return old_addr;

    /* 縮める: はみ出した分を外すだけ (munmap と同じ後始末) */
    if (new_size < old_size) {
        for (uint64_t off = new_size; off < old_size; off += PAGE_SIZE) {
            mmap_drop_page(as, old_base + off);
        }
        arch_syscall_flush_tlb();
        return old_addr;
    }

    /* 伸ばす: 直後が空いていればその場で足す (move 不要) */
    {
        uint64_t end = old_base + old_size;
        uint64_t grow = new_size - old_size;
        uint64_t map_flags;
        int in_place = 1;

        /* **その場で足せるのはユーザーの VA の中だけ。**越えたところへ貼ると
         * Sv39 では添字が折り返してカーネル側の表を引く */
        if (!mmap_range_valid(end, grow) || !mmap_range_unmapped(as, end, grow)) {
            in_place = 0;
        }
        if (in_place) {
            uint64_t mapped = 0;
            map_flags = arch_vm_user_page_flags(writable, executable);
            for (uint64_t off = 0; off < grow; off += PAGE_SIZE) {
                void* phys = pmm_alloc(1);
                if (!phys) {
                    /* **足した分を剥がしてから返す** (mmap の巻き戻しと同じ) */
                    for (uint64_t back = 0; back < mapped; back += PAGE_SIZE) {
                        mmap_drop_page(as, end + back);
                    }
                    arch_syscall_flush_tlb();
                    return mmap_err(LINUX_ENOMEM);
                }
                memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
                arch_vm_map_page(as, end + off, (uint64_t)(uintptr_t)phys, map_flags);
                mapped += PAGE_SIZE;
            }
            arch_syscall_flush_tlb();
            return old_addr;
        }
    }

    if (!(flags & MREMAP_MAYMOVE_FLAG)) return mmap_err(LINUX_ENOMEM);

    /* 直後に空きが無い: 新しい範囲を作り、中身を写してから古い方を外す */
    {
        void* mapped = sys_mmap((flags & MREMAP_FIXED_FLAG) ? new_addr : 0,
                                (size_t)new_size, prot,
                                MAP_PRIVATE | MAP_ANONYMOUS |
                                ((flags & MREMAP_FIXED_FLAG) ? MAP_FIXED : 0),
                                -1, 0);
        uint64_t new_base;
        if ((int64_t)(intptr_t)mapped < 0 && (int64_t)(intptr_t)mapped > -4096) return mapped;
        new_base = (uint64_t)(uintptr_t)mapped;
        for (uint64_t off = 0; off < old_size && off < new_size; off += PAGE_SIZE) {
            uint64_t old_phys = arch_vm_get_phys(as, old_base + off);
            uint64_t new_phys = arch_vm_get_phys(as, new_base + off);
            if (old_phys && new_phys) {
                memcpy((void*)(uintptr_t)PHYS_TO_VIRT(new_phys & ~(PAGE_SIZE - 1ULL)),
                       (void*)(uintptr_t)PHYS_TO_VIRT(old_phys & ~(PAGE_SIZE - 1ULL)),
                       PAGE_SIZE);
            }
        }
        /* **実行可なら命令キャッシュを揃える。**写したばかりのページを
         * そのまま実行しうる */
        if (executable) arch_sync_icache_range((void*)(uintptr_t)new_base, new_size);
        (void)sys_munmap(old_addr, (size_t)old_size);
        return (void*)(uintptr_t)new_base;
    }
}

/* ---- brk (3 アーキ共通) ----------------------------------------------------
 *
 * **別実装 29 組のうちの 1 組を畳んだもの (2026-09-12)。**もとは
 *
 *     x86              kernel/x86_64/sys_vm.c の sys_brk
 *     aarch64/riscv64  kernel/linux_syscall.c の linux_bootstrap_sys_brk
 *
 * の 2 つで、**動作が違っていた:**
 *
 * | | x86 | linux 側 |
 * |---|---|---|
 * | mmap 領域へ食い込む brk | **断る** | **通る** |
 * | TLB | vmm_map_page の invlpg 任せ | 一括で捨てる |
 * | 追跡 | memtrace あり | 無し |
 *
 * **x86 側へ寄せた。**ヒープを伸ばし続けると mmap の下端に届く。断らないと
 * **brk が mmap で貼った範囲を上書きする。**下端はどのアーキも
 * USER_MMAP_BASE_VADDR に揃えてある (2026-09-11 に x86 の MMAP_BASE_ADDR と
 * 一致させた)。
 *
 * **失敗の伝え方は brk(2) の流儀そのまま** —— 途中で足りなくなったら
 * 古い break を返す。呼び手は「変わっていない」ことで失敗を知る。 */

/* x86 だけが持つ追跡 (kernel/sys_trace.c の系)。他のアーキでは何もしない */
__attribute__((weak)) void syscall_memtrace_brk(uint64_t requested, uint64_t old_brk,
                                                uint64_t new_brk, uint64_t pages) {
    (void)requested; (void)old_brk; (void)new_brk; (void)pages;
}

uint64_t sys_brk(uint64_t addr) {
    struct task* current = get_current_task();
    arch_address_space_t as;
    uint64_t old_break;
    uint64_t current_page;
    uint64_t target_page;
    uint64_t pages = 0;
    uint64_t map_flags;

    if (!current) return 0;
    old_break = current->heap_break;

    /* **mmap の領域へ食い込ませない。**変わらない break を返すのが brk の
     * 失敗の伝え方 (もとの x86 の sys_brk と同じ) */
    if (addr == 0 || addr <= old_break || addr >= USER_MMAP_BASE_VADDR) {
        syscall_memtrace_brk(addr, old_break, old_break, 0);
        return old_break;
    }

    current_page = mmap_align_up(old_break);
    target_page = mmap_align_up(addr);
    as = arch_task_context_get_address_space(&current->ctx);
    map_flags = arch_vm_user_page_flags(1, 0);

    while (current_page < target_page) {
        void* phys = pmm_alloc(1);
        if (!phys) {
            /* **貼った分はそのまま残す。**break を伸ばさないので、
             * ユーザーからは見えない範囲になるだけ (もとの両実装と同じ) */
            arch_syscall_flush_tlb();
            syscall_memtrace_brk(addr, old_break, old_break, pages);
            return old_break;
        }
        memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        arch_vm_map_page(as, current_page, (uint64_t)(uintptr_t)phys, map_flags);
        current_page += PAGE_SIZE;
        pages++;
    }

    current->heap_break = addr;
    arch_syscall_flush_tlb();
    syscall_memtrace_brk(addr, old_break, current->heap_break, pages);
    return current->heap_break;
}
