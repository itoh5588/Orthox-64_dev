# 第5章 段階的ヒント — 割り込み・タイマ・SMP

本章は **「ハードウェア仕様 (IDT/LAPIC/IPI) と Orthox-64 の遅延スケジューリング設計」が交差する密度の高い章** です。手が止まりがちな箇所だけ Lv1 → Lv2 → Lv3 の順に開示してください。**まず本書本文を 5.1 〜 5.9 まで通読**してから戻ることを強く推奨します。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、答えそのものは出していません。

---

## クラスタA — IDT の本質 (本書 5.2)

IDT は **「ベクトル番号 (0〜255) → カーネル関数アドレス」を引くだけの 256 個の配列**です。複雑そうに見えるのはエントリ構造体のビットフィールドだけ。

### Lv1 — 最初に確認すること
- `kernel/idt.c:96` 付近の `idt_set_gate` を読む。引数は (ベクトル番号, ハンドラ関数ポインタ, IST インデックス, ゲート種別) の 4 つ。
- `idt_set_gate(14, isr14, 1, IDT_GATE_INTERRUPT);` などの呼び出し箇所を grep。ベクトル 14 がページフォルト。

### Lv2 — それでも進まない場合
- IDT は CPU 内部の **IDTR レジスタ**に「IDT のアドレスとサイズ」を `lidt` 命令で教える。これが終わると CPU は自動的に IDT を参照する。
- **割り込みゲート (`IDT_GATE_INTERRUPT`)** と **トラップゲート (`IDT_GATE_TRAP`)** の違い: 前者は突入時に **割り込み禁止 (IF=0)** にする、後者は割り込みを許可したまま入る。例外用には割り込みゲートが推奨。
- **IST (Interrupt Stack Table)**: 重大な例外 (NMI, ダブルフォルト, ページフォルト等) で「現在のスタックではなく専用のスタックに切り替える」仕組み。`ist` 引数で 1〜7 を指定すると、TSS の `IST[n]` に登録された専用スタックが使われる。

### Lv3 — 典型的な誤解
- 「IDT のエントリは関数ポインタを直接持つ」と AI が言ったら誤り。**16 ビットずつ 3 つに分けて格納** (`offset_low`, `offset_mid`, `offset_high`)。historical reasons (16-bit / 32-bit / 64-bit 互換性) でこうなっている。
- 「すべての割り込みが必ず PUSH_ALL される」は誤り。**CPU が自動退避するのは `ss/rsp/rflags/cs/rip` の 5 つだけ**。汎用レジスタは `interrupt.S` の PUSH_ALL で明示的に退避する。

---

## クラスタB — `interrupt.S` の PUSH_ALL (本書 5.3)

`PUSH_ALL` マクロは **汎用レジスタ 15 本 (rax 〜 r15) を順次 push** し、第6章の `syscall_frame` と同じ要領で `struct interrupt_frame` をスタック上に組み立てます。

### Lv1 — 最初に確認すること
- `kernel/interrupt.S` を全文読む。`.macro PUSH_ALL` と `.macro POP_ALL` の中身。
- `include/idt.h` か周辺で `struct interrupt_frame` の定義を grep。

### Lv2 — それでも進まない場合
- PUSH_ALL の中身は `pushq %rax`、`pushq %rcx`、... を 15 個並べただけ。順序が `struct interrupt_frame` のフィールド宣言と**逆順で対応**する (第6章クラスタC と同じ理由)。
- C 側で `interrupt_dispatch(struct interrupt_frame* frame)` を呼ぶときに `%rsp` を第1引数に渡しているのは、`%rsp` がスタックトップ (PUSH_ALL 直後で最後に push した値) を指しているため、これを `struct interrupt_frame*` にキャストすると先頭フィールドが一致するから。
- **XMM/FPU は PUSH_ALL に含まれない**。割り込みハンドラ内で FPU を使うとユーザーの FPU 状態が破壊される。Orthox-64 では割り込みハンドラから FPU 命令を呼ばないルールで運用。

### Lv3 — 典型的な誤解
- AI が「PUSH_ALL は AVX/SSE レジスタも退避する」と説明したら、`.macro PUSH_ALL` の中身を実際に見せて訂正させる。XMM レジスタ退避には `fxsave` / `fxrstor` が必要で、`switch_context` (第7章) でのみ実行される。
- 「`interrupt.S` の入口が syscall も処理する」は誤り。**割り込み (IDT 経由) と syscall (MSR_LSTAR 経由) は完全に別の入口**。`syscall_entry.S` と `interrupt.S` は別ファイル。

---

