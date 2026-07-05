#!/usr/bin/env python3
"""
build_rootfs_xv6fs.py
xv6fs フォーマットの rootfs イメージを生成する。
Orthox-64 向け拡張:
  FSSIZE=327680 blocks (320 MB), NINODES=8192,
  triple indirect ブロック対応 (最大 ~16 GB/file)
"""
import os
import struct
import sys
from pathlib import Path

# ──────────────────────────────────────────────
# FS 定数 (Orthox-64 拡張版)
# ──────────────────────────────────────────────
BSIZE      = 1024
FSSIZE     = 327680       # 320 MB in 1 KB blocks
NINODES    = 8192
LOGBLOCKS  = 126          # log data-blocks (excluding header block)

NDIRECT    = 9            # 直接ブロック数
NINDIRECT  = BSIZE // 4   # 1 段間接: 256
NDINDIRECT = NINDIRECT * NINDIRECT              # 2 段間接: 65536
NTINDIRECT = NINDIRECT * NINDIRECT * NINDIRECT  # 3 段間接: 16777216
MAXFILE    = NDIRECT + NINDIRECT + NDINDIRECT + NTINDIRECT

FSMAGIC  = 0x10203040
ROOTINO  = 1

T_DIR    = 1
T_FILE   = 2
T_DEVICE = 3
T_FIFO   = 4
MODE_MAGIC = 0x4f4d
DIRSIZ   = 62   # dirent = inum(2) + name(62) = 64 bytes; BSIZE/64 = 16 entries/block

# dinode on-disk layout:
#   short type(2) + short major(2) + short minor(2) + short nlink(2)
#   + uint size(4) + uint addrs[NDIRECT+3](4×12) = 60 bytes
#   addrs[0..8]=direct(9), [9]=indirect, [10]=dindirect, [11]=tindirect
DINODE_ADDRS = NDIRECT + 3            # 12 (9+3)
DINODE_SIZE  = 2 + 2 + 2 + 2 + 4 + 4 * DINODE_ADDRS  # 60
DINODE_FMT   = f"<hhhhI{DINODE_ADDRS}I"

IPB = BSIZE // DINODE_SIZE            # 17 inodes per block

# dirent on-disk layout: ushort inum(2) + char name[62] = 64 bytes
DIRSIZ_BYTES = DIRSIZ
DIRENT_FMT   = f"<H{DIRSIZ}s"

# ──────────────────────────────────────────────
# FS layout (computed)
# ──────────────────────────────────────────────
nlog         = LOGBLOCKS + 1               # 127 (1 header + 126 data)
ninodeblocks = NINODES // IPB + 1
nbitmap      = FSSIZE // (BSIZE * 8) + 1
nmeta        = 2 + nlog + ninodeblocks + nbitmap
nblocks      = FSSIZE - nmeta

logstart     = 2
inodestart   = 2 + nlog
bmapstart    = 2 + nlog + ninodeblocks

# ──────────────────────────────────────────────
# グローバル状態
# ──────────────────────────────────────────────
freeinode: int      = 1
freeblock: int      = nmeta
image: bytearray    = bytearray(FSSIZE * BSIZE)


# ──────────────────────────────────────────────
# ブロック I/O
# ──────────────────────────────────────────────
def wsect(sec: int, data: bytes) -> None:
    assert 0 <= sec < FSSIZE, f"wsect out of range: {sec}"
    assert len(data) == BSIZE, f"wsect wrong size: {len(data)}"
    base = sec * BSIZE
    image[base:base + BSIZE] = data


def rsect(sec: int) -> bytearray:
    assert 0 <= sec < FSSIZE, f"rsect out of range: {sec}"
    base = sec * BSIZE
    return bytearray(image[base:base + BSIZE])


# ──────────────────────────────────────────────
# inode I/O
# ──────────────────────────────────────────────
def _iblock(inum: int) -> int:
    return inum // IPB + inodestart


def winode(inum: int, din: dict) -> None:
    bn  = _iblock(inum)
    buf = rsect(bn)
    off = (inum % IPB) * DINODE_SIZE
    packed = struct.pack(
        DINODE_FMT,
        din['type'], din['major'], din['minor'], din['nlink'],
        din['size'],
        *din['addrs'],
    )
    buf[off:off + DINODE_SIZE] = packed
    wsect(bn, bytes(buf))


def rinode(inum: int) -> dict:
    bn   = _iblock(inum)
    buf  = rsect(bn)
    off  = (inum % IPB) * DINODE_SIZE
    flds = struct.unpack(DINODE_FMT, buf[off:off + DINODE_SIZE])
    return {
        'type':  flds[0],
        'major': flds[1],
        'minor': flds[2],
        'nlink': flds[3],
        'size':  flds[4],
        'addrs': list(flds[5:]),
    }


def ialloc(ftype: int, mode: int | None = None) -> int:
    global freeinode
    inum = freeinode
    freeinode += 1
    assert inum < NINODES, f"out of inodes: {inum} (max {NINODES})"
    if mode is None:
        mode = 0o755 if ftype == T_DIR else 0o644
    din = {'type': ftype, 'major': 0, 'minor': 0, 'nlink': 1,
           'size': 0, 'addrs': [0] * DINODE_ADDRS}
    if ftype in (T_DIR, T_FILE):
        din['major'] = MODE_MAGIC
        din['minor'] = mode & 0o7777
    winode(inum, din)
    return inum


