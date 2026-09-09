# Orthox-64 読者向け AI 家庭教師ワークスペース

本書『Orthox-64』を読みながら、AI エージェント（第一候補: [Claude Code](https://claude.com/claude-code)）を**家庭教師**として併用するためのフォルダです。

## 前提

- Claude Code をインストール済み（または互換 AI エージェント）
- Orthox-64 リポジトリを clone 済み
- 本書（Kindle 等）を別途お手元に

## 使い方

```sh
cd book-reader
claude
```

このフォルダから Claude Code を起動すると、`book-reader/CLAUDE.md`（リポジトリ案内）と `book-reader/.claude/skills/`（便利コマンド）が自動で読み込まれ、AI が即座に「Orthox-64 家庭教師モード」になります。

OS のソースコードは親ディレクトリ（`../kernel/`、`../include/` 等）にあり、AI はそこを参照します。

## できること（一例）

- 「Ch19 の procfs を写経した。動かないので一緒に見て」
- 「PMM の API はどこを読めばいい？」
- 「`make` のターゲットを一覧で教えて」
- 「`/smoketest` で QEMU の起動を試したい」

## 章ごとの「答え合わせブランチ」

詰まったときの参照解です。`git checkout` で実装完成版を読めます。

| 章 | ブランチ | 内容 |
|----|----------|------|
| Ch19 | `origin/book/ch19-procfs-complete` | procfs 最小実装の完成版 |

（章が増えるたびに追記予定）

## 制約

- このフォルダ外（`../kernel/` 等）の編集は、読者ご自身の明示的な指示があるまで AI は行いません。
- 本書本文の編集は著者の領分です。AI はあくまで読者の学習支援役です。
