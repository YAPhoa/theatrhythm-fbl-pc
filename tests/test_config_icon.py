from paths import ROOT
import base64
import importlib.util
import os
import random
import re
import unittest
import zlib
from pathlib import Path
from button_icons import read_bundle
from config_icon import (ASSETS, BLOCKS, CLEAR_BLOCK, CONFIG_BUNDLE, CONFIG_CRC, ICON_FROM, PATCHES, PATCH_BYTES,
                         PATCH_SIZE, TEXTURE_BYTES, atlas_node, build, cbc_decrypt, cbc_encrypt, decrypt, encrypt,
                         original_texture, password, patches, secret_of, tile, untile)

GAME = Path(os.environ['THEATRHYTHM_GAME_DIR']) if os.environ.get('THEATRHYTHM_GAME_DIR') else None


def encoded_password(secret, rng):
    """The inverse of password(): a literal that DecryptKey turns into ab<secret>yz."""
    chars = [ord(c) for c in 'ab' + secret + 'yz']
    words, previous = [], 0x5963
    for k in range(len(chars)):
        value = chars[len(chars) - 1 - k]
        first = rng.getrandbits(16)
        words += [first, previous ^ value ^ first]
        previous = value >> 8 & 0xf
    return base64.b64encode(b''.join(w.to_bytes(2, 'big') for w in words)).decode()


class AesTests(unittest.TestCase):
    def test_fips_197_vector(self):
        key = bytes(range(16))
        plain = bytes.fromhex('00112233445566778899aabbccddeeff')
        cipher = bytes.fromhex('69c4e0d86a7b0430d8cdb78070b4c55a')
        self.assertEqual(cbc_encrypt(key, bytes(16), plain), cipher)
        self.assertEqual(cbc_decrypt(key, bytes(16), cipher), plain)

    def test_cbc_chains_blocks(self):
        rng = random.Random(1)
        key, iv = rng.randbytes(16), rng.randbytes(16)
        plain = bytes(64)  # equal blocks must not encrypt alike
        cipher = cbc_encrypt(key, iv, plain)
        self.assertEqual(len({cipher[i:i + 16] for i in range(0, 64, 16)}), 4)
        self.assertEqual(cbc_decrypt(key, iv, cipher), plain)


class CryptTests(unittest.TestCase):
    def test_password_undoes_the_literal_encoding(self):
        rng = random.Random(2)
        for secret in ('plain-ascii', 'ヴ and ヲ', ''):
            self.assertEqual(password(encoded_password(secret, rng)), secret)

    def test_bundle_round_trip_keeps_the_header(self):
        rng = random.Random(3)
        header = rng.randbytes(32)
        for size in (0, 7, 16, 100):
            plain = b'UnityFS\0' + rng.randbytes(size)
            data = encrypt(plain, header, 'secret')
            self.assertEqual(data[:32], header)
            self.assertEqual(len(data) % 16, 0)
            self.assertEqual(decrypt(data, 'secret'), plain)

    def test_wrong_password_is_refused(self):
        data = encrypt(b'UnityFS\0' + bytes(40), bytes(32), 'secret')
        with self.assertRaisesRegex(ValueError, 'another game version'):
            decrypt(data, 'other')


