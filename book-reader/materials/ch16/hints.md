# 第16章 段階的ヒント — GCC とセルフホスト

本章は **「OS の全層が同時に試される総合試験」** の章で、読者が手を動かす対象は主に**スモークテストの実行と階層的デバッグ**です。手が止まりがちな箇所だけ Lv1 → Lv2 → Lv3 の順に開示してください。**まず本書本文を 16.1 〜 16.14 まで通読**してから戻ることを強く推奨します。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、答えそのものは出していません。

---

## クラスタA — GCC は単一プログラムではない (本書 16.2)

`/bin/gcc` の実体は `user/gcc.c` の**軽量ドライバー**。実際のコンパイル本体は `cc1`、アセンブルは `as`、リンクは `ld`。これらを `fork+execve` で順次起動するパイプライン。

### Lv1 — 最初に確認すること
- `user/gcc.c` の全体構造を眺める。`fork`, `execve`, `wait4` の呼び出しを grep。
- rootfs 上のバイナリ配置を確認: `/bin/gcc`, `/bin/cc1`, `/bin/as`, `/bin/ld` (本書 16.3 の `populate_c_env_musl.sh` を参照)。

### Lv2 — それでも進まない場合
- ドライバーがやること: (1) `argv` を解析、(2) `/tmp/ccXXXXXX.s` のような一時ファイル名を決める、(3) `cc1` を `execve` してソース → アセンブリ生成、(4) `as` を `execve` してアセンブリ → オブジェクト、(5) `ld` を `execve` してオブジェクト → 実行ファイル、(6) `/tmp` の中間ファイルを `unlink`。
- 各段階の出力ファイルは `/tmp` 配下に作られる → 次の段階の入力になる。ファイルシステムが中継役。
- ドライバーが `wait4` で各子プロセスの終了を待つので、一段でも失敗するとパイプライン全体が止まる。
- カーネル視点では: 1 回の `gcc hello.c` 呼び出しで、`fork` 3-4 回、`execve` 3-4 回、`open` 数十回、`mmap` 多数、`unlink` 数回。**プロセス管理と FS のストレステスト**。

### Lv3 — 典型的な誤解
- 「`gcc` が自身でソースを字句解析してアセンブリを出す」は誤り。**`gcc` は何もコンパイルしない**。指揮者役。
- AI が「ドライバーは Python や Bash 製スクリプト」と説明したら誤り。**C で書かれた小さなネイティブバイナリ** (`user/gcc.c` → ELF)。
- 「中間ファイル `/tmp/ccXXXXXX.s` はメモリパイプ経由で渡される」は誤り。**実際のディスクファイル** (xv6fs 上)。これが故に xv6fs の書き込み性能と整合性が試される。

---

## クラスタB — rootfs に開発環境を置く (本書 16.3)

`scripts/populate_c_env_musl.sh` が rootfs に必要な全ファイル (ヘッダ・ライブラリ・スタートアップ・バイナリ) をコピー。これが揃わないと GCC は動きません。

### Lv1 — 最初に確認すること
- `scripts/populate_c_env_musl.sh` 全文を読む。
- コピー先パス: `/usr/include/`, `/usr/lib/`, `/usr/bin/`。これらが `cc1` や `ld` のデフォルト検索パス。

### Lv2 — それでも進まない場合
- **`/usr/include/`**: musl libc のヘッダ (`stdio.h`, `string.h` 等)。`#include <stdio.h>` がここを引く。
- **`/usr/lib/libc.a`**: musl の静的ライブラリ。`ld` がリンク時に展開して実行ファイルに組み込む。
- **`/usr/lib/crt1.o`, `crti.o`, `crtn.o`**: スタートアップオブジェクト。`crt1.o` が `_start` を提供し、`main` を呼ぶ。`crti.o` / `crtn.o` は init_array / fini_array の枠組み。
- **`/usr/bin/gcc`, `/usr/bin/cc1`**: GCC 4.7.4 をホスト側で `--target=x86_64-linux-musl` でビルドしたもの。クロスコンパイラとして Orthox-64 用バイナリを生成できる。
- **`/usr/bin/as`, `/usr/bin/ld`**: binutils 2.26 から取り出した GNU as と GNU ld。
- これらは本書のホスト側ビルドで `ports/gcc-4.7.4/build-musl/` 等にビルドされ、`populate_c_env_musl.sh` で rootfs にコピーされる。

### Lv3 — 典型的な誤解
- AI が「`/usr/include` はカーネルが自動生成」と説明したら誤り。**rootfs スクリプトがホストからコピー**。
- 「`libc.a` は動的リンク用」は誤り。`.a` は**静的アーカイブ**。`.so` (動的) と区別。
- `crt1.o` の役割を AI が説明できなかったら、第14章 hints クラスタE (auxv) との関連を思い出させる。`crt1.o` の `_start` が auxv を読み、main を呼ぶ。

---

## クラスタC — `musl_toolchain_smoke.sh` の 4 段階検証 (本書 16.4)

スモークテストが `-S` → `-c` → リンク → 実行の **4 段階に分けてある理由**を理解すると、デバッグ時に「どのレイヤーを疑うか」が分かります。

