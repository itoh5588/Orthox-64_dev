/*
 * musl 静的リンク用の crt0 (aarch64)。riscv64 版 (user/crt0_musl_riscv64.S)
 * と同じ形で、アーキ固有なのはレジスタの並べ方だけ。
 *
 * カーネルが EL0 へ降ろした時点で、**sp の先頭に argc があり、その 8 バイト
 * 先から argv が並んでいる** (kernel/task_exec.c が積む)。
 *
 *   __libc_start_main(main, argc, argv, init, fini, ldso_fini)
 *     x0=main  x1=argc  x2=argv  x3=_init  x4=_fini  x5=0
 *
 * **`.type` に @function と書かないこと。** aarch64 のアセンブラでは `@` が
 * 行コメントの開始なので、riscv64 版をそのまま写すと型指定ごと消える。
 */
.section .text
.global _start
.type _start, %function
.extern __libc_start_main
.extern main
.extern _init
.extern _fini

_start:
    /* **フレームポインタと戻りアドレスを断ち切る。** ここが最上位なので、
     * 残っているとバックトレースがカーネルの残骸へ迷い込む */
    mov     x29, #0
    mov     x30, #0

    mov     x9, sp                  /* sp を丸める前に argc の在り処を控える */
    ldr     x1, [x9]                /* argc */
    add     x2, x9, #8              /* argv */
    adrp    x0, main
    add     x0, x0, :lo12:main
    adrp    x3, _init
    add     x3, x3, :lo12:_init
    adrp    x4, _fini
    add     x4, x4, :lo12:_fini
    mov     x5, #0
    and     x9, x9, #-16            /* AAPCS64: sp は 16 バイト境界 */
    mov     sp, x9
    bl      __libc_start_main

    /* **__libc_start_main は戻らない** (main の戻り値で exit する)。
     * 戻ってきたときに黙って固まらせない — wfi で寝かせると原因不明の
     * ハングに化けるので、目立つ終了コードで exit する */
    mov     x8, #93                 /* __NR_exit */
    mov     x0, #127
    svc     #0
1:  b       1b
