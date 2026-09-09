#!/usr/bin/env python3
from pathlib import Path
import io
import math
import struct
import sys

USB_IMG = Path(sys.argv[1])
SECTOR_SIZE = 512
SECTORS_PER_CLUSTER = 8
CLUSTER_SIZE = SECTOR_SIZE * SECTORS_PER_CLUSTER
PART_START_LBA = 1
PART_SECTORS = 0x00080000 - 1
RESERVED_SECTORS = 32
FAT_COUNT = 2
FAT_SECTORS = 2048
DATA_START_LBA = PART_START_LBA + RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS
MEDIA_CLUSTERS = (PART_SECTORS - RESERVED_SECTORS - FAT_COUNT * FAT_SECTORS) // SECTORS_PER_CLUSTER
EOC = 0x0FFFFFFF


def write_le16(buf, off, value):
    struct.pack_into('<H', buf, off, value)


def write_le32(buf, off, value):
    struct.pack_into('<I', buf, off, value)


def short_entry(name11, attr, cluster, size):
    ent = bytearray(32)
    ent[0:11] = name11
    ent[11] = attr
    write_le16(ent, 20, (cluster >> 16) & 0xFFFF)
    write_le16(ent, 26, cluster & 0xFFFF)
    write_le32(ent, 28, size)
    return ent


def lfn_checksum(name11):
    s = 0
    for b in name11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def lfn_entry(seq, last, checksum, chunk):
    ent = bytearray([0xFF] * 32)
    ent[0] = seq | (0x40 if last else 0x00)
    ent[11] = 0x0F
    ent[12] = 0x00
    ent[13] = checksum
    ent[26] = 0x00
    ent[27] = 0x00
    positions = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    for i, ch in enumerate(chunk):
        write_le16(ent, positions[i], ch)
    for i in range(len(chunk), 13):
        write_le16(ent, positions[i], 0xFFFF)
    if len(chunk) < 13:
        write_le16(ent, positions[len(chunk)], 0x0000)
    return ent


def lfn_entries(long_name, short_name11):
    chars = [ord(c) for c in long_name]
    chunks = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    if len(chars) % 13 == 0:
        chunks.append([])
    checksum = lfn_checksum(short_name11)
    entries = []
    total = len(chunks)
    for idx in range(total - 1, -1, -1):
        entries.append(lfn_entry(idx + 1, idx == total - 1, checksum, chunks[idx]))
    return entries


def dir_image(entries):
    buf = bytearray(CLUSTER_SIZE)
    off = 0
    for ent in entries:
        buf[off:off + 32] = ent
        off += 32
    return buf


def cluster_to_lba(cluster):
    return DATA_START_LBA + (cluster - 2) * SECTORS_PER_CLUSTER


def write_at_lba(f, lba, data):
    f.seek(lba * SECTOR_SIZE)
    f.write(data)


def write_clusters(f, start_cluster, data):
    for idx in range(0, len(data), CLUSTER_SIZE):
        chunk = bytearray(CLUSTER_SIZE)
        part = data[idx:idx + CLUSTER_SIZE]
        chunk[:len(part)] = part
        cluster = start_cluster + idx // CLUSTER_SIZE
        write_at_lba(f, cluster_to_lba(cluster), chunk)


next_cluster = 3
fat_entries = {0: 0x0FFFFFF8, 1: EOC, 2: EOC}
allocations = []


def alloc_clusters(data_bytes):
    global next_cluster
    count = max(1, math.ceil(len(data_bytes) / CLUSTER_SIZE))
    first = next_cluster
    for i in range(count):
        cluster = first + i
        fat_entries[cluster] = EOC if i == count - 1 else (cluster + 1)
    next_cluster += count
    allocations.append((first, data_bytes))
    return first


