# 第7章 段階的ヒント — プロセス・スケジューラ・コンテキストスイッチ

本章は **「マルチタスクの根幹」「第6章 (syscall) と第4章 (メモリ) の交差点」** が一気に登場します。手が止まりがちな箇所だけ Lv1 → Lv2 → Lv3 の順に開示してください。**まず本書本文を 7.1 〜 7.10 まで通読**してから戻ることを強く推奨します。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、答えそのものは出していません。

---

## クラスタA — task と process、struct task の規模感 (本書 7.1, 7.2)

`struct task` のフィールド一覧を初見で全部覚えようとすると挫折します。**「実行を再開するために必要な情報」というレンズ**で見ると整理できます。

### Lv1 — 最初に確認すること
- `include/task.h` で `struct task` の宣言を確認 (`grep -A 30 "struct task {" ../include/task.h`)。
- フィールドを「(1) CPU/メモリ復元用」「(2) Unix の親子・グループ関係」「(3) 外界とのインターフェース (fd/cwd)」の 3 グループに分けてみる。

### Lv2 — それでも進まない場合
- (1) `kstack_top`, `os_stack_ptr`, `ctx`: コンテキストスイッチで必要。`ctx.cr3` がページテーブル、`ctx` の中の `rsp` がスタック。
- (2) `pid`, `ppid`, `pgid`, `sid`: Unix の系譜と権限グループ。第8章 (`fork`/`wait`) で活躍。
- (3) `fds[MAX_FDS]`, `cwd`: 第9章 (VFS) で活躍。第7章では「あるが触らない」程度の理解で OK。
- `heap_break`, `mmap_end` は第4章の `sys_brk`/`sys_mmap` が更新するフィールド。task ごとに別のヒープ範囲を持つ。

### Lv3 — 典型的な誤解
- 「Linux の `task_struct` と比べて Orthox-64 は機能不足」は誤り。**「教育用に最小化」ではなく「実用最小限」**。本書 7.1 が明示しているように、Python や GCC を動かすのに必要な要素は全部入っている。
- 「`struct task` が小さいので性能が悪い」は誤り。むしろ小さいゆえに L1 キャッシュ効率が良い。

---

## クラスタB — `struct cpu_local` と `%gs:0` / `%gs:8` (本書 7.3)

`struct cpu_local` の先頭 2 フィールドが `kernel_stack` (offset 0)、`user_stack` (offset 8) であることは、**第6章の `swapgs` トリックと不可分**です。

### Lv1 — 最初に確認すること
- `include/task.h:24` 付近で `struct cpu_local` 宣言を読む。先頭フィールドが `kernel_stack`、その次が `user_stack` であることを確認。
- `kernel/syscall_entry.S` で `%gs:0` と `%gs:8` がどう使われているかを再確認 (第6章クラスタB と同じ)。

### Lv2 — それでも進まない場合
- C 構造体のフィールド宣言順とメモリオフセットは一対一対応 (パディングを除く)。なので **「`kernel_stack` を offset 0 に置く」というのは、宣言順で先頭に書く、ということ**。
- このオフセットを動かすと `syscall_entry.S` が壊れる。**`struct cpu_local` の先頭 2 フィールドを並べ替えない**のは暗黙の契約。
- AI に `struct cpu_local` の改造を提案させた場合、先頭フィールドを動かす案が出てきたら即拒否。アセンブリと整合が壊れる。
- `self` フィールド (3 番目) は「`%gs:16` で自分自身のポインタを取得する」ためのトリック。C コードから `task_this_cpu()` で取り出される。

### Lv3 — 典型的な誤解
- 「`cpu_local` は per-task の構造体」は誤り。**per-CPU の構造体**。各 CPU コアが 1 つずつ持つ。
- 「`cpu_local->current_task` が常に最新」は正しいが、書き換えタイミングは `schedule()` 内のみ。割り込みハンドラ等から雑に書き換えるとレースが起きる。

---

## クラスタC — PML4 上位半分のインライン for ループ (本書 7.4)

新しいタスクを作るとき、PML4 の **上位半分 (インデックス 256 以上、カーネル空間)** をカーネルの PML4 からコピーします。これは **専用関数ではなくインライン for ループ**で行われています。

### Lv1 — 最初に確認すること
- `kernel/task.c` で `task_create_on_cpu` を読み、本書 7.4 のコード片と一致する `for (int i = 0; i < 512; i++)` ループを探す (`grep -A 3 "i >= 256" ../kernel/task.c`)。
- `kernel/task_exec.c` の `task_execve` でも同じパターンが繰り返されている可能性を確認。

