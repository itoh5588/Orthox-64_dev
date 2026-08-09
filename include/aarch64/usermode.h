#ifndef ORTHOX_ARCH_AARCH64_USERMODE_H
#define ORTHOX_ARCH_AARCH64_USERMODE_H

/* EL0 側 (kernel/aarch64/user_blob.S) とカーネル側 (kernel/aarch64/usermode.c)
 * が共有する取り決め。**両方に書くと必ずずれる**ので、ここ 1 か所に置く。
 *
 * .S からも読むので、**#define だけ**にしておくこと。C の宣言を足すと
 * アセンブラが通らなくなる。
 */

/* システムコール番号。Linux の generic ABI に合わせる (riscv64 と同じ番号)。
 *   x8 = 番号、x0-x5 = 引数、x0 = 戻り値 */
#define AARCH64_NR_WRITE 64
#define AARCH64_NR_EXIT  93

/* EL0 と EL1 で同じ回数だけ空回しして、tick の入り方を比べるための回数。
 *
 * **判定を境界の上に乗せないこと。** 最初 0x400000 にしていたら EL1 も EL0 も
 * 1 tick (10ms) ぶんちょうどで、実行のたびに 0 と 1 のあいだで揺れた。
 * 0x1000000 に上げたら EL0 で 2 tick になったが、要求も 2 だったので
 * まだ境界の上だった。**実測して倍以上の余裕がある値にする。**
 *
 * 実測 (QEMU virt / cortex-a72 / TCG、2 回とも同じ値):
 *   EL1 の空回し (C の volatile ループ)  0x4000000 で 19 tick
 *   EL0 の空回し (subs + b.ne の 2 命令)  0x4000000 で 11 tick
 * EL1 のほうが遅いのは、volatile が毎回メモリを読み書きするため。 */
#define AARCH64_USER_SPIN 0x4000000

/* 判定に要求する tick 数。EL0 で 11 入るので、2 倍以上の余裕を取って 4 */
#define AARCH64_USER_MIN_TICKS 4

#endif
