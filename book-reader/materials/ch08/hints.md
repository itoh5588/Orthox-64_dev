# 第8章 段階的ヒント — fork / exec / wait の三位一体

本章は **「第7章で築いた `struct task` を、Unix のプロセスライフサイクル (fork → exec → exit → wait) で実際に動かす」** 章です。手が止まりがちな箇所だけ Lv1 → Lv2 → Lv3 の順に開示してください。**まず本書本文を 8.1 〜 8.10 まで通読**してから戻ることを強く推奨します。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、答えそのものは出していません。

---

## クラスタA — `task_fork` が引き継ぐもの (本書 8.3)

`fork` は親のあらゆる「資産」を子にコピーします: pid 以外の識別子、fd table、cwd、ヒープ範囲、ユーザースタックポインタ。何を**コピーし、何を新規割当する**かを区別しましょう。

### Lv1 — 最初に確認すること
- `kernel/task_fork.c:26` 付近の `task_fork` 本体を読む。
- どのフィールドが「親からコピー」されているかをコード上で目視する: `pid` は新規 (`task_next_pid_locked`)、`ppid` は `parent->pid`、`heap_break`, `mmap_end`, `user_entry`, `user_stack` は親から、fd は `fs_clone_fd` で個別コピー、cwd は参照カウント増加。

### Lv2 — それでも進まない場合
- **fd は値コピーではなく参照カウント増加**: `fs_clone_fd` は親の `fs_file_t*` を子の同インデックスにコピーし、`fs_file_t->ref_count` をインクリメント。**親子で同じオフセットを共有**する (第9章クラスタB と同じ理由)。
- **cwd の `vfs_inode_ref`**: cwd は inode へのポインタなので、参照カウントだけ増やす。`exit` 時に減らさないとリーク。
- **PML4 は別途**: `child->ctx.cr3 = vmm_copy_pml4(...)` で**親の PML4 を COW 化してコピー**。fork 直後は親子が物理ページを共有する状態。

### Lv3 — 典型的な誤解
- 「`fork` は子に新しいヒープを割り当てる」は誤り。**`heap_break` も `mmap_end` も親と同じ値**。COW で物理ページは共有、書き込み時に分離される。
- AI が「`fork` 時にカーネルスタックも子にコピー」と説明したら部分的に誤り。**新しいカーネルスタックは `pmm_alloc(4)` で確保**し、その上に親の `syscall_frame` をコピーする (戻り値 rax を 0 に書き換えるため)。スタック内容そのものを丸ごとコピーするわけではない。

---

## クラスタB — COW の発動メカニズム (本書 8.4)

`vmm_copy_pml4` (第4章クラスタE) が COW を**仕込み**、`vmm_page_fault_handler` が COW を**解決**する。この対が `fork` の高速化を支えています。

### Lv1 — 最初に確認すること
- `kernel/vmm.c:209` 付近の `vmm_copy_pml4` を再読。親の PTE で `PTE_WRITABLE` をクリアし `PTE_COW` を立てる行 (line 249, 268 付近) を確認。
- `kernel/vmm.c:371` 付近の `vmm_page_fault_handler` を読む。書き込み違反 (write to read-only) のときに `PTE_COW` を判定して `pmm_alloc(1)` で複製する経路 (line 430-442, 461-480 付近)。

### Lv2 — それでも進まない場合
- 親子で同一物理ページを指している間、`ref_count` (PMM の `ref_counts[]`) が 2 になっている。**ページフォルトハンドラはこれを見て、最後の参照者かどうか判定**する。
- `ref_count == 1` (自分だけが参照中) ならコピー不要、PTE_WRITABLE を立て直すだけで `PTE_COW` を外して終了。**この最適化を AI が省くコード**を出したら指摘。
- 「PML4 上位半 (カーネル空間)」は COW 対象外。インラインの for ループで値コピーされ、PTE_COW は立たない。第4章 hints クラスタE と同じ注意点。

### Lv3 — 典型的な誤解
- 「`vmm_copy_pml4` が物理ページを実際にコピーする」は誤り。**ページテーブル構造 (PML4/PDP/PD/PT) はコピーするが、最末端の物理ページは共有のまま**。物理コピーはページフォルトハンドラが行う。
- AI が「`vmm_copy_pml4` の戻り値はポインタ」と説明したら、`include/vmm.h` で **`uint64_t` (物理アドレス) を返す**ことを確認させる (第4章 hints と同じ)。

