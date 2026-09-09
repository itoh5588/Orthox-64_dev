# RISC-V `virt` Bring-Up Skeleton

This directory is the first landing spot for the RISC-V `virt` port.

Current intent:

- keep the existing x86_64 build unchanged
- stage OpenSBI + DTB bring-up files before wiring them into the build
- give each major porting area a stable file target

Expected early implementation order:

1. `start.S`
2. `boot.c`
3. `trap.S`
4. `trap.c`
5. `entry.S`
6. `vm.c`

Expected firmware contract:

- entry from OpenSBI in S-mode
- `a0 = hartid`
- `a1 = dtb physical address`

Expected first output target:

- UART16550 at `0x10000000` on QEMU `virt`

This directory is intentionally not wired into the current x86 build yet.

Current scaffolding now exists for:

- OpenSBI handoff capture
- UART early output
- DTB header scan and minimal compatible/reg walk
- trap save/restore
- SBI timer arm
- syscall ABI glue
- initial user-return frame shaping
