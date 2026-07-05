# Ch5 — 割り込み・タイマ・SMP (CPU の実行権を掌握する低レイヤの歩哨)

第5章は **IDT・割り込み入口アセンブリ・LAPIC タイマ・SMP・IPI** が一気に登場する低レイヤの一大難所です。第6章 (syscall entry) と並んで「CPU が kernel に飛び込む 2 つの入口」のうちの片方 (割り込み経路) を扱います。

## 章のゴール

* **IDT** が「ベクトル番号 → カーネル関数アドレス」の単純な変換表であることを掴む。
* **`interrupt.S` の PUSH_ALL マクロ** が汎用レジスタ 15 本を退避し、`struct interrupt_frame` を組み立てる仕組みを理解する。
* **LAPIC タイマ周波数のキャリブレーション** が、起動時に PIT (8254) を使って動的に行われていることを知る。
* **遅延スケジューリング** (`resched_pending` フラグを立てるだけで、安全な境界で `kernel_yield`) の存在意義を、「割り込みコンテキスト中にコンテキストスイッチすると壊れる」理由と結びつけて理解する。
* **SMP / IPI** が「BSP が AP を起こす + コア間で割り込みを送り合う」だけのシンプルな構造で、複雑な調停機構ではないことを掴む。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (5.2 〜 5.8) に対応した6つのクラスタに分かれています。

## 取り組み方の推奨フロー

1. 本書 5.1 〜 5.9 を**通読**してから戻る。図 5.1 〜 5.5 を頭の中で描けるか確認。
2. `kernel/idt.c` の `idt_set_gate` (line 96) と `interrupt_dispatch` (line 186) を読む。
3. `kernel/interrupt.S` で `PUSH_ALL` / `POP_ALL` マクロの中身を確認 — **汎用レジスタのみ**で FPU/XMM は含まれない。
4. `kernel/lapic.c` の `pit_prepare_sleep` (line 99) と `PIT_COUNT_FOR_TICK` 使用箇所 (line 120) を確認。
5. `kernel/sched.c` の `task_request_resched` (line 23) と `task_request_resched_cpu` (line 28) を読み、IPI を `kernel/smp.c:230` の `smp_send_resched_ipi` で確認。
6. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# IDT 設定とディスパッチ
grep -n "idt_set_gate\|interrupt_dispatch\|INT_VECTOR_" ../kernel/idt.c

# 割り込み共通入口アセンブリ
cat ../kernel/interrupt.S

# LAPIC タイマとキャリブレーション
grep -n "pit_prepare_sleep\|PIT_COUNT_FOR_TICK\|lapic_timer_init\|lapic_eoi" ../kernel/lapic.c

# SMP と IPI
grep -n "smp_ap_entry\|smp_send_resched_ipi" ../kernel/smp.c

# 遅延スケジューリング
grep -n "task_request_resched\|task_consume_resched\|resched_pending" ../kernel/sched.c ../include/task.h
```

## AI への話しかけ方の例

* 「第5章 5.5 で LAPIC タイマのキャリブレーションに PIT を使うとあった。`kernel/lapic.c:99` 付近の `pit_prepare_sleep` と `PIT_COUNT_FOR_TICK` の使われ方を grep で示して、本書の説明と一致しているか確認したい」
* 「第5章 5.6 で「割り込みコンテキスト中に直接コンテキストスイッチしない」とあった。`kernel/sched.c:23` 付近の `task_request_resched` が単にフラグを立てるだけであることを確認し、なぜ即時 `switch_context` を呼ばないか理由を整理して」
* 「第5章 5.8 で IPI 経由のリスケジュール要求が登場した。`kernel/smp.c:230` 付近の `smp_send_resched_ipi` が LAPIC の ICR にどう書き込んでいるか grep で示して」

AI は本書本文をテキストとしては読めないので、章番号と節番号、登場するシンボル名を添えて質問してください。