### Lv1 — 最初に確認すること
- `tests/musl_toolchain_smoke.sh` 全文を読む。
- 4 段階それぞれが何を検証するか目視: (1) `gcc -S` (cc1 だけ走る → アセンブリ出力)、(2) `gcc -c` (as も走る → .o 出力)、(3) `gcc` (ld も走る → 実行ファイル)、(4) 実行 (loader → ld.so → main)。

### Lv2 — それでも進まない場合
- **段階 1 失敗** = cc1 のバグ or rootfs ヘッダ不足。`#include <stdio.h>` が見つからない等。`populate_c_env_musl.sh` の `/usr/include/` 部分を疑う。
- **段階 1 成功・段階 2 失敗** = as のバグ or `/tmp` 書き込み問題。cc1 がアセンブリを出力できても、as がそれを読んで .o を出せない。xv6fs の `writei` が大きなファイルを扱えるかチェック。
- **段階 2 成功・段階 3 失敗** = ld のバグ or libc.a/crt*.o 配置問題。リンク段階で巨大な `libc.a` (数 MB) を `mmap` するので、第4章の VMM のストレステスト。`/usr/lib/` 内容を再確認。
- **段階 3 成功・段階 4 失敗** = 生成された実行ファイルの問題。動的リンカーが起動できない、auxv が壊れている等。第14章の問題に降りる。
- この**「切り分けの効くテスト設計」**こそが本書のキモ。エラーレベルだけで原因レイヤーが推定できる。

### Lv3 — 典型的な誤解
- 「4 段階は一度にビルドできるので分ける必要なし」は誤り。**デバッグ時の切り分けこそが目的**。production では `gcc hello.c -o hello` で十分。
- 「段階 3 で失敗したら段階 2 を疑う」は誤り。**段階 3 で失敗 = ld を疑う**。段階 2 で生成した .o は正しいことが前段で証明済み。

---

## クラスタD — `make` が動くために必要なもの (本書 16.6)

`make` を動かすには **mtime の精度・`/bin/sh -c` のシェル起動・終了ステータスの正確な伝播**の 3 つが揃っている必要があります。

### Lv1 — 最初に確認すること
- `tests/native_make_smoke.sh` を読む。`make` で Makefile をビルドして結果を確認。
- `kernel/sys_fs.c` で `sys_newfstatat` / `sys_stat` を grep。mtime を返す関数。

### Lv2 — それでも進まない場合
- **mtime**: `make` は「ソースが出力より新しいか」を `mtime` 比較で判定。`stat()` syscall が秒精度以上で `st_mtime` を返さないと、毎回全リビルドになるか、逆に必要なリビルドをスキップしてバグる。
- **`/bin/sh -c "command"`**: `make` の各レシピは内部でシェル起動。直接 `execve` ではない理由は**リダイレクト・パイプ・変数展開をシェルに任せたいから**。`make all` → `make` が `fork+exec("/bin/sh", "-c", "gcc hello.c")` → シェルが `fork+exec("gcc", "hello.c")`。多段の `fork+exec` がカーネルを酷使。
- **終了ステータス**: パイプライン中で `gcc` がエラー終了 (exit 1) すると、`sh` が同じ 1 を返し、`make` が `wait4` で受け取って `make` 自身が 1 で終了。途中の伝播が狂うと「ビルドが成功しているように見えて実際は失敗」になる。

### Lv3 — 典型的な誤解
- AI が「`make` は直接 `execve` でコマンドを実行」と説明したら部分的に誤り。**実際は `/bin/sh -c` 経由**。シェル起動を経由するからこそ複雑なシェル構文が使える。
- 「`mtime` がナノ秒精度でないと `make` は壊れる」は誤り。**秒精度でも動く** (古典的な `make` の仕様)。ただし高速な再ビルドで秒以下の差が出るとバグる可能性。
- `sys_wait4` の戻り値解釈 (`status` の上位ビットが exit code) を AI が混同したら第8章クラスタF を再確認させる。

---

## クラスタE — stage1/stage2/stage3 と Binary Identity (本書 16.8)

セルフホストの究極証明は **「stage2 = stage3」のビット一致 (Binary Identity)**。Orthox-64 の `tests/native_self_rebuild_gcc_smoke.sh` は簡略版でこれを確認します。

### Lv1 — 最初に確認すること
- `tests/native_self_rebuild_gcc_smoke.sh` を読む。`gcc_stage1` でビルドして `gcc_stage2` を作る流れ。
- 図 16.3 (stage 自己再生産) を頭の中で描く。

### Lv2 — それでも進まない場合
- **stage1**: ホストでクロスビルドした GCC (rootfs 配置の `/bin/gcc`)。これで Orthox-64 内の GCC ソースをコンパイルして `gcc_stage1` を作る (これは最初の自己ビルドの種)。
- **stage2**: `gcc_stage1` を使って同じソースをもう一度ビルド。**自分で作った GCC で自分のソースを再コンパイル**。
- **stage3**: `gcc_stage2` を使ってもう一度ビルド。**stage2 とビット一致するなら不動点 (Fixed Point)**。
- 一致しないと: コンパイラ自身にバグがある、または乱数・タイムスタンプ・パス情報がバイナリに埋め込まれている (例: `__DATE__` マクロ)。GCC は `-frandom-seed` 等で抑制可能だが、本書では stage 1-2 までで終了。

