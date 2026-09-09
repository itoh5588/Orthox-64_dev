# Ch10 — xv6fs (V6 の子孫を 64-bit カーネルで読む)

第10章は **MIT xv6 のファイルシステムを Orthox-64 が「実用化魔改造」した姿**を読む章。superblock → inode → directory → bmap (direct/indirect/double/triple) → readi/writei → buffer cache → journal log と、ファイルシステムの全層が登場します。第9章 (VFS) のバックエンドであり、第16章 (GCC セルフホスト) を支える土台。

## 章のゴール

* **inode が「ファイル名以外の全てのメタデータ」**を持ち、ファイル名は単なるラベルであることを理解する。
* **`bmap_lookup` が direct/single/double/triple indirect の 4 階層で論理 → 物理ブロックを変換**する仕組みを掴む。
* **Orthox-64 が xv6 から拡張した点 (ファイル名 14→62 文字、最大ファイル 70KB→16GB、ストレージ層分離)** を区別できる。
* **buffer cache (`xv6buf`) が `int valid; int disk;` の個別フィールドで状態管理**しており、MIT xv6 の `flags` ビットマスクとは別設計であることに気付く。
* **journal log の「全か無か」整合性**が、`xv6log_begin_op` → 書き込み → ホームポジション反映 → `xv6log_recover` の流れで保証されていることを追える。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (10.3 〜 10.10) に対応した6つのクラスタに分かれています。

## 取り組み方の推奨フロー

1. 本書 10.1 〜 10.12 を**通読**してから戻る。図 10.1 〜 10.5 を頭の中で描けるか確認。
2. `include/xv6fs.h` で `XV6FS_NDIRECT = 9` (line 15)、`XV6FS_MAXFILE` (line 19)、`struct xv6fs_dinode`, `struct xv6fs_inode` (line 67/123 付近) を確認。
3. `kernel/xv6fs.c` の `bmap_lookup` 本体を grep で読み、direct/indirect/double/triple の分岐を目視。
4. `kernel/xv6fs.c` の `xv6fs_readi` / `xv6fs_writei` を読み、ブロック境界をまたぐループ処理を確認。
5. `kernel/xv6bio.c` で `struct xv6buf` の宣言と `xv6bread`/`xv6brelse` の実装を読む。
6. `kernel/xv6log.c:121` の `xv6log_recover` と `:141` の `xv6log_begin_op` を読み、ジャーナル領域の使い方を確認。
7. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# inode / dirent / superblock
grep -n "struct xv6fs_dinode\|struct xv6fs_inode\|XV6FS_NDIRECT\|XV6FS_MAXFILE\|XV6FS_DIRSIZ" ../include/xv6fs.h

# inode キャッシュとロック
grep -n "xv6fs_iget\|xv6fs_ilock\|xv6fs_iput\|xv6fs_namei" ../kernel/xv6fs.c

# bmap と readi/writei
grep -n "bmap_lookup\|xv6fs_readi\|xv6fs_writei" ../kernel/xv6fs.c

# buffer cache
grep -n "struct xv6buf\|xv6bread\|xv6brelse" ../kernel/xv6bio.c ../include/xv6fs.h

# journal log
grep -n "xv6log_begin_op\|xv6log_end_op\|xv6log_write\|xv6log_recover" ../kernel/xv6log.c
```

## AI への話しかけ方の例

* 「第10章 10.7 で trip indirect を入れて最大 16GB にしているとあった。`include/xv6fs.h:15` の `XV6FS_NDIRECT = 9` と `:19` の `XV6FS_MAXFILE` 定義を grep で示して、計算 `9 + 256 + 256² + 256³` が成り立っているか確認したい」
* 「第10章 10.9 で buffer cache の `xv6buf` が `int valid; int disk;` の個別フィールドとあった。MIT xv6 の `flags` ビットマスク (`B_VALID` / `B_DIRTY`) との違いを `kernel/xv6bio.c` で確認したい」
* 「第10章 10.10 で `xv6log_recover` がブート時にログから本領域へ書き戻すとあった。`kernel/xv6log.c:121` の本体を grep で見せて、確定フラグ判定の有無で roll-forward / discard を分岐しているか確認したい」

AI は本書本文をテキストとしては読めないので、章番号と節番号、登場するシンボル名を添えて質問してください。