### Lv2 — それでも進まない場合
- なぜインライン化か: (1) コード量が 3 行で済む、(2) 専用関数にすると引数渡しのオーバーヘッドが入る、(3) ロックの境界がはっきりする (関数内に閉じ込めると外側で見えにくくなる)。
- PML4 は 512 エントリ。インデックス 0〜255 がユーザー空間 (各タスク独自)、256〜511 がカーネル空間 (全タスク共有)。Orthox-64 では上位半分を**ポインタ共有ではなく値コピー**にしている。値コピーで十分なのは、カーネル空間の PML4 エントリが起動後ほとんど変わらないため。
- カーネル空間の追加マッピングが必要になった場合 (例: 新規 USB バッファ)、全タスクの PML4 を巡回更新する必要がある。これが教育用 OS の素朴さの代償。

### Lv3 — 典型的な誤解
- AI が「カーネル空間のコピーは `vmm_setup_kernel_mappings` 関数で行う」と説明したら、**その関数は存在しません**。`grep -n vmm_setup_kernel_mappings ../kernel/` で空であることを確認させる。
- 「PML4 全体を共有すれば良い」は誤り。**ユーザー空間 (下位半分) はタスクごとに別マッピング** (本書 4.7 と図 4.4)。下位を共有するとプロセス分離が壊れる。

---

## クラスタD — `schedule()` の儀式 (本書 7.7)

`schedule()` は単にタスクを切り替える以上の **「移行のための準備儀式」** を行います: TSS 更新、FS base 書き換え、ジャイアントロック取得。

### Lv1 — 最初に確認すること
- `kernel/sched.c:45` 付近の `schedule()` 本体を読む。
- `task_lock_irqsave` (line 54 付近)、`tss_set_stack_for_cpu` (line 75 付近)、`task_refresh_cpu_local_msrs_internal` (line 77 付近)、`switch_context` (line 80 付近) の 4 つの呼び出しの**順序**を意識する。

### Lv2 — それでも進まない場合
- **`task_lock_irqsave` (BKL)**: Orthox-64 は **`sched_lock` という細粒度ロックを持たず**、`task_lock_irqsave` というジャイアントロック (BKL: Big Kernel Lock) で全タスク操作をシリアライズする。`task_lock_irqsave()` は割り込み禁止 + ロック取得を 1 関数で行う。
- **TSS 更新の意味**: 次にユーザーから割り込み or syscall でカーネルに入る際、CPU は**ハードウェアの TSS (Task State Segment) からカーネルスタックアドレスを自動取得**する。タスク切替時にこれを次タスク用に書き換えないと、次に割り込みが入ったとき古いタスクのスタックを掴んでクラッシュ。
- **FS base 書き換え**: `%fs` レジスタのベースアドレスは Thread Local Storage (TLS) の起点。musl libc / Python 等が `errno` を含むスレッド固有データをここに置く。タスクごとに `user_fs_base` を保持し、切替時に MSR (`MSR_FS_BASE` または `arch_prctl(ARCH_SET_FS)`) で書き換える。

### Lv3 — 典型的な誤解
- AI が「Orthox-64 は `sched_lock` というスピンロックで…」と一般的なロック名で説明したら、**`sched_lock` は存在しない**ことを `grep -n sched_lock ../kernel/` で確認させる。実際は `task_lock_irqsave/restore` の BKL。
- 「FS base の書き換えはユーザー空間でやる」は誤り。`arch_prctl(ARCH_SET_FS)` 経由でユーザーが**要求**するが、書き換え自体はカーネルが MSR 経由で実行。タスク切替時はカーネルが自動で行う。
- 「TSS は x86_64 では使われない (32-bit 時代の遺物)」は誤り。**特権レベル移行時のカーネルスタック取得**には今も TSS が使われている。タスクスイッチには使われない (`switch_context` がアセンブリでやる) が、TSS の `RSP0` だけは現役。

---

## クラスタE — `switch_context` の手品 (本書 7.8)

`switch_context` は OS で最も美しい手品の 1 つ。**「CR3 書き換え + RSP 書き換え + ret で次タスクの世界に着地する」** のフローを頭の中で再生できれば本章は卒業です。

### Lv1 — 最初に確認すること
- `kernel/task_switch.S` 全文を読む。50 行程度のアセンブリ。
- `mov %rax, %cr3` (CR3 書き換え) と `mov 112(%rdi), %rsp` (RSP 書き換え) の 2 行を**並べて見る**。

