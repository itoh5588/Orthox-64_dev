# 第10章 段階的ヒント — xv6fs / inode / journal log

本章は **「MIT xv6 のファイルシステム設計を頭に入れつつ、Orthox-64 が実用化のために加えた魔改造の差分を見抜く」** 章です。手が止まりがちな箇所だけ Lv1 → Lv2 → Lv3 の順に開示してください。**まず本書本文を 10.1 〜 10.12 まで通読**してから戻ることを強く推奨します。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、答えそのものは出していません。

---

## クラスタA — superblock と on-disk layout (本書 10.3)

ディスクの先頭から数えて「ブート用 → superblock → log → inode 領域 → bmap → data 領域」が固定配置されている、という地図を頭に持ちましょう。

### Lv1 — 最初に確認すること
- `include/xv6fs.h` で `struct xv6fs_superblock` の定義を grep。`magic`, `size`, `nblocks`, `ninodes`, `nlog`, `logstart`, `inodestart`, `bmapstart` の 8 フィールド。
- `kernel/xv6fs.c` で `xv6fs_mount_storage` を grep。第 1 ブロック (block 1) から superblock を読む。

### Lv2 — それでも進まない場合
- block 0 はブートローダ予約 (xv6 由来の規約)。Orthox-64 では使われていないが、xv6 互換のためにスキップ。
- `magic = 0x10203040` がマウント時の整合性チェック。これが合わないと「xv6fs ディスクではない」と判定。
- 各領域の開始ブロック番号 (`logstart`, `inodestart`, `bmapstart`) が superblock に書かれているので、**ディスクサイズが異なっても同じコードで読める**。

### Lv3 — 典型的な誤解
- AI が「superblock は block 0」と説明したら誤り。**block 1** (ブート予約のため)。
- 「superblock は複数ブロックを跨ぐ」は誤り。1 ブロック (1024 バイト) に収まる小さな構造体。

---

## クラスタB — inode という Unix の中心概念 (本書 10.4)

**inode がファイルの実体**であり、ファイル名は単なるラベル。`dinode` (ディスク上 60 バイト) と `inode` (メモリ上キャッシュ) の 2 種類を区別しましょう。

### Lv1 — 最初に確認すること
- `include/xv6fs.h:67` 付近の `struct xv6fs_dinode` (60 バイト) と `:123` 付近の `struct xv6fs_inode` (メモリ上) の宣言を読む。
- 共通フィールド: `type`, `major`, `minor`, `nlink`, `size`, `addrs[XV6FS_NDIRECT + 3]`。メモリ側のみ: `dev`, `inum`, `ref`, `lock`, `valid`。

### Lv2 — それでも進まない場合
- `addrs[]` のサイズは `XV6FS_NDIRECT + 3 = 12`。最初の 9 個が direct、後の 3 個が single/double/triple indirect 用。
- `xv6fs_iget(dev, inum)` がメモリキャッシュから inode を探す or 確保。返り値の `xv6fs_inode*` をすぐ使うのではなく、`xv6fs_ilock(ip)` で**スピンロック取得 + ディスクから dinode を読み込み (valid=1 にする)**。
- `xv6fs_iput(ip)` で `ref` をデクリメント。`ref==0` でメモリスロットが解放可能になる。
- `nlink` (ハードリンク数) がディスク上 dinode に保存されている。`unlink` で 0 になり、ref==0 になったタイミングで実体破棄。

### Lv3 — 典型的な誤解
- 「ファイル名は inode の中に保存されている」は誤り。**ファイル名はディレクトリエントリの中**。inode は名前を一切知らない。これがハードリンク (1 inode に複数名) を成立させる根拠。
- `dinode` のサイズが 60 バイト固定 (ブロック 1024 バイトに 17 個収まる) という事実を AI が忘れがち。`xv6fs_dirent` の 64 バイトと混同しやすいので注意。

---

## クラスタC — directory はファイル名と inode 番号の表 (本書 10.5)

ディレクトリはカーネルだけが書き換える特別なファイル。中身は `xv6fs_dirent` (inum 2 バイト + name 62 バイト = 64 バイト) の配列。

