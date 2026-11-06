from paths import ROOT
import os
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from button_icons import read_bundle, write_bundle
from controller_pad import (ASSET, BLOCKS, BLOCK_BYTES, CLEAR_BLOCK, PAD_BUNDLE, PAD_CRC, PATCH_BYTES, REGIONS,
                            TEXTURE_BYTES, build, tile, untile)

GAME = Path(os.environ['THEATRHYTHM_GAME_DIR']) if os.environ.get('THEATRHYTHM_GAME_DIR') else None


class LayoutTests(unittest.TestCase):
    def test_tiling_round_trips(self):
        linear = bytes(i * 7 & 0xff for i in range(TEXTURE_BYTES))
        self.assertEqual(untile(tile(linear)), linear)
        self.assertNotEqual(tile(linear), linear)

    def test_patch_is_the_regions_blocks(self):
        self.assertEqual(len(ASSET.read_bytes()), PATCH_BYTES)
        for x, y, w, h in REGIONS:
            self.assertLessEqual(x + w, BLOCKS)
            self.assertLessEqual(y + h, BLOCKS)
        # The two base sprites' areas do not overlap, and the CRC block (top right) is outside both.
        (x0, y0, w0, h0), (x1, y1, w1, h1) = REGIONS
        self.assertTrue(y0 + h0 <= y1 or y1 + h1 <= y0)
        for x, y, w, h in REGIONS:
            self.assertFalse(x <= BLOCKS - 1 < x + w and y <= BLOCKS - 1 < y + h)


@unittest.skipUnless(GAME and (GAME / PAD_BUNDLE).is_file(), 'needs THEATRHYTHM_GAME_DIR (the game dump)')
class GameTests(unittest.TestCase):
    def test_only_the_base_areas_change_and_the_crc_stays(self):
        original = (GAME / PAD_BUNDLE).read_bytes()
        patch = ASSET.read_bytes()
        out, before = read_bundle(build(original, patch)), read_bundle(original)
        self.assertEqual(zlib.crc32(out['stream']), PAD_CRC)
        self.assertEqual(out['nodes'], before['nodes'])
        self.assertEqual(out['stream'][:-TEXTURE_BYTES], before['stream'][:-TEXTURE_BYTES])
        new, old = untile(out['stream'][-TEXTURE_BYTES:]), untile(before['stream'][-TEXTURE_BYTES:])
        inside = set()
        at = 0
        for x, y, w, h in REGIONS:
            for row in range(y, y + h):
                start = (row * BLOCKS + x) * BLOCK_BYTES
                self.assertEqual(new[start:start + w * BLOCK_BYTES], patch[at:at + w * BLOCK_BYTES])
                at += w * BLOCK_BYTES
                inside.update(range(row * BLOCKS + x, row * BLOCKS + x + w))
        top_right = BLOCKS * BLOCKS - 1
        for block in range(BLOCKS * BLOCKS):
            if block in inside: continue
            got, had = new[block * 16:block * 16 + 16], old[block * 16:block * 16 + 16]
            if block == top_right: self.assertEqual(got[:12], CLEAR_BLOCK[:12])
            else: self.assertEqual(got, had, f'block {block} changed')

    def test_prepare_writes_both_textures_as_they_land_in_memory(self):
        with tempfile.TemporaryDirectory() as out:
            subprocess.run([sys.executable, str(ROOT / 'scripts' / 'controller_pad.py'), str(GAME), '--out', out],
                           check=True, stdout=subprocess.DEVNULL)
            folder = Path(out) / 'controller_pad'
            ps4, xbox = (folder / 'ps4.texture').read_bytes(), (folder / 'xbox.texture').read_bytes()
            self.assertEqual(ps4, read_bundle((GAME / PAD_BUNDLE).read_bytes())['stream'][-TEXTURE_BYTES:])
            self.assertEqual(xbox, read_bundle((folder / 'xbox.bundle').read_bytes())['stream'][-TEXTURE_BYTES:])
            self.assertNotEqual(ps4, xbox)

    def test_other_game_versions_are_refused(self):
        b = read_bundle((GAME / PAD_BUNDLE).read_bytes())
        changed = bytearray(b['stream']); changed[0] ^= 1
        with self.assertRaisesRegex(ValueError, 'another game version'):
            build(write_bundle(b, bytes(changed)), ASSET.read_bytes())


if __name__ == '__main__':
    unittest.main()
