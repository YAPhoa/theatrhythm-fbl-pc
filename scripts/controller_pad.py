#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the game's Controller atlas bundle with an Xbox pad in place of the DualShock 4.

    python scripts/controller_pad.py GAME --out OUT

The pad the Live Info panel and the tutorial show is drawn from one sprite atlas, a 512x512 BC7
texture in Media/StreamingAssets/aa/PS4/<PAD_BUNDLE> (not encrypted). assets/controller_pad/xbox.bc7
(encoded by tools/encode_controller_pad.py) holds new blocks for the two base sprites' areas
(REGIONS); the red highlight sprites stay the game's. This writes OUT/controller_pad/xbox.bundle,
the game's bundle with those blocks in its atlas, blocks stored uncompressed, the catalog's CRC
kept the way scripts/button_icons.py keeps it, and OUT/controller_pad/{ps4,xbox}.texture, each
atlas's data as it lands in guest memory. The runtime swaps the atlas with the button icons
(src/runtime_button_icons.c) and moves the highlights onto the Xbox pad (src/runtime_xbox_pad.c).

No third-party modules or Python dependencies.
"""
import argparse
from pathlib import Path
import sys
import zlib

from button_icons import forge_crc, read_bundle, write_bundle

PORT = Path(__file__).resolve().parent.parent
ASSET = PORT / 'assets' / 'controller_pad' / 'xbox.bc7'
PAD_BUNDLE = 'Media/StreamingAssets/aa/PS4/d9bc0bbf58d9e64cfb82c0837f3ab818.bundle'
PAD_CRC = 523538824  # catalog.json m_Crc of PAD_BUNDLE (CUSA32853 v01.00 and v01.04)
BLOCKS = 128  # blocks per row and column of the 512x512 atlas
BLOCK_BYTES = 16
TEXTURE_BYTES = BLOCKS * BLOCKS * BLOCK_BYTES
# Block x, block y (rows from the bottom), width, height: Base_01's texture rect (0, 0, 504, 279)
# and Base_02's (0, 287, 232, 124), rounded out to whole blocks. Nothing else is drawn in them.
REGIONS = ((0, 0, 126, 70), (0, 71, 58, 32))
PATCH_BYTES = sum(w * h for _, _, w, h in REGIONS) * BLOCK_BYTES
# The last block of the tiled data is the sheet's top right corner, which no sprite uses. BC7 mode 6
# with every endpoint 0 decodes to transparent black whatever its index bits, so its last 4 bytes
# are free for the CRC.
CLEAR_BLOCK = bytes([0x40]) + bytes(15)


def _tiled_order():
    """Linear block index of each block of the tiled texture, in order (8x8-block tiles, Morton order inside)."""
    morton = []
    for t in range(64):
        x = y = 0
        for bit in range(3):
            x |= (t >> 2 * bit & 1) << bit
            y |= (t >> 2 * bit + 1 & 1) << bit
        morton.append((x, y))
    return [(ty * 8 + y) * BLOCKS + tx * 8 + x
            for ty in range(BLOCKS // 8) for tx in range(BLOCKS // 8) for x, y in morton]


def untile(tiled):
    linear = bytearray(len(tiled))
    for n, i in enumerate(_tiled_order()):
        linear[i * 16:i * 16 + 16] = tiled[n * 16:n * 16 + 16]
    return bytes(linear)


def tile(linear):
    return b''.join(linear[i * 16:i * 16 + 16] for i in _tiled_order())


def atlas_offset(b):
    """Offset of the atlas texture in the data stream (its .resS, at the end of the stream)."""
    textures = [n for n in b['nodes'] if n[3].endswith(b'.resS') and n[1] == TEXTURE_BYTES]
    if len(textures) != 1: raise ValueError('no single atlas texture stream in the Controller bundle')
    offset = textures[0][0]
    if offset + TEXTURE_BYTES != len(b['stream']): raise ValueError('the atlas texture is not at the end of the data stream')
    return offset


def build(original, patch):
    """The Controller bundle (bytes) with `patch` (REGIONS' blocks) in its atlas, CRC kept."""
    b = read_bundle(original)
    crc = zlib.crc32(b['stream'])
    if crc != PAD_CRC: raise ValueError(f'Controller bundle CRC {crc} is not {PAD_CRC}: another game version?')
    if len(patch) != PATCH_BYTES: raise ValueError(f'patch is {len(patch)} bytes, expected {PATCH_BYTES}')
    offset = atlas_offset(b)
    linear = bytearray(untile(b['stream'][offset:]))
    at = 0
    for x, y, w, h in REGIONS:
        for row in range(y, y + h):
            start = (row * BLOCKS + x) * BLOCK_BYTES
            linear[start:start + w * BLOCK_BYTES] = patch[at:at + w * BLOCK_BYTES]
            at += w * BLOCK_BYTES
    # forge_crc rewrites the stream's last 4 bytes: the index bits of CLEAR_BLOCK
    stream = b['stream'][:offset] + tile(linear)[:-16] + CLEAR_BLOCK
    return write_bundle(b, forge_crc(stream, crc))


def texture_of(bundle):
    b = read_bundle(bundle)
    return b['stream'][atlas_offset(b):]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    folder = a.out / 'controller_pad'
    folder.mkdir(parents=True, exist_ok=True)  # run.py: prepared, even with nothing to build
    source = a.game / PAD_BUNDLE
    if not source.is_file():
        print(f'Xbox pad: {source} not found, keeping the DualShock 4')
        return 0
    if not ASSET.is_file():
        print(f'Xbox pad: {ASSET} not found, keeping the DualShock 4')
        return 0
    original = source.read_bytes()
    try:
        bundle = build(original, ASSET.read_bytes())
    except ValueError as e:
        print(f'Xbox pad: {source}: {e}; keeping the DualShock 4')
        return 0
    (folder / 'ps4.texture').write_bytes(texture_of(original))
    (folder / 'xbox.texture').write_bytes(texture_of(bundle))
    (folder / 'xbox.bundle').write_bytes(bundle)
    print(f'Xbox pad: {folder / "xbox.bundle"} ({len(bundle)} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