def main():
    data1 = b'OrthOS USB FAT root file\r\n'
    data2 = b'OrthOS multi-cluster FAT test file.\r\n' * 20
    data3 = b'OrthOS nested file inside DIR1\r\n'
    data4 = b'OrthOS deep nested file in DIR1/DIR2\r\n'
    data5 = b'OrthOS long filename test over USB FAT.\r\n'
    data6 = b'OrthOS nested long filename inside DIR1.\r\n'
    data7 = b'OrthOS deep long filename inside DIR1/DIR2.\r\n'
    data8 = b'OrthOS file inside long directory name.\r\n'

    c_readme = alloc_clusters(data1)
    c_notes = alloc_clusters(data2)
    c_dir1 = alloc_clusters(bytes(CLUSTER_SIZE))
    c_inner = alloc_clusters(data3)
    c_dir2 = alloc_clusters(bytes(CLUSTER_SIZE))
    c_deep = alloc_clusters(data4)
    c_longfile = alloc_clusters(data5)
    c_nested = alloc_clusters(data6)
    c_deep_long = alloc_clusters(data7)
    c_longdir = alloc_clusters(bytes(CLUSTER_SIZE))
    c_longdir_file = alloc_clusters(data8)

    root_entries = [
        short_entry(b'README  TXT', 0x20, c_readme, len(data1)),
        short_entry(b'NOTES   TXT', 0x20, c_notes, len(data2)),
        short_entry(b'DIR1       ', 0x10, c_dir1, 0),
    ]
    root_entries.extend(lfn_entries('VeryLongFileName.txt', b'VERYLN~1TXT'))
    root_entries.append(short_entry(b'VERYLN~1TXT', 0x20, c_longfile, len(data5)))
    root_entries.extend(lfn_entries('VeryLongDirectory', b'VERYLD~1   '))
    root_entries.append(short_entry(b'VERYLD~1   ', 0x10, c_longdir, 0))

    dir1_entries = [
        short_entry(b'.          ', 0x10, c_dir1, 0),
        short_entry(b'..         ', 0x10, 2, 0),
        short_entry(b'INNER   TXT', 0x20, c_inner, len(data3)),
        short_entry(b'DIR2       ', 0x10, c_dir2, 0),
    ]
    root_entries.extend(lfn_entries('long_filename_inside_dir1.txt', b'LONG_F~1TXT'))
    dir1_entries.append(short_entry(b'LONG_F~1TXT', 0x20, c_nested, len(data6)))

    dir2_entries = [
        short_entry(b'.          ', 0x10, c_dir2, 0),
        short_entry(b'..         ', 0x10, c_dir1, 0),
        short_entry(b'DEEP    TXT', 0x20, c_deep, len(data4)),
    ]
    root_entries.extend(lfn_entries('deep_long_filename_in_dir1_dir2.txt', b'DEEP_L~1TXT'))
    dir2_entries.append(short_entry(b'DEEP_L~1TXT', 0x20, c_deep_long, len(data7)))

    longdir_entries = [
        short_entry(b'.          ', 0x10, c_longdir, 0),
        short_entry(b'..         ', 0x10, 2, 0),
        short_entry(b'FILE    TXT', 0x20, c_longdir_file, len(data8)),
    ]

    root_dir = dir_image(root_entries)
    dir1 = dir_image(dir1_entries)
    dir2 = dir_image(dir2_entries)
    longdir = dir_image(longdir_entries)

    mbr = bytearray(SECTOR_SIZE)
    mbr[446:446 + 16] = struct.pack('<BBBBBBBBII', 0x80, 0, 1, 0, 0x0C, 0, 1, 0, PART_START_LBA, PART_SECTORS)
    mbr[510] = 0x55
    mbr[511] = 0xAA

    bs = bytearray(SECTOR_SIZE)
    bs[0:3] = b'\xeb\x58\x90'
    bs[3:11] = b'ORTHOS  '
    write_le16(bs, 11, SECTOR_SIZE)
    bs[13] = SECTORS_PER_CLUSTER
    write_le16(bs, 14, RESERVED_SECTORS)
    bs[16] = FAT_COUNT
    write_le16(bs, 17, 0)
    write_le16(bs, 19, 0)
    bs[21] = 0xF8
    write_le16(bs, 22, 0)
    write_le16(bs, 24, 0)
    write_le16(bs, 26, 0)
    write_le32(bs, 28, PART_SECTORS)
    write_le32(bs, 32, 0)
    write_le32(bs, 36, FAT_SECTORS)
    write_le16(bs, 40, 0)
    write_le16(bs, 42, 0)
    write_le32(bs, 44, 2)
    write_le16(bs, 48, 1)
    write_le16(bs, 50, 0)
    bs[66] = 0x29
    write_le32(bs, 67, 0x12345678)
    bs[71:82] = b'ORTHOS USB '
    bs[82:90] = b'FAT32   '
    bs[510] = 0x55
    bs[511] = 0xAA

    fat_region = bytearray(FAT_SECTORS * SECTOR_SIZE)
    for cluster, value in fat_entries.items():
        write_le32(fat_region, cluster * 4, value)

    with USB_IMG.open('r+b') as f:
        write_at_lba(f, 0, mbr)
        write_at_lba(f, 1, bs)
        write_at_lba(f, PART_START_LBA + RESERVED_SECTORS, fat_region)
        write_at_lba(f, PART_START_LBA + RESERVED_SECTORS + FAT_SECTORS, fat_region)
        write_clusters(f, 2, root_dir)
        write_clusters(f, c_dir1, dir1)
        write_clusters(f, c_dir2, dir2)
        write_clusters(f, c_longdir, longdir)
        for first, data in allocations:
            if first in (c_dir1, c_dir2, c_longdir):
                continue
            write_clusters(f, first, data)


if __name__ == '__main__':
    main()
