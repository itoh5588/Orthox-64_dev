# Ch6 — syscall entry (境界を越える一命令)

第6章は本書のタイトル『syscall から始める自作 OS』の **タイトル句が指す核心** です。アセンブリ命令 `syscall` の挙動、MSR 4 本の役割、`swapgs` の魔術、`struct syscall_frame` の並び、Linux 互換 ABI と独自拡張の共存——本書最大の密度を持つ章の一つです。

## 章のゴール

* `syscall` 命令が実行された瞬間の CPU 内部挙動 (RCX/R11 自動保存、SS/CS の自動切替) を **ハードウェア仕様レベル** で把握する。
* MSR_EFER / MSR_STAR / MSR_LSTAR / MSR_SFMASK の **4 本それぞれの役割** を区別できる。
* `swapgs` 命令が GS_BASE と KERNEL_GS_BASE を物理的に入れ替えるという挙動と、それを使ったカーネルスタック切り替えのトリックを再現できる。
* `struct syscall_frame` のフィールド宣言順とアセンブリの `push` 順が **逆方向** であることに気付き、混乱しない。
* `syscall_dispatch` が **switch-case** であり、関数ポインタテーブルではない、という Orthox-64 の設計判断を AI が混同したら正せる。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (6.2 〜 6.8) に対応した6つのクラスタに分かれています。

## 取り組み方の推奨フロー

1. 本書 6.1 〜 6.10 を**通読**してから戻る。図 6.1 〜 6.4 を頭の中で描けるか確認。
2. `kernel/syscall.c` の `syscall_init_cpu` (line 29 付近) を読み、本書 6.2 の MSR 4 本の wrmsr と対応付ける。
3. `kernel/syscall_entry.S` を読み、本書 6.3 の `swapgs` + スタック切替アセンブリと突き合わせる。
4. `kernel/task_internal.h` で `struct syscall_frame` の宣言順と、`syscall_entry.S` の `push` 順を**並べて見る** (逆順)。
5. `kernel/syscall.c` の `syscall_dispatch` (line 42 付近) と `default:` 節 (line 418 付近) を確認。
6. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# MSR と syscall 初期化
grep -n "MSR_\|wrmsr\|syscall_init_cpu" ../kernel/syscall.c ../include/syscall.h

# アセンブリ突入経路
cat ../kernel/syscall_entry.S

# ディスパッチ本体
grep -n "syscall_dispatch\|switch (syscall_no)\|default:" ../kernel/syscall.c

# syscall_frame 構造体
grep -n "struct syscall_frame" ../kernel/task_internal.h
```

## AI への話しかけ方の例

* 「第6章 6.3 で `swapgs` がカーネルスタック切替に使われるとあった。`kernel/syscall_entry.S` の冒頭3行を見せて、`%gs:0` と `%gs:8` がそれぞれ `cpu_local` のどのフィールドに対応するか、`include/task.h` の `struct cpu_local` 宣言と一致しているか確認したい」
* 「第6章 6.5 で第4引数が R10 とあった。これがハードウェア仕様 (`syscall` 命令が RCX に RIP を強制退避する) ゆえの妥協、という説明が本書脚注にあった。`syscall_dispatch` の `case SYS_MMAP:` 周辺で `frame->r10` がどう参照されているか grep で示して」
* 「第6章 6.6 で `default:` が `-38` (ENOSYS 相当) を返すとあった。`kernel/syscall.c` の該当行を grep で見せて、本書記述と完全一致するか確認したい」

AI は本書本文をテキストとしては読めないので、章番号と節番号、登場するシンボル名を添えて質問してください。
