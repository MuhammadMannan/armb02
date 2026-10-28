#!/usr/bin/env python3
"""List or extract files from an Xbox 360 STFS package (LIVE/PIRS/CON), stdlib only.
Title updates and DLC ship in this format.

  stfs.py list    <package>
  stfs.py extract <package> <path-in-package> <output file>
"""
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = (0xAA, 0x70E4, 0x4AF768)


def u24le(b):
    return b[0] | b[1] << 8 | b[2] << 16


class Stfs:
    def __init__(self, f):
        self.f = f
        f.seek(0)
        head = f.read(0x3A0)
        if head[:4] not in (b"LIVE", b"PIRS", b"CON "):
            raise SystemExit("Not an STFS package")
        header_size = struct.unpack_from(">I", head, 0x340)[0]
        vd = head[0x379:0x379 + 0x24]
        if vd[0] != 0x24:
            raise SystemExit("Not an STFS volume (SVOD packages are not supported)")
        self.read_only = vd[2] & 1
        self.table_blocks = struct.unpack_from("<H", vd, 3)[0]
        self.table_start = u24le(vd[5:8])
        self.base = (header_size + 0xFFF) & ~0xFFF
        self.shift = 0 if self.base == 0xB000 or self.read_only else 1

    def data_block(self, index):
        """Raw block number of a data block, skipping the interleaved hash tables."""
        block = index
        for level in PER_LEVEL:
            block += ((index + level) // level) << self.shift
            if index < level:
                break
        return block

    def hash_block(self, index):
        """Raw block number of the level 0 hash table that covers a data block."""
        if index < PER_LEVEL[0]:
            return 0
        block = (index // PER_LEVEL[0]) * (PER_LEVEL[0] + (1 << self.shift))
        block += ((index // PER_LEVEL[1]) + 1) << self.shift
        if index < PER_LEVEL[1]:
            return block
        return block + (1 << self.shift)

    def read_block(self, index):
        self.f.seek(self.base + self.data_block(index) * BLOCK)
        return self.f.read(BLOCK)

    def next_block(self, index):
        self.f.seek(self.base + self.hash_block(index) * BLOCK + (index % PER_LEVEL[0]) * 0x18 + 0x15)
        b = self.f.read(3)
        return b[0] << 16 | b[1] << 8 | b[2]

    def blocks(self, start, count, contiguous):
        index = start
        for _ in range(count):
            yield index
            index = index + 1 if contiguous else self.next_block(index)

    def entries(self):
        raw = b"".join(self.read_block(b) for b in self.blocks(self.table_start, self.table_blocks, False))
        items = []
        for off in range(0, len(raw), 0x40):
            e = raw[off:off + 0x40]
            flags = e[0x28]
            if flags & 0x3F == 0:
                break
            items.append({
                "name": e[:flags & 0x3F].decode("ascii", "replace"),
                "dir": bool(flags & 0x80),
                "contiguous": bool(flags & 0x40),
                "blocks": u24le(e[0x29:0x2C]),
                "start": u24le(e[0x2F:0x32]),
                "parent": struct.unpack_from(">h", e, 0x32)[0],
                "size": struct.unpack_from(">I", e, 0x34)[0],
            })
        for item in items:
            parts, p = [item["name"]], item["parent"]
            while p != -1 and 0 <= p < len(items):
                parts.append(items[p]["name"])
                p = items[p]["parent"]
            item["path"] = "/".join(reversed(parts))
        return items

    def extract(self, item, out):
        remaining = item["size"]
        for index in self.blocks(item["start"], item["blocks"], item["contiguous"]):
            chunk = self.read_block(index)[:remaining]
            out.write(chunk)
            remaining -= len(chunk)
            if remaining <= 0:
                break


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("list", "extract"):
        raise SystemExit(__doc__)
    with open(sys.argv[2], "rb") as f:
        pkg = Stfs(f)
        items = pkg.entries()
        if sys.argv[1] == "list":
            for item in items:
                if not item["dir"]:
                    print(f"{item['size']:>12}  {item['path']}")
            return
        want = sys.argv[3].lower()
        item = next((i for i in items if i["path"].lower() == want and not i["dir"]), None)
        if item is None:
            raise SystemExit(f"{sys.argv[3]} not found in package")
        with open(sys.argv[4], "wb") as out:
            pkg.extract(item, out)
        print(f"Extracted {item['path']} ({item['size']} bytes) to {sys.argv[4]}")


if __name__ == "__main__":
    main()
