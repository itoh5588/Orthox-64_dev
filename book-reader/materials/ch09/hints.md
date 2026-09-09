# 第9章 段階的ヒント — file descriptor と VFS

本章は **「Unix の最大の抽象化哲学 (everything is a file) を、コードで触れる」** 章です。手が止まりがちな箇所だけ Lv1 → Lv2 → Lv3 の順に開示してください。**まず本書本文を 9.1 〜 9.10 まで通読**してから戻ることを強く推奨します。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、答えそのものは出していません。

---

## クラスタA — fd が「ただの整数」である理由 (本書 9.1, 9.2)

fd はカーネル内ポインタの **インデックス参照**であり、ユーザー空間にポインタを渡さないことで安全性を担保しています。

### Lv1 — 最初に確認すること
- `include/fs.h:8` で `#define MAX_FDS 256` を確認。
- `struct task` 内に `file_descriptor_t fds[MAX_FDS];` がある (第7章クラスタA で見たフィールド) ことを再確認。

### Lv2 — それでも進まない場合
- ユーザー空間から見える fd は **per-task の配列インデックス**。プロセス A の fd 3 とプロセス B の fd 3 は完全に別物。
- カーネルが fd を受け取ったら必ず: (1) 範囲チェック (`fd < 0 || fd >= MAX_FDS`)、(2) `in_use` チェック、(3) `current->fds[fd].file` から `fs_file_t*` を取得、の順序で検証。これを怠るとユーザーが負の fd や巨大な fd を渡してカーネルを落とせる。
- 標準入出力 (0, 1, 2) は予約済み。新規 fd 割当は `i = 3` から空きを探す (`kernel/fs.c:1289` の for ループで確認可能)。

### Lv3 — 典型的な誤解
- 「fd の最大数は 64」と古いドキュメントを見て AI が誤答することがある。**実際は 256** (`include/fs.h:8`)。grep で訂正させる。
- 「fd はシステム全体で一意」は誤り。**プロセスごとに別空間**。これが Unix のシンプルさの源泉。
- AI が「カーネルがユーザー空間にポインタを返してもよい」と言ったら、セキュリティ境界の話で訂正。**ポインタを渡すと特権昇格脆弱性に直結**。

---

## クラスタB — `file_descriptor_t` と `fs_file_t` の 2 層構造 (本書 9.3)

この 2 層構造があるからこそ `fork` 後に親子で同じファイルオフセットを共有でき、`dup2` で同じファイルを別 fd 番号から開ける、という挙動が成立します。

### Lv1 — 最初に確認すること
- `include/fs.h:23` 付近で `file_descriptor_t` と `struct fs_file` の宣言を読む (`grep -A 10 "file_descriptor_t\|struct fs_file" ../include/fs.h`)。
- `ref_count`, `offset`, `type`, `private_data`, `ops` の 5 フィールドが `fs_file` 側にあることを確認。

### Lv2 — それでも進まない場合
- `fork` 時: `fs_clone_fd` (第8章クラスタA) が**親の `file_descriptor_t->file` ポインタをそのまま子にコピー**し、`fs_file->ref_count` をインクリメント。**実体は 1 個のまま、複数の `file_descriptor_t` がそれを指す**。
- これにより親が `read(fd, buf, 100)` でオフセット 100 進めると、子から見たオフセットも 100 に進む。**シェルのリダイレクト (`ls > out.txt`) がパイプラインで動く根本理由**。
- `close(fd)` は `ref_count` をデクリメント。0 になったら `ops->release()` を呼んで実体を解放。

### Lv3 — 典型的な誤解
- AI が「`fork` 時に `fs_file_t` を新規に複製する」と説明したら、**ref_count 増加だけ**であることを `fs_clone_fd` の実装で訂正させる。
- 「`dup2` は新しい `fs_file_t` を作る」は誤り。**既存の `fs_file_t` を別 fd 番号から指すようにする**。ref_count が増える。
- 「`close` で必ず実体が解放される」は誤り。**`ref_count == 0` の時だけ**。`fork` した後で親が close しても、子がまだ参照していれば実体は残る。

---

## クラスタC — `vfs_find_mountpoint` の最長一致 (本書 9.5)

VFS は登録されたマウントポイントの中から、**与えられたパスに最も長くマッチする**ものを返します。これにより `/usb/photos/cat.jpg` は `/usb` バックエンドへ、`/etc/hosts` はルート (`/`) バックエンドへ、と振り分けられます。

