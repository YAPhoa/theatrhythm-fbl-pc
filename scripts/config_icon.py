#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the Options menu's Config atlas bundle with the Display Settings and Cheats icons in unused areas.

    python scripts/config_icon.py GAME --out OUT

The Options buttons take their icons from the Config sprite atlas, a 1024x1024 BC7 texture in
Media/StreamingAssets/aa/PS4/<CONFIG_BUNDLE>. Unlike the button icon bundle, this one is encrypted
by the game's Game.File.AssetBundleCryptProvider (Game.AssetBundle.AssetBundleCryptography): a
32-byte header carries the PBKDF2 salt and the IV, then AES-128-CBC with PKCS#7 padding. The
password is a string literal in the game's own metadata, decoded the way DecryptKey does it.

This writes OUT/config_icon/display.bundle: the game's bundle with assets/config_icon/display.bc7
and cheats.bc7 (encoded by tools/encode_config_icon.py) in atlas blocks no sprite uses, blocks stored
uncompressed, encrypted again with the original header. No sprite shows those blocks: the runtime
gives the Display Settings and Cheats buttons sprites of their own over them
(src/runtime_display_page.c, show_icon), so Logo Usage keeps its icon.
The catalog's CRC of the decompressed data is kept by forging the index bits of a transparent block
in a corner no sprite uses, as scripts/button_icons.py does. The runtime mounts the bundle over the
game's file (src/probe.c).

