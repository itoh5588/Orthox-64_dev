#!/bin/bash
# exec イメージキャッシュの正しさを検証する。
#   - キャッシュヒット: 同一バイナリを 15 回 exec しても毎回正しく動く
#   - 無効化: 実行ファイルを別バイナリで上書き (cp = O_TRUNC) すると
#     次の exec は新しい内容を実行する (stale を返さない)
# 期待: "Hello ..." がちょうど 16 回、上書き後は "at_test: PASS" が出る。
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/exec-cache-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/exec-cache-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/exec_cache_smoke.ash"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true; wait "${QEMU_PID}" 2>/dev/null || true
    fi
    [ -f "${BOOTCMD_BACKUP}" ] && { cp "${BOOTCMD_BACKUP}" "${BOOTCMD_PATH}"; rm -f "${BOOTCMD_BACKUP}"; }
    if [ -f "${SCRIPT_BACKUP}" ]; then
        if [ -s "${SCRIPT_BACKUP}" ]; then cp "${SCRIPT_BACKUP}" "${SCRIPT_PATH}"; else rm -f "${SCRIPT_PATH}"; fi
        rm -f "${SCRIPT_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
if [ -f "${SCRIPT_PATH}" ]; then cp "${SCRIPT_PATH}" "${SCRIPT_BACKUP}"; else : > "${SCRIPT_BACKUP}"; fi

cat > "${SCRIPT_PATH}" <<'EOF'
export PATH=/bin:/usr/bin:/
echo exec-cache-start
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do /bin/hello_std; done
echo hit-loop-done
cp /bin/hello_std /cprobe
echo inv-run1
/cprobe
cp /bin/at_test.elf /cprobe
echo inv-run2
/cprobe
/bin/rm -f /cprobe
echo exec-cache-ok
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/exec_cache_smoke.ash
EOF

echo "Building ISO..."
make orthos.iso >/tmp/exec-cache-build.out 2>&1

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"
qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -cdrom "${ISO}" -boot d \
    -display none -audio none -serial "file:${SERIAL_LOG}" -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for exec cache smoke..."
for _ in $(seq 1 120); do
    grep -q 'exec-cache-ok' "${SERIAL_LOG}" 2>/dev/null && break
    sleep 1
done
kill "${QEMU_PID}" 2>/dev/null || true; wait "${QEMU_PID}" 2>/dev/null || true; QEMU_PID=""

fail() { echo "exec cache smoke FAIL: $1"; echo "---- serial tail ----"; tail -n 60 "${SERIAL_LOG}"; exit 1; }

grep -q 'exec-cache-start' "${SERIAL_LOG}" || fail "start marker missing"
grep -q 'exec-cache-ok'    "${SERIAL_LOG}" || fail "ok marker missing (hang/crash?)"
grep -qi 'panic'           "${SERIAL_LOG}" && fail "kernel panic during exec"
HELLO=$(grep -c 'Hello from Rust Standard Library on Orthox-64!' "${SERIAL_LOG}" || true)
[ "${HELLO}" = "16" ] || fail "hello count = ${HELLO}, expected 16 (hit loop 15 + inv-run1). stale/lost cache?"
grep -q 'at_test: PASS'    "${SERIAL_LOG}" || fail "invalidation failed: /cprobe still ran hello_std after overwrite (no at_test output)"

echo "exec cache smoke PASS"
echo "  hello_std runs: ${HELLO} (expected 16)"
echo "  invalidation: /cprobe ran at_test after overwrite -> $(grep -c 'at_test: PASS' "${SERIAL_LOG}") PASS line(s)"
