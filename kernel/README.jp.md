# Orthox-64 Kernel Source Structure

このディレクトリには、Orthox-64 カーネルのコアソースコードが含まれています。

## 現在の配置（2026-09-13）

以下のパスは `kernel/` からの相対パスです。アーキごとのビルド対象は
トップレベルの [Makefile](../Makefile) で定義されています。
直下のファイルがすべてのアーキで使われるとは限りません。

- `kernel/`: 共通 subsystem / syscall 実装と、使用アーキが限定される dispatcher / wrapper。
- `x86_64/`: x86 のハードウェア、エントリコード、残る x86 syscall helper。
- `aarch64/`: AArch64 のエントリとハードウェア（QEMU virt / Raspberry Pi 4）。
- `riscv64/`: RISC-V のエントリとハードウェア、および独自の FS / socket backend。

### 3 アーキ共通の syscall 実装

9 月 8 日の重複調査は当時の記録であり、現在の TODO 一覧ではありません。
次のファイルは現在、3 アーキすべてでビルドされます。

| 所有ファイル | 主な責務 |
|---|---|
| `sys_mmap.c` | アーキ別 VM hook を使う `brk` / `mmap` / `munmap` / `mprotect` / `mremap` |
| `sys_rlimit.c` | `getrlimit` / `setrlimit` / `prlimit64` |
| `sys_uname.c` | アーキ識別 hook を使う `uname` |
| `sys_task.c` | PID/UID/GID、futex、`set_tid_address`、`sysinfo`、`getcwd`、`nanosleep`、`clock_gettime`、`exit`、`wait4` |
| `sys_random.c` | `arch_random_fill` を使う `getrandom` |
| `sys_signal.c` | signal action / mask、pending、alternate stack |
| `sys_iov.c` | 選択された FS backend を使う `readv` / `writev` |
| `sys_access.c` | `faccessat` |
| `sys_lseek.c` | FS サイズ更新用アーキ hook を使う `lseek` |
| `sys_tty.c` | `TIOCGPGRP` / `TIOCSPGRP` helper と foreground process-group 状態 |

### 残る境界と課題

- x86 の dispatcher は `syscall.c`。AArch64 / RISC-V はアーキ別の入口から
  `linux_syscall.c` を使います。
- x86 / AArch64 は `sys_fs.c` と `fs.c`、RISC-V は `riscv64/fs.c` を使います。
  syscall 名が同じでも、FS backend が同じとは限りません。
- `fstat` は x86_64 と asm-generic の stat レイアウト差を変換する必要があります。
  機械的に取り除くべき重複ではありません。
- ioctl の foreground process-group 処理は統合済みです。termios のレイアウト
  （`orth_termios` / `linux_termios`）、`FIOCLEX` / `FIONCLEX` 対応、
  console fd 判定には差が残っています。linux 側 dispatcher は現在、
  termios / window-size 要求で fd がコンソールか確認していません。
- `exit` / `wait4` は共通化済みですが、bootstrap 終了（`ppid == 0`）の
  実行検証は [9 月 13 日の日報](../Docs/Daily_log/日報2026-09-13.md) で残件です。

## ファイル一覧

### コアと初期化

- **`x86_64/init.c`**: カーネルのエントリポイント兼初期 bring-up シーケンス。Limine から制御を受け取り、メモリ管理、GDT/IDT、割り込み、syscall、タスク管理、PCI、ネットワーク、USB を順に初期化し、最初のユーザープロセスを起動します。
- **`elf.c`**: ELF ローダー。初回ユーザータスクの起動と、その後の `execve()` 系の実行ファイルロードで共通に使われます。
- **`x86_64/kassert.c`**: カーネル全体で使う `KASSERT()` / `KBUG_ON()` と `kernel_panic()` の実装。失敗時は CPU を停止し、式・関数名・ファイル・行番号を serial に出力します。

### メモリ管理

- **`x86_64/pmm.c`**: 物理メモリマネージャー。空き物理ページを追跡してページ単位の割り当て・解放を提供し、COW の基盤となるページ参照カウントも保持します。
- **`x86_64/vmm.c`**: 仮想メモリマネージャー。x86_64 の 4 段ページテーブルを構築・更新し、ユーザー/カーネル空間のマッピング、アドレス空間切り替え、COW を含むページフォルト処理を担当します。