### Lv3 — 典型的な誤解
- AI が「stage1 が動けばセルフホスト完成」と説明したら不十分。**stage2 が完成して初めて「自己ビルド可能」**。理想的には stage3 でビット一致 (本書 16.8 がこの点に言及)。
- 「stage は無限に続く」は誤り。**stage3 で十分** (stage2 = stage3 なら不動点)。

---

## クラスタF — フリースタンディングカーネルビルド (本書 16.9, 16.10)

ユーザー空間ビルドとは違う **`-ffreestanding`, `-mcmodel=kernel`, `-nostdlib`, `-T kernel.ld`** の 4 大フラグでカーネルを組み立てます。最後に `tests/native_kernel_boot_smoke.sh` で「QEMU 内ビルド → ホスト抽出 → ISO 再生成 → 再起動」の輪廻ループを回します。

### Lv1 — 最初に確認すること
- `scripts/Makefile.kernel-native` で `CC1FLAGS` と `KERNEL_LDFLAGS` を確認。
- `tests/native_kernel_boot_smoke.sh` を読む。4 ステップの流れ。

### Lv2 — それでも進まない場合
- **`-ffreestanding`**: 標準ライブラリ (`printf` 等) を仮定しない。`cc1` が組み込み最適化で `printf("%s", x)` を `puts(x)` に置き換える等を抑制。カーネルでは libc が無いのでこの最適化が壊れる。
- **`-mcmodel=kernel`**: カーネル空間 (高位アドレス、`0xFFFFFFFF80000000` 以降) に最適化したアドレッシング。`mov $absaddr, %rax` のような 64-bit リテラルロードが生成される。デフォルト (`-mcmodel=small`) は 2GB 以内のアドレスを仮定するので、カーネル空間で動かない。
- **`-nostdlib`**: `crt1.o`, `crti.o`, `libc.a` をリンクしない。カーネルにはこれらが不要 (`_start` ではなく Limine が指定するエントリ)。
- **`-T kernel.ld`**: 自作リンカースクリプトでセクション配置を制御。`.text` を `0xFFFFFFFF80100000` から、`.bss` を続けて、等。
- 4 ステップ輪廻: (1) QEMU 内で `make -f Makefile.kernel-native` 実行 → `/kernel.elf` 生成、(2) ホスト側が `qemu-img` で抽出、(3) ホスト側で Limine + 新 ISO 再生成、(4) 新 ISO で QEMU 起動 → プロンプト到達確認。

### Lv3 — 典型的な誤解
- AI が「カーネルビルドでも `-lc` でリンクできる」と書いたら誤り。**`-nostdlib` 必須**。libc は Ring 3 用、カーネルは Ring 0。
- 「`-T kernel.ld` がなくてもデフォルトリンカースクリプトで動く」は誤り。**カーネル特有のセクション配置 (高位アドレス) はデフォルトで生成不可**。
- 「`-mno-red-zone` は最適化抑制」は半分正しい。**より重要なのはカーネルが割り込みで `rsp - 128` を上書きするから**。ユーザーランドの red-zone 規約はカーネルで成立しない。

---

## AI への聞き方のコツ

本書 16.15「AI アシスタント」節の質問例をそのまま使うのが基本ですが、以下の追加ルールを守ると精度が上がります。

- **GCC ドライバーと cc1 の分離を必ず**: AI が「gcc が直接機械語を出す」と書いたら指摘。
- **rootfs スクリプトを実コードベースとして引かせる**: `scripts/populate_c_env_musl.sh` の中身が情報の根拠。
- **4 段階スモークテストの意義**: デバッグ切り分けが本質。production 用テストではない。
- **`-ffreestanding`, `-mcmodel=kernel`, `-nostdlib`, `-T kernel.ld` の 4 フラグセット**: 1 つでも欠けるとカーネルは作れない。
- **stage2 = stage3 の Binary Identity**: セルフホスト不動点の数学的意味。
- **/tmp と xv6fs の連動**: GCC は /tmp に大量の一時ファイルを書く。第10章 (xv6fs) の writei と journal log を再確認。

---

## 関連

- 本章本文: 第16章 (読者の手元書籍)
- 実コード入口: `../user/gcc.c`, `../scripts/populate_c_env_musl.sh`, `../scripts/Makefile.kernel-native`, `../tests/musl_toolchain_smoke.sh`, `../tests/native_*.sh`
- ホスト側ビルドターゲット: `../Makefile` 内の `musltoolchainsmoke`, `nativekernelbootsmoke`, `nativeselfrebuildgccsmoke` 等
- 関連章: 第8章 (`fork`/`exec`/`wait` を massive に呼ぶ), 第10章 (xv6fs に大量の一時ファイル書き込み), 第14章 (ELF/動的リンク — 生成された実行ファイルが ld.so 経由で起動), 第4章 (`brk`/`mmap` を GCC が酷使)
