# user/riscv64-bin — rootfs の /bin に入る自作コマンド

ここに `foo.c` を置くと、`make riscv64-rootfs` で riscv64 musl 静的リンクされ、
xv6fs rootfs の `/bin/foo` として入る。Makefile への追記は不要 (wildcard で拾う)。

- 1 ファイル 1 コマンド。`main()` を書くだけでよい
- リンクは `user/crt0_musl_riscv64.S` + `ports/musl-install-riscv64/lib/libc.a`
  (busybox と同じツールチェーン。`ports/orthos-riscv64-musl-gcc.sh` 相当の設定)
- 複数ファイルからなるコマンドが必要になったら、その時に個別ルールを足すこと

確認:

```
make riscv64-ash-run     # 対話起動して /bin/<name> を叩く
```
