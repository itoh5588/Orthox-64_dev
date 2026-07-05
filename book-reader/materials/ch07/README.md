# Ch7 — プロセスとスケジューラ (複数の生命を宿すための心臓)

第7章は `struct task` と `struct cpu_local` の二大構造体、コンテキストスイッチの「手品」、ラウンドロビン・スケジューラ、idle task と `hlt` 命令——マルチタスク OS の基幹が一度に登場する章です。第6章 (syscall entry) と密接に絡む `cpu_local` 構造体、第4章 (メモリ管理) と密接に絡む PML4 / CR3 の取り扱いを再確認する場でもあります。

## 章のゴール

* `struct task` が「実行を再開するために必要なすべての情報を持つ台帳」であることを、フィールド単位で説明できる。
* `struct cpu_local` の **先頭 8 バイトに `kernel_stack`、次 8 バイトに `user_stack`** が並ぶ理由を、第6章の `swapgs` トリックと結びつけて思い出せる。
* `task_create_on_cpu` 内で **PML4 の上位半分をインライン for ループでコピー** している (専用関数ではない) ことに気付く。
* `schedule()` が呼ぶ「儀式」 — TSS 更新、FS base 書き換え、`switch_context` — の各意味を区別できる。
* `switch_context` がアセンブリで実行する「CR3 書き換え + RSP 書き換え + ret で別タスクの世界へワープ」の手品を、頭の中で再生できる。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (7.2 〜 7.9) に対応した6つのクラスタに分かれています。

## 取り組み方の推奨フロー

1. 本書 7.1 〜 7.10 を**通読**してから戻る。図 7.1 を頭の中で描けるか確認。
2. `include/task.h` の `struct task` と `struct cpu_local` の定義を眺め、本書 7.2 と 7.3 のコード片と一致を確認。
3. `kernel/task.c` の `task_create_on_cpu` を読み、本書 7.4 と diff。
4. `kernel/sched.c` の `schedule` (line 45 付近) を読み、本書 7.7 の流れを 1 行ずつ照合。
5. `kernel/task_switch.S` の `switch_context` を読み、本書 7.8 の「ret で別世界へワープ」を確認。
6. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# struct task と struct cpu_local
grep -n "struct task\|struct cpu_local" ../include/task.h

# task 作成
grep -n "task_create_on_cpu\|alloc_task_struct" ../kernel/task.c

# スケジューラ本体
grep -n "schedule\|task_lock_irqsave\|tss_set_stack" ../kernel/sched.c

# コンテキストスイッチ
cat ../kernel/task_switch.S

# idle task
grep -n "task_idle_loop\|hlt" ../kernel/sched.c
```

## AI への話しかけ方の例

* 「第7章 7.3 で `struct cpu_local` の先頭が `kernel_stack`、次が `user_stack` とあった。これは第6章 `swapgs` トリックで `%gs:0` と `%gs:8` で読まれる構造と一致している。`include/task.h` の宣言を grep で見せて、フィールド順がこのオフセットと矛盾しないか確認したい」
* 「第7章 7.4 で『PML4 の上位半分をインライン for ループでコピー』と本文にあった。`task_create_on_cpu` の該当行を `grep -A 5 'i >= 256'` のような形で示して、`vmm_setup_kernel_mappings` のような専用関数ではないことを確認したい」
* 「第7章 7.8 で `switch_context` の最後の `andq $~0x100, (%rsp)` でトラップフラグをクリアしている。`kernel/task_switch.S` で本当にこの行があるか、ない実装と比較した場合に何が起こるか説明して」

AI は本書本文をテキストとしては読めないので、章番号と節番号、登場するシンボル名を添えて質問してください。
