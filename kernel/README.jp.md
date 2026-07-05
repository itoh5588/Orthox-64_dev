# Orthox-64 Kernel Source Structure

このディレクトリには、Orthox-64 カーネルのコアソースコードが含まれています。

## ファイル一覧

### コアと初期化
- **`init.c`**: カーネルのエントリポイント兼初期 bring-up シーケンス。Limine から制御を受け取り、メモリ管理、GDT/IDT、割り込み、syscall、タスク管理、PCI、ネットワーク、USB を順に初期化し、最初のユーザープロセスを起動します。
- **`elf.c`**: ELF ローダー。初回ユーザータスクの起動と、その後の `execve()` 系の実行ファイルロードで共通に使われます。
- **`kassert.c`**: カーネル全体で使う `KASSERT()` / `KBUG_ON()` と `kernel_panic()` の実装。失敗時は CPU を停止し、式・関数名・ファイル・行番号を serial に出力します。

### メモリ管理
- **`pmm.c`**: 物理メモリマネージャー。空き物理ページを追跡してページ単位の割り当て・解放を提供し、COW の基盤となるページ参照カウントも保持します。
- **`vmm.c`**: 仮想メモリマネージャー。x86_64 の 4 段ページテーブルを構築・更新し、ユーザー/カーネル空間のマッピング、アドレス空間切り替え、COW を含むページフォルト処理を担当します。

### CPU セットアップと割り込み
- **`gdt.c` / `gdt_flush.S`**: GDT と TSS の設定。カーネル/ユーザー間の特権遷移に必要な基盤を構成します。
- **`idt.c` / `interrupt.S`**: IDT の設定と割り込み・例外エントリの低レベル stub。legacy PIC IRQ と MSI/MSI-X vector を中央の dispatcher へルーティングします。
- **`lapic.c`**: Local APIC タイマーと時刻取得まわり。スケジューラや `lwIP` の timeout 処理に使われます。
- **`pic.c`**: 旧来の 8259 PIC の制御。IRQ ルーティング互換を支えます。
- **`irq.c`**: legacy IRQ と MSI/MSI-X vector の中央ディスパッチ。各ドライバは init 時にここへ handler を登録し、IDT 層はこのモジュールへ委譲します。

### 同期 / 待機基盤 / SMP
- **`spinlock.c`**: spinlock プリミティブ、IRQ 保存/復帰補助、およびカーネル粗粒度区間を直列化するグローバルカーネルロックを実装します。
- **`wait.c`**: `wait_queue` と `completion` API。`wait_event()`、`wait_event_timeout()`、`wake_up_one()`、`wake_up_all()`、および割り込み駆動 I/O が使う `complete()` 系を提供します。
- **`bottom_half.c`**: deferred work キュー。IRQ ハンドラから軽量 callback を enqueue し、idle 経路で実行することで割り込みコンテキストを短く保ちます。
- **`smp.c`**: SMP bring-up。AP の起動、per-CPU state のセットアップ、プロセッサ間通信補助を提供します。

### タスク管理とスケジューラ
- **`task.c` / `task_switch.S`**: task object のライフサイクル、run queue プリミティブ、task の確保・reap、低レベルなコンテキスト切り替えを所有します。
- **`sched.c`**: スケジューラポリシー。per-CPU run queue 管理、fork-spread の CPU 選択、タイマー tick、resched 要求、`schedule()`、idle loop を担当します。
- **`task_exec.c`**: `execve()` の実装。ELF イメージ置換、初期ユーザースタック構築、`argv`/`envp`/`auxv` の配置を行います。
- **`task_fork.c`**: `fork()` の実装。COW PML4 コピー、fd 複製、シグナルハンドラコピー、子 syscall frame の構築を行います。
- **`task_internal.h`**: `task.c` と `task_*.c` 兄弟ファイル間で共有するヘッダ。`struct syscall_frame` と内部 helper の prototype を所有します。

