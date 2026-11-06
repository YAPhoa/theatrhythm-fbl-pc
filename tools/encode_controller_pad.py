#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Encode the Xbox pad drawing as a patch for the game's Controller atlas.

    python tools/encode_controller_pad.py ATLAS.png

ATLAS.png is the 512x512 Controller atlas (top row first) with the Xbox pad drawn in the two base
sprites' areas (Controller_DualShock_Base_01 and _02). Only those areas are kept: this writes
assets/controller_pad/xbox.bc7, their BC7 blocks (scripts/controller_pad.py REGIONS, rows counted
from the bottom as Unity stores them, each region row by row), and assets/controller_pad/xbox.png,
the same areas alone for review. scripts/controller_pad.py puts the blocks into the game's own
atlas at prepare time. Needs Pillow and etcpak (pip install pillow etcpak); the .bc7 file is
checked in, so the game image does not.
"""
import argparse
from pathlib import Path
import sys

import etcpak
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
from controller_pad import BLOCKS, BLOCK_BYTES, PATCH_BYTES, REGIONS  # noqa: E402

SIZE = BLOCKS * 4
OUT = Path(__file__).resolve().parent.parent / 'assets' / 'controller_pad'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('atlas', type=Path)
    a = p.parse_args()
    atlas = Image.open(a.atlas).convert('RGBA')
    if atlas.size != (SIZE, SIZE):
        raise SystemExit(f'expected a {SIZE}x{SIZE} atlas, got {atlas.size[0]}x{atlas.size[1]}')
    rgba = atlas.transpose(Image.FLIP_TOP_BOTTOM)  # Unity stores rows bottom-up
    params = etcpak.BC7CompressBlockParams()
    params.init_linear_weights()
    params.m_uber_level = 4
    params.m_max_partitions = 64
    params.m_try_least_squares = True
    linear = etcpak.compress_bc7(rgba.tobytes(), SIZE, SIZE, params)
    patch = bytearray()
    kept = Image.new('RGBA', (SIZE, SIZE))
    for x, y, w, h in REGIONS:
        for row in range(y, y + h):
            at = (row * BLOCKS + x) * BLOCK_BYTES
            patch += linear[at:at + w * BLOCK_BYTES]
        box = (x * 4, y * 4, (x + w) * 4, (y + h) * 4)
        kept.paste(rgba.crop(box), box[:2])
    assert len(patch) == PATCH_BYTES, len(patch)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'xbox.bc7').write_bytes(bytes(patch))
    kept.transpose(Image.FLIP_TOP_BOTTOM).save(OUT / 'xbox.png')
    print(f'{OUT / "xbox.bc7"}: {len(patch)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