---

## クラスタC — 親と子で戻り値が違う仕組み (本書 8.5)

`fork` の「1 回呼んで戻り値が 2 回返る」謎は、**子の `syscall_frame->rax` を強制的に 0 に書き換える**ことで実現されます。

### Lv1 — 最初に確認すること
- `kernel/task_fork.c` で `child_frame->rax = 0;` 相当の行を grep (`grep -n "child_frame->rax" ../kernel/task_fork.c`)。
- 親側の戻り値は `task_fork` の `return child->pid;` で `%rax` に乗る (第6章クラスタE と同じ)。

### Lv2 — それでも進まない場合
- 子は **`switch_context` でスケジューラから初めて選ばれた時**に、書き換えられた `child_frame` をベースに `iretq` でユーザー空間に戻る。その瞬間 `%rax = 0` がユーザーから見える。
- 「親が `fork` から戻る」と「子が `fork` から戻る」は **異なる物理的な瞬間**。親はカーネル内で `task_fork` が `child->pid` を返した直後、子は schedule 後にユーザーモード復帰した瞬間。
- 親の `syscall_frame` は触らない (上書きしてしまうと親の戻り値が壊れる)。子のフレームだけ書き換える。

### Lv3 — 典型的な誤解
- AI が「子の戻り値を 0 にするためにスケジューラが特別な処理をする」と説明したら誤り。**スケジューラは何もしない**。`task_fork` 内でフレームを書き換えた段階で全てが決まっている。
- 「子の `%rax` は task 構造体のフィールドで決まる」は誤り。**カーネルスタック上の `syscall_frame->rax`** で決まる (これは task ごとに別のカーネルスタックなので衝突しない)。

---

## クラスタD — `task_execve` の順序 (本書 8.6, 8.7)

`execve` で最も致命的なバグは**「旧アドレス空間を破棄した後で、ユーザー引数を読もうとする」**こと。Orthox-64 は最初にユーザー引数をカーネル領域に退避してから旧空間を破棄します。

### Lv1 — 最初に確認すること
- `kernel/task_exec.c:321` 付近の `task_execve` を読み、関数全体の流れをトレース。
- `copy_user_cstring` / `copy_user_string_vector` の呼び出しが、新 PML4 確保 (`pmm_alloc(1) for pml4_phys`) より**前**に来ていることを確認。

### Lv2 — それでも進まない場合
- 順序の根拠: ユーザー空間の引数文字列は**旧 PML4 でしかマップされていない**。新 PML4 に切り替えた後で読もうとするとページフォルト or 別データを読む。
- 退避先は `exec_copy` という一時バッファ (`EXEC_COPY_PAGES` ページ確保)。`exec_copy->path`, `exec_copy->argv`, `exec_copy->envp`, `exec_copy->argv_storage`, `exec_copy->envp_storage` に詰め直す。
- `task_prepare_initial_user_stack` でユーザースタックに argc/argv/envp/auxv を配置するときに、この退避済みデータを参照する (第14章クラスタE と密接に関連)。

### Lv3 — 典型的な誤解
- AI が「`execve` は古い PML4 を残したまま新しい PML4 を構築する」と説明したら、**最終的には旧 PML4 を解放する**ことを確認させる (`pmm_free(old_pml4_phys, ...)` 等の経路)。古い物理ページの解放を忘れるとメモリリーク。
- 「`execve` は新プログラムをロードしたら旧 fd を全て閉じる」は誤り。**`FD_CLOEXEC` フラグが立っている fd だけ閉じる** (`fs_close_cloexec_descriptors`)。残りは新プログラムに引き継がれる (第9章クラスタE と同じ)。

---

## クラスタE — ELF ロードと初期スタック (本書 8.7)

第14章への直接の伏線。`elf_load` で各 `PT_LOAD` セグメントをマップし、`PT_INTERP` があれば動的リンカーも別ベースでロード、最後に `task_prepare_initial_user_stack` で argc/argv/envp/auxv を積みます。

### Lv1 — 最初に確認すること
- `kernel/task_exec.c` 内で `elf_load` の呼び出しを 2 回 grep (メインバイナリ用 line 363 付近、インタープリタ用 line 382 付近)。
- `t->user_entry = info.has_interp ? interp_info.entry : info.entry;` の三項演算子。

