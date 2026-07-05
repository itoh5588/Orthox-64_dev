# Ch11 — pipe / signal / socket (協調するプロセスたちの対話)

第11章は **「ファイル以外のもの (パイプ・シグナル・ソケット) を Unix がどう抽象化したか」** を扱う章。第9章 (fd 統一モデル) のバックエンドであり、第15章 (BearSSL/Python のネットワーク) と第16章 (シェルのパイプライン) の土台。3 つの異なる通信機構が 1 章に詰まっているので構成が密です。

## 章のゴール

* **パイプが `PIPE_BUF_SIZE = 4000` バイトの円環バッファ + 読み手/書き手 1 つずつの waiter ポインタ**で成り立つ最小構成を理解する。
* **sleep/wakeup と EOF 伝播**（書き手全閉 → 読み手が 0 で即返る）の仕組みを `pipe_read`/`pipe_write` の sleep ループで追える。
* **`pipe2` の `O_CLOEXEC`** が、シェルや BusyBox のハングアップバグを防ぐ陰の主役であることを把握する。
* **シグナルは fd の中ではなく `struct task` 内 (`sig_pending`/`sig_mask`/`sig_handlers[]`)** で管理されることに気付く。
* **`sys_kill` が SIGINT/SIGTERM/SIGKILL でショートカット** (即ゾンビ + 親に SIGCHLD) する設計、`-ESRCH` ではなく `-1` を返す挙動を確認できる。
* **ソケットは `sys_net.c` の thin wrapper → `net_socket.c` の `net_socket_backend_t` → lwIP** という 3 層構造で、状態フラグ + waiter ポインタの待機キューで非同期パケットイベントとスケジューラを噛み合わせる。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (11.2 〜 11.10) に対応した6つのクラスタに分かれています (パイプ/シグナル/ソケット 各 2 クラスタ)。

## 取り組み方の推奨フロー

1. 本書 11.1 〜 11.12 を**通読**してから戻る。図 11.1 〜 11.3 を頭の中で描けるか確認。
2. `include/fs.h:52` の `PIPE_BUF_SIZE = 4000` と `:63` の `pipe_t` 構造体を確認。
3. `kernel/fs.c:1982` の `fs_pipe`、`:1632`/`:1819` の pipe write/read 本体を読む。
4. `kernel/sys_proc.c:171` の `sys_kill` を読み、SIGINT/SIGTERM/SIGKILL のショートカット分岐を確認。
5. `kernel/sys_signal.c:41` の `sys_rt_sigprocmask` と周辺の `sys_rt_sigaction` を読む。
6. `kernel/sys_net.c` の thin wrapper を眺め、`kernel/net_socket.c` の `net_socket_backend_t` 宣言と waiter ポインタを確認。
7. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# パイプ本体
grep -n "fs_pipe\|fs_pipe2\|pipe_t\|PIPE_BUF_SIZE\|pipe_take_waiter_locked" ../kernel/fs.c ../include/fs.h

# シグナル
grep -n "sys_kill\|sig_pending\|sig_mask\|task_signal_add_locked" ../kernel/sys_proc.c
grep -n "sys_rt_sigaction\|sys_rt_sigprocmask\|orth_sigaction\|linux_rt_sigaction_k" ../kernel/sys_signal.c

# ソケット
grep -n "sys_socket\|sys_connect\|sys_bind\|sys_listen\|sys_accept" ../kernel/sys_net.c
grep -n "net_socket_backend\|rx_waiter\|tx_waiter\|connect_waiter\|accept_waiter" ../kernel/net_socket.c
```

## AI への話しかけ方の例

* 「第11章 11.3 でパイプの読み手が空バッファでスリープし、書き手が起こすとあった。`kernel/fs.c:1819` 付近の pipe_read 実装で `pipe_take_waiter_locked` 経由で waiter を起こす流れを grep で示して」
* 「第11章 11.6 で `sys_kill` が SIGINT/SIGTERM/SIGKILL のとき即ゾンビ + 親に SIGCHLD とあった。`kernel/sys_proc.c:171` 付近の `if (sig == 2 || sig == 15 || sig == 9)` 分岐を grep で見せて、対象不在時の戻り値が `-1` (Linux の -ESRCH ではない) であることを確認したい」
* 「第11章 11.10 で `net_socket_backend_t` が `rx_waiter`, `tx_waiter`, `connect_waiter`, `accept_waiter` の 4 つの待機ポインタを持つとあった。`kernel/net_socket.c` で構造体宣言を grep で示して、各 waiter がそれぞれどの syscall で寝かされてどの lwIP コールバックで起こされるか整理して」

AI は本書本文をテキストとしては読めないので、章番号と節番号、登場するシンボル名を添えて質問してください。