### Lv1 — 最初に確認すること
- `include/xv6fs.h` で `XV6FS_DIRSIZ` (= 62) と `struct xv6fs_dirent` を grep。
- `kernel/xv6fs.c` で `xv6fs_namei` を grep。パス名解決の本体。

### Lv2 — それでも進まない場合
- MIT xv6 は `XV6FS_DIRSIZ = 14` でファイル名 14 文字制限。**Orthox-64 は 62 文字に拡張** (本書 10.5 の魔改造)。1024 バイトブロックに 16 エントリ (1024/64) が綺麗に収まる。
- `xv6fs_namei("/bin/sh")` の動き: ルート inode (inum=1) を開く → ディレクトリデータを `readi` で先頭から走査 → "bin" 一致 → inum 取得 → 次の inode を開く → "sh" 一致 → 最終 inode 返却。
- ディレクトリエントリの先頭 2 バイトが `inum`、次が `name`。`inum == 0` のエントリは「空き枠」。

### Lv3 — 典型的な誤解
- AI が `XV6FS_DIRSIZ = 14` (MIT xv6 値) と答えたら、`include/xv6fs.h` を grep して **Orthox-64 では 62** であることを訂正させる。
- 「ディレクトリエントリは可変長 (ext2 風)」は誤り。**xv6fs は固定 64 バイト**。エントリの長さフィールドはない。

---

## クラスタD — bmap と 4 階層インダイレクト (本書 10.6, 10.7)

**論理ブロック番号 → 物理ブロック番号の変換**が `bmap_lookup` の責務。Orthox-64 の核心拡張がここにあります。

### Lv1 — 最初に確認すること
- `kernel/xv6fs.c` で `bmap_lookup` を grep。`alloc` 引数の有無で挙動が変わる。
- `include/xv6fs.h:19` の `XV6FS_MAXFILE = NDIRECT + NINDIRECT + NDINDIRECT + NTINDIRECT` で最大ファイルサイズが決まる。

### Lv2 — それでも進まない場合
- **direct**: `addrs[0..8]` の 9 個が物理ブロック番号を直接持つ。最大 `9 × 1024 = 9KB` のファイルまで。
- **single indirect (`addrs[9]`)**: 1 物理ブロックに 256 個 (1024/4) のブロック番号 → +256KB。
- **double indirect (`addrs[10]`)**: 1 物理ブロックに「single indirect ブロック」を 256 個 → +64MB。
- **triple indirect (`addrs[11]`)**: 1 物理ブロックに「double indirect ブロック」を 256 個 → +16GB。
- `alloc=1` (write 時) では、たどる途中で未割当のブロックがあれば**フリービットマップから空きを取って割当 + addrs に記入**。
- `alloc=0` (read 時) で未割当なら 0 を返し、`xv6fs_readi` がゼロ埋めする (スパースファイル対応)。

### Lv3 — 典型的な誤解
- AI が「triple indirect は xv6 デフォルト」と説明したら誤り。**MIT xv6 は single indirect まで**。Orthox-64 の拡張。
- 計算: `9 + 256 + 256² + 256³ = 9 + 256 + 65,536 + 16,777,216 ≒ 16,843,017 ブロック ≒ 16.06 GB`。AI が違う数値を出したら検算させる。
- 「addrs[12] 以降はある」は誤り。**配列サイズは `NDIRECT + 3 = 12` 固定**。

---

## クラスタE — `readi` / `writei` とバッファキャッシュ (本書 10.8, 10.9)

`xv6fs_readi` と `xv6fs_writei` がブロック境界をまたぐループで `bmap_lookup` → `xv6bread` → memcpy する。バッファキャッシュ `xv6buf` が物理 I/O を吸収します。

### Lv1 — 最初に確認すること
- `kernel/xv6fs.c` の `xv6fs_readi` を読む (本書 10.8 のコード片と diff)。`bmap_lookup` → `xv6bread` → `memcpy` → `xv6brelse` のループ。
- `kernel/xv6bio.c` の `struct xv6buf` 宣言を grep。`int valid; int disk;` の個別フィールド (`flags` ビットマスクではない)。

