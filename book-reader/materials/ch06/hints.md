# 第6章 段階的ヒント — syscall entry の境界アセンブリを読み解く

本章は **「ハードウェア仕様」と「Orthox-64 の独自設計」が交差する密度の高い章** です。手が止まりがちな箇所だけ Lv1 → Lv2 → Lv3 の順に開示してください。**まず本書本文を 6.1 〜 6.10 まで通読**してから戻ることを強く推奨します。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、答えそのものは出していません。

---

## クラスタA — MSR 4 本の役割分担 (本書 6.2)

`syscall_init_cpu` が触る MSR は **EFER / STAR / LSTAR / SFMASK の 4 本**。それぞれが何を制御しているかを混同しがちです。

### Lv1 — 最初に確認すること
- `include/syscall.h` で MSR 定数のアドレス定義を grep (`grep -n "MSR_" ../include/syscall.h`)。
- `kernel/syscall.c` の `syscall_init_cpu` (line 29 付近) で `wrmsr` が 4 回呼ばれている順番を読む。

### Lv2 — それでも進まない場合
- **MSR_EFER (0xC0000080)**: Extended Feature Enable Register。bit 0 (SCE: System Call Enable) を立てると `syscall` 命令そのものが有効化される。これを忘れると `syscall` が無効命令例外 (#UD) で死ぬ。
- **MSR_STAR (0xC0000081)**: `[47:32]` にカーネル CS、`[63:48]` にユーザー CS のベース。Orthox-64 ではカーネル CS=0x08、ユーザー CS のベース=0x10。`syscall` 命令時に CS/SS が自動でセットされる。
- **MSR_LSTAR (0xC0000082)**: `syscall` 命令の **ジャンプ先アドレス**。Orthox-64 はここに `syscall_entry` (アセンブリ突入口) のアドレスを書く。
- **MSR_SFMASK (0xC0000084)**: `syscall` 突入時に CPU が自動的にクリアする RFLAGS のビット。Orthox-64 は `0x200` (IF: 割り込みフラグ) をクリア → 突入直後は割り込み禁止。

### Lv3 — 典型的な誤解
- 「MSR_STAR にジャンプ先アドレスを書く」は誤り。STAR はセグメントセレクタ用、LSTAR がアドレス用。混同しないこと。
- 「SMP では BSP だけが `syscall_init_cpu` を呼べば十分」は誤り。**各 AP が起動時に自分で呼ぶ必要**がある。MSR はコアごとに独立しているため。
- AI が MSR の数を 3 本や 5 本と説明したら、`syscall_init_cpu` の `wrmsr` 呼び出し回数を直接 grep して確認させてください。

---

## クラスタB — `swapgs` と GS_BASE / KERNEL_GS_BASE (本書 6.3)

`swapgs` 命令の効果は **「現在の GS_BASE と KERNEL_GS_BASE を物理的に入れ替える」** だけ。これだけでカーネルスタック切替が成立する仕組みが、初見の読者には魔法に見えます。

### Lv1 — 最初に確認すること
- `kernel/syscall_entry.S` の冒頭 3 行 (`swapgs` → `mov %rsp, %gs:8` → `mov %gs:0, %rsp`) を読む。
- `include/task.h` の `struct cpu_local` の宣言を確認 (`grep -n "struct cpu_local" ../include/task.h`)。**先頭 (offset 0) が `kernel_stack`、次 (offset 8) が `user_stack`**。

### Lv2 — それでも進まない場合
- `%gs:0` は「`%gs` セグメントのベースアドレス + オフセット 0」で読まれる。`swapgs` の後は GS_BASE が `cpu_local` を指しているため、`%gs:0` = `cpu_local->kernel_stack`。
- Orthox-64 の特殊な事情: **初期化時、`MSR_GS_BASE` と `MSR_KERNEL_GS_BASE` の両方に同じ `cpu_local` ポインタが書き込まれている** (`kernel/task.c` 周辺で確認可能)。`swapgs` で入れ替えても値は同じ。これは「ユーザー空間にいる間も GS は CPU ローカルを指すが、Ring 3 からは触れない」という設計。
- なぜ「両方同じ値」で OK か？ → Linux のように「ユーザー空間でも GS_BASE をユーザーデータ用に活用したい」というケースを Orthox-64 は許していないため。素朴さと安全性を取った設計判断。

### Lv3 — 典型的な誤解
- AI が「`swapgs` の後はユーザーの GS_BASE が KERNEL_GS_BASE に退避される」と Linux 風に説明したら、**Orthox-64 では退避しても入れ替え後の値が同じ**であることを `task_refresh_cpu_local_msrs_internal` 周辺で確認させてください。
- 「`swapgs` は割り込み突入時にも使われる」は正しい (一般論)。Orthox-64 では `kernel/interrupt.S` でも対応する処理が走るので、ペアで確認可能。
- 「`%gs:0` が物理的に `cpu_local` のメモリを直接指している」というイメージは正しい。「セグメントレジスタ」というと過去の x86 segmentation を連想しがちだが、x86_64 では `%gs` の役割は「`%gs:0` 等のアクセス時に GS_BASE 値を加算する」だけに退化している。

---

## クラスタC — `struct syscall_frame` の並びと `push` 順 (本書 6.4)

アセンブリの `push` 順とC構造体の宣言順は **逆方向に対応** します。スタックは下に伸びるため、最後に `push` した値が `[%rsp+0]` に来る、という x86 のスタック規約を踏まえないと混乱します。

### Lv1 — 最初に確認すること
- `kernel/task_internal.h` で `struct syscall_frame` の宣言を読む (`grep -A 5 "struct syscall_frame" ../kernel/task_internal.h`)。
- `kernel/syscall_entry.S` で `pushq` 命令の連続を読む。

### Lv2 — それでも進まない場合
- C 構造体: `uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, rax;` (低オフセット → 高オフセット)。
- アセンブリ push 順: `rax → rdi → rsi → rdx → r10 → r8 → r9 → rbx → rbp → r12 → r13 → r14 → r15` (上から下へ push)。
- **C 構造体は「メモリ上の低アドレスから順に並ぶ」、`push` は「スタックを下に伸ばす」ので、両者が逆順で一致**する。`%rsp` がスタックの先端 (最後にpushしたr15を指す) → `struct syscall_frame*` を `%rsp` にキャストすれば、先頭フィールド r15 が正しく対応。
- フレームの上 (高アドレス側) には `rip, cs, rflags, rsp, ss` が積まれる。これは `iretq` で復帰するための CPU 規定フォーマット。

### Lv3 — 典型的な誤解
- AI が「アセンブリの push 順を C 構造体宣言と同じ順序にすればよい」と説明したら誤り。**必ず逆順**。スタックは下に伸びる x86 規約を理解させる。
- 「`syscall_frame` のサイズは sizeof(uint64_t) × フィールド数」は正しいが、**パディングが入らないこと**を必ず `static_assert` 等で守る設計が望ましい (本書には明示なし)。

---

## クラスタD — RCX/R11 自動保存と第4引数 R10 (本書 6.5)

`syscall` 命令を実行した瞬間、CPU は **RIP を RCX に、RFLAGS を R11 に強制退避**します。これは x86_64 のハードウェア仕様で、ソフト側で変えられません。

### Lv1 — 最初に確認すること
- 本書 6.5 末尾の脚注 (Intel SDM Vol 2B 引用) を確認。
- `kernel/syscall.c` の `case SYS_MMAP:` 周辺で `frame->r10` が第4引数として参照されている箇所を grep。

### Lv2 — それでも進まない場合
- System V AMD64 ABI (通常の関数呼出) では第4引数が `%rcx`。だが `syscall` 命令が RCX を上書きするため、syscall ABI では `%r10` に置き換えられた。
- この食い違いは musl libc の `syscall` ラッパーが吸収する: ユーザーが C 関数として `syscall(SYS_MMAP, addr, len, prot, flags, fd, off)` と呼ぶと、libc 側で 4 番目の `flags` を `%rcx` から `%r10` に move する。
- 戻り時の `iretq` は `frame->rcx` と `frame->r11` を復元するが、これらは元のユーザー値ではなく **syscall 命令が保存した RIP と RFLAGS**。`iretq` がスタック上の `rip/rflags` を使って復帰するため、`frame->rcx/r11` の値はある意味どうでもよい。

### Lv3 — 典型的な誤解
- AI が「カーネルが RCX を退避して引数に使う」と説明したら誤り。**CPU が勝手に RIP を RCX に書き込んでしまうため、引数には使えない**。「カーネルの選択」ではなく「ハードウェアの強制」。
- 「R10 を引数に使うのは Orthox-64 の独自設計」は誤り。**Linux/FreeBSD/macOS 等すべての x86_64 OS で共通の規約**。musl libc もそのつもりで動いている。

---

## クラスタE — switch ディスパッチと ENOSYS (本書 6.6)

Orthox-64 の `syscall_dispatch` は **巨大な switch-case 文** であり、Linux のような関数ポインタテーブル (`sys_call_table[]`) ではありません。

### Lv1 — 最初に確認すること
- `kernel/syscall.c:42` 付近で `syscall_dispatch` 本体を確認。
- `kernel/syscall.c:49` の `switch (syscall_no)` と `kernel/syscall.c:418` の `default:` 節を直接読む。

### Lv2 — それでも進まない場合
- `default:` は `frame->rax = (uint64_t)-38;` (`-ENOSYS` 相当)。Linux と同じ番号。libc 側で `errno = ENOSYS` に変換される。
- switch-case を選ぶ理由: (1) 関数ポインタテーブルは syscall 番号が**密**でないと無駄が出る (Orthox-64 は 0〜57 + 1000 番台で穴だらけ)、(2) コンパイラが各 case のジャンプテーブルを最適化生成してくれる、(3) 明示的に書かれているので grep でディスパッチ網羅を確認しやすい。
- 番号が `1000` 以上の Orthox 独自 syscall も同じ switch 内で扱われている。`include/syscall.h` の `ORTH_SYS_BASE` 周辺で定義を確認。

### Lv3 — 典型的な誤解
- AI が「Orthox-64 は `sys_call_table[]` 配列で…」と Linux と混同したら、`grep -n sys_call_table ../kernel/` で**何も出ない**ことを示して訂正させる。
- 「`default:` でカーネルパニックする」は誤り。あくまで `-38` 返却で、プロセスは生き残る。プロセスが死ぬかどうかは libc 側の判断。

---

## クラスタF — `errno` の往復 (本書 6.8)

カーネルは **`errno` という変数を直接知らない**。負の整数を `%rax` に詰めて返すだけ。libc がそれを `errno` に変換する。

### Lv1 — 最初に確認すること
- `kernel/sys_fs.c` の `sys_open` を読む (`grep -A 5 "sys_open" ../kernel/sys_fs.c`)。`errno` への代入が**一切ない**ことを確認。
- `kernel/fs.c` の `fs_open` 周辺で「失敗時の return -2」のようなパターンを確認。

### Lv2 — それでも進まない場合
- カーネルが返す値の範囲: `0` 以上は成功 (FD番号、サイズ、等)、`-1` 〜 `-4095` はエラーコード。libc はこの範囲を「エラー」と判定。
- libc 側の処理: `if (rax > -4096UL) { errno = -rax; rax = -1; }` のような形 (musl libc の `syscall_arch.h` 周辺を読むと面白い)。
- `errno` が **スレッド局所変数** なのは、複数スレッドが並行して syscall を呼んでも互いに上書きしないため。Orthox-64 でも task の `user_fs_base` (TLS) によって各スレッドが自分の `errno` を持つ。

### Lv3 — 典型的な誤解
- AI が「カーネル内で `set_errno()` のような関数を呼んでいる」と説明したら、**そのような関数は Orthox-64 に存在しない**。`grep -n "errno" ../kernel/` でも出てこないはず (test や musl 関連を除く)。
- 「`errno` は syscall ごとに別の番号」は誤り。`errno` の値は POSIX で定義された定数 (`ENOENT=2`, `EBADF=9`, etc.) で、libc/カーネル間で共通。

---

## AI への聞き方のコツ

本書 6.11「AI アシスタント」節の質問例をそのまま使うのが基本ですが、以下の追加ルールを守ると精度が上がります。

- **MSR は 4 本ある**: AI が 3 本や 5 本と答えたら、`syscall_init_cpu` の `wrmsr` 回数を直接 grep して確認させる。
- **`sys_call_table[]` は無い**: Linux と混同したら `grep -n sys_call_table ../kernel/` で空であることを示す。
- **R10 の理由はハードウェア強制**: 「Orthox-64 の独自選択」と答えたら脚注を見せて訂正。
- **`default:` は `-38`**: `kernel/syscall.c:418` 付近を直接示す。
- **GS_BASE と KERNEL_GS_BASE は初期化時同値**: Linux 風の「ユーザー空間で GS_BASE はユーザーデータを指す」説明が来たら Orthox-64 では違うことを `task.c` の MSR 初期化で確認させる。

---

## 関連

- 本章本文: 第6章 (読者の手元書籍)
- 実コード入口: `../kernel/syscall.c`, `../kernel/syscall_entry.S`, `../include/syscall.h`, `../kernel/task_internal.h`
- 関連章: 第4章 (PTE_USER で Ring 3 アクセスを制限), 第7章 (`cpu_local` 構造体の宣言が `swapgs` トリックの基盤)
