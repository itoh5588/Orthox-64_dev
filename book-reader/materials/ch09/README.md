# Ch9 — file descriptor と VFS (すべてを統一的に掴むための手)

第9章は **「すべてはファイルである」という Unix の最大の抽象化哲学**を、`file_descriptor_t` / `fs_file_t` / VFS / マウントポイント / ファイルタイプ列挙型として具現化する章です。第10章 (xv6fs) と第11章 (pipe/signal/socket) の土台。

## 章のゴール

* **fd が「カーネル内ポインタではなく、ただの整数」**である理由 (セキュリティ境界) を理解する。
* **`file_descriptor_t` (per-task) と `fs_file_t` (共有オブジェクト) の 2 層構造**が、`fork` 後の親子オフセット同期や `dup2` を成り立たせていることを掴む。
* **VFS の `vfs_find_mountpoint` が最長一致でマウントポイントを選ぶ**仕組みを `kernel/vfs.c` で確認できる。
* **`fs_read` が switch ではなく if 連鎖で多態性を実現**しており、コンソール・パイプはインライン処理、ソケットや xv6fs は専用関数呼び出し、という構造を区別できる。
* **`FD_CLOEXEC` が `execve` で閉じられる流れ**を `fs_close_cloexec_descriptors` (`kernel/fs.c:358`) で確認できる。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (9.2 〜 9.8) に対応した6つのクラスタに分かれています。

## 取り組み方の推奨フロー

1. 本書 9.1 〜 9.10 を**通読**してから戻る。図 9.1 (fd の統一ハンドル) と図 9.2 (親子共有 fs_file_t) を頭の中で描けるか確認。
2. `include/fs.h` で `MAX_FDS = 256` (line 8) と `file_type_t` (line 23 付近) を確認。
3. `kernel/fs.c:1278` の `fs_open` を読み、fd 割当ループ (line 1289) と `vfs_find_mountpoint` 呼び出し (line 1299) を追う。
4. `kernel/vfs.c:57` の `vfs_find_mountpoint` を読み、最長一致の実装を確認。
5. `kernel/fs.c:215` の `fs_fd_type` と、`fs_read` (周辺) の if 連鎖を読む。
6. `kernel/fs.c:358` の `fs_close_cloexec_descriptors` を読む。**引数は `struct task* task`** であって void ではない。
7. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# fd / fs_file_t / 型定義
grep -n "MAX_FDS\|file_type_t\|FT_\|file_descriptor_t\|fs_file" ../include/fs.h

# fs_open と fd 割当
grep -n "fs_open\|i = 3.*MAX_FDS\|vfs_find_mountpoint" ../kernel/fs.c

# fs_read の多態
grep -n "fs_read\|fs_fd_type\|FT_CONSOLE\|FT_PIPE\|FT_SOCKET\|FT_XV6FS" ../kernel/fs.c

# VFS のマウントポイント
grep -n "vfs_find_mountpoint\|vfs_register_mountpoint" ../kernel/vfs.c

# CLOEXEC
grep -n "fs_close_cloexec_descriptors\|FD_CLOEXEC" ../kernel/fs.c
```

## AI への話しかけ方の例

* 「第9章 9.5 で VFS の `vfs_find_mountpoint` が最長一致でマウントポイントを返すとあった。`kernel/vfs.c:57` の実装を grep で示して、文字列比較が境界 (`/` 等) で正しく切れる実装か確認したい」
* 「第9章 9.6 で `fs_read` が型に応じて分岐するとあった。`kernel/fs.c` の `fs_read` を grep して、コンソール・パイプ・ソケット・xv6fs の 4 分岐が if 連鎖で実装されている (switch ではない) ことを確認したい」
* 「第9章 9.7 で `execve` 時に `FD_CLOEXEC` フラグの立った fd だけ閉じるとあった。`kernel/fs.c:358` の `fs_close_cloexec_descriptors` が `struct task*` を引数に取り、`i = 3` から始まる (標準入出力 0/1/2 はスキップ) ことを確認したい」

AI は本書本文をテキストとしては読めないので、章番号と節番号、登場するシンボル名を添えて質問してください。
