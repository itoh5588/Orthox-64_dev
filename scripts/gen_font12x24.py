#!/usr/bin/env python3
"""kernel/aarch64/font12x24.c を Spleen の BDF から作る。

    python3 scripts/gen_font12x24.py third_party/spleen/spleen-12x24.bdf \
        > kernel/aarch64/font12x24.c

**字形は Spleen (BSD-2-Clause) のもの。**BDF の 1 行は 16bit で、
上位 12bit が字形 (bit15 が左端)。ASCII 0x20-0x7e の 95 文字に、
「表に無い文字」の升目を 1 つ足して 96 個にする。
"""
import sys

FIRST, LAST = 0x20, 0x7e
W, H = 12, 24


def read_bdf(path):
    glyphs = {}
    enc = None
    rows = None
    for line in open(path, encoding="latin-1"):
        line = line.strip()
        if line.startswith("ENCODING "):
            enc = int(line.split()[1])
        elif line == "BITMAP":
            rows = []
        elif line == "ENDCHAR":
            if rows is not None and enc is not None:
                glyphs[enc] = rows
            rows, enc = None, None
        elif rows is not None:
            rows.append(int(line, 16))
    return glyphs


def missing_box():
    """表に無い文字の升目。**空白にしない** — 「字が無い」と「空白」を
    混ぜると化けに気づけなくなる。枠だけの箱を出す。"""
    out = []
    for y in range(H):
        if y < 2 or y >= H - 2:
            out.append(0x0000)
        elif y in (2, H - 3):
            out.append(0x7FE0)          # 上下の辺
        else:
            out.append(0x6060)          # 左右の辺
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_font12x24.py <spleen-12x24.bdf>")
    g = read_bdf(sys.argv[1])

    print("/*")
    print(" * 12x24 の等幅ビットマップフォント (ASCII 0x20-0x7e)。")
    print(" *")
    print(" * **自動生成。手で直さないこと。** 作り直しは")
    print(" *   python3 scripts/gen_font12x24.py \\")
    print(" *       third_party/spleen/spleen-12x24.bdf > kernel/aarch64/font12x24.c")
    print(" *")
    print(" * ---- 字形の出どころ ------------------------------------------------")
    print(" *")
    print(" * Spleen 12x24 <https://github.com/fcambus/spleen>")
    print(" * Copyright (c) 2018-2026, Frederic Cambus")
    print(" * SPDX-License-Identifier: BSD-2-Clause")
    print(" * ライセンス全文は third_party/spleen/LICENSE。")
    print(" *")
    print(" * 1 文字 24 行。**1 行 16bit で、上位 12bit が字形 (bit15 が左端)。**")
    print(" */")
    print("#include <stdint.h>")
    print()
    print("#define AARCH64_FONT12X24_MISSING 95")
    print()
    print("const uint16_t aarch64_font12x24[96][24] = {")
    for code in range(FIRST, LAST + 1):
        rows = g.get(code)
        if rows is None:
            sys.exit(f"BDF に 0x{code:02x} が無い")
        if len(rows) != H:
            sys.exit(f"0x{code:02x} の行数が {len(rows)} (期待 {H})")
        ch = chr(code)
        label = "'\\''" if ch == "'" else f"'{ch}'"
        if ch == "\\":
            label = "'\\\\'"
        print(f"    /* 0x{code:02x} {label} */")
        print("    {", end="")
        for i, r in enumerate(rows):
            if i % 8 == 0:
                print("\n     ", end="")
            print(f" 0x{r:04x},", end="")
        print("\n    },")
    print("    /* 表に無い文字 */")
    print("    {", end="")
    for i, r in enumerate(missing_box()):
        if i % 8 == 0:
            print("\n     ", end="")
        print(f" 0x{r:04x},", end="")
    print("\n    },")
    print("};")


if __name__ == "__main__":
    main()
