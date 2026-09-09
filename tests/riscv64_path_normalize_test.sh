#!/bin/bash
# riscv64_fs_normalize_path() の単体テスト (ホスト側・1 秒)
#
# なぜ要るか:
#   Orthox 上で GCC を動かしたとき、
#     /src/gcc-self/build/gcc + ../../gcc/ada/gcc-interface/ada-tree.def
#   が 64 文字になり file_descriptor_t の name[64] を超えて黙って切り捨てられ、
#   別のパスとして扱われて I/O error になった。'..' を畳めば 47 文字で収まる。
#
#   そして畳む処理を入れた際、区切りの '/' を要素の後ろに書いたせいで
#   最後の要素の直後に書いた '/' が「読み取り側がまだ見ている終端 NUL」を潰し、
#   /dev/null が /dev/null/-user になるバグを作り込んだ。
#   カーネルに入れてスモークを回すと 1 サイクル 10 分だが、ここなら 1 秒で分かる。
#
# 実装は kernel/riscv64/fs.c から **その場で抜き出す** ので、
# 本体を直せばこのテストも自動的に追随する (コピーを持たない)。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/kernel/riscv64/fs.c"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[ -f "$SRC" ] || { echo "error: $SRC が無い" >&2; exit 1; }

# 関数本体を抜き出す (定義行から桁 0 の '}' まで)
awk '/^static void riscv64_fs_normalize_path\(/{f=1} f{print} f&&/^}/{exit}' \
    "$SRC" > "$WORK/body.c"
if [ ! -s "$WORK/body.c" ]; then
    echo "error: riscv64_fs_normalize_path が $SRC に見つからない" >&2
    exit 1
fi

{
  printf '#include <stdio.h>\n#include <string.h>\n#include <stddef.h>\n\n'
  cat "$WORK/body.c"
  cat <<'EOF'

struct { const char *in; const char *want; } cases[] = {
    /* 基本 */
    { "/",                 "/" },
    { "/bin/busybox",      "/bin/busybox" },
    { "/usr/bin/gcc",      "/usr/bin/gcc" },

    /* 連続スラッシュ */
    { "//bootstrap-user",  "/bootstrap-user" },
    { "/a//b",             "/a/b" },

    /* 末尾スラッシュ */
    { "/tmp/",             "/tmp" },

    /* '.' */
    { "/a/./b",            "/a/b" },
    { "/./a",              "/a" },

    /* '..' */
    { "/a/b/../c",         "/a/c" },
    { "/a/b/..",           "/a" },
    { "/..",               "/" },
    { "/a/b/../../..",     "/" },

    /* 終端 NUL を潰す退行 (要素の後ろに '/' を書くと /dev/null/-user になった) */
    { "/dev/null",         "/dev/null" },
    { "/dev/console",      "/dev/console" },

    /* 本番で踏んだやつ: 64 文字 → 47 文字 */
    { "/src/gcc-self/build/gcc/../../gcc/ada/gcc-interface/ada-tree.def",
      "/src/gcc-self/gcc/ada/gcc-interface/ada-tree.def" },
};

int main(void)
{
    char buf[512];
    unsigned i, bad = 0;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        /* 直前の内容を残して、終端を越えて読む不具合を検出しやすくする */
        memset(buf, 'X', sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        strcpy(buf, cases[i].in);
        riscv64_fs_normalize_path(buf);
        if (strcmp(buf, cases[i].want) != 0) {
            printf("  BAD  %-64s -> %-50s (期待 %s)\n",
                   cases[i].in, buf, cases[i].want);
            bad++;
        }
    }
    if (bad) {
        printf("riscv64 path normalize test: FAIL (%u / %u 件)\n",
               bad, (unsigned)(sizeof(cases) / sizeof(cases[0])));
        return 1;
    }
    printf("riscv64 path normalize test: PASS (%u 件)\n",
           (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return 0;
}
EOF
} > "$WORK/test.c"

cc -std=gnu99 -Wall -Wextra -o "$WORK/test" "$WORK/test.c"
"$WORK/test"
