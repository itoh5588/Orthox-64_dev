# 第19章 段階的ヒント — procfs を Orthox-64 に組み込む

本章は **AI のパッチ案を VFS 境界でレビューしながら最小 procfs を組み込む** 章です。手が止まりがちな箇所だけ Lv1 → Lv2 → Lv3 の順に開示するヒントを並べました。**まず本書本文を最後まで読んでから**、詰まった箇所だけここを参照してください。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、実装コードそのものは出していません。完成形が必要なときは答え合わせブランチ `book/ch19-procfs-complete` をチェックアウトしてください。

---

## クラスタA — VFS / fs.h を拡張する（本書 19.3〜19.5）

新しいマウント種別とファイル種別を列挙型に追加する、ごく短い作業です。本書は概念コードで `VFS_MOUNT_PROCFS = 3` と書いていますが、enum の末尾に追加するだけです。

### Lv1 — 最初に確認すること
- `include/vfs.h` の `enum vfs_mount_kind` を grep で開く。既存値は `VFS_MOUNT_NONE / VFS_MOUNT_USB_FAT / VFS_MOUNT_IMAGE_FS` の3つ。
- `include/fs.h` の `file_type_t` を開く。`FT_UNUSED` から `FT_DIR` まで10種類ある。

### Lv2 — それでも進まない場合
- 既存の `VFS_MOUNT_USB_FAT` がどこで参照されているかを `grep -n VFS_MOUNT_USB_FAT kernel/` で追う。**この grep 結果が、自分が追加すべき分岐の「鏡像」になります**。USB FAT が登場するすべての場所に procfs の分岐が必要、というわけではありませんが、ほぼすべてが「ここに何かする可能性がある」候補地です。
- `FT_USB` についても同じ grep を取って、ファイル種別が読み書き経路でどう参照されているかの全体像を掴む。

### Lv3 — 実装方針の概略
- enum の追加は末尾に置く。既存値の番号は変えない（バイナリ互換やデバッグログの読みやすさのため）。
- 値を `3` のように明示するか自動採番に任せるかは好みだが、本書は明示派。

---

## クラスタB — `/proc` をマウント登録する（本書 19.4）

`fs_init()` の中でマウントテーブルに `proc` を登録します。**先頭スラッシュを書かない**点が唯一の落とし穴です。

### Lv1 — 最初に確認すること
- `kernel/fs.c` の `fs_init` 関数を開き、`vfs_register_mountpoint("usb", VFS_MOUNT_USB_FAT, 0);` の行を探す（grep: `vfs_register_mountpoint`）。
- すぐ後ろに同じ形で `proc` 用の1行を追加する場所を決める。

### Lv2 — それでも進まない場合
- なぜ `"proc"` で `"/proc"` ではないのか？ → 第9章で扱った VFS のパス正規化を読み直す。マウントポイントは「先頭 `/` を取り除いた形」で登録される。
- ブート時のシリアル出力に `procfs registered` 等のログを足して、初期化が呼ばれたか目視できるようにすると後段のデバッグが楽。

### Lv3 — 実装方針の概略
- 既存の `VFS_MOUNT_USB_FAT` 登録行の直後または直前に1行追加するだけ。順序は機能に影響しない（前方一致は重複しないため）。

---

## クラスタC — `procfs_open` とバッファ寿命（本書 19.6, 19.8, 19.10）

procfs の本体は新規ファイル `kernel/procfs.c` です。「**1回の open でスナップショットを丸ごと作り、close でまとめて解放する**」というのが本書のキーアイデアです。ここで最も事故が多いのが**バッファ解放のサイズと型**です。

### Lv1 — 最初に確認すること
- `pmm_alloc` と `pmm_free` の宣言を `include/pmm.h` で確認。引数の単位は「ページ数」か「バイト数」か？
- `fs_file_t` の構造を `include/fs.h` で確認。`private_data`, `size`, `offset`, `aux0`, `aux1`, `ops` のフィールドがどう使えるかを把握する。
- 既存の `fs_file_ops_t` に `.read` や `.write` のメンバーがあるか確認 — **無い**。`.release` だけ。AI が `.read` を生やしたコードを出してきたら却下対象。