## クラスタC — LAPIC タイマとキャリブレーション (本書 5.5)

LAPIC タイマの周波数は CPU 製品ごとに異なるため、起動時に **PIT (8254) を使って「10 ms に何カウント減るか」を測定**します。これが「物理的なハードウェア仕様の違いを吸収するキャリブレーション」です。

### Lv1 — 最初に確認すること
- `kernel/lapic.c:99` 付近の `pit_prepare_sleep` を読む。PIT (port 0x40, 0x43) への OUT 命令で 1 ショットモード設定。
- `kernel/lapic.c:120` 付近の `pit_prepare_sleep(PIT_COUNT_FOR_TICK)` 呼び出しで、何 ms 待つかを決めている定数。

### Lv2 — それでも進まない場合
- PIT は世界共通で 1.193182 MHz で動作する古典タイマ。`PIT_COUNT_FOR_TICK = 11932` 等の値が「約 10 ms」を表す。
- 待つ間 LAPIC タイマカウンタを観察すれば「10 ms に何カウント減るか」が分かる → これを `ticks_per_interval` として `lapic_timer_init` に渡す。
- 周波数測定後、LAPIC を **Periodic モード** で設定すれば、指定した周期で自動的にタイマ割り込み (Vector 32) を発火し続ける。

### Lv3 — 典型的な誤解
- AI が「`pit_prepare` という関数で…」と省略形を使ったら、**実コードは `pit_prepare_sleep`** (`kernel/lapic.c:99`) を grep で示す。
- 「キャリブレーション時間は常に 10 ms」は誤り。`PIT_COUNT_FOR_TICK` の値次第。コード更新で変わる可能性があるので AI が断言したら定数定義を確認させる。
- 「LAPIC タイマは全 CPU で同じ周波数」は正しい (同じ製品なら) が、AI が「カーネルが手動で同期している」と説明したら過剰。CPU 製造時点で周波数は一意。

---

## クラスタD — `interrupt_dispatch` の交通整理 (本書 5.4)

`interrupt_dispatch` は **「if-else 連鎖でベクトル番号を判定し、専用ハンドラへ振り分ける」** だけのシンプル設計。switch ではなく if 連鎖なのが Orthox-64 の特徴。

### Lv1 — 最初に確認すること
- `kernel/idt.c:186` 付近の `interrupt_dispatch` 本体を読む。
- 各 `if (frame->int_no == INT_VECTOR_xxx)` 分岐の中で `lapic_eoi()` または `pic_eoi(irq)` が呼ばれていることを確認。

### Lv2 — それでも進まない場合
- **`lapic_eoi()` は LAPIC 経由の割り込み (タイマ、IPI 等) で必須**。これを呼ばないと CPU が「割り込み処理中」と判定し続け、同優先度以下の割り込みを受け付けなくなる。
- **`pic_eoi(irq)` はレガシー PIC 経由の割り込み (キーボード、シリアル等) で必須**。LAPIC とは別系統。
- タイマ割り込みハンドラ内で `net_poll()` が呼ばれているのは Orthox-64 の素朴な設計判断 (本書 5.4 の注釈)。ネットワークの専用 IRQ を取らず、タイマ起点でポーリングする。

### Lv3 — 典型的な誤解
- AI が「割り込みハンドラ内で `vmm_page_fault_handler` のような関数を直接呼んでよい」と説明するのは正しいが、**`kernel_lock_enter() / kernel_lock_exit()` のペアでロックを取らないと SMP で壊れる**。`interrupt_dispatch` 内のすべての分岐がロックを取っていることを確認させる。
- 「EOI は割り込みハンドラの先頭で呼ぶべき」は誤り。**ハンドラ末尾で呼ぶ**。先頭で呼ぶと処理途中で同じ割り込みが多重発火する。

---

## クラスタE — 遅延スケジューリング (本書 5.6)

「タイマ割り込みハンドラ内で直接 `switch_context` を呼ばない」のは **割り込みコンテキストとタスクコンテキストでスタックの意味が違うから**。`resched_pending` フラグを立てて、安全な境界で改めて `kernel_yield` を呼びます。

### Lv1 — 最初に確認すること
- `kernel/sched.c:23` 付近の `task_request_resched` を読む。`cpu->resched_pending = 1;` を立てるだけの短い関数。
- `kernel/sched.c` 周辺で `task_consume_resched` を grep。`resched_pending` を読み取って消費する関数。
- `kernel/sched.c:114` 付近の `task_idle_loop` (第7章でも参照) で、`task_consume_resched` の戻り値で `kernel_yield` を呼ぶフロー。

