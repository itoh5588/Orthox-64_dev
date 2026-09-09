# 第11章 段階的ヒント — pipe / signal / socket

本章は **「3 つの異なる通信機構を 1 章で扱う」** ため構成が密です。パイプ・シグナル・ソケットそれぞれ独立した話題なので、必要なクラスタだけ参照してください。**まず本書本文を 11.1 〜 11.12 まで通読**してから戻ることを強く推奨します。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、答えそのものは出していません。

---

## クラスタA — パイプの最小構成 (本書 11.2)

パイプは `PIPE_BUF_SIZE = 4000` バイトの円環バッファ 1 個 + 読み手/書き手の waiter ポインタ 2 個 + spinlock 1 個。`fs_pipe` は **fd を 2 個割り当て、両方とも同じ `pipe_t*` を `fs_file_t->private_data` に格納**する。

### Lv1 — 最初に確認すること
- `include/fs.h:52` で `#define PIPE_BUF_SIZE 4000`、`:63` 付近で `pipe_t` typedef を確認。
- `kernel/fs.c:1982` 付近で `fs_pipe` 本体を読む。`for (i = 3; i < MAX_FDS; i++)` の fd 線形探索が 2 回回る。

### Lv2 — それでも進まない場合
- `pipe_t` のメンバ: `buffer[4000]`, `read_pos`, `write_pos`, `count`, `ref_count`, `read_waiter`, `write_waiter`, `lock`。第19章 hints クラスタC と似た構造 (バッファ + lock + waiter)。
- `pmm_alloc(1)` で 1 ページ確保し、その先頭に `pipe_t` を配置。`PHYS_TO_VIRT` で仮想ポインタに変換 (第4章クラスタA と同じパターン)。
- `fs_alloc_file()` で `fs_file_t` を 2 個確保し、両方に同じ `pipe` ポインタを `.private_data` で格納、`.type = FT_PIPE`、`.ops = &g_pipe_file_ops`。
- 2 個の fd は: `fds[fd1]` が読み手側 (`flags = O_RDONLY`)、`fds[fd2]` が書き手側 (`flags = O_WRONLY`)。`pipefd[0] = fd1; pipefd[1] = fd2;`。

### Lv3 — 典型的な誤解
- AI が `PIPE_BUF_SIZE = 4096` と書いたら誤り。**4000**。1 ページ (4096) から `pipe_t` の他のフィールド分を引いた残り。
- 「`fs_pipe` は `task_alloc_fd()` でスロットを取得」は誤り。**Orthox-64 はそんなヘルパー関数を持たず、`for (i = 3; ...)` で線形探索**。
- 「`pipe_t` は heap (malloc) で確保」は誤り。**`pmm_alloc(1)` で物理ページ 1 枚を直接確保**。

---

## クラスタB — `pipe_read` / `pipe_write` の sleep ループと EOF (本書 11.3)

書き手と読み手の速度差を吸収する **sleep/wakeup の往復**。最大の落とし穴は**「書き手全閉 → 読み手は EOF (0 返却) でスリープせず終了」** の特殊処理。

### Lv1 — 最初に確認すること
- `kernel/fs.c:1819` 付近で pipe_read 該当ブロック (`fs_fd_type(f) == FT_PIPE`) を読む。
- `kernel/fs.c:1632` 付近で pipe_write 該当ブロックを読む。
- `kernel/fs.c:77` 付近の `pipe_take_waiter_locked` を読む。waiter ポインタを atomic に取り出して起こす関数。

### Lv2 — それでも進まない場合
- **読み手のスリープ条件**: `pipe->count == 0` (空) かつ `pipe->write_count != 0` (書き手まだいる)。書き手全閉なら `return 0;` で EOF。
- **書き手のスリープ条件**: `pipe->count == PIPE_BUF_SIZE` (満杯)。読み手全閉ならエラー (SIGPIPE 等)。
- spinlock の取り方: lock 取得 → 条件チェック → スリープ前に lock 解放 → `kernel_yield` → 起床後 lock 再取得 → 再チェック。**lock を持ったままスリープしてはいけない** (他タスクが起こせなくなる)。
- 起床のタイミング: 読み手がデータを消費して空きが出たら `write_waiter` に通知。書き手が書き込んだらデータが入った時点で `read_waiter` に通知。
- EOF 伝播: シェルパイプライン `cat file | grep ERROR` で、`cat` が終了 (write 側 fd close) すると `grep` の `read` が 0 を返し、`grep` も終了。

### Lv3 — 典型的な誤解
- AI が「書き手と読み手それぞれ多数の waiter をリストで保持する」と説明したら誤り。**Orthox-64 のパイプは waiter ポインタを 1 個ずつ**しか持たない (本書 11.2 の `pipe_t` 宣言で確認)。並行読み/書きを想定していない素朴設計。
- 「ロックを取ったまま `kernel_yield` できる」は誤り。**必ずロック解放してから yield**。
- 「EOF は書き手全閉と同時に検知される」は半分正しい。**読み手側が次に `pipe_read` を呼んだ時に検知**される。close は単にカウンタを減らすだけ。

