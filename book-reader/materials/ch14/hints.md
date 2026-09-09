# 第14章 段階的ヒント — ELF loader / dynamic link / TLS

本章は **「現代の Linux バイナリが Orthox-64 で動くために必要な全アスタブル要素」** を一気に扱う本書最大の密度の章。手が止まりがちな箇所だけ Lv1 → Lv2 → Lv3 の順に開示してください。**まず本書本文を 14.1 〜 14.14 まで通読**してから戻ることを強く推奨します。

ヒントは「どこを読めば自分で答えに辿り着けるか」を示すもので、答えそのものは出していません。

---

## クラスタA — セクション vs セグメント、プログラムヘッダの役割 (本書 14.2)

ELF には**コンパイル時用の「セクション」と実行ロード時用の「セグメント (プログラムヘッダ)」の 2 系統のメタデータ**があります。OS ローダーが見るのは後者のみ。

### Lv1 — 最初に確認すること
- `include/elf64.h:49-52` で `PT_LOAD = 1`, `PT_INTERP = 3`, `PT_TLS = 7` の値を確認。
- 同ファイルで `Elf64_Phdr` 構造体の宣言を確認 (`p_type`, `p_flags`, `p_offset`, `p_vaddr`, `p_filesz`, `p_memsz`, `p_align` の 8 フィールド)。

### Lv2 — それでも進まない場合
- セクションヘッダ (`.text`, `.data` 等) は**コンパイラ/リンカーが結合作業で使う**もの。ローダーは見ない。
- プログラムヘッダ (`PT_LOAD` 等) は**ローダーが「メモリ配置の指令」として使う**もの。
- なぜ別系統か: コンパイル時の細粒度な情報 (`.rodata` と `.text` の境目等) は実行時には不要。複数のセクションを 1 つの `PT_LOAD` (例: コード+rodata) にまとめて配置することが多い。
- `p_filesz < p_memsz` の場合は BSS 領域あり。ファイル末尾から `p_memsz` までゼロ埋め。

### Lv3 — 典型的な誤解
- AI が「`.text` セクションをロードする」「`.data` セクションをマップする」と説明したら、**ローダーは `PT_LOAD` を見るのであってセクションは見ない**ことを訂正。実行ファイルからセクションヘッダを削除しても動く (`strip` ツールがやること)。
- 「`Elf64_Phdr` のフィールド数は環境依存」は誤り。**ELF 仕様で 8 フィールド固定**。

---

## クラスタB — `PT_LOAD` のページ単位マッピング (本書 14.3)

`PT_LOAD` を仮想アドレス空間にマップする際、**ページ境界の処理と BSS のゼロ埋め**が落とし穴。

### Lv1 — 最初に確認すること
- `kernel/elf.c:58` 付近の `elf_load` を読む。`PT_LOAD` 分岐 (`if (phdr[i].p_type == PT_LOAD)` line 99 付近) のループ。
- `vmm_get_phys` で既存マッピング確認 → 無ければ `pmm_alloc(1)` → `kernel_memset` (ゼロクリア) → `vmm_map_page` → ファイル内容を `kernel_memcpy`、の流れを目視。

### Lv2 — それでも進まない場合
- ページごとに処理する理由: 1 つの `PT_LOAD` セグメントが複数ページにまたがる場合、各ページを独立にマップする。
- **既にマップ済みのページは `update_page_flags` で権限マージ**: 複数のセグメントが同一ページに重なる境界部分 (例: コードと rodata が 1 ページに同居) で、両者の権限が両立するように OR で結合。
- `p_filesz` を超えた領域 (`p_memsz - p_filesz` バイト) は**ゼロ初期化されたまま**残る。これが BSS。

### Lv3 — 典型的な誤解
- AI が「BSS は別の `PT_LOAD` セグメントになる」と説明したら、**同じ `PT_LOAD` 内で `p_filesz < p_memsz` で表現される**ことを訂正。
- 「`p_offset` が常にページ境界に揃っている」は誤り。**バイト単位**。ファイル先頭からの任意のオフセット。`p_vaddr` 側は通常ページ境界。
- ページ境界をまたぐコピーの計算 (`offset_in_page`, `size_in_page`, `bytes_to_copy`) を AI が省くと一見動いてもエッジケースで壊れる。

---

## クラスタC — `ET_EXEC` vs `ET_DYN`、load_bias (本書 14.4)

`ET_EXEC` は固定アドレス、`ET_DYN` は位置独立 (PIE)。後者には `load_bias` を加算してずらします。

### Lv1 — 最初に確認すること
- `kernel/task_exec.c:363` 付近で `exec_load_bias = (ehdr->e_type == ET_DYN) ? EXEC_ET_DYN_LOAD_BASE : 0;` を確認。
- `EXEC_ET_DYN_LOAD_BASE` の値を grep で確認 (`grep -n EXEC_ET_DYN_LOAD_BASE ../kernel/task_exec.c`)。

