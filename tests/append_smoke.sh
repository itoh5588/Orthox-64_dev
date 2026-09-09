#!/bin/bash
# O_APPEND / truncate 回帰テスト。
#   - `>>` は追記され truncate しないこと (RESULT_LINES=3, RESULT_FIRST=AAA)
#   - `>`  は従来どおり truncate すること  (TRUNC_LINES=1, TRUNC_ONLY=YYY)
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/append-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/append-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/append_smoke.ash"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    if [ -f "${BOOTCMD_BACKUP}" ]; then
        cp "${BOOTCMD_BACKUP}" "${BOOTCMD_PATH}"
        rm -f "${BOOTCMD_BACKUP}"
    fi
    if [ -f "${SCRIPT_BACKUP}" ]; then
        if [ -s "${SCRIPT_BACKUP}" ]; then
            cp "${SCRIPT_BACKUP}" "${SCRIPT_PATH}"
        else
            rm -f "${SCRIPT_PATH}"
        fi
        rm -f "${SCRIPT_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
if [ -f "${SCRIPT_PATH}" ]; then
    cp "${SCRIPT_PATH}" "${SCRIPT_BACKUP}"
else
    : > "${SCRIPT_BACKUP}"
fi

cat > "${SCRIPT_PATH}" <<'EOF'
export PATH=/bin:/usr/bin:/
echo append-smoke-start
echo AAA > /apptest.txt
echo BBB >> /apptest.txt
echo CCC >> /apptest.txt
echo "RESULT_LINES=$(/bin/wc -l < /apptest.txt)"
echo "RESULT_FIRST=$(/bin/head -n 1 /apptest.txt)"
echo "RESULT_LAST=$(/bin/tail -n 1 /apptest.txt)"
echo XXX > /trunctest.txt
echo YYY > /trunctest.txt
echo "TRUNC_LINES=$(/bin/wc -l < /trunctest.txt)"
echo "TRUNC_ONLY=$(/bin/cat /trunctest.txt)"
/bin/rm /apptest.txt
/bin/rm /trunctest.txt
echo append-smoke-ok
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/append_smoke.ash
EOF

echo "Building ISO..."
make orthos.iso >/tmp/append-build.out 2>&1

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audio none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for append smoke..."
for _ in $(seq 1 120); do
    if grep -q 'append-smoke-ok' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

fail() { echo "append smoke FAIL: $1"; echo "---- serial tail ----"; tail -n 60 "${SERIAL_LOG}"; exit 1; }

grep -q 'append-smoke-start' "${SERIAL_LOG}" || fail "start marker missing"
grep -q 'append-smoke-ok'    "${SERIAL_LOG}" || fail "ok marker missing (hang/crash?)"
grep -q 'RESULT_LINES=3'     "${SERIAL_LOG}" || fail ">> did not append (line count != 3 -> truncate bug)"
grep -q 'RESULT_FIRST=AAA'   "${SERIAL_LOG}" || fail ">> lost the first line (truncate bug)"
grep -q 'RESULT_LAST=CCC'    "${SERIAL_LOG}" || fail ">> last line wrong"
grep -q 'TRUNC_LINES=1'      "${SERIAL_LOG}" || fail "> did not truncate (line count != 1)"
grep -q 'TRUNC_ONLY=YYY'     "${SERIAL_LOG}" || fail "> truncate produced wrong content"

echo "append smoke PASS"
grep -E 'RESULT_LINES|RESULT_FIRST|RESULT_LAST|TRUNC_LINES|TRUNC_ONLY' "${SERIAL_LOG}"