---

## クラスタC — `pipe2` と `O_CLOEXEC` (本書 11.4)

`pipe2` は `pipe` + フラグ引数。`O_CLOEXEC` を立てれば作成と同時に `FD_CLOEXEC` がセットされ、後続の `execve` で自動 close される。

### Lv1 — 最初に確認すること
- `kernel/fs.c:2043` 付近の `fs_pipe2` を読む。`fs_pipe` を呼んでから、フラグに応じて `fds[fd].fd_flags |= FD_CLOEXEC` を立てる。
- 第9章クラスタE の `fs_close_cloexec_descriptors` を再確認。

### Lv2 — それでも進まない場合
- なぜ `pipe2` が必要か: `pipe` + `fcntl(fd, F_SETFD, FD_CLOEXEC)` の 2 段階だと**間に競合の窓**ができる。マルチスレッドで他スレッドが `fork+exec` した瞬間、CLOEXEC が立つ前の fd が漏れる。`pipe2` は atomic に両方をやる。
- BusyBox のシェルが内部で頻繁にパイプを作り、子に exec させる。CLOEXEC が無いと「シェルが作ったパイプの書き手 fd が子プロセスに残る → 親側が close しても ref_count > 0 → 読み手が EOF を検知できずハング」というバグになる。

### Lv3 — 典型的な誤解
- 「`pipe2` は Linux 独自で POSIX には無い」は半分正しい (POSIX に取り込まれたのは比較的最近)。Orthox-64 は musl libc の依存に応えるために実装している。
- AI が `O_CLOEXEC` と `FD_CLOEXEC` を混同したら指摘。**前者は open/pipe2 時のフラグ、後者は `fcntl` で操作する fd 側の属性**。

---

## クラスタD — シグナルが `struct task` 内にある (本書 11.5)

シグナルは fd ではなく、**task 構造体の `sig_pending` (届いて未処理のビットマップ)、`sig_mask` (ブロック中のビットマップ)、`sig_handlers[]` (各シグナルのハンドラ関数ポインタ配列)** で管理されます。

### Lv1 — 最初に確認すること
- `include/task.h` で `struct task` の `sig_*` フィールドを grep。
- `kernel/sys_proc.c:171` 付近の `sys_kill` を読む。`task_signal_add_locked(t, sig);` でビットマップに追加。

### Lv2 — それでも進まない場合
- `sig_pending` は 64 ビット (`uint64_t`)。シグナル番号 1〜64 をビット 1〜64 で表す (POSIX 仕様)。
- `sig_mask` で「現在ブロック中」を表現。`sigprocmask` で読者が変更。マスクされた信号は届いても保留 (pending に残るがハンドラに渡らない)。
- シグナル配送のタイミング: **ユーザーモードに戻る直前** (syscall ハンドラ末尾、割り込み復帰直前)。カーネルコード実行中には届かない (これが「非同期割り込み」と「シグナル」の本質的な違い)。
- 配送が決まると: スタックに `sigframe` を積み、ユーザー空間のハンドラ関数アドレスに `rip` を飛ばし、ハンドラ実行後 `sigreturn` で元の実行点に復帰。

### Lv3 — 典型的な誤解
- 「シグナルは fd で送受信できる」は半分正しい (Linux の `signalfd` はある)。**Orthox-64 は `signalfd` を実装していない**。素朴な配送のみ。
- AI が「Orthox-64 はリアルタイムシグナル (32-63) を完全サポート」と言ったら誤り。**標準シグナル中心の最小実装**で、SIGRTMIN 等は明示的なサポートなし。
- 「シグナルハンドラはカーネルモードで実行」は誤り。**ユーザー空間で実行**。ハンドラ自体は libc が登録した関数。

---

## クラスタE — `sys_kill` のショートカット (本書 11.6)

`sys_kill(pid, sig)` は通常はシグナルを追加するだけだが、**SIGINT (2) / SIGTERM (15) / SIGKILL (9) のときは即ゾンビ化 + 親に SIGCHLD (20)** というショートカットがあります。

### Lv1 — 最初に確認すること
- `kernel/sys_proc.c:171` 付近の `sys_kill` 本体を読む。
- `if (sig == 2 || sig == 15 || sig == 9)` の分岐で `task_mark_zombie(t, 128 + sig);` と `task_signal_add_locked(parent, 20);`。

### Lv2 — それでも進まない場合
- 終了ステータス `128 + sig` は POSIX 慣習: シグナル殺しは終了コード `128+SIGNAL` で報告 (例: SIGKILL=9 → exit code 137)。シェルが `$?` で読める。
- ショートカットの意義: **本来はターゲットプロセスが自分のシグナルハンドラで処理してから exit する**べきだが、Orthox-64 は素朴さのためカーネル側で即終了させている。これで BusyBox の `Ctrl-C` や `kill -9` が即座に効く。
- `sig == 0`: プロセス生存確認のための特殊呼び出し。何もせず存在チェック結果を返す (`0` または `-1`)。
- 対象 pid 不在時の戻り値は **`-1`** (Linux の `-ESRCH` ではない)。AI が `-ESRCH` を返すコードを出したら指摘。