### Lv1 — 最初に確認すること
- `kernel/vfs.c:57` 付近の `vfs_find_mountpoint` を読む。
- `vfs_register_mountpoint` で起動時にどんなマウントが登録されるかを `kernel/fs.c` 周辺で grep (`grep -n vfs_register_mountpoint ../kernel/`)。

### Lv2 — それでも進まない場合
- 最長一致の重要性: `/usb` と `/usb_backup` が両方マウントされている時、`/usb_backup/foo` は `/usb` ではなく `/usb_backup` にマッチすべき。
- マウントポイントは**先頭スラッシュを除いた形**で登録される (例: `"usb"`, `"proc"`)。第19章 hints クラスタB と同じ注意点。
- 戻り値の `subpath` 引数は「マウントポイント部分を除いた残り」。`/usb/photos/cat.jpg` を `/usb` にマッチさせると `subpath = "photos/cat.jpg"` になる。

### Lv3 — 典型的な誤解
- AI が「文字列の `strncmp` だけでマッチを判定」と説明したら不十分。**境界 (`/` または末尾) で切れていないと、`/usb` と `/usbfoo` を誤判定する**。実装が境界をチェックしているか確認させる。
- 「ルート `/` は特別扱い」は半分正しい。すべてのパスに最低限マッチするデフォルトとして機能する。

---

## クラスタD — `fs_read` の多態 (本書 9.6)

`fs_read` は `fs_fd_type` で型を判定し、**if 連鎖 (switch ではない)** で各ファイルタイプの処理に振り分けます。コンソール・パイプはインライン、ソケット・xv6fs は専用関数。

### Lv1 — 最初に確認すること
- `kernel/fs.c:215` 付近の `fs_fd_type` を読む。`file_descriptor_t*` から `fs_file_t->type` を取り出すだけ。
- `kernel/fs.c` で `fs_read` 本体を探し (`grep -n "fs_read" ../kernel/fs.c`)、各 `if (type == FT_xxx)` 分岐を確認。

### Lv2 — それでも進まない場合
- **`FT_CONSOLE`**: キーボード入力ループ + シリアルエコー。割り込み待ちと sleep/wake を含むのでインラインが必須。
- **`FT_PIPE`**: パイプバッファからの読み出しと、書き手の wake 通知。これもスリープ制御がインライン。
- **`FT_SOCKET`**: `net_socket_read_fd` を呼ぶ。lwIP との橋渡し。
- **`FT_XV6FS`**: `xv6fs_ilock` → `xv6fs_readi` → `xv6fs_iunlock` の 3 段階。**ロックを取らずに `readi` を呼ぶと壊れる**。
- **`FT_RAMFS`, `FT_USB`, `FT_RAWDEV`, `FT_DIR` 等**: 各バックエンド呼び出し。`include/fs.h:23` 付近の列挙順を確認。

### Lv3 — 典型的な誤解
- AI が「`fs_read` は関数ポインタテーブル (`ops->read`) で多態」と説明したら、**Orthox-64 の実コードは if 連鎖**であることを grep で示す (第6章の `syscall_dispatch` と同じ判断: switch 方が grep しやすい)。`fs_file_ops_t` に `.read` フィールド**は存在しない** (`.release` のみ)。
- 「コンソールの read は単純な `serial_read()` 呼び出し」は誤り。**キーボード+シリアル両方を扱うインラインループ**。

---

## クラスタE — `dup2` / `fork` / `execve` と fd の寿命 (本書 9.7)

3 つのライフサイクルイベントで fd がどう変化するかを区別する。

### Lv1 — 最初に確認すること
- `dup2`: `kernel/sys_fs.c` か `kernel/fs.c` で `sys_dup2` または `fs_dup2` を grep。oldfd の `fs_file_t*` を newfd にコピー、ref_count をインクリメント。
- `fork`: 第8章クラスタA で確認した `fs_clone_fd` を再読。
- `execve`: `kernel/fs.c:358` の `fs_close_cloexec_descriptors` を読む。

