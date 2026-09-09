#!/bin/bash
set -euo pipefail

WSLG_PULSE_SERVER="/mnt/wslg/PulseServer"
WSLG_RUNTIME_DIR="/mnt/wslg/runtime-dir"

if [ -S "${WSLG_PULSE_SERVER}" ]; then
    export PULSE_SERVER="${PULSE_SERVER:-unix:${WSLG_PULSE_SERVER}}"
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-${WSLG_RUNTIME_DIR}}"
fi

if [ -n "${QEMU_AUDIODEV:-}" ]; then
    QEMU_AUDIODEV="${QEMU_AUDIODEV}"
elif [ -S "${WSLG_PULSE_SERVER}" ]; then
    QEMU_AUDIODEV="pa,id=audio0,server=unix:${WSLG_PULSE_SERVER}"
else
    QEMU_AUDIODEV="pa,id=audio0"
fi

QEMU_AUDIO_DEVICE="${QEMU_AUDIO_DEVICE:-AC97,audiodev=audio0}"

TTY_STATE=""
if [ -t 0 ]; then
    TTY_STATE="$(stty -g)"
    trap 'stty "$TTY_STATE"' EXIT INT TERM
    stty raw -echo
fi

exec qemu-system-x86_64 \
    -machine q35 \
    -audiodev "${QEMU_AUDIODEV}" \
    -device "${QEMU_AUDIO_DEVICE}" \
    -cpu max \
    -m 2G \
    -cdrom "${ORTHOS_ISO:-orthos.iso}" \
    -boot d \
    "$@" \
    -serial stdio