No third-party modules or Python dependencies.
"""
import argparse
import base64
import hashlib
from pathlib import Path
import struct
import sys
import zlib

from button_icons import forge_crc, read_bundle, write_bundle

PORT = Path(__file__).resolve().parent.parent
ASSETS = PORT / 'assets' / 'config_icon'
CONFIG_BUNDLE = 'Media/StreamingAssets/aa/PS4/0aecbe09e3673328eba2f60a0720e8b5.bundle'
CONFIG_CRC = 1805126692  # catalog.json m_Crc of CONFIG_BUNDLE (CUSA32853 v01.00)
METADATA = 'Media/Metadata/global-metadata.dat'
KEY_LITERAL = 1152  # string literal that AssetBundleCryptographyParam.get_EncryptionKey decodes
TEXTURE_BYTES = 1024 * 1024  # BC7, one byte per texel, no mipmaps
BLOCKS = 256  # blocks per row and column
# Logo Usage's icon (ConfigTop_Button_Icon_07) covers atlas columns 211-269, rows 95-148 (top row
# first). A patch is drawn on the whole blocks around it, ICON_FROM: block columns 52-67 and,
# counted from the bottom row as Unity stores rows, block rows 218-232. The patches go to PATCHES
# instead, where nothing is drawn within 40 px (10 blocks) but the other patch, 44 px away: room for
# each sprite's whole 92x92 rect. src/runtime_display_page.c moves Logo Usage's sprite by a patch's
# place - ICON_FROM (icon_move, in the order of PATCHES).
ICON_FROM = (52, 218)
PATCH_SIZE = (16, 15)  # width, height in blocks
PATCH_BYTES = PATCH_SIZE[0] * PATCH_SIZE[1] * 16
PATCHES = (('display', (208, 192)), ('cheats', (208, 218)))  # the asset's name; block x, block y
# The last block of the tiled data is the sheet's top right corner (texels 1020-1023 of the top
# rows), which no sprite uses. BC7 mode 6 with every endpoint 0 decodes to transparent black
# whatever its index bits, so its last 4 bytes are free for the CRC.
CLEAR_BLOCK = bytes([0x40]) + bytes(15)


def literal(metadata, index):
    """String literal `index` of global-metadata.dat v29 (table: length, data offset)."""
    sanity, version = struct.unpack_from('<Ii', metadata, 0)
    if sanity != 0xFAB11BAF or version != 29: raise ValueError(f'unsupported metadata (version {version})')
    table, table_size, data = struct.unpack_from('<3I', metadata, 8)
    if index * 8 + 8 > table_size: raise ValueError(f'no string literal {index}')
    length, offset = struct.unpack_from('<II', metadata, table + index * 8)
    return metadata[data + offset:data + offset + length].decode('utf-8')


def password(encoded):
    """AssetBundleCryptography.DecryptKey, then get_EncryptionKey's Substring(2, Length - 4).

    The literal is base64 of big-endian 16-bit words; each pair of words XORed with the previous
    character's bits 8-11 (0x5963 to start) gives one character, last character first."""
    raw = base64.b64decode(encoded)
    words = [raw[i] << 8 | raw[i + 1] for i in range(0, len(raw) - 1, 2)]
    chars = [0] * (len(words) // 2)
    previous = 0x5963
    for k in range(len(chars)):
        value = previous ^ words[2 * k + 1] ^ words[2 * k]
        chars[len(chars) - 1 - k] = value
        previous = value >> 8 & 0xf
    return ''.join(map(chr, chars))[2:-2]


# AES-128 (FIPS-197) with the usual combined round tables.
def _gmul(a, b):
    p = 0
    while b:
        if b & 1: p ^= a
        a = (a << 1 ^ (0x11b if a & 0x80 else 0)) & 0xff
        b >>= 1
    return p


def _tables():
    inverse = [0] * 256
    for a in range(1, 256):
        inverse[a] = next(b for b in range(1, 256) if _gmul(a, b) == 1)
    sbox = []
    for a in range(256):
        x = inverse[a]
        sbox.append(x ^ (x << 1 | x >> 7) & 0xff ^ (x << 2 | x >> 6) & 0xff ^ (x << 3 | x >> 5) & 0xff
                    ^ (x << 4 | x >> 4) & 0xff ^ 0x63)
    inv_sbox = [0] * 256
    for a, s in enumerate(sbox): inv_sbox[s] = a
    def rotations(word):
        return [((word >> 8 * k) | (word << (32 - 8 * k))) & 0xffffffff for k in range(4)]
    enc = [rotations(_gmul(s, 2) << 24 | s << 16 | s << 8 | _gmul(s, 3)) for s in sbox]
    dec = [rotations(_gmul(s, 14) << 24 | _gmul(s, 9) << 16 | _gmul(s, 13) << 8 | _gmul(s, 11)) for s in inv_sbox]
    return sbox, inv_sbox, [list(t) for t in zip(*enc)], [list(t) for t in zip(*dec)]


SBOX, INV_SBOX, TE, TD = _tables()


def _round_keys(key):
    w = [int.from_bytes(key[i:i + 4], 'big') for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = w[i - 1]
        if i % 4 == 0:
            t = (SBOX[t >> 16 & 255] << 24 | SBOX[t >> 8 & 255] << 16 | SBOX[t & 255] << 8 | SBOX[t >> 24]) ^ rcon << 24
            rcon = _gmul(rcon, 2)
        w.append(w[i - 4] ^ t)
    enc = [w[4 * r:4 * r + 4] for r in range(11)]
    # equivalent inverse cipher: middle round keys go through InvMixColumns
    dec = [enc[10]] + [[TD[0][SBOX[x >> 24]] ^ TD[1][SBOX[x >> 16 & 255]] ^ TD[2][SBOX[x >> 8 & 255]]
                        ^ TD[3][SBOX[x & 255]] for x in enc[r]] for r in range(9, 0, -1)] + [enc[0]]
    return enc, dec


def _block(state, keys, t, sbox, shift):
    """One AES block (4 big-endian words). shift 1: encryption rows, 3: decryption rows."""
    s = [state[i] ^ keys[0][i] for i in range(4)]
    t0, t1, t2, t3 = t
    for k in keys[1:10]:
        s = [t0[s[i] >> 24] ^ t1[s[(i + shift) % 4] >> 16 & 255] ^ t2[s[(i + 2) % 4] >> 8 & 255]
             ^ t3[s[(i + 3 * shift) % 4] & 255] ^ k[i] for i in range(4)]
    k = keys[10]
    return [(sbox[s[i] >> 24] << 24 | sbox[s[(i + shift) % 4] >> 16 & 255] << 16
             | sbox[s[(i + 2) % 4] >> 8 & 255] << 8 | sbox[s[(i + 3 * shift) % 4] & 255]) ^ k[i] for i in range(4)]


def _words(data, at):
    return list(struct.unpack_from('>4I', data, at))


def cbc_encrypt(key, iv, data):
    keys, _ = _round_keys(key)
    out = bytearray()
    chain = _words(iv, 0)
    for at in range(0, len(data), 16):
        chain = _block([a ^ b for a, b in zip(_words(data, at), chain)], keys, TE, SBOX, 1)
        out += struct.pack('>4I', *chain)
    return bytes(out)


def cbc_decrypt(key, iv, data):
    _, keys = _round_keys(key)
    out = bytearray()
    chain = _words(iv, 0)
    for at in range(0, len(data), 16):
        block = _words(data, at)
        out += struct.pack('>4I', *(a ^ b for a, b in zip(_block(block, keys, TD, INV_SBOX, 3), chain)))
        chain = block
    return bytes(out)


def bundle_key(header, secret):
    """AssetBundleCryptography.readAESParameter + setupAES: the header's 8-byte quarters are
    salt[0:8], iv[8:16], iv[0:8], salt[8:16]; the key is PBKDF2-HMAC-SHA1(secret, salt, 1000)."""
    salt = header[0:8] + header[24:32]
    iv = header[16:24] + header[8:16]
    return hashlib.pbkdf2_hmac('sha1', secret.encode('utf-8'), salt, 1000, 16), iv


def decrypt(data, secret):
    if len(data) < 48 or len(data) % 16: raise ValueError('not an encrypted bundle')
    plain = cbc_decrypt(*bundle_key(data[:32], secret), data[32:])
    pad = plain[-1]
    if not 1 <= pad <= 16 or plain[-pad:] != bytes([pad]) * pad or not plain.startswith(b'UnityFS\0'):
        raise ValueError('cannot decrypt it: another game version?')
    return plain[:-pad]


def encrypt(plain, header, secret):
    pad = 16 - len(plain) % 16
    return header + cbc_encrypt(*bundle_key(header, secret), plain + bytes([pad]) * pad)


def _morton():
    """(x, y) of the 64 blocks of an 8x8-block tile, in the PS4's order."""
    order = []
    for t in range(64):
        x = y = 0
        for bit in range(3):
            x |= (t >> 2 * bit & 1) << bit
            y |= (t >> 2 * bit + 1 & 1) << bit
        order.append((x, y))
    return order


def _tiled_order():
    """Linear block index of each block of the tiled texture, in order."""
    return [(ty * 8 + y) * BLOCKS + tx * 8 + x
            for ty in range(BLOCKS // 8) for tx in range(BLOCKS // 8) for x, y in _morton()]


def untile(tiled):
    linear = bytearray(len(tiled))
    for n, i in enumerate(_tiled_order()):
        linear[i * 16:i * 16 + 16] = tiled[n * 16:n * 16 + 16]
    return bytes(linear)


def tile(linear):
    return b''.join(linear[i * 16:i * 16 + 16] for i in _tiled_order())


def secret_of(game):
    return password(literal((game / METADATA).read_bytes(), KEY_LITERAL))


def atlas_node(b):
    """Offset of the atlas texture in the data stream (its .resS, at the end of the stream)."""
    textures = [n for n in b['nodes'] if n[3].endswith(b'.resS') and n[1] == TEXTURE_BYTES]
    if len(textures) != 1: raise ValueError('no single atlas texture stream in the Config bundle')
    offset = textures[0][0]
    if offset + TEXTURE_BYTES != len(b['stream']): raise ValueError('the atlas texture is not at the end of the data stream')
    return offset


def original_texture(game):
    """The game's Config atlas data, tiled as stored."""
    b = read_bundle(decrypt((game / CONFIG_BUNDLE).read_bytes(), secret_of(game)))
    return b['stream'][atlas_node(b):]


def patches():
    """The checked-in patches, in the order of PATCHES."""
    return [(ASSETS / f'{name}.bc7').read_bytes() for name, _ in PATCHES]


def build(original, secret, blocks):
    """The encrypted Config bundle (bytes) with `blocks` (a patch's blocks, row by row, for each of
    PATCHES) in the atlas."""
    b = read_bundle(decrypt(original, secret))
    crc = zlib.crc32(b['stream'])
    if crc != CONFIG_CRC: raise ValueError(f'Config bundle CRC {crc} is not {CONFIG_CRC}: another game version?')
    if len(blocks) != len(PATCHES): raise ValueError(f'{len(blocks)} patches, expected {len(PATCHES)}')
    offset = atlas_node(b)
    linear = bytearray(untile(b['stream'][offset:]))
    w, h = PATCH_SIZE
    for (name, (x, y)), patch in zip(PATCHES, blocks):
        if len(patch) != PATCH_BYTES: raise ValueError(f'patch {name} is {len(patch)} bytes, expected {PATCH_BYTES}')
        for row in range(h):
            at = ((y + row) * BLOCKS + x) * 16
            linear[at:at + w * 16] = patch[row * w * 16:(row + 1) * w * 16]
    # forge_crc rewrites the stream's last 4 bytes: the index bits of CLEAR_BLOCK
    stream = b['stream'][:offset] + tile(linear)[:-16] + CLEAR_BLOCK
    return encrypt(write_bundle(b, forge_crc(stream, crc)), original[:32], secret)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    folder = a.out / 'config_icon'
    folder.mkdir(parents=True, exist_ok=True)  # run.py: prepared, even with nothing to build
    target = folder / 'display.bundle'
    target.unlink(missing_ok=True)
    source = a.game / CONFIG_BUNDLE
    if not source.is_file() or not (a.game / METADATA).is_file():
        print(f"Config icon: {source} or the metadata not found; Display Settings and Cheats show Logo Usage's icon")
        return 0
    try:
        bundle = build(source.read_bytes(), secret_of(a.game), patches())
    except (OSError, ValueError, UnicodeDecodeError) as e:
        print(f"Config icon: {e}; Display Settings and Cheats show Logo Usage's icon")
        return 0
    target.write_bytes(bundle)
    print(f'Config icon: {target} ({len(bundle)} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