### Lv3 — 典型的な誤解
- 「`sys_kill` 内で `task_wakeup` を呼ぶ」と AI が書いたら誤り。**起床は `task_signal_add_locked` の内部で隠蔽**されている (sleeping ならその場で起こす)。
- 「SIGTERM はキャッチ可能、SIGKILL は不可」は標準的だが、Orthox-64 のショートカットでは**どちらも即終了**。ユーザーハンドラはバイパスされる。これは厳密には POSIX 違反だが、教育用 OS の割り切り。
- AI が `-ESRCH` (定数 3) を Orthox-64 で使うコードを書いたら、`grep -n ESRCH ../kernel/` で確認させる (たぶん未定義)。

---

## クラスタF — ソケットと lwIP backend、waiter キュー (本書 11.8, 11.9, 11.10)

ソケットは **`sys_net.c` (thin wrapper) → `net_socket.c` (`net_socket_backend_t` 構造体管理) → lwIP (TCP/IP 本体)** の 3 層構造。状態フラグと 4 種の waiter ポインタで lwIP コールバックとスケジューラを噛み合わせます。

### Lv1 — 最初に確認すること
- `kernel/sys_net.c` で `sys_connect` 等の thin wrapper を読む (1〜3 行で `net_socket_*` に委譲)。
- `kernel/net_socket.c` で `net_socket_backend_t` の宣言を grep。`rx_waiter`, `tx_waiter`, `connect_waiter`, `accept_waiter` の 4 つの待機ポインタ。

### Lv2 — それでも進まない場合
- **3 層分離の意義**: (1) `sys_net.c` は ABI 境界 (`copy_from_user` 等)、(2) `net_socket.c` は OS 内 IPC とロック・スリープ管理、(3) lwIP は純粋なプロトコル処理 (TCP/IP)。
- **状態フラグ**: `connected`, `connecting`, `listening`, `eof`, `error` 等。状態遷移を 1 タスクで安全に追える。
- **waiter ポインタ 4 つの役割**: `rx_waiter` = read で待ち中、`tx_waiter` = write で送信完了待ち、`connect_waiter` = connect で 3-way handshake 待ち、`accept_waiter` = accept で接続到着待ち。各 1 個ずつ (パイプ同様、並行 read/write は想定なし)。
- **lwIP コールバック**: パケット受信割り込み (タイマ起点ポーリング、第5章) → lwIP が pcb を更新 → コールバックで該当 waiter を `task_wakeup`。
- **受信キュー**: `socket_rx_chunk_t` のリンクリスト (`rx_head` / `rx_tail`)。専用 queue 構造体ではなく素朴なリスト。

### Lv3 — 典型的な誤解
- AI が「ソケットには専用の `net_rx_queue` 構造体がある」と言ったら誤り。**`socket_rx_chunk_t` ポインタを `rx_head/rx_tail` で繋ぐ単純リスト**。
- 「`accept_waiter` キューは複数タスク同時待機可能」は誤り。**ポインタ 1 個**。同時待機できるのは 1 タスクだけ。
- 「ソケットレベルの spinlock がある」は誤り。**ソケットオブジェクトには `lock` フィールドが無い**。グローバルなジャイアントロック (BKL) で守られる。

---

## AI への聞き方のコツ

本書 11.13「AI アシスタント」節の質問例をそのまま使うのが基本ですが、以下の追加ルールを守ると精度が上がります。

- **`PIPE_BUF_SIZE = 4000`**: 4096 と勘違いしやすい。
- **`pipe_t` も `net_socket_backend_t` も waiter ポインタ 1 個ずつ**: キュー構造体ではない。
- **`sys_kill` 対象不在エラーは `-1`、`-ESRCH` ではない**。
- **SIGINT/TERM/KILL ショートカット**: 通常のシグナル配送と挙動が違う。AI が一般的シグナル配送を前提に説明したら指摘。
- **`task_signal_add_locked` が起床を含む**: 二重に `task_wakeup` を書かせない。
- **lwIP は外部スタック、Orthox-64 は薄い結合**: AI に Linux ソケットコードを Orthox-64 と混同させない。

---

## 関連

- 本章本文: 第11章 (読者の手元書籍)
- 実コード入口: `../kernel/fs.c` (pipe), `../kernel/sys_proc.c` (kill), `../kernel/sys_signal.c` (sigaction), `../kernel/sys_net.c` (thin wrappers), `../kernel/net_socket.c` (backend)
- 関連章: 第8章 (`sys_exit` の SIGCHLD 送信 + `sys_wait4`), 第9章 (`FT_PIPE` / `FT_SOCKET` の fd 多態), 第5章 (タイマ割り込みでの `net_poll` → lwIP コールバック起動), 第14章 (TLS で `errno` 等のスレッド変数 — シグナルハンドラから読まれる)
