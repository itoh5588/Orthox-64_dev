SH_STAGE2_PROBES='sh_probe_fat_find.c sh_probe_fat_read_entry.c sh_probe_fat_resolve2.c'
SH_STAGE2_PROBE_CFLAGS=''
set -ex
export PATH=/bin:/usr/bin:/
echo native-sh-stage2-probe-start
WORK_DIR="/tmp"
cp /src/syscall.h "${WORK_DIR}/syscall.h"
for src in ${SH_STAGE2_PROBES:-sh_probe_core.c sh_probe_usb_boot.c sh_probe_fat_helpers.c sh_probe_fat_iter.c sh_probe_fat_resolve.c sh_probe_fat_shell.c sh_probe_usb_load.c sh_probe_fat_lba.c sh_probe_fat_iter2.c sh_probe_fat_find.c sh_probe_fat_read_entry.c sh_probe_fat_resolve2.c sh_probe_listing.c sh_probe_run_builtin.c sh_probe_run_command_child.c sh_probe_shell_exec.c sh_probe_run_once.c sh_probe_run_extended.c sh_probe_run_line.c sh_probe_try_bootcmd.c sh_probe_full.c}; do
    cp "/src/${src}" "${WORK_DIR}/${src}"
done
cd "${WORK_DIR}"
for src in ${SH_STAGE2_PROBES:-sh_probe_core.c sh_probe_usb_boot.c sh_probe_fat_helpers.c sh_probe_fat_iter.c sh_probe_fat_resolve.c sh_probe_fat_shell.c sh_probe_usb_load.c sh_probe_fat_lba.c sh_probe_fat_iter2.c sh_probe_fat_find.c sh_probe_fat_read_entry.c sh_probe_fat_resolve2.c sh_probe_listing.c sh_probe_run_builtin.c sh_probe_run_command_child.c sh_probe_shell_exec.c sh_probe_run_once.c sh_probe_run_extended.c sh_probe_run_line.c sh_probe_try_bootcmd.c sh_probe_full.c}; do
    echo native-sh-stage2-probe:${src}:start
    cc -std=c99 ${SH_STAGE2_PROBE_CFLAGS} -S "${src}" -o "${src%.c}.s"
    echo native-sh-stage2-probe:${src}:end
done
echo native-sh-stage2-probe: PASS
echo native-sh-stage2-probe-end
