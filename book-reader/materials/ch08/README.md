# Ch8 — fork / exec / wait (輪廻転生するプロセスの美学)

第8章は Unix の三位一体システムコール `fork` / `execve` / `wait4` を扱う、第7章の直接の続編です。第4章の COW (Copy-on-Write) が**実際に発動する**章であり、第14章 (ELF/動的リンク) への入口でもあります。

## 章のゴール

* **`fork` がプロセスの全資産 (fd table, cwd, ヒープ範囲) を子に引き継ぐ**仕組みを `task_fork.c` で確認できる。
* **COW** が `vmm_copy_pml4` で **PTE_WRITABLE をクリアして PTE_COW を立てる** ことで成立し、書き込み時のページフォルトで物理コピーが発動する流れを追える。
* **`fork` が「1 回呼んで戻り値が 2 回返る」謎**を、子の `syscall_frame->rax` を強制的に `0` に書き換えるトリックで解決できる。
* **`execve` の「ユーザー引数退避 → 旧アドレス空間破棄 → 新アドレス空間構築」の順序**が必須であることを理解し、AI が順番を入れ替えた実装を出してきたら指摘できる。
* **`exit` 後に `wait4` が来るまでゾンビ状態で残る**理由を、親が終了ステータスを回収する権利と結びつけて説明できる。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (8.3 〜 8.9) に対応した6つのクラスタに分かれています。

## 取り組み方の推奨フロー

1. 本書 8.1 〜 8.10 を**通読**してから戻る。図 8.1 (ライフサイクル) と図 8.2 (COW) を頭の中で描けるか確認。
2. `kernel/task_fork.c:26` 付近の `task_fork` を読み、本書 8.3 のコード片と diff。
3. `kernel/vmm.c` の `vmm_copy_pml4` を再読 (第4章 hints クラスタE)、`PTE_COW` の立て方を確認。
4. `kernel/task_exec.c:321` 付近の `task_execve` を読み、本書 8.6, 8.7 と diff。引数退避 → PML4 作成 → ELF ロード → スタック準備 の順序。
5. `kernel/sys_proc.c:121` 付近の `sys_exit` と `:138` の `sys_wait4` を読み、ゾンビと reap の関係を確認。
6. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# fork
grep -n "task_fork\|fs_clone_fd" ../kernel/task_fork.c

# COW のページテーブル側
grep -n "vmm_copy_pml4\|PTE_COW\|vmm_page_fault_handler" ../kernel/vmm.c

# execve と ELF ロード
grep -n "task_execve\|elf_load\|copy_user_string_vector\|task_prepare_initial_user_stack" ../kernel/task_exec.c

# exit / wait
grep -n "sys_exit\|sys_wait4\|task_mark_zombie\|task_reap\|task_signal_add" ../kernel/sys_proc.c
```

## AI への話しかけ方の例

* 「第8章 8.4 で `fork` 時に親子のページを共有して書き込み時に複製とあった。`kernel/vmm.c` の `vmm_copy_pml4` で PTE_WRITABLE をクリアし PTE_COW を立てる行と、`vmm_page_fault_handler` で COW を判定する行を grep で示して」
* 「第8章 8.5 で子の戻り値だけ 0 に書き換える仕組みがあった。`kernel/task_fork.c` の `child_frame->rax = 0;` 相当の行を見せて、本書のコード片と一致するか確認したい」
* 「第8章 8.7 で `task_execve` がユーザー引数を退避してから新アドレス空間を作るとあった。`kernel/task_exec.c` の `copy_user_string_vector` 呼び出しが、新 PML4 確保より前に来ていることを行番号で示して」

AI は本書本文をテキストとしては読めないので、章番号と節番号、登場するシンボル名を添えて質問してください。
