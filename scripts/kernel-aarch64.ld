/* AArch64 + QEMU virt 用のリンカスクリプト。
 *
 * **カーネルは上位半分 (TTBR1 の領域) にリンクする (M3b)。**
 * これで TTBR0 がまるごとユーザー専用になり、プロセス切り替えで TTBR0 だけ
 * 差し替えればよくなる。riscv64 / x86 がカーネル領域の写しを各プロセスの
 * テーブルに作っていたところが、AArch64 では要らなくなる。
 *
 * ただし **ELF の entry を上位 VA にはできない。** QEMU は -kernel で渡した
 * ELF の entry に MMU off のまま飛ぶので、0xffffff80... を書くと即座に迷子に
 * なる。そこで 2 つの領域に分ける:
 *
 *   .boot    0x40200000 に VA = PA で置く。QEMU が飛んでくる入口。
 *            MMU を入れて上位 VA へ飛ぶまでの、ごく短い区間だけを担う
 *   その他   上位 VA でリンクし、物理は .boot の直後に載せる (AT(...))
 *
 * **.boot と上位側のあいだは PC 相対で届かない** (リンク上の距離が 2^40 ある)。
 * .boot から上位側のシンボルを参照するときは、絶対アドレスを取ってから
 * KERNEL_VA_OFFSET を引いて物理に直すこと。start.S にその実装がある。
 *
 * 逆に上位側どうしの参照は PC 相対で届くので、**MMU を入れる前 (PC が物理)
 * でもそのまま動く**。adrp は「リンク時の相対距離」を埋め込むので、
 * 実行時の PC が物理なら結果も物理になる。ここが直感に反する。
 *
 * 配置は DTB の実測値に合わせてある:
 *   memory@40000000  reg = 0x40000000  size = 0x20000000 (512MB)
 */
ENTRY(_start)

/* 上位半分の起点。TCR_EL1.T1SZ = 25 (VA 39bit) なので、TTBR1 が受け持つのは
 * VA[63:39] が全部 1 の範囲 = 0xffffff8000000000 以上。
 * カーネルの VA は「物理アドレス + この値」にする。
 * **include/aarch64/vm.h の AARCH64_KERNEL_VA_OFFSET と同じ値にすること** */
KERNEL_VA_OFFSET = 0xffffff8000000000;

SECTIONS
{
    . = 0x40200000;
    __boot_start = .;

    /* QEMU が飛んでくる入口。ここだけ VA = PA */
    .boot : ALIGN(0x1000) {
        KEEP(*(.text.start))
        *(.boot .boot.*)
        . = ALIGN(0x1000);
    }
    __boot_end = .;

    /* ここから上位 VA。物理は .boot の直後に続けて載せる */
    . = __boot_end + KERNEL_VA_OFFSET;
    __kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VA_OFFSET) {
        __text_start = .;
        *(.text .text.*)
        . = ALIGN(0x1000);
        __text_end = .;
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VA_OFFSET) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(0x1000);
        __rodata_end = .;
    }

    .data : AT(ADDR(.data) - KERNEL_VA_OFFSET) {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(0x1000);
        __data_end = .;
    }

    /* EL0 で動くコードとデータ。**カーネルの区画とは別のページに置く。**
     * 同じページにカーネルのものが混ざっていると、ユーザー権限を与えた
     * 瞬間にカーネルの中身まで EL0 から見えてしまう。
     *
     * M3b からは TTBR0 の専用テーブルに、**カーネルとは無関係の VA**
     * (AARCH64_USER_VA_BASE) で張る。ここでの配置は「イメージのどこに
     * 載っているか」を決めているだけ */
    .user_text : AT(ADDR(.user_text) - KERNEL_VA_OFFSET) {
        __user_text_start = .;
        *(.user_text)
        . = ALIGN(0x1000);
        __user_text_end = .;
    }

    .user_data : AT(ADDR(.user_data) - KERNEL_VA_OFFSET) {
        __user_data_start = .;
        *(.user_data)
        . = ALIGN(0x1000);
        __user_data_end = .;
    }

    .bss : AT(ADDR(.bss) - KERNEL_VA_OFFSET) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(0x1000);
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
    }
}
