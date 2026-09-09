#!/bin/bash
set -euo pipefail

TTY_STATE=""
QEMU_AUDIO_ARGS=()

if [ -t 0 ]; then
    TTY_STATE="$(stty -g)"
    trap 'stty "$TTY_STATE"' EXIT INT TERM
    stty raw -echo
fi

case "$(uname -s)" in
    Darwin)
        QEMU_AUDIO_ARGS=(
            -machine pc,pcspk-audiodev=audio0
            -audiodev coreaudio,id=audio0
            -device sb16,audiodev=audio0
        )
        ;;
    *)
        QEMU_AUDIO_ARGS=(
            -machine pc
        )
        ;;
esac

exec qemu-system-x86_64 \
    "${QEMU_AUDIO_ARGS[@]}" \
    -cpu max \
    -m 2G \
    -cdrom orthos.iso \
    -boot d \
    "$@" \
    -serial mon:stdio
