#!/usr/bin/env python3
"""xv6fs イメージからファイルを取り出す (build_rootfs_xv6fs.py の逆操作)。

Orthox 上で作った成果物 (ネイティブビルドしたカーネルなど) を
ホスト側で検証するために要る。

  python3 scripts/extract_rootfs_xv6fs.py out/rootfs-riscv64-xv6.img /kernel-riscv64.elf out/native-kernel.elf
  python3 scripts/extract_rootfs_xv6fs.py out/rootfs-riscv64-xv6.img --list /

レイアウト定数は build_rootfs_xv6fs.py と同じ。ずれると読めなくなるので、
あちらを変えたらこちらも合わせること。
"""
import struct
import sys

BSIZE = 1024
NINODES = 8192
LOGBLOCKS = 126
FSSIZE = 327680

NDIRECT = 9
NINDIRECT = BSIZE // 4

FSMAGIC = 0x10203040
ROOTINO = 1
T_DIR = 1
T_FILE = 2

DIRSIZ = 62
DINODE_ADDRS = NDIRECT + 3
DINODE_SIZE = 2 + 2 + 2 + 2 + 4 + 4 * DINODE_ADDRS
DINODE_FMT = "<hhhhI%dI" % DINODE_ADDRS
IPB = BSIZE // DINODE_SIZE
DIRENT_FMT = "<H%ds" % DIRSIZ

nlog = LOGBLOCKS + 1
ninodeblocks = NINODES // IPB + 1
nbitmap = FSSIZE // (BSIZE * 8) + 1
inodestart = 2 + nlog


class Image(object):
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        magic = struct.unpack("<I", self.sect(1)[0:4])[0]
        if magic != FSMAGIC:
            raise SystemExit("superblock magic 不一致: 0x%08x" % magic)

    def sect(self, n):
        return self.data[n * BSIZE:(n + 1) * BSIZE]

    def inode(self, inum):
        bn = inum // IPB + inodestart
        off = (inum % IPB) * DINODE_SIZE
        f = struct.unpack(DINODE_FMT, self.sect(bn)[off:off + DINODE_SIZE])
        return {"type": f[0], "size": f[4], "addrs": list(f[5:])}

    def _bmap(self, ino, bn):
        """ファイル内ブロック番号 bn → ディスク上のブロック番号"""
        a = ino["addrs"]
        if bn < NDIRECT:
            return a[bn]
        bn -= NDIRECT
        if bn < NINDIRECT:
            tbl = self.sect(a[NDIRECT])
            return struct.unpack("<I", tbl[bn * 4:bn * 4 + 4])[0]
        bn -= NINDIRECT
        if bn < NINDIRECT * NINDIRECT:              # 2 段間接
            tbl = self.sect(a[NDIRECT + 1])
            l1 = struct.unpack("<I", tbl[(bn // NINDIRECT) * 4:(bn // NINDIRECT) * 4 + 4])[0]
            tbl2 = self.sect(l1)
            k = bn % NINDIRECT
            return struct.unpack("<I", tbl2[k * 4:k * 4 + 4])[0]
        bn -= NINDIRECT * NINDIRECT                 # 3 段間接
        tbl = self.sect(a[NDIRECT + 2])
        i0 = bn // (NINDIRECT * NINDIRECT)
        l1 = struct.unpack("<I", tbl[i0 * 4:i0 * 4 + 4])[0]
        tbl2 = self.sect(l1)
        i1 = (bn // NINDIRECT) % NINDIRECT
        l2 = struct.unpack("<I", tbl2[i1 * 4:i1 * 4 + 4])[0]
        tbl3 = self.sect(l2)
        i2 = bn % NINDIRECT
        return struct.unpack("<I", tbl3[i2 * 4:i2 * 4 + 4])[0]

    def read_file(self, ino):
        out = bytearray()
        left = ino["size"]
        bn = 0
        while left > 0:
            blk = self._bmap(ino, bn)
            take = min(BSIZE, left)
            out += self.sect(blk)[:take]
            left -= take
            bn += 1
        return bytes(out)

    def readdir(self, ino):
        raw = self.read_file(ino)
        ents = []
        for off in range(0, len(raw), 64):
            inum, name = struct.unpack(DIRENT_FMT, raw[off:off + 64])
            if inum == 0:
                continue
            ents.append((inum, name.split(b"\0")[0].decode("utf-8", "replace")))
        return ents

    def lookup(self, path):
        cur = ROOTINO
        for part in [p for p in path.split("/") if p]:
            ino = self.inode(cur)
            if ino["type"] != T_DIR:
                raise SystemExit("%s: ディレクトリではない" % path)
            hit = [i for i, n in self.readdir(ino) if n == part]
            if not hit:
                raise SystemExit("%s: 見つからない (%s)" % (path, part))
            cur = hit[0]
        return cur


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    img = Image(argv[1])
    if argv[2] == "--list":
        target = argv[3] if len(argv) > 3 else "/"
        ino = img.inode(img.lookup(target))
        for inum, name in img.readdir(ino):
            child = img.inode(inum)
            kind = "d" if child["type"] == T_DIR else "-"
            print("%s %10d  %s" % (kind, child["size"], name))
        return 0

    inum = img.lookup(argv[2])
    ino = img.inode(inum)
    if ino["type"] != T_FILE:
        raise SystemExit("%s: 通常ファイルではない" % argv[2])
    data = img.read_file(ino)
    out = argv[3] if len(argv) > 3 else None
    if out:
        with open(out, "wb") as f:
            f.write(data)
        print("取り出した: %s → %s (%d バイト)" % (argv[2], out, len(data)))
    else:
        sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
