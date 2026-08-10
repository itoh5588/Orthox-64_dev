#!/usr/bin/env python3
"""riscv64 の syscall 番号監査。

過去 2 度、x86 レガシー番号 (include/syscall.h の SYS_*) を riscv64 の
ディスパッチャに混ぜたせいで番号が衝突し、実害が出ている:
  - SYS_FORK(57) vs close(57)      → `close(0)` が fork として実行された
  - SYS_GETDENTS(78) vs readlinkat → realpath が壊れた

2 件目のとき、監査が「カーネルが名前を持つ番号」しか突き合わせていなかった
ために見落とした。ここでは musl の bits/syscall.h (asm-generic の全番号) を
正として突き合わせる。

チェック内容:
  1. include/linux_syscalls.h の各値が musl の __NR_* と一致するか
  2. 同じ番号を 2 つの名前に割り当てていないか
  3. kernel/riscv64/ にレガシー SYS_* の参照が残っていないか (コメントは除く)

  make riscv64-syscall-audit で実行する。
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(REPO, "include/linux_syscalls.h")
MUSL = os.path.join(REPO, "ports/musl-install-riscv64/include/bits/syscall.h")
SRC_DIR = os.path.join(REPO, "kernel/riscv64")


def fail(msg):
    print("NG: " + msg)
    return 1


def main():
    errors = 0

    if not os.path.exists(MUSL):
        print("skip: %s が無い (make riscv64-musl-sysroot が未実行)" % MUSL)
        return 0

    musl = {}
    for m in re.finditer(r"#define\s+__NR_(\w+)\s+(\d+)", open(MUSL).read()):
        musl[m.group(1)] = int(m.group(2))

    ours = {}
    for m in re.finditer(r"#define\s+LINUX_SYS_(\w+)\s+(\d+)", open(HDR).read()):
        ours[m.group(1).lower()] = int(m.group(2))

    # 1. musl の表と一致するか
    for name, val in sorted(ours.items()):
        if name not in musl:
            errors += fail("%s=%d は asm-generic に無い番号" % (name, val))
        elif musl[name] != val:
            errors += fail("%s: ours=%d asm-generic=%d" % (name, val, musl[name]))

    # 2. 番号の重複
    by_value = {}
    for name, val in ours.items():
        by_value.setdefault(val, []).append(name)
    for val, names in sorted(by_value.items()):
        if len(names) > 1:
            errors += fail("番号 %d に複数の名前: %s" % (val, ", ".join(sorted(names))))

    # 3. レガシー番号の残存 (コメント行は除く)
    legacy = re.compile(r"(?<![\w])SYS_[A-Z0-9_]+")
    for root, _dirs, files in os.walk(SRC_DIR):
        for fname in sorted(files):
            if not fname.endswith((".c", ".h", ".S")):
                continue
            # macOS が残す AppleDouble (._foo.c) は中身がバイナリなので読まない。
            # 拡張子だけで拾うと UnicodeDecodeError で監査ごと落ちる。
            if fname.startswith("._"):
                continue
            path = os.path.join(root, fname)
            in_block = False
            for lineno, line in enumerate(open(path), 1):
                code = line
                # 複数行にまたがるブロックコメントを除去する。過去の衝突の経緯を
                # コメントで残しているので、ここを雑にやると誤検知になる
                out = []
                i = 0
                while i < len(code):
                    if in_block:
                        end = code.find("*/", i)
                        if end < 0:
                            i = len(code)
                        else:
                            in_block = False
                            i = end + 2
                    else:
                        start = code.find("/*", i)
                        line_c = code.find("//", i)
                        if line_c >= 0 and (start < 0 or line_c < start):
                            out.append(code[i:line_c])
                            break
                        if start < 0:
                            out.append(code[i:])
                            break
                        out.append(code[i:start])
                        in_block = True
                        i = start + 2
                code = re.sub(r"LINUX_SYS_\w+", "", "".join(out))
                if legacy.search(code):
                    rel = os.path.relpath(path, REPO)
                    errors += fail("%s:%d にレガシー SYS_* が残っている: %s"
                                   % (rel, lineno, line.strip()))

    if errors:
        print("riscv64 syscall audit: FAIL (%d 件)" % errors)
        return 1
    print("riscv64 syscall audit: PASS (%d 番号を asm-generic と照合)" % len(ours))
    return 0


if __name__ == "__main__":
    sys.exit(main())
