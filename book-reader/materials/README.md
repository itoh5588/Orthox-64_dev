# 章別教材 — Orthox-64 読者向け

本書を読みながら手を動かすときの**補助資料**です。本書本文を置き換えるものではなく、「詰まりやすい所だけ段階的にヒントを開示する」位置付け。

## 使い方

1. まず本書本文を最後まで読む。
2. 実装に取り掛かって**詰まったときだけ** `ch<N>/hints.md` を開く。
3. それでも進まないときは答え合わせブランチをチェックアウト。

## 章一覧

| 章 | テーマ | 教材 | 答え合わせブランチ |
|----|--------|------|---------------------|
| Ch4 | メモリ管理 (PMM / VMM / ページテーブル / COW) | [ch04/](ch04/) | — (読解章) |
| Ch5 | 割り込み・タイマ・SMP (IDT / LAPIC / IPI / 遅延スケジューリング) | [ch05/](ch05/) | — (読解章) |
| Ch6 | syscall entry (swapgs / MSR / `syscall_frame`) | [ch06/](ch06/) | — (読解章) |
| Ch7 | プロセスとスケジューラ (`switch_context` / `cpu_local`) | [ch07/](ch07/) | — (読解章) |
| Ch8 | fork / exec / wait (COW 発動 / ELF ロード / ゾンビと reap) | [ch08/](ch08/) | — (読解章) |
| Ch9 | file descriptor と VFS (`fs_file_t` 2 層構造 / マウントポイント / 多態) | [ch09/](ch09/) | — (読解章) |
| Ch10 | xv6fs (inode / bmap 4 階層 / buffer cache / journal log) | [ch10/](ch10/) | — (読解章) |
| Ch11 | pipe / signal / socket (4000B 円環 / `sys_kill` ショートカット / lwIP waiter) | [ch11/](ch11/) | — (読解章) |
| Ch14 | ELF / 動的リンク / TLS (`PT_LOAD`/`PT_INTERP`/auxv/`arch_prctl`) | [ch14/](ch14/) | — (読解章) |
| Ch16 | GCC とセルフホスト (4 段階スモーク / stage1-3 / フリースタンディング) | [ch16/](ch16/) | — (実証章) |
| Ch19 | procfs を最小実装で組み込む | [ch19/](ch19/) | `book/ch19-procfs-complete` |

その他の章については hints は順次拡充されます。それまでの間も、CLAUDE.md の指示に従って AI が実コードを `grep` で確認しながら家庭教師として伴走します。

## AI 家庭教師との連携

`book-reader/CLAUDE.md` が AI に対し、読者から章番号付きの質問が来たら該当 `materials/ch<N>/hints.md` を優先的に参照するよう指示しています。読者自身は AI に「第19章で詰まった」と言うだけで、AI 側で適切なヒントレベルが選ばれる想定です。