### Syscall ディスパッチと実装
- **`syscall.c` / `syscall_entry.S`**: SYSCALL エントリ stub、MSR 初期化、および C 側 dispatcher。Linux 互換 syscall 番号と Orthox 私的 syscall 番号をカテゴリ別の実装ファイルへルーティングします。
- **`sys_time.c`**: `clock_gettime`、`gettimeofday`、`nanosleep`、`sched_yield`、`uname`、`sysinfo`、`getrlimit`、`prlimit64`、およびカーネル内 `sleep_ms` helper。
- **`sys_signal.c`**: Linux 互換 `rt_sigaction`、`rt_sigprocmask`、`sigpending`、`sigaltstack`、および Orthox 私的 signal wrapper を所有します。
- **`sys_vm.c`**: ユーザーアドレス空間と VM syscall (`brk`、`mmap`、`munmap`、`mprotect`、`mremap`、`madvise`)、boot 時の `sys_brk_init()` を所有します。
- **`sys_proc.c`**: process lifecycle 系 syscall (`wait4`、`exit`、`kill`、PID/PPID/UID/GID stub)、futex / thread 補助 (`arch_prctl`、`futex`、`set_tid_address`、`set_robust_list`)、session / process group、TTY foreground process group helper を所有します。
- **`sys_fs.c`**: FS/fd syscall wrapper — `open`、`read`、`write`、`close`、`fcntl`、`dup2`、`pipe`/`pipe2`、`stat`/`fstat`/`lstat`/`fstatat`、`access`/`faccessat`、`readlink`/`readlinkat`、`lseek`、`getdents`/`getdents64`、`chdir`/`fchdir`/`getcwd`、`truncate`/`ftruncate`、`utimensat`、`sync`、`unlink`/`unlinkat`、`rename`、`chmod`、`mkdir`/`mkdirat`、`rmdir`、`pread64`/`pwrite64`、`readv`/`writev`、`ioctl`、termios、mount module root、mount status、private `ls` syscall。各エントリは `fs.c` の対応する `fs_*` 実装へ委譲します。
- **`sys_net.c`**: socket syscall wrapper (`socket`、`connect`、`bind`、`listen`、`accept`、`send*`、`recv*`、`shutdown`、`setsockopt`、`getsockname`、`getpeername`) と Orthox 私的 `ORTH_SYS_DNS_LOOKUP` wrapper。バックエンド実装は `net_socket.c` に存在します。
- **`sys_device.c`**: Orthox 私的デバイス syscall — framebuffer 情報・マッピング、キーボードイベント取得、sound (矩形波 / PCM)、USB 情報 / ブロック読み出し、CPU id / runq 統計、fork-spread ポリシーノブ、低レベル serial 出力 helper。
- **`sys_random.c`**: `getrandom` syscall と RDRAND・フォールバックエントロピー helper。
- **`sys_trace.c`**: `ORTHOX_MEM_TRACE` / `ORTHOX_MEM_PROGRESS` helper — `mmap`/`mremap`/`mprotect`/`munmap` の memtrace 整形と `mmap`/`brk` の progress counter。

### ファイルシステムとストレージ
- **`fs.c`**: VFS/fd 実装本体。fd テーブル、マウント処理、RAMFS、パス解決とディスパッチを所有し、`sys_fs.c` から呼ばれる `fs_*` API を公開します。
- **`vfs.c`**: マウントポイントとパス正規化の VFS helper。FS モジュール間で共有されます。
- **`storage.c`**: ストレージデバイスの抽象化。memory-backed と `virtio-blk` のバックエンドを登録し、FS 層へ `storage_read_blocks` / `storage_write_blocks` を公開します。
- **`xv6bio.c`**: xv6fs のバッファキャッシュ。storage 抽象の上で `bget`/`brelse`/`bread`/`bwrite` を提供します。
- **`xv6log.c`**: xv6fs のジャーナリング層。書き込みをトランザクションで包んでオンディスクログへコミットし、クラッシュ後の復元も担当します。
- **`xv6fs.c`**: xv6fs ファイルシステムコア（Orthox-64 拡張）。inode 確保、bitmap、二重・三重間接ブロック、ディレクトリ操作、大きな書き込みのチャンク化を実装します。

### コンソール入力
- **`keyboard.c`**: PS/2 キーボードドライバ。shell が読むコンソール入力バッファへ文字を供給します。

### PCI / USB / Sound
- **`pci.c`**: PCI 列挙。`virtio-blk`、`virtio-net`、オーディオ、xHCI などのデバイスを発見し、MSI/MSI-X capability も取り扱います。
- **`usb.c`**: USB/xHCI ホストコントローラと mass storage 寄りの処理。USB ストレージアクセスや rootfs mount 実験の土台です。
- **`sound.c`**: AC97 と SB16 フォールバックの PCM 再生機能を実装します。

### VirtIO とネットワーク
- **`virtio.c`**: VirtIO 共通コード。virtqueue レイアウトの算出補助、descriptor ring 構築、共有 MSI-X queue vector の接続を担当します。
- **`virtio_blk.c`**: `virtio-blk` ドライバ。xv6fs 用ブロックデバイスを担い、inflight request プール、IRQ 駆動の completion、bottom-half での used ring 回収、タイムアウト処理を実装します。
- **`virtio_net.c`**: 最小 `virtio-net` ドライバ。RX/TX virtqueue、MAC 取得、IRQ 駆動の frame 送受信に加え polling フォールバックを持ちます。
- **`net.c`**: NIC の薄い抽象層。frame 送信、MAC 参照、RX callback 登録、polling を上位ネットワークコードへ公開します。
- **`lwip_port.c`**: `lwIP` 統合層 (`NO_SYS=1`)。netif bring-up、DHCP/DNS/timeout 処理、ARP/ICMP 診断、UDP echo、kernel 側 DNS lookup glue を担当します。
- **`net_socket.c`**: `lwIP` 上の kernel socket バックエンド。`AF_INET` の UDP/TCP socket を実装し、`net_socket_*` エントリポイントが `sys_net.c` から呼ばれます。

### freestanding libc 断片
- **`cstring.c`**: カーネル本体や `lwIP` などの vendored component が必要とする最小限の文字列・メモリ操作を実装します。
- **`cstdio.c`**: カーネル内で使う最小限の整形出力と serial 指向の stdio 補助を実装します。
- **`cstdlib.c`**: freestanding カーネルで必要になる最小限の libc 風ユーティリティ関数を実装します。
