#!/usr/bin/env python3
"""Make the PS5 title's tile and background from the user's own copy of the game.

The repository ships no artwork. The game disc carries its Xbox dashboard art
in `nxeart`, an STFS package (magic PIRS) with a 1920x1080 background and a
420x320 banner as JPEG files. This reads that file from the locally extracted
game folder and writes, into an output folder that is not in the repository:

    background.png   1920x1080, the dashboard background
    icon.png         512x512, the corner of the background that carries the logo

ps5/title_build.sh then converts them to the console's formats (through the
driver project's tools/prepare-assets.sh) when ART_DIR points at that folder.

    make_title_art.py <game folder> <output folder>

Needs Pillow.
"""
import io
import os
import struct
import sys

from PIL import Image

BLOCK = 0x1000
BLOCKS_PER_HASH = 0xAA  # 170 data blocks, then a hash block
BLOCKS_PER_LEVEL1 = 0x70E4


def data_area(package: bytes) -> bytes:
    """The package's data blocks in order, with the interleaved hash blocks removed."""
    header_size = struct.unpack('>I', package[0x340:0x344])[0]
    first_table = (header_size + 0xFFF) & 0xFFFFF000
    # Read-only packages (PIRS, LIVE) keep one copy of each hash table.
    shift = 0 if (first_table >> 12) == 0xB else 1

    def offset(block: int) -> int:
        adjust = 0
        if block >= BLOCKS_PER_HASH:
            adjust += ((block // BLOCKS_PER_HASH) + 1) << shift
        if block > BLOCKS_PER_LEVEL1:
            adjust += ((block // BLOCKS_PER_LEVEL1) + 1) << shift
        return 0xC000 + ((block + adjust) << 12) if shift == 0 else 0xD000 + ((block + adjust) << 12)

    out = bytearray()
    block = 0
    while True:
        start = offset(block)
        if start >= len(package):
            break
        out += package[start:start + BLOCK]
        block += 1
    return bytes(out)


def jpegs(data: bytes):
    """Every JPEG in the data that Pillow can open, largest first."""
    found = []
    position = 0
    while True:
        start = data.find(b'\xff\xd8\xff', position)
        if start < 0:
            break
        position = start + 3
        try:
            image = Image.open(io.BytesIO(data[start:]))
            image.load()
        except Exception:
            continue
        found.append(image.convert('RGB'))
    found.sort(key=lambda image: -(image.width * image.height))
    return found


def main() -> int:
    game, out = sys.argv[1], sys.argv[2]
    with open(os.path.join(game, 'nxeart'), 'rb') as handle:
        package = handle.read()
    if package[:4] not in (b'PIRS', b'LIVE', b'CON '):
        print('nxeart is not an STFS package')
        return 1
    images = jpegs(data_area(package))
    background = next((i for i in images if i.size == (1920, 1080)), None)
    banner = next((i for i in images if i.size == (420, 320)), None)
    if background is None:
        print('no 1920x1080 background found in nxeart; sizes present:', sorted({i.size for i in images}))
        return 1
    os.makedirs(out, exist_ok=True)
    background.save(os.path.join(out, 'background.png'))

    # The tile: the square of the background that holds the game's logo (top
    # right, over the skyline). The banner in the package is a car with no
    # logo on it, which says little on a home screen. The fractions are of the
    # 1920x1080 image.
    left, top, side = round(background.width * 0.565), round(background.height * 0.052), round(background.height * 0.50)
    icon = background.crop((left, top, left + side, top + side)).resize((512, 512), Image.LANCZOS)
    icon.save(os.path.join(out, 'icon.png'))
    print('wrote background.png %dx%d and icon.png 512x512 (the logo corner of the background) to %s' % (
        background.width, background.height, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
