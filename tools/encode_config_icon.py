#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Encode an Options button's icon patch (Display Settings, Cheats) as Config atlas blocks.

    python tools/encode_config_icon.py assets/config_icon/display.png
    python tools/encode_config_icon.py assets/config_icon/cheats.png

Writes the .bc7 file next to the PNG: the 64x60 patch (as the whole blocks around Logo Usage's icon,
atlas columns 208-271, rows 92-151: the icon itself inside the 55x51 box at (5,4)) as BC7 blocks
the way the PS4 texture stores them, rows bottom-up, block rows one after another
(scripts/config_icon.py PATCHES; it tiles them into the atlas). Needs Pillow and etcpak
(pip install pillow etcpak); the .bc7 files are checked in, so the game image does not.
"""
import argparse
from pathlib import Path
import sys

import etcpak
from PIL import Image

WIDTH, HEIGHT = 64, 60


def encode(patch):
    if patch.size != (WIDTH, HEIGHT):
        raise SystemExit(f'expected a {WIDTH}x{HEIGHT} patch, got {patch.size[0]}x{patch.size[1]}')
    rgba = patch.convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)  # Unity stores rows bottom-up
    params = etcpak.BC7CompressBlockParams()  # as tools/encode_button_icons.py: alpha kept exact
    params.init_linear_weights()
    params.m_uber_level = 4
    params.m_max_partitions = 64
    params.m_try_least_squares = True
    return etcpak.compress_bc7(rgba.tobytes(), WIDTH, HEIGHT, params)


def decode(blocks):
    """The patch as the game will show it (for checking an encode)."""
    import texture2ddecoder
    bgra = texture2ddecoder.decode_bc7(blocks, WIDTH, HEIGHT)
    return Image.frombytes('RGBA', (WIDTH, HEIGHT), bgra, 'raw', 'BGRA').transpose(Image.FLIP_TOP_BOTTOM)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('patch', type=Path)
    p.add_argument('--preview', action='store_true', help='also write <name>.decoded.png')
    a = p.parse_args()
    data = encode(Image.open(a.patch))
    assert len(data) == WIDTH * HEIGHT, len(data)
    a.patch.with_suffix('.bc7').write_bytes(data)
    if a.preview:
        decode(data).save(a.patch.with_suffix('.decoded.png'))
    print(f'{a.patch.with_suffix(".bc7")}: {len(data)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