### CPU セットアップと割り込み

- **`x86_64/gdt.c` / `x86_64/gdt_flush.S`**: GDT と TSS の設定。カーネル/ユーザー間の特権遷移に必要な基盤を構成します。
- **`x86_64/idt.c` / `x86_64/interrupt.S`**: IDT の設定と割り込み・例外エントリの低レベル stub。legacy PIC IRQ と MSI/MSI-X vector を中央の dispatcher へルーティングします。
- **`x86_64/lapic.c`**: Local APIC タイマーと時刻取得まわり。スケジューラや `lwIP` の timeout 処理に使われます。
- **`x86_64/pic.c`**: 旧来の 8259 PIC の制御。IRQ ルーティング互換を支えます。
- **`irq.c`**: legacy IRQ と MSI/MSI-X vector の中央ディスパッチ。各ドライバは init 時にここへ handler を登録し、IDT 層はこのモジュールへ委譲します。

### 同期 / 待機基盤 / SMP

- **`x86_64/spinlock.c`**: spinlock プリミティブ、IRQ 保存/復帰補助、およびカーネル粗粒度区間を直列化するグローバルカーネルロックを実装します。
- **`wait.c`**: `wait_queue` と `completion` API。`wait_event()`、`wait_event_timeout()`、`wake_up_one()`、`wake_up_all()`、および割り込み駆動 I/O が使う `complete()` 系を提供します。
- **`bottom_half.c`**: deferred work キュー。IRQ ハンドラから軽量 callback を enqueue し、idle 経路で実行することで割り込みコンテキストを短く保ちます。
- **`x86_64/smp.c`**: SMP bring-up。AP の起動、per-CPU state のセットアップ、プロセッサ間通信補助を提供します。

### タスク管理とスケジューラ

- **`task.c` / `x86_64/task_switch.S`**: task object のライフサイクル、run queue プリミティブ、task の確保・reap、低レベルなコンテキスト切り替えを所有します。
- **`sched.c`**: スケジューラポリシー。per-CPU run queue 管理、fork-spread の CPU 選択、タイマー tick、resched 要求、`schedule()`、idle loop を担当します。
- **`task_exec.c`**: `execve()` の実装。ELF イメージ置換、初期ユーザースタック構築、`argv`/`envp`/`auxv` の配置を行います。
- **`task_fork.c`**: 共通の `fork()` 処理。アーキ hook 経由のアドレス空間複製、fd / signal 状態の複製、子 syscall frame の構築。
- **`task_internal.h`**: task 内部の定数と helper 宣言。syscall frame 型は `include/arch_syscall.h` 経由で参照します。

### Syscall ディスパッチと実装

- **`syscall.c` / `x86_64/syscall_entry.S`**: x86 の C dispatcher は `syscall.c`、エントリ stub は `x86_64/syscall_entry.S`、MSR 初期化は `x86_64/syscall_msr.c`。
- **`x86_64/sys_time.c`**: x86 の時刻 helper は `x86_64/sys_time.c`。RTC 読み出し、`gettimeofday`、`sched_yield`、`sleep_ms` を担当。共通の時計・sleep 処理は `sys_task.c`。
- **`sys_signal.c`**: Linux 互換 `rt_sigaction`、`rt_sigprocmask`、`sigpending`、`sigaltstack`、および Orthox 私的 signal wrapper を所有します。
- **`x86_64/sys_vm.c`**: x86 の残る VM helper は `x86_64/sys_vm.c`（`sys_brk_init`、`madvise`）。主要 VM syscall は共通の `sys_mmap.c`。
- **`sys_proc.c`**: x86 向け `arch_prctl`、`set_robust_list`、`kill`、session / process-group 処理。終了・待機・PID・futex は `sys_task.c`、TTY foreground 状態は `sys_tty.c`。
- **`sys_fs.c`**: x86 / AArch64 の FS/fd wrapper と termios/ioctl、pselect、mount helper。主に `fs.c` へ委譲。`readv` / `writev` は `sys_iov.c`、`faccessat` は `sys_access.c`、`lseek` は `sys_lseek.c`、`getcwd` は `sys_task.c`。`readlinkat` は dispatcher から `fs_readlinkat` を直接呼びます。
- **`sys_net.c`**: Orthox 私的 DNS wrapper `sys_dns_lookup`。socket 呼び出しは各 dispatcher から `net_socket_*` へ直接委譲します。
- **`x86_64/sys_device.c`**: Orthox 私的デバイス syscall — framebuffer 情報・マッピング、キーボードイベント取得、sound (矩形波 / PCM)、USB 情報 / ブロック読み出し、CPU id / runq 統計、fork-spread ポリシーノブ、低レベル serial 出力 helper。
- **`sys_random.c`**: 3 アーキ共通の `getrandom`。乱数生成は `arch_random_fill` に委譲します。
- **`sys_trace.c`**: `ORTHOX_MEM_TRACE` / `ORTHOX_MEM_PROGRESS` helper — `mmap`/`mremap`/`mprotect`/`munmap` の memtrace 整形と `mmap`/`brk` の progress counter。