### Lv2 — それでも進まない場合
- `xv6bread(dev, blockno)`: メモリ上の LRU キャッシュを検索 → 既にあれば即返却、なければ `storage_read_blocks` で物理ディスクから読み → 返却。返り値のバッファは「使用中」マーク (refcnt++)。
- `xv6brelse(bp)`: refcnt-- でキャッシュを「解放可能」状態に戻す。LRU リストの先頭に移動。
- `xv6fs_writei`: 同じループを書き込み版で。ジャーナル経由で `xv6log_write` を呼ぶことで、整合性を保ったまま更新。
- `valid` = ディスクから読み込み済み、`disk` = ディスク転送中 (排他用)。MIT xv6 の `B_VALID`, `B_DIRTY` ビットフラグを個別変数に分解した形。

### Lv3 — 典型的な誤解
- AI が `struct buf` (xv6 の名前) と書いたら、**Orthox-64 では `struct xv6buf`** に rename されていることを確認させる。
- 「`xv6bread` でディスクから直接データを返す」は誤り。**メモリキャッシュ経由**。直接 I/O ではない。
- `flags` ビットマスクの code パターンを AI が出したら、**`int valid; int disk;` の個別フィールド**で代替されていることを `kernel/xv6bio.c` で訂正させる。

---

## クラスタF — journal log と crash consistency (本書 10.10)

ファイルシステムの**「全か無か」整合性保証**が log layer の仕事。`xv6log_begin_op` → 更新群 → `xv6log_end_op` (commit) → ホーム反映、というトランザクション構造で、途中でクラッシュしても次回起動時に `xv6log_recover` が回復します。

### Lv1 — 最初に確認すること
- `kernel/xv6log.c:121` 付近の `xv6log_recover` を読む。
- `kernel/xv6log.c:141` 付近の `xv6log_begin_op` を読む。

### Lv2 — それでも進まない場合
- log 領域は superblock の `logstart` から `nlog` ブロック分。専用予約。
- 更新を伴う syscall (write, create, unlink 等) は `xv6log_begin_op` でトランザクション開始 → 更新は実際には log 領域に書き込み → `xv6log_end_op` で commit (確定フラグ書き込み) → 確定後にホームポジションへコピー。
- クラッシュ時: (1) 確定フラグ未書き込みなら全破棄 (rollback)、(2) 確定済みなら未反映分をホームに書き戻し (roll-forward)。これが `xv6log_recover` の役割。
- 複数同時操作は **グループコミット**: 複数 op を 1 つのトランザクションにまとめて 1 回の commit で済ませる。

### Lv3 — 典型的な誤解
- AI が「log は ext4 の journal と同じ」と説明したら部分的に正しいが、**xv6 の log は data も含むフルジャーナル**。ext4 のデフォルト (metadata only) より強い保証。
- `recover_from_log` (内部 static 関数) を呼ぶよう説明したら、**パブリック API は `xv6log_recover()`** であることを訂正。
- 「log 領域がいっぱいになったらディスクフル」は誤り。**commit でホーム反映後 log は空に戻る**。log は循環バッファ。

---

## AI への聞き方のコツ

本書 10.13「AI アシスタント」節の質問例をそのまま使うのが基本ですが、以下の追加ルールを守ると精度が上がります。

- **MIT xv6 と Orthox-64 の差分を意識**: ファイル名上限、最大ファイルサイズ、buffer フラグ表現で混同しやすい。
- **`struct xv6buf` (rename) を `struct buf` と書かない**。
- **`XV6FS_DIRSIZ = 62`**（MIT xv6 の 14 ではない）。
- **`XV6FS_NDIRECT = 9`**、`addrs[]` の長さは `NDIRECT + 3 = 12`。
- **`xv6log_recover` がパブリック API**。

---

## 関連

- 本章本文: 第10章 (読者の手元書籍)
- 実コード入口: `../kernel/xv6fs.c`, `../kernel/xv6bio.c`, `../kernel/xv6log.c`, `../kernel/storage.c`, `../include/xv6fs.h`
- 関連章: 第9章 (`fs_read` から `xv6fs_readi` への分岐), 第16章 (GCC が大量の一時ファイルを `xv6fs_writei` で書き込み、journal が酷使される)
