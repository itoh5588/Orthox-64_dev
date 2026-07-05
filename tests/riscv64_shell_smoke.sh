#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

cleanup() {
    make -B -C "$REPO_ROOT" \
        RISCV64_BOOTSTRAP_USER_SRC_ELF=out/bootstrap-user-riscv64-default.elf \
        out/kernel-riscv64.elf >/dev/null
}
trap cleanup EXIT

make -B -C "$REPO_ROOT" \
    RISCV64_BOOTSTRAP_USER_SRC_ELF=out/busybox-riscv64-musl.elf \
    RISCV64_BOOTSTRAP_ARG0_VALUE=sh \
    RISCV64_BOOTSTRAP_ARG1_VALUE=-c \
    RISCV64_BOOTSTRAP_ARG2_VALUE='ls /; exit' \
    out/kernel-riscv64.elf >/dev/null

RISCV64_BOOTSTRAP_EXPECT=busybox-ls sh "$SCRIPT_DIR/riscv64_smoke.sh" >/dev/null

grep -q "bootstrap-user" "$REPO_ROOT/LOGs/riscv64-serial.log"
mkdir -p LOGs

echo "riscv64 shell smoke test: PASS"