### Lv2 — それでも進まない場合
- `ET_EXEC`: ELF 内の `p_vaddr` 値がそのまま絶対アドレス。`load_bias = 0`。古典的バイナリ。
- `ET_DYN`: ELF 内の `p_vaddr` 値は「相対オフセット」として解釈、実際の配置は `p_vaddr + load_bias`。PIE バイナリと共有ライブラリ。
- 現代の Linux ディストリビューションのバイナリはほぼ ET_DYN (ASLR でランダム配置するため)。
- 動的リンカー自身も ET_DYN なので、専用ベース `EXEC_INTERP_LOAD_BASE` (= 0x7fc000000000) を使う。メインバイナリ用ベースとは別領域にしないとアドレス衝突。

### Lv3 — 典型的な誤解
- AI が「`ET_DYN` は共有ライブラリ専用」と説明したら誤り。**PIE 実行ファイルも `ET_DYN`**。`file` コマンドで `position-independent executable` と表示される。
- 「`load_bias` はランダム化される」は ASLR の話。**Orthox-64 は ASLR を実装していない** (`EXEC_ET_DYN_LOAD_BASE` 固定値)。AI が ASLR を前提に説明したら指摘。

---

## クラスタD — `PT_INTERP` と動的リンカーの起動 (本書 14.5, 14.6)

`PT_INTERP` セグメントに書かれたパス (例: `/lib/ld-musl-x86_64.so.1`) を**カーネルがファイルシステムから読んで、もう一度 `elf_load` を呼んで別ベースでロード**します。エントリ点はメインバイナリではなく**動的リンカー側**に切り替わります。

### Lv1 — 最初に確認すること
- `kernel/elf.c:84` 付近の `PT_INTERP` 抽出処理。`info.interp_path` バッファに文字列をコピー。
- `kernel/task_exec.c:382` 付近の `interp_info = elf_load(pml4_virt, interp_file_addr, EXEC_INTERP_LOAD_BASE);` 呼び出し。
- 同ファイルで `t->user_entry = info.has_interp ? interp_info.entry : info.entry;` の三項演算子。

### Lv2 — それでも進まない場合
- `PT_INTERP` がないバイナリ = 静的リンク = 動的リンカー不要 → メインバイナリのエントリ直行。
- `PT_INTERP` があるバイナリ = 動的リンク = ld.so の助けが必要 → ld.so のエントリに入る → ld.so が auxv を読んでメインバイナリの場所を知る → 依存ライブラリ (libc.so 等) をロード → 再配置 → メインバイナリのエントリ (`AT_ENTRY`) に jmp。
- `resolve_interp_file` (本書 14.6) はカーネル側のファイル読み込みヘルパ。インタープリタもディスク上のファイルなので普通に `fs_open` 経路で読む。

### Lv3 — 典型的な誤解
- AI が「動的リンカーをロードする処理は libc がやる」と説明したら誤り。**カーネル (`task_execve`) がロード**する。libc は動的リンカーが起動された後でロードされる被リンクライブラリ。
- 「メインバイナリのエントリに最初に jmp する」は静的バイナリの話。**動的バイナリは動的リンカーのエントリに jmp**。これを理解しないと auxv の必要性が腑に落ちない。

---

## クラスタE — 初期ユーザースタックと auxv (本書 14.7, 14.8, 14.9)

ユーザースタックのトップから順に `argv 文字列実体 → envp 文字列実体 → auxv → envp ポインタ配列 → argv ポインタ配列 → argc` の順で積み上げます。**1 バイトでもずれると musl ld.so が起動直後に SIGSEGV**。

### Lv1 — 最初に確認すること
- `kernel/task_exec.c:216` 付近の `task_prepare_initial_user_stack` を読む。
- `stack_write_u64` の呼び出しを連続で grep (`grep -n "stack_write_u64" ../kernel/task_exec.c`)。書き込み順序を目視。
- auxv の各 type 定数: `AT_PHDR = 3`, `AT_BASE = 7`, `AT_ENTRY = 9` 等。`kernel/task_exec.c:203` 付近で確認。

### Lv2 — それでも進まない場合
- スタックは**高位アドレスから低位アドレスへ伸びる**。だから `current_str_addr -= 8;` で減らしながら書き込む。
- 最終的にスタックポインタ (`%rsp`) が指すのは**argc が書かれた位置**。ユーザー空間 main が `argc` を `rdi` から、`argv` を `rsi` から、`envp` を `rdx` から取れるのは、`task_execve` が `frame->rdi/rsi/rdx` を設定しているため (本書 8.7 の最後のコード片)。
- **auxv の終端は `AT_NULL (0)`**。これがないと ld.so が auxv を読み続けて暴走。

### Lv3 — 典型的な誤解
- AI が「auxv は環境変数の延長」と説明したら誤り。**`envp[]` の NULL 終端の次に積まれる別データ**。envp は文字列ポインタ配列、auxv は (type, value) ペア配列。
- 「`AT_PHDR` はファイル内オフセット」は誤り。**仮想メモリ上にマップされた後の仮想アドレス**。`load_bias` を考慮済み。
- AI が auxv の積み込み順を逆 (argc 先) に書いたら、ld.so から見たメモリレイアウトが壊れることを指摘。Lv2 の「高位アドレスから低位へ」を再確認させる。

---

## クラスタF — TLS テンプレートと `PT_TLS` (本書 14.10)

