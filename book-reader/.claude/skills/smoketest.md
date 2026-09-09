---
name: smoketest
description: QEMU を使った Orthox-64 のスモークテストを実行する。読者が「動作確認」「テスト」「スモーク」を希望したときに使用。
---

# /smoketest — Orthox-64 スモークテスト実行

読者が変更を加えた後の動作確認、または既存機能の検査を行うときに使うスキルです。

## 標準的な流れ

1. **どのスモークを走らせるか決める**
   - 読者の意図を聞く（例: 「procfs を試した」→ procfs 関連の手作りスモーク／「musl が動くか確認」→ `muslbusyboxsmoke` 等）
   - 既存のスモーク一覧は `make -C .. -pn 2>/dev/null | grep -E "^[a-z]+smoke:"` で確認可能
   - 既存スクリプトは `../tests/*.sh`

2. **既存ターゲットがあればそれを優先**

   ```sh
   make -C .. <target>smoke
   ```

   例: `make -C .. preadpwritesmoke`

3. **無ければ手作りスモークを組む**
   - `../rootfs/etc/bootcmd` に自動実行コマンドを書く（バックアップを取ること）
   - 完了マーカー（例: `procfs-smoke-ok`）を `echo` してシリアルに出す
   - ISO をビルド: `make -C .. orthos-retrofs.iso`
   - QEMU 起動（シリアルログを `/tmp/qemu_serial.log` 等にリダイレクト）
   - 完了マーカーを `grep` で検査
   - **必ず後始末**: `../rootfs/etc/bootcmd` を元に戻す

4. **失敗時の情報収集**
   - シリアルログ全文を確認
   - パニック・スタックトレースの有無
   - 直近の `make` 出力で警告がないか

## 注意

- `make` と QEMU は時間がかかります。`Bash` の timeout は **300000ms 以上**を推奨。
- バックグラウンド実行は使わない（本プロジェクトの方針）。
- ファイルシステム破壊を伴うテスト（書き込み系）は読者に確認してから実施。

## 参考

- `../tests/` 配下のシェルスクリプトが多数あり、命名規約と典型パターンの参考になります。
- `../Makefile` の `*smoke:` ターゲットが既存スモークの一覧です。
