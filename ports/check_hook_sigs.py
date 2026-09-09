#!/usr/bin/env python3
"""移植した RISC-V バックエンドの target hook signature を GCC の target.def と照合する。

signature がずれたフックは **コンパイルは通ってしまう** (フック表は関数ポインタを
void* 相当で受ける) 一方、呼ばれた瞬間に引数がずれて落ちる。実際 4.7 への移植では
TARGET_RTX_COSTS の引数 1 つ違いで cc1 が空ファイルでも SIGSEGV した。
ビルドが通ったあとでも一度これを通しておくと早く気付ける。

  python3 ports/check_hook_sigs.py ports/gcc-4.7.4-riscv

注意: 4.7 の target.def には legitimize_address / legitimate_address_p のように
同名で addr_space 版が併存するフックがある。ここでは引数の少ない通常版と
多い addr_space 版の**両方**と突き合わせ、どちらとも合わないものだけを報告する。
"""
import io
import os
import re
import sys


def params_of(sig):
    """関数引数リストを、入れ子の括弧を無視して分割する"""
    depth, cur, out = 0, '', []
    for ch in sig:
        if ch == '(':
            depth += 1
            continue
        if ch == ')':
            depth -= 1
            continue
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
            continue
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return [p for p in out if p and p != 'void']


def main(tree):
    rc_path = os.path.join(tree, 'gcc/config/riscv/riscv.c')
    td_path = os.path.join(tree, 'gcc/target.def')
    rc = io.open(rc_path, encoding='utf-8', errors='surrogateescape').read()
    td = io.open(td_path, encoding='utf-8', errors='surrogateescape').read()

    # target.def は同名フックが複数あり得るので、名前 → 引数数の集合で持つ
    hooks = {}
    for m in re.finditer(r'^\((\w+),\s*\n\s*"[^"]*",\s*\n\s*([^,]+),\s*\((.*?)\),',
                         td, re.M | re.S):
        hooks.setdefault(m.group(1), []).append(params_of(m.group(3)))

    regs = re.findall(r'^#define\s+(TARGET_[A-Z0-9_]+)\s+(riscv_\w+)\s*$', rc, re.M)

    checked, bad = 0, []
    for macro, fn in regs:
        key = macro[len('TARGET_'):].lower()
        if key not in hooks:
            continue
        m = re.search(r'^\w[\w \*]*\n' + re.escape(fn) + r'\s*\((.*?)\)\s*\n\{',
                      rc, re.M | re.S)
        if not m:
            continue
        impl = params_of(m.group(1))
        checked += 1
        arities = [len(p) for p in hooks[key]]
        if len(impl) not in arities:
            bad.append((macro, fn, len(impl), hooks[key]))

    print("登録フック %d 個中 %d 個を target.def と照合" % (len(regs), checked))
    if not bad:
        print("引数数の不一致: なし")
        return 0
    print("引数数が一致しないフック: %d 件" % len(bad))
    for macro, fn, ni, sigs in bad:
        print("  %s (%s): 実装 %d 引数" % (macro, fn, ni))
        for s in sigs:
            print("      target.def: (%s)" % ', '.join(s))
    return 1


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
