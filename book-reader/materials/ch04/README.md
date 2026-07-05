# Ch4 — メモリ管理 (PMM / VMM / ページテーブル) を頭の中で迷子にならない

第4章は「物理メモリ」「仮想メモリ」「4段ページテーブル」「Copy-on-Write」が一気に登場する、本書最大の難所の1つです。本文はコード片を交えて進みますが、頭の中で「いま自分が触っているのは物理アドレスか仮想アドレスか」を見失った瞬間に迷子になります。

## 章のゴール

* **PMM が物理ページフレームを切り出すだけ** で、仮想アドレスは何も知らない、という分業を体感する。
* **HHDM (Higher Half Direct Map)** が「物理アドレスを uint64_t* として直接編集する」ための窓口だと納得する。
* **`vmm_map_page()` が PML4 → PDP → PD → PT を掘る** 過程と、中間テーブルが動的に確保される仕組みを追える。
* **`PTE_COW` が x86_64 ハードウェアビットではなく、Orthox-64 がソフトウェア用領域 (bit 9) に定義した独自フラグ** であることを区別できる。
* **`vmm_page_fault_handler()` が CR2 を読んで COW を解決する流れ** を、本書の説明と実コードの両方で確認できる。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (4.2 〜 4.8) に対応した6つのクラスタに分かれています。詰まった所だけ参照してください。

## 取り組み方の推奨フロー

1. 本書 4.1 〜 4.9 を**通読**してから戻る。図 4.1 〜 4.5 を頭の中で繰り返し描けるか確認。
2. `include/vmm.h` と `include/pmm.h` を眺め、表 4.1 の用語と実シンボルを突き合わせる。
3. `kernel/vmm.c` の `vmm_map_page` (around line 144) と `get_next_level` (around line 99) を読み、本書 4.5 の概念コードと実コードを diff。
4. `vmm_copy_pml4` (around line 209) と `vmm_page_fault_handler` (around line 371) を読み、COW の往復を追う。
5. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# PMM / VMM 本体
grep -n "vmm_map_page\|get_next_level\|vmm_copy_pml4\|vmm_page_fault_handler" ../kernel/vmm.c

# PTE フラグ定義
grep -n "PTE_" ../include/vmm.h

# sys_brk / sys_mmap の入口
grep -n "sys_brk\|sys_mmap" ../kernel/sys_vm.c
```

第4章は「読みながら写経すれば動くもの」ではなく **「実コードを読み解くための地図」** です。手を動かす対象は QEMU 上での観察 (`info mem` 等) や、自分の手元紙への図描きです。

## AI への話しかけ方の例

* 「第4章 4.5 で PML4 / PDP / PD / PT のビットフィールド分割が出てきた。`kernel/vmm.c` の `PML4_IDX` 等のマクロ定義を `grep -n` で見せて、本書の説明と一致しているか確認したい」
* 「第4章 4.7 で `PTE_COW` という独自フラグが説明されている。`include/vmm.h` での定義と、`kernel/vmm.c` の `vmm_page_fault_handler` で実際にこれを判定している箇所を grep して、ソフトビット (bit 9) であることを確認したい」
* 「第4章 4.8 で `sys_brk` と `sys_mmap` がどちらも最終的に `pmm_alloc + vmm_map_page` に降りるとあった。`kernel/sys_vm.c` で本当にそうなっているか辿りたい」

AI は本書本文をテキストとしては読めないので、**章番号と節番号、登場するシンボル名**を添えて質問すると、対応する hints.md と実コードを正確に引いてくれます。