### Lv2 — それでも進まない場合
- `cr3` を書き換えた瞬間、CPU から見たメモリの景色が次タスクのものに切り替わる。ただし、**現在実行中のコード自体は両タスクの PML4 で同じ物理ページにマップされている** (カーネル空間は共有) ので、命令ストリームは継続する。
- `rsp` を書き換えた瞬間、関数の戻り先・ローカル変数すべてが次タスクのものになる。スタックの **トップ (rsp が指す位置) に何が積まれているか** が次の行先を決める。
- `ret` 命令はスタックトップから戻り先アドレスをポップしてジャンプ。**次タスクが過去に `switch_context` を呼んだ直後のアドレスがそこに積まれている** ので、次タスクが目覚める。
- 結果として「`switch_context` を呼んだら、関数が return したときには別タスクの世界にいる」という不思議な挙動になる。

### Lv3 — 典型的な誤解
- AI が「`switch_context` は呼び出し元に戻ってから次タスクが実行される」と説明したら誤り。**`ret` した先が既に次タスクの世界**。
- `andq $~0x100, (%rsp)` (TF クリア) を AI が省略したコードを書いたら指摘。TF (Trap Flag) が残っているとシングルステップ例外が出続けてカーネルが固まる。
- 「`switch_context` はカーネルモードで動くからユーザーレジスタは保存不要」は誤り。**ユーザーレジスタは syscall/interrupt の入口で `syscall_frame`/`interrupt_frame` に保存済み**、`switch_context` が触るのは callee-saved レジスタ (rbx, rbp, r12-r15) のみ。

---

## クラスタF — idle task と `hlt` (本書 7.9)

すべてのタスクが寝てしまったときに動く idle task は単純ですが、**`hlt` 命令を入れるか入れないかで、自作 OS が「実用」になるかが決まります**。

### Lv1 — 最初に確認すること
- `kernel/sched.c:114` 付近の `task_idle_loop` を読む。`while(1)` の中で `hlt` 命令と `kernel_yield` が呼ばれている。
- `task_consume_resched()` が何を返すか確認 (グローバル `resched_pending` フラグ消費)。

### Lv2 — それでも進まない場合
- `hlt` は「次の割り込みが入るまで CPU を停止」する命令。これがないと idle ループが CPU を 100% 消費し、QEMU で動かしている場合はホスト側のファンが回り始める。
- 各 CPU コアが**自分専用の idle_task** を持つ (本書 7.9)。`cpu_local->idle_task`。SMP では各コアが独立して `hlt` する。
- `task_wakeup` がイベント発生時に呼ばれ、寝ているタスクを READY に戻し `resched_pending` を立てる。次のタイマ割り込みで `schedule()` が走り、idle から実タスクへ切り替わる。

### Lv3 — 典型的な誤解
- 「idle task は OS 設計の枝葉」は誤り。**idle task と `hlt` がないと QEMU が動かない (重すぎて)**。本書 7.9 が「実用プラットフォームになるための必須作法」と明記。
- AI が「idle task は per-OS で 1 つ」と説明したら誤り。**per-CPU で各 1 つ**。

---

## AI への聞き方のコツ

本書 7.11「AI アシスタント」節の質問例をそのまま使うのが基本ですが、以下の追加ルールを守ると精度が上がります。

- **`sched_lock` は無い、`task_lock_irqsave` を使う**: AI が一般的な細粒度ロックを前提に解説したら、Orthox-64 は BKL であることを `grep -n sched_lock ../kernel/` で示す。
- **`vmm_setup_kernel_mappings` は無い、インライン for ループ**: 第4章 hints クラスタE と同じ注意点を再強調。
- **`cpu_local` の先頭フィールド順は固定**: AI に構造体改造を頼むと壊れがち。先頭 `kernel_stack` / `user_stack` は動かさない。
- **`switch_context` の TF クリア**: AI のアセンブリ生成で `andq $~0x100, (%rsp)` が抜けたら必ず補わせる。

---

## 関連

- 本章本文: 第7章 (読者の手元書籍)
- 実コード入口: `../kernel/task.c`, `../kernel/sched.c`, `../kernel/task_switch.S`, `../include/task.h`
- 関連章: 第4章 (CR3/PML4 と COW), 第6章 (`cpu_local` の先頭 2 フィールドと `swapgs`), 第8章 (`fork`/`exec`/`wait` でタスク生成・破棄の本番)
