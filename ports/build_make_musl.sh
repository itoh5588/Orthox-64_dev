#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="4.4.1"
ARCHIVE="make-${VERSION}.tar.gz"
URL="https://ftp.gnu.org/gnu/make/${ARCHIVE}"
TARBALL="${ROOT}/ports/${ARCHIVE}"
SRC_DIR="${ROOT}/ports/make-${VERSION}"
BUILD_DIR="${SRC_DIR}/build-musl"
PATCH_FILE="${ROOT}/ports/make-${VERSION}-orthos.patch"
OUT="${1:-${ROOT}/user/make.elf}"

case "${OUT}" in
    /*) ;;
    *) OUT="${ROOT}/${OUT}" ;;
esac

if [ ! -f "${TARBALL}" ]; then
    curl -L "${URL}" -o "${TARBALL}"
fi

rm -rf "${SRC_DIR}"
tar -C "${ROOT}/ports" -xf "${TARBALL}"
patch -d "${SRC_DIR}" -p4 < "${PATCH_FILE}"

python3 - "${SRC_DIR}/src/job.c" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
text = path.read_text()
text, count = re.subn(
    r'(#else\s*\n\s*const char \*default_shell = )"/bin/sh"(;\s*\n\s*int batch_mode_shell = 0;\s*\n\s*#endif)',
    r'\1"/bin/sh"\2',
    text,
    count=1,
)
if count != 1:
    raise SystemExit("expected default_shell block not found in src/job.c")

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

old = """    pid = vfork ();"""
new = """    pid = fork ();"""
if old not in text:
    raise SystemExit("expected vfork call not found in src/job.c")
text = text.replace(old, new, 1)

path.write_text(text)
PY

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

make_cv_sys_gnu_glob=yes \
ac_cv_func_posix_spawn=no \
ac_cv_func_posix_spawnattr_setsigmask=no \
"${SRC_DIR}/configure" \
    --host=x86_64-pc-linux-musl \
    --build=x86_64-pc-linux-gnu \
    --disable-nls \
    --without-guile \
    CC="${ROOT}/ports/orthos-musl-gcc.sh" \
    CFLAGS="-static -fno-PIC -fno-PIE -D__ORTHOS__ -D_GNU_SOURCE" \
    LDFLAGS="-static -fno-PIC -fno-PIE"

make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

cp "${BUILD_DIR}/make" "${OUT}"
chmod +x "${OUT}"