`PT_TLS` は**スレッドローカル変数の初期データのテンプレート**。カーネルはマップするだけ、**実際の per-thread 領域確保とコピーはユーザーランドランタイム (musl libc)** が担当します。

### Lv1 — 最初に確認すること
- `kernel/elf.c:92` 付近の `PT_TLS` 処理。`info.tls_vaddr`, `info.tls_filesz`, `info.tls_memsz`, `info.tls_align` を保存するだけ。
- `task_execve` 内でこれらが `struct task` の `t->tls_vaddr` 等にコピーされる箇所を grep。

### Lv2 — それでも進まない場合
- メインスレッド用の TLS は ld.so が起動初期に `mmap` で確保し、`PT_TLS` テンプレート (filesz バイト) をコピーして残り (memsz - filesz) をゼロ埋めする。
- 追加スレッドを `pthread_create` で作るときも、ld.so/libc が同じ処理を per-thread にやる。
- カーネルがやるのは「PT_TLS の情報を auxv 経由で ld.so に伝える」までで、メモリ確保はしない。

### Lv3 — 典型的な誤解
- AI が「カーネルが per-thread の TLS 領域を確保する」と説明したら誤り。**ユーザーランドの仕事**。
- 「`PT_TLS` セグメントが直接スレッドのアクセス対象になる」は誤り。**テンプレートで、コピー元として 1 回読まれるだけ**。実際のアクセスは ld.so が確保した別領域に対して `%fs:` 経由で行う。

---

## クラスタG — `arch_prctl` と FS base、`dlopen` の境界 (本書 14.11, 14.12)

`%fs:` 経由の TLS アクセスを成立させるには **FS base レジスタにスレッド固有領域のアドレス**を入れる必要があり、これはユーザーから直接書けないので `arch_prctl(ARCH_SET_FS, addr)` syscall を経由します。

### Lv1 — 最初に確認すること
- `kernel/sys_proc.c:46` 付近の `sys_arch_prctl` を読む。`code == ARCH_SET_FS` のときに `wrmsr(MSR_FS_BASE, addr)` を実行。
- `t->user_fs_base = addr;` で task に保存。schedule 時に `task_refresh_cpu_local_msrs_internal` (第7章クラスタD) が同じ値を書き戻す。

### Lv2 — それでも進まない場合
- なぜ `MSR_FS_BASE` を**ユーザーから直接書けない**か: `wrmsr` は特権命令 (Ring 0 のみ)。Ring 3 から実行すると一般保護例外 (#GP) で死ぬ。
- `FS_BASE` を毎回 schedule 時に書き換える理由: task 切替で別スレッドのコンテキストになる → そのスレッドの `user_fs_base` を MSR に反映 → ユーザーが `%fs:0x0` を読むと正しい TLS が見える。
- **`dlopen` はカーネル非関与**: メインプロセス起動後、ユーザー空間の ld.so がライブラリ関数として動く。共有ライブラリのロードは `mmap` syscall を裏で呼ぶだけ。

### Lv3 — 典型的な誤解
- AI が「`dlopen` はカーネルの専用 syscall」と説明したら誤り。**libc の関数 (`dlopen`, `dlsym`)、内部で `mmap` を呼ぶ**だけ。
- 「FS base はカーネルが固定値を入れる」は誤り。**スレッドごと、`arch_prctl` で動的設定**。
- 「`ARCH_GET_FS` は無い、`ARCH_SET_FS` だけ」は誤り。**両方ある** (`kernel/sys_proc.c` で確認可)。

---

## AI への聞き方のコツ

本書 14.15「AI アシスタント」節の質問例をそのまま使うのが基本ですが、以下の追加ルールを守ると精度が上がります。

- **セクションとセグメントを峻別**: ローダーはセグメント (プログラムヘッダ) しか見ない。
- **動的リンカーのロードはカーネルがやる**: libc がやるわけではない。
- **メインバイナリと動的リンカーは別ベース**: `EXEC_ET_DYN_LOAD_BASE` と `EXEC_INTERP_LOAD_BASE` を混同しない。
- **TLS のメモリ確保はユーザーランド**: カーネルは PT_TLS の情報を auxv で伝えるだけ。
- **`arch_prctl` で MSR_FS_BASE を書く必要があるのは特権命令だから**: ハードウェア仕様の説明を AI が省いたら追わせる。
- **`dlopen` はユーザー空間の関数、カーネル syscall ではない**: 起動時動的リンクと混同させない。
- **auxv の終端は `AT_NULL`**: AI が auxv を書くコードで終端を忘れたら指摘。

---

## 関連

- 本章本文: 第14章 (読者の手元書籍)
- 実コード入口: `../kernel/elf.c`, `../kernel/task_exec.c`, `../kernel/sys_proc.c`, `../include/elf64.h`
- 関連章: 第4章 (`PTE_USER`, `PTE_WRITABLE` 等のページ属性), 第7章 (`user_fs_base` の schedule 時書き戻し), 第8章 (`execve` の引数退避と新 PML4 構築), 第15章 (Python/NumPy 起動 — 本章の動的リンク機構が現実に試される章)
