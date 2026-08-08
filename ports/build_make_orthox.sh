#!/bin/bash
# GNU make 4.4.1 を **Orthox riscv64 上で動く形**で組む。
# build_make_musl.sh (x86 版) の riscv64 版。ソース加工は同一で、
# ツールチェーンと --host だけ差し替えている:
#   - Orthox 用パッチ (ports/make-4.4.1-orthos.patch)
#   - job.c: execvp を PATH 手動探索の execve に置き換え (Orthox に execvpe が無い)
#   - job.c: vfork → fork (Orthox の vfork はアドレス空間を共有しない)
#
#   build = x86_64-unknown-linux-gnu
#   host  = riscv64-linux-musl   (= Orthox)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="4.4.1"
ARCHIVE="make-${VERSION}.tar.gz"
TARBALL="${ROOT}/ports/${ARCHIVE}"
SRC_DIR="${ROOT}/ports/make-${VERSION}-riscv64"
BUILD_DIR="${SRC_DIR}/build-orthox"
PATCH_FILE="${ROOT}/ports/make-${VERSION}-orthos.patch"
PREFIX="${ROOT}/ports/orthox-native"
CROSS="${ROOT}/ports/cross-riscv64/bin/riscv64-linux-musl"

[ -x "${CROSS}-gcc" ] || { echo "error: ${CROSS}-gcc が無い。stage-2 を先に" >&2; exit 1; }
[ -f "${TARBALL}" ] || { echo "error: ${TARBALL} が無い" >&2; exit 1; }

rm -rf "${SRC_DIR}"
mkdir -p "${SRC_DIR}"
tar -C "${SRC_DIR}" --strip-components=1 -xf "${TARBALL}"
patch -d "${SRC_DIR}" -p4 < "${PATCH_FILE}"

python3 - "${SRC_DIR}/src/job.c" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

# execvp は PATH 探索の前に環境を差し替えてしまう & Orthox に execvpe が無い。
# PATH を自分で辿って execve する形に置き換える (x86 版と同じ)。
old = """  /* Run the program.  Don't use execvpe() as we want the search for argv[0]
     to use the new PATH, but execvpe() searches before resetting PATH.  */
  environ = envp;
  execvp (argv[0], argv);"""
new = """  environ = envp;
  if (strchr (argv[0], '/') == 0)
    {
      const char *path = 0;
      char **ep;
      size_t argv0_len = strlen (argv[0]);

      for (ep = envp; ep && *ep; ++ep)
        if (strncmp (*ep, "PATH=", 5) == 0)
          {
            path = (*ep) + 5;
            break;
          }

      if (!path || !*path)
        path = "/bin:/usr/bin:/";

      while (1)
        {
          const char *next = strchr (path, ':');
          size_t path_len = next ? (size_t)(next - path) : strlen (path);
          char *cmd = alloca (path_len + argv0_len + 3);

          if (path_len == 0)
            sprintf (cmd, "./%s", argv[0]);
          else if (path_len == 1 && path[0] == '/')
            sprintf (cmd, "/%s", argv[0]);
          else
            {
              memcpy (cmd, path, path_len);
              cmd[path_len] = '/';
              memcpy (cmd + path_len + 1, argv[0], argv0_len + 1);
            }
          execve (cmd, argv, envp);
          if (!next)
            break;
          path = next + 1;
        }
    }

  execve (argv[0], argv, envp);"""
if old not in text:
    raise SystemExit("expected execvp block not found in src/job.c")
text = text.replace(old, new, 1)

# Orthox の vfork は clone(CLONE_VFORK) を通常 fork として受けるだけで
# アドレス空間を共有しない。make は vfork の子で環境を組み立てるので fork にする。
old = """    pid = vfork ();"""
new = """    pid = fork ();"""
if old not in text:
    raise SystemExit("expected vfork call not found in src/job.c")
text = text.replace(old, new, 1)

path.write_text(text)
print("src/job.c を加工した")
PY

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# クロスなので実行を伴う検査は先に答えを与える
make_cv_sys_gnu_glob=yes \
ac_cv_func_posix_spawn=no \
ac_cv_func_posix_spawnattr_setsigmask=no \
"${SRC_DIR}/configure" \
    --build=x86_64-unknown-linux-gnu \
    --host=riscv64-linux-musl \
    --prefix=/usr \
    --disable-nls \
    --without-guile \
    CC="${CROSS}-gcc" \
    AR="${CROSS}-ar" \
    RANLIB="${CROSS}-ranlib" \
    CFLAGS="-O2 -static -fno-PIC -fno-pie -D__ORTHOS__ -D_GNU_SOURCE" \
    LDFLAGS="-static"

make -j"$(nproc)"

install -D -m 755 "${BUILD_DIR}/make" "${PREFIX}/usr/bin/make"
echo
echo "=== 完了 ==="
ls -la "${PREFIX}/usr/bin/make"
"${CROSS}-readelf" -h "${PREFIX}/usr/bin/make" | grep -E "Type:|Machine:"
