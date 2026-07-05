# Ch14 — ELF loader / dynamic link / TLS

第14章は本書最大の難所の一つ。**ELF プログラムヘッダ → アドレス空間配置 → 動的リンカー (musl ld.so) の呼び出し → 初期スタック (argc/argv/envp/auxv) → TLS と FS base** という、現代の実行ファイル起動の全シーケンスを扱います。

## 章のゴール

* **ELF はセクションヘッダではなくプログラムヘッダ (`PT_LOAD`, `PT_INTERP`, `PT_TLS`)** をローダーが読むことを区別できる。
* **`ET_EXEC` (絶対アドレス) と `ET_DYN` (PIE / 共有オブジェクト)** の違いと、`load_bias` の役割を理解する。
* **動的リンカーが `PT_INTERP` パスから別ベース (`EXEC_INTERP_LOAD_BASE = 0x7fc000000000`) でロード**され、エントリ点がメインバイナリではなく**動的リンカー側**になる仕組みを掴む。
* **auxv (`AT_PHDR`, `AT_BASE`, `AT_ENTRY` 等)** が「カーネルから動的リンカーへの手紙」であることを、`task_prepare_initial_user_stack` の積み込み順序で確認できる。
* **TLS が `PT_TLS` テンプレート + ユーザー空間ランタイムによる per-thread 確保 + `arch_prctl(ARCH_SET_FS)` による FS base 設定**の三段構造で成り立つことを理解する。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (14.2 〜 14.12) に対応した7つのクラスタに分かれています (本章は密度が高いため通常の 6 ではなく 7 クラスタ)。

## 取り組み方の推奨フロー

1. 本書 14.1 〜 14.14 を**通読**してから戻る。図 14.1 〜 14.5 を頭の中で描けるか確認。
2. `include/elf64.h` で `PT_LOAD = 1`, `PT_INTERP = 3`, `PT_TLS = 7` の値と `Elf64_Phdr` 構造体を確認。
3. `kernel/elf.c:58` の `elf_load` を読み、本書 14.3 の `PT_LOAD` ループと diff。
4. `kernel/task_exec.c:363, 382` の `elf_load` 2 回呼び出しと、`EXEC_INTERP_LOAD_BASE` 定数 (line 15) を確認。
5. `kernel/task_exec.c:216` の `task_prepare_initial_user_stack` を読み、本書 14.7-14.9 のスタック配置と auxv 書き込みを追う。
6. `kernel/sys_proc.c:46` の `sys_arch_prctl` を読み、`ARCH_SET_FS` で MSR_FS_BASE を書き換える流れを確認。
7. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# ELF 定数と構造体
grep -n "PT_LOAD\|PT_INTERP\|PT_TLS\|ET_EXEC\|ET_DYN\|Elf64_Phdr" ../include/elf64.h

# ELF ローダー本体
grep -n "elf_load\|PT_LOAD\|PT_INTERP\|PT_TLS\|update_page_flags" ../kernel/elf.c

# execve 内の ELF 呼び出しと bias
grep -n "elf_load\|EXEC_ET_DYN_LOAD_BASE\|EXEC_INTERP_LOAD_BASE\|task_prepare_initial_user_stack" ../kernel/task_exec.c

# auxv
grep -n "AT_PHDR\|AT_BASE\|AT_ENTRY\|AT_TLS\|stack_write_u64" ../kernel/task_exec.c

# arch_prctl と FS base
grep -n "sys_arch_prctl\|ARCH_SET_FS\|MSR_FS_BASE" ../kernel/sys_proc.c
```

## AI への話しかけ方の例

* 「第14章 14.3 で `PT_LOAD` セグメントを `pmm_alloc` + `vmm_map_page` で配置するとあった。`kernel/elf.c:58` 付近の `elf_load` のメインループを grep で見せて、`p_filesz < p_memsz` (BSS 領域) のゼロ初期化処理が正しいか確認したい」
* 「第14章 14.6 で動的リンカーが `EXEC_INTERP_LOAD_BASE` でロードされるとあった。`kernel/task_exec.c:15` の定数定義 (`0x7fc000000000`) と、`elf_load(pml4_virt, interp_file_addr, EXEC_INTERP_LOAD_BASE)` の呼び出し (line 382 付近) を grep で示して、メインバイナリ用の `exec_load_bias` と別ベースになっていることを確認したい」
* 「第14章 14.11 で `arch_prctl(ARCH_SET_FS, addr)` が FS base を書き換えるとあった。`kernel/sys_proc.c:46` の `sys_arch_prctl` で `wrmsr(MSR_FS_BASE, addr)` が呼ばれていることを確認したい」

AI は本書本文をテキストとしては読めないので、章番号と節番号、登場するシンボル名を添えて質問してください。