# ──────────────────────────────────────────────
# ブロック割り当て補助
# ──────────────────────────────────────────────
def _alloc_block() -> int:
    global freeblock
    b = freeblock
    freeblock += 1
    assert b < FSSIZE, f"out of data blocks: {b} (FSSIZE={FSSIZE})"
    return b


def _read_uints(bno: int) -> list:
    return list(struct.unpack(f"<{BSIZE // 4}I", rsect(bno)))


def _write_uints(bno: int, arr: list) -> None:
    wsect(bno, struct.pack(f"<{BSIZE // 4}I", *arr))


# ──────────────────────────────────────────────
# iappend — double indirect 対応
# ──────────────────────────────────────────────
def iappend(inum: int, data: bytes | bytearray) -> None:
    din    = rinode(inum)
    addrs  = din['addrs']
    off    = din['size']
    mv     = memoryview(bytes(data))
    remain = len(mv)

    while remain > 0:
        fbn     = off // BSIZE
        blkoff  = off % BSIZE
        n1      = min(remain, BSIZE - blkoff)

        assert fbn < MAXFILE, f"iappend: file too large (fbn={fbn}, MAXFILE={MAXFILE})"

        if fbn < NDIRECT:
            if addrs[fbn] == 0:
                addrs[fbn] = _alloc_block()
            x = addrs[fbn]

        elif fbn < NDIRECT + NINDIRECT:
            # 1 段間接 (addrs[NDIRECT])
            if addrs[NDIRECT] == 0:
                addrs[NDIRECT] = _alloc_block()
                _write_uints(addrs[NDIRECT], [0] * (BSIZE // 4))
            ind = _read_uints(addrs[NDIRECT])
            idx = fbn - NDIRECT
            if ind[idx] == 0:
                ind[idx] = _alloc_block()
                _write_uints(addrs[NDIRECT], ind)
            x = ind[idx]

        elif fbn < NDIRECT + NINDIRECT + NDINDIRECT:
            # 2 段間接 (addrs[NDIRECT+1])
            if addrs[NDIRECT + 1] == 0:
                addrs[NDIRECT + 1] = _alloc_block()
                _write_uints(addrs[NDIRECT + 1], [0] * (BSIZE // 4))
            dind = _read_uints(addrs[NDIRECT + 1])
            rel  = fbn - NDIRECT - NINDIRECT
            i2   = rel // NINDIRECT
            i1   = rel % NINDIRECT
            if dind[i2] == 0:
                dind[i2] = _alloc_block()
                _write_uints(addrs[NDIRECT + 1], dind)
                _write_uints(dind[i2], [0] * (BSIZE // 4))
            ind = _read_uints(dind[i2])
            if ind[i1] == 0:
                ind[i1] = _alloc_block()
                _write_uints(dind[i2], ind)
            x = ind[i1]

        else:
            # 3 段間接 (addrs[NDIRECT+2])
            if addrs[NDIRECT + 2] == 0:
                addrs[NDIRECT + 2] = _alloc_block()
                _write_uints(addrs[NDIRECT + 2], [0] * (BSIZE // 4))
            rel = fbn - NDIRECT - NINDIRECT - NDINDIRECT
            i3  = rel // NDINDIRECT
            r2  = rel % NDINDIRECT
            i2  = r2 // NINDIRECT
            i1  = r2 % NINDIRECT
            # L1 テーブル
            tind = _read_uints(addrs[NDIRECT + 2])
            if tind[i3] == 0:
                tind[i3] = _alloc_block()
                _write_uints(addrs[NDIRECT + 2], tind)
                _write_uints(tind[i3], [0] * (BSIZE // 4))
            # L2 テーブル
            dind = _read_uints(tind[i3])
            if dind[i2] == 0:
                dind[i2] = _alloc_block()
                _write_uints(tind[i3], dind)
                _write_uints(dind[i2], [0] * (BSIZE // 4))
            # L3 テーブル
            ind = _read_uints(dind[i2])
            if ind[i1] == 0:
                ind[i1] = _alloc_block()
                _write_uints(dind[i2], ind)
            x = ind[i1]

        buf = rsect(x)
        buf[blkoff:blkoff + n1] = mv[:n1]
        wsect(x, bytes(buf))

        mv     = mv[n1:]
        remain -= n1
        off    += n1

    din['size']  = off
    din['addrs'] = addrs
    winode(inum, din)


# ──────────────────────────────────────────────
# ディレクトリ操作
# ──────────────────────────────────────────────
def _dirent(inum: int, name: str | bytes) -> bytes:
    if isinstance(name, str):
        nb = name.encode('utf-8', errors='replace')
    else:
        nb = name
    nb = nb[:DIRSIZ].ljust(DIRSIZ, b'\x00')
    return struct.pack(DIRENT_FMT, inum, nb)


def add_dirent(parent: int, name: str, child: int) -> None:
    iappend(parent, _dirent(child, name))


def mkdirnode(parent: int | None, name: str | None, mode: int | None = None) -> int:
    inum = ialloc(T_DIR, mode)
    iappend(inum, _dirent(inum, "."))
    iappend(inum, _dirent(parent if parent is not None else inum, ".."))
    if parent is not None and name is not None:
        add_dirent(parent, name, inum)
    return inum


def mkdevnode(parent: int, name: str, major: int, minor: int) -> int:
    """T_DEVICE inode を作成する。major/minor は実デバイス番号として
    そのまま格納する (T_FILE/T_DIR の MODE_MAGIC スキームは適用しない)。"""
    inum = ialloc(T_DEVICE)
    din = rinode(inum)
    din['major'] = major
    din['minor'] = minor
    winode(inum, din)
    add_dirent(parent, name, inum)
    return inum


# ホストツリーに実デバイスノードは置けないため、/dev はここで生成する。
# major/minor は Linux 慣例に合わせる。
DEV_NODES = [
    ("null",    1, 3),
    ("zero",    1, 5),
    ("random",  1, 8),
    ("urandom", 1, 9),
    ("tty",     5, 0),
    ("console", 5, 1),
]


def should_skip(name: str) -> bool:
    return name == '.DS_Store' or name.startswith('._')


# ──────────────────────────────────────────────
# rootfs ツリーの書き込み
# ──────────────────────────────────────────────
def populate(host_path: Path, dir_inum: int, depth: int = 0) -> None:
    try:
        entries = sorted(host_path.iterdir(), key=lambda p: (p.is_dir(), p.name))
    except PermissionError as e:
        print(f"\nSKIP(perm) {host_path}: {e}", file=sys.stderr)
        return

    total = len(entries)
    done  = 0

    for entry in entries:
        done += 1
        name = entry.name
        if should_skip(name):
            continue

        name_b = name.encode('utf-8', errors='replace')
        if len(name_b) > DIRSIZ:
            truncated = name_b[:DIRSIZ].decode('latin-1')
            print(f"\nWARN: name truncated {name!r} -> {truncated!r}", file=sys.stderr)
            name = truncated

        if entry.is_symlink():
            try:
                target = os.readlink(entry).encode()
                child  = ialloc(T_FILE, entry.lstat().st_mode & 0o7777)
                add_dirent(dir_inum, name, child)
                iappend(child, target)
            except OSError as e:
                print(f"\nSKIP symlink {entry}: {e}", file=sys.stderr)

        elif entry.is_dir():
            child = mkdirnode(dir_inum, name, entry.stat().st_mode & 0o7777)
            populate(entry, child, depth + 1)

        else:
            try:
                fsize = entry.stat().st_size
                child = ialloc(T_FILE, entry.stat().st_mode & 0o7777)
                add_dirent(dir_inum, name, child)
                with entry.open('rb') as f:
                    while True:
                        chunk = f.read(512 * 1024)
                        if not chunk:
                            break
                        iappend(child, chunk)
                if depth == 0 or fsize >= 1 << 20:
                    print(f"\n  {'  ' * depth}{name}  ({fsize:,} B)", end='')
            except (PermissionError, OSError) as e:
                print(f"\nSKIP {entry}: {e}", file=sys.stderr)

        if depth == 0:
            pct = done * 100 // max(total, 1)
            print(f"\r  [{done:4d}/{total}] {pct:3d}%  {name[:32]:<32}", end='', flush=True)

    if depth == 0:
        print()


# ──────────────────────────────────────────────
# ビットマップ書き込み
# ──────────────────────────────────────────────
def write_bitmap() -> None:
    print(f"bitmap: {freeblock} blocks used, {nbitmap} bitmap block(s) at {bmapstart}")
    for i in range(nbitmap):
        buf  = bytearray(BSIZE)
        base = i * BSIZE * 8
        lim  = max(0, min(freeblock - base, BSIZE * 8))
        for b in range(lim):
            buf[b // 8] |= 1 << (b % 8)
        wsect(bmapstart + i, bytes(buf))


# ──────────────────────────────────────────────
# xv6fs ファイル抽出 (読み取り専用)
# ──────────────────────────────────────────────
def _get_data_block(addrs: list, fbn: int) -> int:
    """ファイルブロック番号 fbn に対応するディスクブロック番号を返す (未割り当て=0)。"""
    if fbn < NDIRECT:
        return addrs[fbn]
    fbn -= NDIRECT
    if fbn < NINDIRECT:
        if addrs[NDIRECT] == 0:
            return 0
        return _read_uints(addrs[NDIRECT])[fbn]
    fbn -= NINDIRECT
    if fbn < NDINDIRECT:
        if addrs[NDIRECT + 1] == 0:
            return 0
        dind = _read_uints(addrs[NDIRECT + 1])
        i2, i1 = fbn // NINDIRECT, fbn % NINDIRECT
        if dind[i2] == 0:
            return 0
        return _read_uints(dind[i2])[i1]
    fbn -= NDINDIRECT
    if fbn < NTINDIRECT:
        if addrs[NDIRECT + 2] == 0:
            return 0
        tind = _read_uints(addrs[NDIRECT + 2])
        i3  = fbn // NDINDIRECT
        r2  = fbn % NDINDIRECT
        i2, i1 = r2 // NINDIRECT, r2 % NINDIRECT
        if tind[i3] == 0:
            return 0
        dind = _read_uints(tind[i3])
        if dind[i2] == 0:
            return 0
        return _read_uints(dind[i2])[i1]
    return 0


def _read_file_data(din: dict) -> bytes:
    """inode の全データバイトを読み取って返す。"""
    size   = din['size']
    addrs  = din['addrs']
    result = bytearray()
    fbn    = 0
    while len(result) < size:
        blk = _get_data_block(addrs, fbn)
        to_read = min(BSIZE, size - len(result))
        if blk == 0:
            result.extend(bytes(to_read))
        else:
            result.extend(rsect(blk)[:to_read])
        fbn += 1
    return bytes(result)


def _type_name(ftype: int) -> str:
    return {
        0: "free",
        T_DIR: "dir",
        T_FILE: "file",
        T_DEVICE: "device",
        T_FIFO: "fifo",
    }.get(ftype, f"unknown({ftype})")


def _iter_dirents(din: dict) -> list[tuple[int, str]]:
    data = _read_file_data(din)
    entry_size = 2 + DIRSIZ
    entries: list[tuple[int, str]] = []
    for i in range(0, len(data), entry_size):
        raw = data[i:i + entry_size]
        if len(raw) < entry_size:
            break
        child_inum, name_b = struct.unpack(DIRENT_FMT, raw)
        if child_inum == 0:
            continue
        name = name_b.rstrip(b'\x00').decode('utf-8', errors='replace')
        entries.append((child_inum, name))
    return entries


def _file_block_refs(din: dict) -> tuple[list[tuple[int, int]], list[tuple[str, int]]]:
    """Return data block refs and indirect metadata block refs for an inode."""
    addrs = din['addrs']
    total = (din['size'] + BSIZE - 1) // BSIZE
    data_refs: list[tuple[int, int]] = []
    meta_refs: list[tuple[str, int]] = []

    for fbn in range(min(total, NDIRECT)):
        if addrs[fbn] != 0:
            data_refs.append((fbn, addrs[fbn]))

    remaining = total - NDIRECT
    if remaining <= 0:
        return data_refs, meta_refs

    if addrs[NDIRECT] != 0:
        meta_refs.append(("indirect", addrs[NDIRECT]))
        ind = _read_uints(addrs[NDIRECT])
        for idx in range(min(remaining, NINDIRECT)):
            if ind[idx] != 0:
                data_refs.append((NDIRECT + idx, ind[idx]))
    remaining -= NINDIRECT
    if remaining <= 0:
        return data_refs, meta_refs

    if addrs[NDIRECT + 1] != 0:
        meta_refs.append(("double", addrs[NDIRECT + 1]))
        dind = _read_uints(addrs[NDIRECT + 1])
        limit = min(remaining, NDINDIRECT)
        for rel in range(limit):
            i2, i1 = rel // NINDIRECT, rel % NINDIRECT
            if dind[i2] == 0:
                continue
            if i1 == 0:
                meta_refs.append((f"double[{i2}]", dind[i2]))
            ind = _read_uints(dind[i2])
            if ind[i1] != 0:
                data_refs.append((NDIRECT + NINDIRECT + rel, ind[i1]))
    remaining -= NDINDIRECT
    if remaining <= 0:
        return data_refs, meta_refs

    if addrs[NDIRECT + 2] != 0:
        meta_refs.append(("triple", addrs[NDIRECT + 2]))
        tind = _read_uints(addrs[NDIRECT + 2])
        limit = min(remaining, NTINDIRECT)
        seen_dind: set[int] = set()
        seen_ind: set[int] = set()
        for rel in range(limit):
            i3 = rel // NDINDIRECT
            r2 = rel % NDINDIRECT
            i2, i1 = r2 // NINDIRECT, r2 % NINDIRECT
            if tind[i3] == 0:
                continue
            if i3 not in seen_dind:
                meta_refs.append((f"triple[{i3}]", tind[i3]))
                seen_dind.add(i3)
            dind = _read_uints(tind[i3])
            if dind[i2] == 0:
                continue
            ind_key = i3 * NINDIRECT + i2
            if ind_key not in seen_ind:
                meta_refs.append((f"triple[{i3}][{i2}]", dind[i2]))
                seen_ind.add(ind_key)
            ind = _read_uints(dind[i2])
            if ind[i1] != 0:
                data_refs.append((NDIRECT + NINDIRECT + NDINDIRECT + rel, ind[i1]))

    return data_refs, meta_refs


def _bitmap_used_blocks() -> set[int]:
    used: set[int] = set()
    for bno in range(FSSIZE):
        bp = bmapstart + bno // (BSIZE * 8)
        off = bno % (BSIZE * 8)
        buf = rsect(bp)
        if buf[off // 8] & (1 << (off % 8)):
            used.add(bno)
    return used


def _set_bitmap_block(bno: int, used: bool) -> None:
    if bno < 0 or bno >= FSSIZE:
        raise ValueError(f"bitmap block out of range: {bno}")
    bp = bmapstart + bno // (BSIZE * 8)
    off = bno % (BSIZE * 8)
    buf = rsect(bp)
    if used:
        buf[off // 8] |= 1 << (off % 8)
    else:
        buf[off // 8] &= ~(1 << (off % 8))
    wsect(bp, bytes(buf))


def _find_dirent(dir_inum: int, din: dict, name: str) -> int | None:
    """ディレクトリ内の name を検索し、子 inum を返す。見つからなければ None。"""
    data = _read_file_data(din)
    entry_size = 2 + DIRSIZ
    for i in range(0, len(data), entry_size):
        raw = data[i:i + entry_size]
        if len(raw) < entry_size:
            break
        child_inum, name_b = struct.unpack(DIRENT_FMT, raw)
        if child_inum == 0:
            continue
        if name_b.rstrip(b'\x00').decode('utf-8', errors='replace') == name:
            return child_inum
    return None


def extract_file_from_image(img_path: str, fs_path: str, out_path: str) -> int:
    """xv6fs イメージから fs_path のファイルを out_path に書き出す。"""
    global image, inodestart, bmapstart, logstart

    raw = Path(img_path).read_bytes()
    image = bytearray(raw)

    sb_fields = struct.unpack("<IIIIIIII", bytes(image[BSIZE:BSIZE + 32]))
    magic, _, _, _, _, logstart_sb, inodestart_sb, bmapstart_sb = sb_fields
    if magic != FSMAGIC:
        print(f"ERROR: bad magic 0x{magic:08x} (expected 0x{FSMAGIC:08x})", file=sys.stderr)
        return 1

    inodestart = inodestart_sb
    bmapstart  = bmapstart_sb
    logstart   = logstart_sb

    parts = [p for p in fs_path.strip('/').split('/') if p]
    inum  = ROOTINO

    for part in parts:
        din = rinode(inum)
        if din['type'] != T_DIR:
            print(f"ERROR: inum={inum} is not a directory (looking for '{part}')", file=sys.stderr)
            return 1
        child = _find_dirent(inum, din, part)
        if child is None:
            print(f"ERROR: '{part}' not found", file=sys.stderr)
            return 1
        inum = child

    din = rinode(inum)
    if din['type'] != T_FILE:
        print(f"ERROR: {fs_path!r} is not a regular file (type={din['type']})", file=sys.stderr)
        return 1

    data = _read_file_data(din)
    Path(out_path).write_bytes(data)
    print(f"Extracted {len(data):,} bytes: {fs_path} -> {out_path}")
    return 0


def _load_image(img_path: str) -> int:
    """既存 xv6fs イメージを読み込み、superblock 由来の layout を反映する。"""
    global image, inodestart, bmapstart, logstart

    raw = Path(img_path).read_bytes()
    image = bytearray(raw)

    sb_fields = struct.unpack("<IIIIIIII", bytes(image[BSIZE:BSIZE + 32]))
    magic, _, _, _, _, logstart_sb, inodestart_sb, bmapstart_sb = sb_fields
    if magic != FSMAGIC:
        print(f"ERROR: bad magic 0x{magic:08x} (expected 0x{FSMAGIC:08x})", file=sys.stderr)
        return 1

    inodestart = logstart_sb + (inodestart_sb - logstart_sb)
    bmapstart  = bmapstart_sb
    logstart   = logstart_sb
    return 0


def _lookup_path(fs_path: str) -> int | None:
    parts = [p for p in fs_path.strip('/').split('/') if p]
    inum  = ROOTINO

    for part in parts:
        din = rinode(inum)
        if din['type'] != T_DIR:
            print(f"ERROR: inum={inum} is not a directory (looking for '{part}')", file=sys.stderr)
            return None
        child = _find_dirent(inum, din, part)
        if child is None:
            print(f"ERROR: '{part}' not found", file=sys.stderr)
            return None
        inum = child
    return inum


def _allocated_file_blocks(din: dict) -> list[int]:
    blocks = []
    total = (din['size'] + BSIZE - 1) // BSIZE
    for fbn in range(total):
        blk = _get_data_block(din['addrs'], fbn)
        if blk == 0:
            break
        blocks.append(blk)
    return blocks


def replace_file_in_image(img_path: str, fs_path: str, host_path: str) -> int:
    """既存 regular file の割り当て済みブロック内で内容だけを差し替える。

    inode や block の新規割り当ては行わない。/kbuild のキャッシュを保持したまま
    /etc/bootcmd のような小ファイルを差し替えるための保守的な操作。
    """
    if _load_image(img_path) != 0:
        return 1

    inum = _lookup_path(fs_path)
    if inum is None:
        return 1

    din = rinode(inum)
    if din['type'] != T_FILE:
        print(f"ERROR: {fs_path!r} is not a regular file (type={din['type']})", file=sys.stderr)
        return 1

    data = Path(host_path).read_bytes()
    blocks = _allocated_file_blocks(din)
    capacity = len(blocks) * BSIZE
    if len(data) > capacity:
        print(
            f"ERROR: replacement too large for allocated file blocks: "
            f"{len(data)} > {capacity} bytes ({fs_path})",
            file=sys.stderr,
        )
        return 1

    written = 0
    for blk in blocks:
        buf = bytearray(BSIZE)
        chunk = data[written:written + BSIZE]
        buf[:len(chunk)] = chunk
        wsect(blk, bytes(buf))
        written += len(chunk)
        if written >= len(data):
            # Zero the rest of the previously allocated blocks so stale bytes
            # cannot leak if a reader ignores inode size.
            for rest in blocks[blocks.index(blk) + 1:]:
                wsect(rest, bytes(BSIZE))
            break

    din['size'] = len(data)
    winode(inum, din)
    Path(img_path).write_bytes(image)
    print(f"Replaced {len(data):,} bytes: {host_path} -> {fs_path} in {img_path}")
    return 0


def sparsify_zero_blocks_in_image(img_path: str, fs_path: str) -> int:
    """Convert fully zero-filled data blocks in a file into sparse holes."""
    if _load_image(img_path) != 0:
        return 1

    inum = _lookup_path(fs_path)
    if inum is None:
        return 1

    din = rinode(inum)
    if din['type'] != T_FILE:
        print(f"ERROR: {fs_path!r} is not a regular file (type={din['type']})", file=sys.stderr)
        return 1

    addrs = din['addrs']
    total = (din['size'] + BSIZE - 1) // BSIZE
    zero_block = bytes(BSIZE)
    cleared = 0

    for fbn in range(total):
        blk = _get_data_block(addrs, fbn)
        if blk == 0 or bytes(rsect(blk)) != zero_block:
            continue

        if fbn < NDIRECT:
            addrs[fbn] = 0
        elif fbn < NDIRECT + NINDIRECT:
            ind = _read_uints(addrs[NDIRECT])
            ind[fbn - NDIRECT] = 0
            _write_uints(addrs[NDIRECT], ind)
        elif fbn < NDIRECT + NINDIRECT + NDINDIRECT:
            rel = fbn - NDIRECT - NINDIRECT
            dind = _read_uints(addrs[NDIRECT + 1])
            ind_blk = dind[rel // NINDIRECT]
            ind = _read_uints(ind_blk)
            ind[rel % NINDIRECT] = 0
            _write_uints(ind_blk, ind)
        else:
            rel = fbn - NDIRECT - NINDIRECT - NDINDIRECT
            tind = _read_uints(addrs[NDIRECT + 2])
            dind_blk = tind[rel // NDINDIRECT]
            dind = _read_uints(dind_blk)
            r2 = rel % NDINDIRECT
            ind_blk = dind[r2 // NINDIRECT]
            ind = _read_uints(ind_blk)
            ind[r2 % NINDIRECT] = 0
            _write_uints(ind_blk, ind)

        _set_bitmap_block(blk, False)
        cleared += 1

    din['addrs'] = addrs
    winode(inum, din)
    Path(img_path).write_bytes(image)
    print(f"Sparsified {cleared} zero data block(s): {fs_path} in {img_path}")
    return 0


def list_path_in_image(img_path: str, fs_path: str) -> int:
    if _load_image(img_path) != 0:
        return 1

    inum = _lookup_path(fs_path)
    if inum is None:
        return 1

    din = rinode(inum)
    if din['type'] != T_DIR:
        print(f"ERROR: {fs_path!r} is not a directory (type={din['type']})", file=sys.stderr)
        return 1

    print(f"{fs_path or '/'}: inum={inum} entries={len(_iter_dirents(din))}")
    for child_inum, name in _iter_dirents(din):
        child = rinode(child_inum)
        print(
            f"{child_inum:5d}  {_type_name(child['type']):8s}  "
            f"{child['size']:10d}  {name}"
        )
    return 0


def stat_path_in_image(img_path: str, fs_path: str) -> int:
    if _load_image(img_path) != 0:
        return 1

    inum = _lookup_path(fs_path)
    if inum is None:
        return 1

    din = rinode(inum)
    data_refs, meta_refs = _file_block_refs(din)
    holes = ((din['size'] + BSIZE - 1) // BSIZE) - len(data_refs)
    print(f"path: {fs_path or '/'}")
    print(f"  inum:  {inum}")
    print(f"  type:  {_type_name(din['type'])} ({din['type']})")
    print(f"  nlink: {din['nlink']}")
    print(f"  size:  {din['size']} bytes")
    print(f"  data blocks:     {len(data_refs)}")
    print(f"  sparse holes:    {max(0, holes)}")
    print(f"  indirect blocks: {len(meta_refs)}")
    print(f"  addrs: {' '.join(str(a) for a in din['addrs'])}")
    return 0


def dump_inode_in_image(img_path: str, inum_text: str) -> int:
    if _load_image(img_path) != 0:
        return 1

    try:
        inum = int(inum_text, 0)
    except ValueError:
        print(f"ERROR: bad inode number: {inum_text}", file=sys.stderr)
        return 1
    if inum < 0 or inum >= NINODES:
        print(f"ERROR: inode out of range: {inum}", file=sys.stderr)
        return 1

    din = rinode(inum)
    data_refs, meta_refs = _file_block_refs(din)
    print(f"inode {inum}")
    print(f"  type:  {_type_name(din['type'])} ({din['type']})")
    print(f"  major: {din['major']}")
    print(f"  minor: {din['minor']}")
    print(f"  nlink: {din['nlink']}")
    print(f"  size:  {din['size']} bytes")
    print(f"  addrs: {' '.join(str(a) for a in din['addrs'])}")
    if meta_refs:
        print("  indirect metadata blocks:")
        for label, blk in meta_refs:
            print(f"    {label:18s} -> {blk}")
    if data_refs:
        print("  data blocks:")
        for fbn, blk in data_refs:
            print(f"    fbn {fbn:8d} -> {blk}")
    if din['type'] == T_DIR:
        print("  dirents:")
        for child_inum, name in _iter_dirents(din):
            print(f"    {child_inum:5d}  {name}")
    return 0


def _block_range_summary(blocks: set[int], limit: int = 12) -> str:
    if not blocks:
        return "none"
    vals = sorted(blocks)
    shown = ", ".join(str(v) for v in vals[:limit])
    if len(vals) > limit:
        shown += f", ... (+{len(vals) - limit})"
    return shown


def check_image(img_path: str) -> int:
    if _load_image(img_path) != 0:
        return 1

    errors: list[str] = []
    warnings: list[str] = []
    referenced: dict[int, str] = {}

    raw_sb = struct.unpack("<IIIIIIII", bytes(image[BSIZE:BSIZE + 32]))
    magic, sb_size, sb_nblocks, sb_ninodes, sb_nlog, sb_logstart, sb_inodestart, sb_bmapstart = raw_sb
    if magic != FSMAGIC:
        errors.append(f"bad magic 0x{magic:08x}")
    if sb_size != FSSIZE:
        warnings.append(f"superblock size differs from tool constant: sb={sb_size} tool={FSSIZE}")
    if sb_ninodes != NINODES:
        warnings.append(f"superblock ninodes differs from tool constant: sb={sb_ninodes} tool={NINODES}")
    if sb_logstart != logstart or sb_inodestart != inodestart or sb_bmapstart != bmapstart:
        errors.append("superblock layout was not loaded consistently")
    if sb_nblocks != nblocks:
        warnings.append(f"superblock nblocks differs from tool constant: sb={sb_nblocks} tool={nblocks}")
    if sb_nlog != nlog:
        warnings.append(f"superblock nlog differs from tool constant: sb={sb_nlog} tool={nlog}")

    root = rinode(ROOTINO)
    if root['type'] != T_DIR:
        errors.append(f"ROOTINO={ROOTINO} is not a directory (type={root['type']})")

    used_bitmap = _bitmap_used_blocks()
    fixed_blocks = set(range(nmeta))
    allocated_inodes = 0

    for inum in range(1, NINODES):
        din = rinode(inum)
        if din['type'] == 0:
            continue
        allocated_inodes += 1
        if din['type'] not in (T_DIR, T_FILE, T_DEVICE, T_FIFO):
            errors.append(f"inode {inum}: unknown type {din['type']}")
        if din['nlink'] <= 0:
            warnings.append(f"inode {inum}: nlink <= 0 ({din['nlink']})")
        if din['size'] > MAXFILE * BSIZE:
            errors.append(f"inode {inum}: size exceeds MAXFILE ({din['size']})")
        if din['type'] == T_DIR and din['size'] % (2 + DIRSIZ) != 0:
            errors.append(f"inode {inum}: directory size is not dirent-aligned ({din['size']})")

        if din['type'] == T_DIR:
            for child_inum, name in _iter_dirents(din):
                if child_inum >= NINODES:
                    errors.append(f"inode {inum}: dirent {name!r} points out of range ({child_inum})")
                elif rinode(child_inum)['type'] == 0:
                    errors.append(f"inode {inum}: dirent {name!r} points to free inode {child_inum}")

        data_refs, meta_refs = _file_block_refs(din)
        for label, blk in [(f"data fbn {fbn}", blk) for fbn, blk in data_refs]:
            if blk < nmeta or blk >= FSSIZE:
                errors.append(f"inode {inum}: {label} block out of data range: {blk}")
                continue
            owner = referenced.setdefault(blk, f"inode {inum} {label}")
            if owner != f"inode {inum} {label}":
                errors.append(f"block {blk}: duplicate reference by {owner} and inode {inum} {label}")
        for label, blk in meta_refs:
            if blk < nmeta or blk >= FSSIZE:
                errors.append(f"inode {inum}: {label} metadata block out of data range: {blk}")
                continue
            owner = referenced.setdefault(blk, f"inode {inum} {label}")
            if owner != f"inode {inum} {label}":
                errors.append(f"block {blk}: duplicate reference by {owner} and inode {inum} {label}")

    referenced_blocks = set(referenced)
    missing_bitmap = referenced_blocks - used_bitmap
    if missing_bitmap:
        errors.append(f"referenced blocks missing from bitmap: {_block_range_summary(missing_bitmap)}")

    allocated_unreferenced = used_bitmap - fixed_blocks - referenced_blocks
    if allocated_unreferenced:
        warnings.append(
            f"bitmap-allocated data blocks not referenced by live inodes: "
            f"{len(allocated_unreferenced)} ({_block_range_summary(allocated_unreferenced)})"
        )

    free_but_fixed = fixed_blocks - used_bitmap
    if free_but_fixed:
        errors.append(f"metadata blocks missing from bitmap: {_block_range_summary(free_but_fixed)}")

    print("xv6fs check:")
    print(f"  image:          {img_path}")
    print(f"  size blocks:    {sb_size}")
    print(f"  inodes used:    {allocated_inodes} / {NINODES}")
    print(f"  bitmap used:    {len(used_bitmap)} / {FSSIZE}")
    print(f"  referenced fs:  {len(referenced_blocks)} data/meta blocks")
    print(f"  unreferenced:   {len(allocated_unreferenced)} bitmap-allocated data blocks")

    for warning in warnings:
        print(f"WARN: {warning}", file=sys.stderr)
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)

    if errors:
        print(f"FAILED: {len(errors)} error(s), {len(warnings)} warning(s)")
        return 1
    print(f"OK: {len(warnings)} warning(s)")
    return 0


# ──────────────────────────────────────────────
# main
# ──────────────────────────────────────────────
def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == '--extract':
        if len(sys.argv) != 5:
            print("usage: build_rootfs_xv6fs.py --extract FS_PATH OUT_FILE IMG_FILE", file=sys.stderr)
            return 1
        return extract_file_from_image(sys.argv[4], sys.argv[2], sys.argv[3])

    if len(sys.argv) >= 2 and sys.argv[1] == '--replace':
        if len(sys.argv) != 5:
            print("usage: build_rootfs_xv6fs.py --replace FS_PATH HOST_FILE IMG_FILE", file=sys.stderr)
            return 1
        return replace_file_in_image(sys.argv[4], sys.argv[2], sys.argv[3])

    if len(sys.argv) >= 2 and sys.argv[1] == '--ls':
        if len(sys.argv) != 4:
            print("usage: build_rootfs_xv6fs.py --ls FS_PATH IMG_FILE", file=sys.stderr)
            return 1
        return list_path_in_image(sys.argv[3], sys.argv[2])

    if len(sys.argv) >= 2 and sys.argv[1] == '--stat':
        if len(sys.argv) != 4:
            print("usage: build_rootfs_xv6fs.py --stat FS_PATH IMG_FILE", file=sys.stderr)
            return 1
        return stat_path_in_image(sys.argv[3], sys.argv[2])

    if len(sys.argv) >= 2 and sys.argv[1] == '--dump-inode':
        if len(sys.argv) != 4:
            print("usage: build_rootfs_xv6fs.py --dump-inode INUM IMG_FILE", file=sys.stderr)
            return 1
        return dump_inode_in_image(sys.argv[3], sys.argv[2])

    if len(sys.argv) >= 2 and sys.argv[1] == '--check':
        if len(sys.argv) != 3:
            print("usage: build_rootfs_xv6fs.py --check IMG_FILE", file=sys.stderr)
            return 1
        return check_image(sys.argv[2])

    if len(sys.argv) >= 2 and sys.argv[1] == '--sparsify-zero-blocks':
        if len(sys.argv) != 4:
            print("usage: build_rootfs_xv6fs.py --sparsify-zero-blocks FS_PATH IMG_FILE", file=sys.stderr)
            return 1
        return sparsify_zero_blocks_in_image(sys.argv[3], sys.argv[2])

    if len(sys.argv) < 3:
        print("usage: build_rootfs_xv6fs.py ROOT_DIR OUT_IMG", file=sys.stderr)
        print("       build_rootfs_xv6fs.py --extract FS_PATH OUT_FILE IMG_FILE", file=sys.stderr)
        print("       build_rootfs_xv6fs.py --replace FS_PATH HOST_FILE IMG_FILE", file=sys.stderr)
        print("       build_rootfs_xv6fs.py --ls FS_PATH IMG_FILE", file=sys.stderr)
        print("       build_rootfs_xv6fs.py --stat FS_PATH IMG_FILE", file=sys.stderr)
        print("       build_rootfs_xv6fs.py --dump-inode INUM IMG_FILE", file=sys.stderr)
        print("       build_rootfs_xv6fs.py --check IMG_FILE", file=sys.stderr)
        print("       build_rootfs_xv6fs.py --sparsify-zero-blocks FS_PATH IMG_FILE", file=sys.stderr)
        return 1

    root_dir = Path(sys.argv[1])
    out_img  = Path(sys.argv[2])

    if not root_dir.is_dir():
        print(f"not a directory: {root_dir}", file=sys.stderr)
        return 1

    # superblock (block 1)
    sb_buf = bytearray(BSIZE)
    sb_buf[:32] = struct.pack(
        "<IIIIIIII",
        FSMAGIC, FSSIZE, nblocks, NINODES,
        nlog, logstart, inodestart, bmapstart,
    )
    wsect(1, bytes(sb_buf))

    print("xv6fs image parameters:")
    print(f"  FSSIZE={FSSIZE} blocks = {FSSIZE * BSIZE // 1024 // 1024} MB")
    print(f"  nmeta={nmeta}  log={nlog}@{logstart}  inode={ninodeblocks}@{inodestart}  bitmap={nbitmap}@{bmapstart}")
    print(f"  nblocks={nblocks}  NINODES={NINODES}  IPB={IPB}  DINODE_SIZE={DINODE_SIZE}")
    print(f"  MAXFILE={MAXFILE} blocks = {MAXFILE * BSIZE // 1024 // 1024} MB/file")

    # ルートディレクトリ (inum=1=ROOTINO)
    rootino = ialloc(T_DIR)
    assert rootino == ROOTINO, f"rootino={rootino} expected {ROOTINO}"
    iappend(rootino, _dirent(rootino, "."))
    iappend(rootino, _dirent(rootino, ".."))

    # rootfs ツリー書き込み
    print(f"\npopulating from {root_dir} ...")
    populate(root_dir, rootino)

    # /dev とデバイスノード生成 (ホストツリー由来の /dev があれば流用)
    root_din = rinode(rootino)
    dev_inum = _find_dirent(rootino, root_din, "dev")
    if dev_inum is None:
        dev_inum = mkdirnode(rootino, "dev")
    for dev_name, dev_major, dev_minor in DEV_NODES:
        dev_din = rinode(dev_inum)
        if _find_dirent(dev_inum, dev_din, dev_name) is None:
            mkdevnode(dev_inum, dev_name, dev_major, dev_minor)
    print(f"  /dev: {len(DEV_NODES)} device nodes")

    # ルートディレクトリサイズを BSIZE に align (xv6 mkfs 互換)
    din      = rinode(rootino)
    din['size'] = ((din['size'] + BSIZE - 1) // BSIZE) * BSIZE
    winode(rootino, din)

    write_bitmap()

    out_img.write_bytes(image)

    used_mb = freeblock * BSIZE // 1024 // 1024
    print(f"\nimage written: {out_img}  ({FSSIZE * BSIZE // 1024 // 1024} MB total)")
    print(f"  inodes used : {freeinode - 1} / {NINODES}")
    print(f"  blocks used : {freeblock} / {FSSIZE}  ({used_mb} MB)")

    magic = struct.unpack("<I", bytes(image[BSIZE:BSIZE + 4]))[0]
    if magic == FSMAGIC:
        print(f"  superblock magic: OK  (0x{magic:08x})")
        return 0
    else:
        print(f"  ERROR: bad magic 0x{magic:08x}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