class TextureTests(unittest.TestCase):
    def test_tile_round_trip(self):
        linear = bytes(random.Random(4).getrandbits(8) for _ in range(TEXTURE_BYTES))
        tiled = tile(linear)
        self.assertEqual(untile(tiled), linear)
        # the first tiled blocks are the first tile's Morton order: (0,0) (1,0) (0,1) (1,1)
        self.assertEqual(tiled[16:32], linear[16:32])
        self.assertEqual(tiled[32:48], linear[BLOCKS * 16:BLOCKS * 16 + 16])

    def test_checked_in_patches_are_whole_blocks(self):
        w, h = PATCH_SIZE
        for name, (x, y) in PATCHES:
            self.assertEqual((ASSETS / f'{name}.bc7').stat().st_size, PATCH_BYTES, name)
            self.assertLessEqual(x + w, BLOCKS)
            self.assertLessEqual(y + h, BLOCKS)
        self.assertEqual(len(set(patches())), len(PATCHES))  # each its own picture

    def test_patches_keep_clear_of_each_other(self):
        # a button's sprite is 92x92 around its 64x60 patch: 16 px (4 blocks) more above and below
        w, h = PATCH_SIZE
        places = [place for _, place in PATCHES]
        for i, (ax, ay) in enumerate(places):
            for bx, by in places[i + 1:]:
                self.assertTrue(abs(ax - bx) >= w + 8 or abs(ay - by) >= h + 8, (ax, ay, bx, by))

    def test_runtime_moves_the_sprites_to_the_patches(self):
        # a button shows Logo Usage's sprite moved by its patch's place - ICON_FROM blocks; the
        # buttons are in the order of PATCHES (Display Settings, Cheats)
        source = (ROOT / 'src' / 'runtime_display_page.c').read_text(encoding='utf-8')
        table = re.search(r'icon_move\[BUTTONS\]=\{(.*?)\};', source)
        self.assertIsNotNone(table)
        moves = re.findall(r'\{\((\d+)-(\d+)\)\*4,\((\d+)-(\d+)\)\*4\}', table.group(1))
        self.assertEqual([tuple(map(int, m)) for m in moves],
                         [(x, ICON_FROM[0], y, ICON_FROM[1]) for _, (x, y) in PATCHES])
        order = re.search(r'enum \{ BUTTON_DISPLAY, BUTTON_CHEATS, BUTTONS \};', source)
        self.assertIsNotNone(order)
        self.assertEqual([name for name, _ in PATCHES], ['display', 'cheats'])


@unittest.skipUnless(GAME and (GAME / CONFIG_BUNDLE).is_file(), 'needs the game dump (THEATRHYTHM_GAME_DIR)')
class GameBundleTests(unittest.TestCase):
    def test_replacement_keeps_the_crc_and_changes_only_the_icons(self):
        original = (GAME / CONFIG_BUNDLE).read_bytes()
        secret = secret_of(GAME)
        blocks = patches()
        out = build(original, secret, blocks)
        self.assertEqual(out[:32], original[:32])
        before, after = read_bundle(decrypt(original, secret)), read_bundle(decrypt(out, secret))
        self.assertEqual(zlib.crc32(after['stream']), CONFIG_CRC)
        self.assertEqual(after['nodes'], before['nodes'])
        offset = atlas_node(before)
        self.assertEqual(after['stream'][:offset], before['stream'][:offset])
        old, new = untile(before['stream'][offset:]), untile(after['stream'][offset:])
        w, h = PATCH_SIZE
        changed = {i for i in range(BLOCKS * BLOCKS) if old[i * 16:i * 16 + 16] != new[i * 16:i * 16 + 16]}
        inside = {(y + r) * BLOCKS + x + c for _, (x, y) in PATCHES for r in range(h) for c in range(w)}
        self.assertEqual(changed - inside, {BLOCKS * BLOCKS - 1})  # the CRC block in the corner
        self.assertEqual(new[-16:-4], CLEAR_BLOCK[:12])
        for (_, (x, y)), patch in zip(PATCHES, blocks):
            for r in range(h):
                at = ((y + r) * BLOCKS + x) * 16
                self.assertEqual(new[at:at + w * 16], patch[r * w * 16:(r + 1) * w * 16])

    @unittest.skipUnless(importlib.util.find_spec('texture2ddecoder'), 'needs texture2ddecoder')
    def test_patches_go_where_nothing_is_drawn(self):
        # a button's sprite is 92x92 around the icon, so 10 blocks around a patch must be clear too
        import texture2ddecoder
        bgra = texture2ddecoder.decode_bc7(untile(original_texture(GAME)), BLOCKS * 4, BLOCKS * 4)
        alpha = bgra[3::4]
        w, h = PATCH_SIZE
        margin = 10
        for name, (x, y) in PATCHES:
            drawn = [(bx, by) for by in range(y - margin, y + h + margin) for bx in range(x - margin, x + w + margin)
                     if any(alpha[(by * 4 + r) * BLOCKS * 4 + bx * 4:(by * 4 + r) * BLOCKS * 4 + bx * 4 + 4] != bytes(4)
                            for r in range(4))]
            self.assertEqual(drawn, [], name)

    def test_other_game_versions_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'another game version'):
            build((GAME / CONFIG_BUNDLE).read_bytes(), secret_of(GAME) + 'x', patches())


if __name__ == '__main__':
    unittest.main()
