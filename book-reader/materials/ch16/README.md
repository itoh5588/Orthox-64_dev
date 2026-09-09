# Ch16 — GCC とセルフホスト

第16章は本書の到達点。**Orthox-64 上で GCC を動かし、自分自身のカーネルをコンパイルして再起動する**まで。これまでの全ての章 (メモリ、プロセス、ファイルシステム、動的リンク、シグナル、ネットワーク) が **総合試験**として一斉に動きます。読者の関与は **「スモークテストを正しい順序で走らせ、失敗した場合に階層的にデバッグする」** ことが主。

## 章のゴール

* **GCC は単一プログラムではなくドライバー (`user/gcc.c`)** が `cc1` → `as` → `ld` を `fork`/`execve` で順次起動するパイプラインであることを掴む。
* **`scripts/populate_c_env_musl.sh`** が rootfs にヘッダ・libc・crt*.o・ツール本体をコピーして開発環境を組み立てる流れを理解する。
* **`tests/musl_toolchain_smoke.sh` の 4 段階検証** (`-S` → `-c` → リンク → 実行) で問題のレイヤーを切り分ける方法を知る。
* **`tests/native_toolchain_work_smoke.sh`** で相対パス (`cd /work` → `cc test.c`) の VFS 解決が正しく動くか確認する意味を理解する。
* **`tests/native_make_smoke.sh`** で `make` が動くために必要な要件 (mtime、`/bin/sh -c`、終了ステータス伝播) を区別できる。
* **stage1/stage2/stage3 の自己再生産ループ**と「stage2 と stage3 のバイナリ一致 (Binary Identity)」の意味を説明できる。
* **`scripts/Makefile.kernel-native`** のフリースタンディングフラグ (`-ffreestanding`, `-mcmodel=kernel`, `-nostdlib`, `-T kernel.ld`) の役割を Linux 風ユーザービルドと区別できる。
* **`tests/native_kernel_boot_smoke.sh`** が「ビルド → 抽出 → ISO 再パッケージ → boot」の輪廻ループであることを把握する。

## このディレクトリの中身

* [hints.md](hints.md) — 段階的ヒント (Lv1/Lv2/Lv3)。本書の節 (16.2 〜 16.10, 16.13) に対応した6つのクラスタに分かれています。

## 取り組み方の推奨フロー

1. 本書 16.1 〜 16.14 を**通読**してから戻る。図 16.1 〜 16.5 を頭の中で描けるか確認。
2. `scripts/populate_c_env_musl.sh` を全文読み、何が rootfs にコピーされるか把握。
3. `tests/musl_toolchain_smoke.sh` を全文読み、4 段階の検証フェーズを目視。
4. `scripts/Makefile.kernel-native` の `CC1FLAGS` と `KERNEL_LDFLAGS` を読み、フリースタンディング設定を確認。
5. `user/gcc.c` を読み、ドライバーラッパーの最小実装を把握 (cc1/as/ld を fork+exec)。
6. `tests/native_kernel_boot_smoke.sh` の流れを読み、QEMU 内ビルド → ホスト抽出 → ISO 再生成 → 再起動の輪廻を確認。
7. 詰まった項目だけ [hints.md](hints.md) の該当クラスタを Lv1 から段階的に開示。

## 実コードへの飛び込み口

```sh
# GCC ドライバー (Orthox-64 のラッパー)
cat ../user/gcc.c | head -100

# 開発環境セットアップ
cat ../scripts/populate_c_env_musl.sh

# スモークテスト群 (順序付き)
ls ../tests/musl_toolchain_smoke.sh ../tests/native_toolchain_work_smoke.sh ../tests/native_make_smoke.sh \
   ../tests/native_rebuild_gcc_smoke.sh ../tests/native_self_rebuild_gcc_smoke.sh \
   ../tests/native_kernel_build_smoke.sh ../tests/native_kernel_boot_smoke.sh

# カーネルビルド Makefile (フリースタンディング)
grep -nE "CC1FLAGS|KERNEL_LDFLAGS|ffreestanding|mcmodel|nostdlib|kernel\.ld" ../scripts/Makefile.kernel-native

# ホスト側ビルドターゲット (ISO 再生成等)
grep -nE "musltoolchainsmoke|nativekernelbootsmoke|nativeselfrebuildgccsmoke" ../Makefile | head -10
```

## AI への話しかけ方の例

* 「第16章 16.4 で `tests/musl_toolchain_smoke.sh` が `-S` → `-c` → リンク → 実行の 4 段階に分けていた意義を整理して。`/bin/gcc -S hello.c` が成功するが `/bin/gcc -c hello.c` が失敗する場合、どのレイヤー (cc1/as) を疑うべきか説明して」
* 「第16章 16.9 で `-ffreestanding` と `-mcmodel=kernel` と `-nostdlib` の組合せが必須とあった。`scripts/Makefile.kernel-native` を grep で見せて、それぞれが何を意味するか整理して」
* 「第16章 16.10 で native kernel boot smoke が QEMU 内ビルド → 抽出 → ISO 再生成 → 再起動の輪廻だとあった。`tests/native_kernel_boot_smoke.sh` を読み、各ステップで何が成功条件か段階的に教えて」

AI は本書本文をテキストとしては読めないので、章番号と節番号、登場するスクリプト名やフラグ名を添えて質問してください。
