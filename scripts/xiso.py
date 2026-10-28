#!/usr/bin/env python3
"""List or extract files from an Xbox 360 disc image (XDVDFS), stdlib only.

  xiso.py list    <image.iso>
  xiso.py extract <image.iso> <path-in-image> <output file>
"""
import struct
import sys

SECTOR = 0x800
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: plain XISO, XGD3, XGD1, XGD2.
PARTITION_OFFSETS = (0x0, 0x2080000, 0xFD90000, 0x18300000)


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        header = f.read(0x1C)
        if header[:20] == MAGIC:
            root_sector, root_size = struct.unpack_from("<II", header, 20)
            return base, root_sector, root_size
    raise SystemExit("No XDVDFS volume found in this image")


def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attrs, name_len = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:
            continue
        name = table[off + 14:off + 14 + name_len].decode("ascii", "replace")
        path = prefix + name
        if attrs & 0x10:
            if length:
                yield from walk(f, base, start, length, path + "/")
        else:
            yield path, start, length
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("list", "extract"):
        raise SystemExit(__doc__)
    with open(sys.argv[2], "rb") as f:
        base, root_sector, root_size = find_partition(f)
        files = {p: (s, n) for p, s, n in walk(f, base, root_sector, root_size)}
        if sys.argv[1] == "list":
            print(f"# partition at 0x{base:X}, {len(files)} files")
            for p in sorted(files):
                print(f"{files[p][1]:>12}  {p}")
            return
        want = sys.argv[3].lower()
        match = next((p for p in files if p.lower() == want), None)
        if match is None:
            raise SystemExit(f"{sys.argv[3]} not found in image")
        start, length = files[match]
        f.seek(base + start * SECTOR)
        with open(sys.argv[4], "wb") as out:
            remaining = length
            while remaining:
                chunk = f.read(min(remaining, 1 << 20))
                if not chunk:
                    raise SystemExit("Image is truncated")
                out.write(chunk)
                remaining -= len(chunk)
        print(f"Extracted {match} ({length} bytes) to {sys.argv[4]}")


if __name__ == "__main__":
    main()