### Lv2 — それでも進まない場合
- 割り込み中は **CPU が `interrupt_frame` をスタックに積んでいる繊細な状態**。ここで `switch_context` を呼ぶと、別タスクのスタックに切り替わった後で `iretq` を実行することになり、フレーム整合性が壊れてトリプルフォルト。
- 「安全な境界」とは: (1) システムコールハンドラ終了時にユーザー空間に戻る直前、(2) カーネル内で明示的に `kernel_yield()` が呼ばれた時、(3) `task_idle_loop` の `hlt` 復帰直後。これらの地点ではスタックが「タスクの kernel stack」に戻っており、安全に切替可能。
- この設計は **「割り込みコンテキスト」と「プロセスコンテキスト」の区別**という Unix 系 OS 共通の概念。Linux でも同じ (実装は `softirq` / `tasklet` で複雑化しているが思想は同じ)。

### Lv3 — 典型的な誤解
- AI が「タイマ割り込みハンドラの最後で直接 `schedule()` を呼んでよい」と言ったら危険。**Orthox-64 では絶対にやらない**。`task_request_resched` でフラグを立てるのみ。
- 「`resched_pending` はグローバル変数」は誤り。**per-CPU の `cpu_local->resched_pending`**。SMP で各コア独立に管理される。

---

## クラスタF — SMP と IPI (本書 5.7, 5.8)

SMP は「BSP が AP を Limine 経由で起こす」「コア間で `INT_VECTOR_RESCHED` を送り合う」だけの構造。複雑な調停機構はありません。

### Lv1 — 最初に確認すること
- `kernel/smp.c:136` 付近の `smp_ap_entry` を読む。各 AP が起動時に通る共通エントリ。
- `kernel/smp.c:230` 付近の `smp_send_resched_ipi` を読む。LAPIC の **ICR (Interrupt Command Register, offset 0x300)** にベクトル番号と送信先 APIC ID を書き込む。

### Lv2 — それでも進まない場合
- BSP = Bootstrap Processor (起動時の第1コア)。AP = Application Processor (それ以外のコア)。Limine が `mp_request` で「合計 N 個の CPU がある」「各 CPU の APIC ID は X, Y, Z」を教えてくれる。
- AP は起動直後に各自で `lidt` (IDT ロード)、`syscall_init_cpu` (MSR 設定)、`lapic_timer_init` (LAPIC タイマ初期化) を実行する必要がある。これらは **per-CPU の MSR / レジスタを触る**ので BSP が代行できない。
- IPI は「相手コアに割り込みを送る」だけ。受け取った相手は通常の `interrupt_dispatch` 経路で処理する (`INT_VECTOR_RESCHED` に対応する分岐は `lapic_eoi()` を呼ぶだけ)。`resched_pending` の方は `task_request_resched_cpu` (`kernel/sched.c:28`) で先に立てておく。

### Lv3 — 典型的な誤解
- AI が「`smp_send_resched_ipi` は割り込みを受信側で同期的に処理させる」と説明したら誤り。**送信は非同期**。送った側は ICR に書いてすぐ抜ける。受信側はいずれ割り込みが入る (タイミングは LAPIC 任せ)。
- 「SMP では BSP が全 CPU の MSR を一括設定する」は誤り。**各 AP が自分の MSR を設定する**。

---

## AI への聞き方のコツ

本書 5.10「AI アシスタント」節の質問例をそのまま使うのが基本ですが、以下の追加ルールを守ると精度が上がります。

- **割り込み入口と syscall 入口は別ファイル**: `kernel/interrupt.S` と `kernel/syscall_entry.S` を混同したら指摘。
- **PUSH_ALL は汎用レジスタのみ**: XMM/FPU を含めた説明が来たら `.macro PUSH_ALL` を見せて訂正。
- **`pit_prepare_sleep` が正確な関数名**: 短縮形を出してきたら grep で訂正。
- **EOI を忘れない**: AI 生成のハンドラコードに `lapic_eoi()` / `pic_eoi()` が欠けていたら必ず指摘。
- **遅延スケジューリングを尊重**: 割り込みハンドラ内から `schedule()` 直接呼び出しは禁止。`task_request_resched` を使う。

---

## 関連

- 本章本文: 第5章 (読者の手元書籍)
- 実コード入口: `../kernel/idt.c`, `../kernel/interrupt.S`, `../kernel/lapic.c`, `../kernel/smp.c`, `../kernel/sched.c`
- 関連章: 第6章 (syscall entry — 割り込みとは別の入口), 第7章 (`switch_context` — 安全な境界での切替)