### Lv2 — それでも進まない場合
- 解放時に必要なのは 2 つの情報：「バッファの**仮想**アドレス」と「**何ページ**確保したか」。本書はこれを `file->private_data`（仮想ポインタ）と `file->aux0`（ページ数）に持たせている。自分の実装でも同じ方針で OK。
- `pmm_free` には**物理アドレス**を渡す必要がある。`VIRT_TO_PHYS` マクロを噛ませる箇所を見落としやすい。
- `procfs_release_file` は二重呼び出しに耐えるか？ ポインタを `0` に戻したか？ ガード `if (!file->private_data)` を入れているか？

### Lv3 — 実装方針の概略
- スナップショット構造は実は不要。バッファ 1 ページを `pmm_alloc(1)` で確保し、ポインタを `file->private_data` に、ページ数 `1` を `file->aux0` に入れるだけで足りる。`file->size` にバイト数を書き、`file->offset = 0`。
- release コールバックは「`private_data` が非 NULL かつ `aux0` が非 0 なら、`VIRT_TO_PHYS` で物理アドレスに戻して `pmm_free` し、ポインタを `0` クリア」する関数。
- `fs_file_t` 自体は別途 `pmm_alloc(1)` で確保し、こちらも開放経路を踏むはずだが詳細は答え合わせブランチを参照。

---

## クラスタD — `fs_open` への分岐挿入と読み出し経路（本書 19.7, 19.9）

カーネル中央 (`kernel/fs.c`) に「procfs だけのための分岐」を挿入する箇所と、**何も書かなくて済む箇所**を見極めるのが要点です。

### Lv1 — 最初に確認すること
- `kernel/fs.c` の `fs_open` を探す（`grep -n "^int fs_open"`）。冒頭でパス正規化と `vfs_find_mountpoint` が走り、`mount` 変数に解決済みマウントが入っているはず。
- 既存の `if (mount && mount->kind == VFS_MOUNT_USB_FAT)` ブロックの形を観察する。これが**そのまま** procfs 用の鏡像になる。
- `fs_read` の中で `FT_USB` や `FT_XV6FS` がどう分岐されているか確認。

### Lv2 — それでも進まない場合
- **読み出しは新規の分岐がいらない可能性**を本書 19.9 が示唆している。`fs_read` の汎用フォールバックは何を見ているか？ `file->private_data` と `file->size` と `file->offset` だけ。procfs_open でこの3つを正しく設定すれば、`fs_read` には**1行も触らなくて済む**。
- 本書の概念コード `task_alloc_fd()` や `fs_file_put()` は**擬似名**。実コードで該当する関数を grep で見つける（既存 USB FAT 経路の fd 取得ロジックを真似する）。
- 本書の概念コード `copy_to_user()` も擬似名。Orthox-64 の汎用 read フォールバックがどうユーザー空間へコピーしているかを `kernel/fs.c` で確認すれば、自分で書く必要すらないことが分かる。

### Lv3 — 実装方針の概略
- `fs_open` に追加するのは「USB FAT 分岐の隣に、`VFS_MOUNT_PROCFS` 用の分岐を1ブロック」だけ。中身は `procfs_open(mount_subpath, flags)` を呼んで返ってきた `fs_file_t*` を fd テーブルに登録する流れ（USB FAT 分岐の登録ロジックを参考にする）。
- `fs_read` は触らない。代わりに `procfs_open` 内で `file->size`, `file->offset = 0`, `file->private_data` を正しく設定する。

---

## クラスタE — 個別ファイルの生成（本書 19.11〜19.14）

`/proc/uptime` → `/proc/meminfo` → `/proc/mounts` → `/proc/tasks` の順で着手するのが本書推奨。**この順番には理由がある**ことを意識してください。