### Lv2 — それでも進まない場合
- メインバイナリが `ET_DYN` (PIE) の場合は `exec_load_bias = EXEC_ET_DYN_LOAD_BASE` でずらしてロード。詳細は第14章クラスタC。
- インタープリタは `EXEC_INTERP_LOAD_BASE` (別のベース) でロード。これによりメインバイナリとインタープリタが衝突しない。
- `task_prepare_initial_user_stack` の中で auxv の `AT_PHDR`, `AT_BASE`, `AT_ENTRY` を書き込む (第14章クラスタE で詳細)。インタープリタはこれを読んでメインバイナリの場所を知る。

### Lv3 — 典型的な誤解
- 「`execve` で動的リンカーが要らない場合 (静的バイナリ) なら `interp_info` は無視」は正しいが、auxv 自体は**静的バイナリでも構築する**。`AT_PHDR`, `AT_ENTRY` 等は静的バイナリでも必要。

---

## クラスタF — `exit` と `wait4` の対 (本書 8.8, 8.9)

`exit` した瞬間に task 構造体を解放しないのは、**親が `wait4` で終了ステータスを回収する権利があるから**。この間 task は `TASK_ZOMBIE` 状態で台帳に残ります。

### Lv1 — 最初に確認すること
- `kernel/sys_proc.c:121` 付近の `sys_exit` を読む。fd 全クローズ → `task_mark_zombie` → 親に SIGCHLD (`task_signal_add_locked`, 番号 20) → 無限 `kernel_yield`。
- `kernel/sys_proc.c:138` 付近の `sys_wait4` を読む。task_list を巡回し、自分の子 (`ppid == current->pid`) でゾンビ状態のものを探し、`task_reap` で完全解放。

### Lv2 — それでも進まない場合
- `sys_exit` の最後の `while(1) { kernel_yield(); }` は、**死んだタスクが二度とスケジューラに選ばれないため**の無限ループ。`TASK_ZOMBIE` 状態のタスクは runqueue から外され、`task_reap` でやっと解放される。
- `sys_wait4` で子が見つからない場合の戻り値は **`-1`** (Linux の `-ECHILD` と異なる)。AI が `-ECHILD` を返す実装を出したら、`kernel/sys_proc.c:138` の実装を確認させる。
- 「子が生きている場合は親はスリープ」は `task_mark_sleeping(current)` + `kernel_yield()` で実現。子が `exit` 時に `task_signal_add_locked(parent, 20)` で SIGCHLD を立てると、親がスリープから起き直して再度ループ。

### Lv3 — 典型的な誤解
- 「`exit` 直後に親がいない (孤児) なら即解放」は誤り。**孤児は init (pid 1) に reparenting** される (本書 8.11 のヒント質問にもある通り)。Orthox-64 の実装で `task_reparent_to_init` 等の関数が grep で見つかるはず。
- AI が「`sys_exit` 内で `pmm_free` を直接呼んでカーネルスタックを解放する」と書いたら危険。**現在実行中のスタックを解放したら直後にクラッシュ**する。解放は `task_reap` (他のタスクの文脈で実行) が担当。

---

## AI への聞き方のコツ

本書 8.11「AI アシスタント」節の質問例をそのまま使うのが基本ですが、以下の追加ルールを守ると精度が上がります。

- **`task_list_lock` は無い、BKL を使う**: AI が一般的なリストロックを前提に解説したら、Orthox-64 は `task_lock_irqsave` の BKL であることを `grep -n task_list_lock ../kernel/` で示す (第7章クラスタD と同じ)。
- **`vmm_setup_kernel_mappings` は無い**: `task_execve` でも PML4 上位半は**インライン for ループ**でコピーする (第4章 hints と同じ)。
- **`-ECHILD` ではなく `-1`**: `sys_wait4` の子不在エラーコード。
- **`exit` 後の無限 yield**: AI が省略したら追加させる。これがないと死んだタスクが再実行されうる。

---

## 関連

- 本章本文: 第8章 (読者の手元書籍)
- 実コード入口: `../kernel/task_fork.c`, `../kernel/task_exec.c`, `../kernel/sys_proc.c`, `../kernel/vmm.c`
- 関連章: 第4章 (COW の PTE_COW 仕組み), 第7章 (`struct task` と `switch_context`), 第14章 (ELF ロード詳細・auxv)