### ファイルシステムとストレージ

- **`fs.c`**: VFS/fd 実装本体。fd テーブル、マウント処理、RAMFS、パス解決とディスパッチを所有し、`sys_fs.c` から呼ばれる `fs_*` API を公開します。
- **`vfs.c`**: マウントポイントとパス正規化の VFS helper。FS モジュール間で共有されます。
- **`storage.c`**: ストレージデバイスの抽象化。memory-backed と `virtio-blk` のバックエンドを登録し、FS 層へ `storage_read_blocks` / `storage_write_blocks` を公開します。
- **`xv6bio.c`**: xv6fs のバッファキャッシュ。storage 抽象の上で `bget`/`brelse`/`bread`/`bwrite` を提供します。
- **`xv6log.c`**: xv6fs のジャーナリング層。書き込みをトランザクションで包んでオンディスクログへコミットし、クラッシュ後の復元も担当します。
- **`xv6fs.c`**: xv6fs ファイルシステムコア（Orthox-64 拡張）。inode 確保、bitmap、二重・三重間接ブロック、ディレクトリ操作、大きな書き込みのチャンク化を実装します。

### コンソール入力

- **`x86_64/keyboard.c`**: PS/2 キーボードドライバ。shell が読むコンソール入力バッファへ文字を供給します。

### PCI / USB / Sound

- **`x86_64/pci.c`**: PCI 列挙。`virtio-blk`、`virtio-net`、オーディオ、xHCI などのデバイスを発見し、MSI/MSI-X capability も取り扱います。
- **`usb.c`**: USB/xHCI ホストコントローラと mass storage 寄りの処理。USB ストレージアクセスや rootfs mount 実験の土台です。
- **`x86_64/sound.c`**: AC97 と SB16 フォールバックの PCM 再生機能を実装します。

### VirtIO とネットワーク

- **`x86_64/virtio.c`**: VirtIO 共通コード。virtqueue レイアウトの算出補助、descriptor ring 構築、共有 MSI-X queue vector の接続を担当します。
- **`x86_64/virtio_blk.c`**: `virtio-blk` ドライバ。xv6fs 用ブロックデバイスを担い、inflight request プール、IRQ 駆動の completion、bottom-half での used ring 回収、タイムアウト処理を実装します。
- **`x86_64/virtio_net.c`**: 最小 `virtio-net` ドライバ。RX/TX virtqueue、MAC 取得、IRQ 駆動の frame 送受信に加え polling フォールバックを持ちます。
- **`net.c`**: NIC の薄い抽象層。frame 送信、MAC 参照、RX callback 登録、polling を上位ネットワークコードへ公開します。
- **`lwip_port.c`**: `lwIP` 統合層 (`NO_SYS=1`)。netif bring-up、DHCP/DNS/timeout 処理、ARP/ICMP 診断、UDP echo、kernel 側 DNS lookup glue を担当します。
- **`net_socket.c`**: x86 / AArch64 の lwIP socket backend。各 dispatcher が `net_socket_*` を直接呼びます。RISC-V は `riscv64/net_socket.c` を使用。

### freestanding libc 断片

- **`cstring.c`**: カーネル本体や `lwIP` などの vendored component が必要とする最小限の文字列・メモリ操作を実装します。
- **`cstdio.c`**: カーネル内で使う最小限の整形出力と serial 指向の stdio 補助を実装します。
- **`cstdlib.c`**: freestanding カーネルで必要になる最小限の libc 風ユーティリティ関数を実装します。