### Lv1 — 最初に確認すること
- `lapic_get_ticks_ms()`、`pmm_get_total_pages()`、`pmm_get_free_pages()`、`vfs_list_mountpoints()` がそれぞれどこに定義されているかを grep。これらは既に揃っているので、procfs 側は呼ぶだけ。
- `snprintf` の戻り値の意味（書き込んだ文字数 or 必要だった文字数）を確認。Orthox-64 のカーネル `snprintf` がどちらの規約か `kernel/printf*.c` で確認。

### Lv2 — それでも進まない場合
- なぜ uptime から始めるか？ → 他サブシステムの状態を参照しない＝ロックも競合もない、最もクリーンな検証ができるから。ここで procfs の「中央幹線」（open → snapshot → read → close）が通れば、残りは生成ロジックの差し替えだけ。
- `/proc/tasks` は**最後**。理由は本書 19.14 が詳述。スピンロック取得 → 走査 → 解放 の枠を必須とする。AI に「task list を走査するコード書いて」と頼むとロックを忘れがちなので、出てきたパッチに `task_lock` 系の呼び出しがあるか必ず確認。
- ロックを取った状態で `snprintf` を回し続けるパッチは**性能的に NG**。ロック中は最小限の情報をローカル配列にコピーし、ロック解放後にフォーマットする設計の方が好ましい（が、最小実装としては許容範囲）。

### Lv3 — 実装方針の概略
- 各 `build_<name>(char* buf, size_t cap)` を 1 関数として書き、`switch (node)` で呼び分ける。
- `/proc/mounts` は `vfs_list_mountpoints` で配列を取り、kind を文字列に変換しつつ各行を `snprintf` で詰める。バッファ上限のクランプを忘れない。
- `/proc/tasks` はまず「pid と name だけのバージョン」で先に通し、状態文字列や CPU 番号は通った後に拡張する方が安全。

---

## クラスタF — 退行確認（本書 19.15）

新機能が動くだけでなく、**既存機能を壊していないこと**を確認して初めて完了です。Orthox-64 には QEMU スモークの仕組みが整っているので活用してください。

### Lv1 — 最初に確認すること
- `ls ../tests/*.sh` で既存スモークを眺める。
- `make -C .. run` で起動し、シェルが立ち上がるか手で確認。

### Lv2 — それでも進まない場合
- 退行が出る場合、`fs_open` に挿入した分岐の **else / fall-through** ロジックが既存パスを壊している可能性が高い。`mount->kind` の判定が漏れて procfs 経路に流れ込んでいないか確認。
- ハングする場合、procfs 内で確保したバッファが解放されず PMM が枯渇している可能性。`/proc/meminfo` で `free_pages` の推移を見ると一発で分かる。

### Lv3 — 実装方針の概略
- 最低限の手順:
  1. `make -C ..` でビルドが通る
  2. `make -C .. run` で起動、シェルプロンプトが出る
  3. `ls /` と `cat /etc/hosts` 相当が壊れていない（USB 経路）
  4. `cat /proc/uptime` で数値が読める
  5. 連続 100 回程度 `cat /proc/uptime` を実行しても `free_pages` が減り続けない（リーク検知）

---

## AI への聞き方のコツ

本書 19.17「AI アシスタント」節の質問例をそのまま使うのも良いですが、以下の追加ルールを守ると精度が上がります。

- 「**実コードを `grep` で確認してから答えて**」と必ず添える。AI は本書の概念コードをそのまま実コードと信じがち。
- パッチ案が出てきたら「**既存関数を丸ごと書き換えていないか**」を最初にチェック。本章のキーは「最小の差分挿入」。
- `fs_file_ops_t` に `.read` を追加するような提案が来たら**即却下**。実コードには `.release` しか無いことを再確認させる。
- ロックを取った状態での `snprintf` 連発は**性能赤旗**。指摘して書き直させる。

---

## 関連

- 答え合わせブランチ: `git fetch && git checkout book/ch19-procfs-complete`
- 全体差分: `git diff main book/ch19-procfs-complete -- kernel/ include/`
- 親プロジェクトの本章本文: 第19章（読者の手元書籍）
