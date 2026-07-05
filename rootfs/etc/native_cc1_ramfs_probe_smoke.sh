CC1_RAMFS_PROBE_MODE='ramfs'
set -ex
export PATH=/bin:/usr/bin:/
echo native-cc1-ramfs-probe-start
cp /src/syscall.h /work/syscall.h
cp /src/sh_probe_full.c /work/sh_ramfs.c
ls /src/sh_probe_full.c /work/sh_ramfs.c /work/syscall.h

if [ "${CC1_RAMFS_PROBE_MODE}" = "retro" ] || [ "${CC1_RAMFS_PROBE_MODE}" = "both" ]; then
    echo native-cc1-ramfs-probe:retro:start
    cc -std=c99 -DORTHOS_SH_ENABLE_USB=0 -S /src/sh_probe_full.c -o /work/sh_retro.s
    echo native-cc1-ramfs-probe:retro:end
fi

if [ "${CC1_RAMFS_PROBE_MODE}" = "ramfs" ] || [ "${CC1_RAMFS_PROBE_MODE}" = "both" ]; then
    echo native-cc1-ramfs-probe:ramfs:start
    cc -std=c99 -DORTHOS_SH_ENABLE_USB=0 -S /work/sh_ramfs.c -o /work/sh_ramfs.s
    echo native-cc1-ramfs-probe:ramfs:end
fi

echo native-cc1-ramfs-probe: PASS
echo native-cc1-ramfs-probe-end
