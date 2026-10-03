#!/usr/bin/env python3
"""Extract game files from a user-supplied Xbox 360 disc image (XDVDFS / XGD2).

Usage: extract_game.py <image.iso> [output_dir]

Copies the files the recompiled game needs out of the image, unmodified.
Nothing is decrypted here; the XEX is loaded as-is by the SDK.
The output directory is gitignored. Never commit its contents.
"""
import os
import struct
import sys

SECTOR = 0x800
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: raw XDVDFS, XGD2, XGD1, XGD3, and redump variants.
PARTITION_OFFSETS = (0x0, 0xFD90000, 0x2080000, 0x18300000, 0x89D80000)
SKIP_DIRS = ("/$SystemUpdate",)
EXPECTED_TITLE_ID = 0x545407F8


def find_partition(f, size):
    for base in PARTITION_OFFSETS:
        if base + 32 * SECTOR + 28 > size:
            continue
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            root_sector, root_size = struct.unpack("<II", f.read(8))
            return base, root_sector, root_size
    raise SystemExit("error: no XDVDFS volume found; is this an Xbox 360 disc image?")


def walk(f, base, sector, dir_size, path, out):
    f.seek(base + sector * SECTOR)
    data = f.read(dir_size)
    stack = [0]
    while stack:
        off = stack.pop() * 4
        if off + 14 > len(data):
            continue
        left, right, sec, size, attr, name_len = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF:
            continue
        name = data[off + 14:off + 14 + name_len].decode("latin-1")
        full = f"{path}/{name}"
        if attr & 0x10:
            if size:
                walk(f, base, sec, size, full, out)
        else:
            out.append((full, sec, size))
        if left:
            stack.append(left)
        if right:
            stack.append(right)


def xex_title_id(path):
    with open(path, "rb") as x:
        head = x.read(0x4000)
    if head[:4] != b"XEX2":
        return None
    count = struct.unpack_from(">I", head, 0x14)[0]
    for i in range(count):
        key, value = struct.unpack_from(">II", head, 0x18 + i * 8)
        if key == 0x00040006:
            return struct.unpack_from(">I", head, value + 12)[0]
    return None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    iso = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "game")

    size = os.path.getsize(iso)
    with open(iso, "rb") as f:
        base, root_sector, root_size = find_partition(f, size)
        files = []
        walk(f, base, root_sector, root_size, "", files)
        files = [e for e in sorted(files) if not e[0].startswith(SKIP_DIRS)]
        total = sum(e[2] for e in files)
        print(f"{len(files)} files, {total / 2**30:.2f} GiB -> {out_dir}")
        for path, sec, fsize in files:
            dest = os.path.join(out_dir, *path.strip("/").split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if os.path.exists(dest) and os.path.getsize(dest) == fsize:
                print(f"  skip  {path}")
                continue
            print(f"  write {path} ({fsize / 2**20:.1f} MiB)")
            f.seek(base + sec * SECTOR)
            remaining = fsize
            with open(dest, "wb") as o:
                while remaining:
                    chunk = f.read(min(remaining, 1 << 24))
                    if not chunk:
                        raise SystemExit(f"error: image truncated while reading {path}")
                    o.write(chunk)
                    remaining -= len(chunk)

    title = xex_title_id(os.path.join(out_dir, "default.xex"))
    if title is None:
        print("warning: default.xex missing or not XEX2")
    elif title != EXPECTED_TITLE_ID:
        print(f"warning: title id {title:08X}, expected {EXPECTED_TITLE_ID:08X}; "
              "this build is untested")
    else:
        print(f"ok: title id {title:08X}")


if __name__ == "__main__":
    main()
