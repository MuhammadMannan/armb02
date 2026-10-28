"""Write runtime/install/manifest.inc: the file list the installer checks a
user's disc against (relative path, size, SHA-256 of every file).
Adapted from SpeedBreaker's scripts/gen_install_manifest.py. GPL-3.0-or-later (see COPYING).

    python3 -I scripts/gen_install_manifest.py path/to/BO2.iso --config config/bo2_sp.toml \
        --out runtime/install/manifest.inc

The disc's $SystemUpdate/ (console firmware, not game content) is skipped.

Run it over a known-good disc image (.iso, read with scripts/xiso.py) or an
extracted dump (extract-xiso output). It reads the disc
and writes only names, sizes and hashes, never game content, so the output is
committed. default.xex must match the hash this build was recompiled from
(the config's comment); --expect-xex overrides that for test manifests.

The manifest version is derived from the entries, so installs made with an
older manifest stop counting as valid (FindGameInstall) whenever it changes.
"""
import argparse
import hashlib
import os
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import xiso  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
FORMAT = 1                        # bump if the meaning of the entries changes
SKIP_DIRS = {'$SystemUpdate'}    # console firmware on the disc, not game content
CHUNK = 8 << 20


def expected_xex_hash(config):
    text = pathlib.Path(config).read_text()
    m = re.search(r'default\.xex SHA-256 ([0-9a-f]{64})', text)
    if not m:
        sys.exit(f'{config} has no "default.xex SHA-256 <hash>" comment; pass --expect-xex')
    return m.group(1)


def xex_title_id(data):
    # XEX2 header: optional header count at 0x14, then (key, value) pairs.
    # Key 0x00040006 is the execution info, whose title ID is at +12.
    data = data[:0x10000]
    if data[:4] != b'XEX2':
        sys.exit('default.xex is not an XEX2 executable')
    count = struct.unpack_from('>I', data, 0x14)[0]
    for i in range(min(count, 64)):
        key, value = struct.unpack_from('>II', data, 0x18 + 8 * i)
        if key == 0x00040006 and value + 16 <= len(data):
            return struct.unpack_from('>I', data, value + 12)[0]
    sys.exit('default.xex has no execution info header')


def chunks_of(f, offset, size):
    f.seek(offset)
    while size:
        chunk = f.read(min(size, CHUNK))
        if not chunk:
            sys.exit('the disc image is truncated')
        size -= len(chunk)
        yield chunk


def sha256_of(rel, f, offset, size, done, total):
    h = hashlib.sha256()
    for chunk in chunks_of(f, offset, size):
        h.update(chunk)
        done[0] += len(chunk)
        if sys.stderr.isatty():
            print(f'\r{done[0] * 100 // max(total, 1):3d}%  {rel[-48:]:<48}', end='', file=sys.stderr, flush=True)
    return h.hexdigest()


def disc_files(game):
    """(relative path, open file, offset, size) for every disc file, from an image or a folder."""
    if game.is_file():
        f = open(game, 'rb')
        base, root_sector, root_size = xiso.find_partition(f)
        for rel, sector, size in xiso.walk(f, base, root_sector, root_size):
            if rel.split('/')[0] not in SKIP_DIRS:
                yield rel, f, base + sector * xiso.SECTOR, size
        return
    for dirpath, dirnames, filenames in os.walk(game):
        # Hidden entries are host clutter (.DS_Store, AppleDouble ._ files on
        # exFAT drives, the install marker), never disc files; the installer
        # skips them the same way.
        dirnames[:] = sorted(d for d in dirnames if not d.startswith('.') and not (dirpath == str(game) and d in SKIP_DIRS))
        for name in filenames:
            if name.startswith('.'):
                continue
            path = pathlib.Path(dirpath) / name
            yield path.relative_to(game).as_posix(), open(path, 'rb'), 0, path.stat().st_size


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('game_dir', type=pathlib.Path, help='disc image (.iso) or extracted disc folder')
    ap.add_argument('--out', type=pathlib.Path, required=True)
    ap.add_argument('--config', required=True, help='XenonRecomp config with a "default.xex SHA-256 <hash>" comment')
    ap.add_argument('--expect-xex', help='required default.xex SHA-256 (default: from --config)')
    args = ap.parse_args()

    game = args.game_dir.resolve()
    files = []
    for rel, f, offset, size in disc_files(game):
        if not re.fullmatch(r'[\x21-\x7e ]+', rel) or '"' in rel or '\\' in rel:
            sys.exit(f'unexpected characters in {rel!r}')
        files.append((rel, f, offset, size))
    files.sort(key=lambda e: e[0])
    by_lower = {}
    for rel, *_ in files:
        if rel.lower() in by_lower:
            sys.exit(f'{rel} and {by_lower[rel.lower()]} differ only in case')
        by_lower[rel.lower()] = rel
    if 'default.xex' not in [rel for rel, *_ in files]:
        sys.exit(f'{game} has no default.xex at its root')

    total = sum(size for *_, size in files)
    done = [0]
    entries = [(rel, size, sha256_of(rel, f, offset, size, done, total)) for rel, f, offset, size in files]
    if sys.stderr.isatty():
        print(file=sys.stderr)

    xex_hash = next(h for rel, _, h in entries if rel == 'default.xex')
    expected = args.expect_xex or expected_xex_hash(args.config)
    if xex_hash != expected.lower():
        sys.exit(f'default.xex is {xex_hash}, expected {expected}: not the dump this build was recompiled from')

    canonical = f'armb02-install-manifest {FORMAT}\n' + ''.join(f'{rel}\t{size}\t{h}\n' for rel, size, h in entries)
    version = hashlib.sha256(canonical.encode()).hexdigest()[:16]
    _, f, offset, size = next(e for e in files if e[0] == 'default.xex')
    title_id = xex_title_id(b''.join(chunks_of(f, offset, min(size, 0x10000))))

    lines = [
        '// Generated by scripts/gen_install_manifest.py from a known-good disc. Do not edit;',
        '// rerun the script. File names, sizes and SHA-256 hashes only: no game content.',
        '// Adapted from SpeedBreaker. GPL-3.0-or-later (see COPYING).',
        '//',
        '// MANIFEST_INFO(version, default.xex title ID, file count, total bytes)',
        '// MANIFEST_FILE(path relative to the disc root, size in bytes, SHA-256)',
        '',
        f'MANIFEST_INFO("{version}", 0x{title_id:08X}, {len(entries)}, {total}ull)',
    ]
    lines += [f'MANIFEST_FILE("{rel}", {size}ull, "{h}")' for rel, size, h in entries]
    args.out.write_text('\n'.join(lines) + '\n')
    shown = args.out.resolve()
    shown = shown.relative_to(ROOT) if shown.is_relative_to(ROOT) else shown
    print(f'{shown}: {len(entries)} files, {total:,} bytes, manifest {version}, title {title_id:08X}')


if __name__ == '__main__':
    main()