### Lv2 — それでも進まない場合
- `dup2(3, 1)` は「fd 1 (stdout) を閉じてから fd 3 を fd 1 として複製」する。シェルのリダイレクト (`cmd > file`) の核心: `open(file)` で fd 3 取得 → `dup2(3, 1)` で標準出力差し替え → `close(3)` → `exec(cmd)`。
- `execve` で fd を**引き継ぐ理由**: シェルが事前にパイプを組んでから exec すれば、新プログラムは何もせず最初からパイプに繋がった状態で起動できる。
- `FD_CLOEXEC` は**例外的に閉じる**ためのフラグ。`fcntl(fd, F_SETFD, FD_CLOEXEC)` で立てる。機密ファイルや管理用 fd に立てて exec で漏らさないようにする。

### Lv3 — 典型的な誤解
- AI が `fs_close_cloexec_descriptors` を `void` 引数で書いたら、**実際は `struct task* task` 引数**であることを `kernel/fs.c:358` で確認させる。
- `i = 3` から始まるループ理由: **標準入出力 (0, 1, 2) は CLOEXEC で閉じない**仕様。これを守らないと exec したプログラムが標準入出力を失う。
- 「`execve` 時にすべての fd が閉じられる」は誤り。**CLOEXEC が立っているものだけ**。残りは引き継がれる。

---

## クラスタF — pipe / socket / device も fd になる (本書 9.8)

`file_type_t` の列挙メンバが「fd の向こう側の正体」のラインナップ。何が fd で表現できるかを把握すると、Unix 哲学の射程が掴めます。

### Lv1 — 最初に確認すること
- `include/fs.h` で `file_type_t` の列挙メンバを全て grep。`FT_UNUSED, FT_CONSOLE, FT_MODULE, FT_RAMFS, FT_PIPE, FT_SOCKET, FT_USB, FT_XV6FS, FT_RAWDEV, FT_DIR` の 10 種。
- 各メンバが使われている箇所を `grep -rn FT_PIPE ../kernel/` 等で巡回。

### Lv2 — それでも進まない場合
- `FT_PIPE`: `pipe()` syscall で**読み込み用 fd と書き込み用 fd の 2 個**を同時に作る。中間にメモリバッファ。
- `FT_SOCKET`: lwIP のソケット構造体を `private_data` に格納。`socket()` syscall で fd を返す。
- `FT_RAWDEV`: `/dev/kout` 等の素朴なデバイスファイル。`debug_write` 直結。
- `FT_MODULE`: Limine がブート時に直接ロードしたファイル (rootfs に含まれる前の状態)。
- `FT_DIR`: `readdir` の応答用に動的に生成されるディレクトリリスト。

### Lv3 — 典型的な誤解
- AI が `file_type_t` のメンバを誤った順序で出したり、`FT_NONE` のような架空のメンバを挙げたら、`include/fs.h:23` 付近の宣言を grep で示す。
- 「すべてのデバイスが `FT_RAWDEV` で表現される」は不正確。**Orthox-64 は素朴な設計**で、コンソールやシリアルは `FT_CONSOLE`、ソケットは `FT_SOCKET` のように専用 type を持つ。Linux のような統一デバイスファイル抽象 (`/dev/*` に何でも入れる) はしていない。

---

## AI への聞き方のコツ

本書 9.11「AI アシスタント」節の質問例をそのまま使うのが基本ですが、以下の追加ルールを守ると精度が上がります。

- **`MAX_FDS = 256`**: 古い値 (64) を AI が出したら grep で訂正。
- **`fs_file_ops_t` に `.read` は無い、`.release` のみ**: 第19章 hints と同じ。AI が `.read` を生やしたコードを出したら却下。
- **`fs_read` は if 連鎖、switch ではない**: 関数ポインタテーブル多態を AI が前提にしたら指摘。
- **`fs_close_cloexec_descriptors(struct task*)`**: void 引数ではない。`i = 3` から開始。
- **`vfs_find_mountpoint` は最長一致**: 単純 strncmp ではなく境界判定が必要。

---

## 関連

- 本章本文: 第9章 (読者の手元書籍)
- 実コード入口: `../kernel/fs.c`, `../kernel/vfs.c`, `../kernel/sys_fs.c`, `../include/fs.h`
- 関連章: 第7章 (`struct task` の fd table), 第8章 (`fork` での fd 引き継ぎと CLOEXEC), 第10章 (xv6fs の `xv6fs_readi`), 第11章 (pipe/socket の実装), 第19章 (procfs を新しい `file_type_t` として追加する例)
