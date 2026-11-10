#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the icons of the DLC maker and the save editor from the game's icon
(packaging/windows/theatrhythm-fbl.ico), so the three can be told apart on the taskbar:

    tools/dlcmaker/app/theatrhythm-dlcmaker.ico      violet backdrop, a pack box with notes on it
    tools/saveeditor/app/theatrhythm-saveeditor.ico  green backdrop, a floppy disk and a pencil

Each is the game's icon with its blue backdrop turned to another colour, a little smaller in the top
left, and a sticker over its bottom right corner that reaches past it. The stickers are drawn here (colour fill, an edge in a dark shade of that colour and a
soft shadow). Small sizes get their own drawing: a larger sticker with fewer details.

    python tools/make_tool_icons.py [--preview PNG]

Run it again after tools/make_app_icon.py changes the game's icon.
"""
import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

PORT = Path(__file__).resolve().parent.parent
GAME_ICON = PORT / 'packaging' / 'windows' / 'theatrhythm-fbl.ico'
SIZES = (256, 128, 64, 48, 40, 32, 24, 20, 16)
S = 8            # supersampling of the sticker drawing
EYE = (126, 97, 16)   # the eye in the 256 px icon: centre x, y and radius
PAD = 14         # room around the 100x100 sticker box for its edge and shadow, in sticker units
WHITE = (255, 255, 255)
TILE = 0.80       # the game's icon takes this share of the canvas, from the top left


# ---- sticker drawing: masks in a 100x100 box ------------------------------------------------------

def mask():
    side = (100 + 2 * PAD) * S
    return Image.new("L", (side, side), 0)


def u(v):
    return (v + PAD) * S


def rrect(m, x0, y0, x1, y1, r, value=255):
    ImageDraw.Draw(m).rounded_rectangle((u(x0), u(y0), u(x1), u(y1)), r * S, fill=value)


def poly(m, pts, value=255):
    ImageDraw.Draw(m).polygon([(u(x), u(y)) for x, y in pts], fill=value)


def ellipse(m, cx, cy, rx, ry, value=255):
    ImageDraw.Draw(m).ellipse((u(cx - rx), u(cy - ry), u(cx + rx), u(cy + ry)), fill=value)


def grow(m, units):
    """The mask grown by `units` on every side (round corners)."""
    r = units * S
    return m.filter(ImageFilter.GaussianBlur(r / 2)).point(lambda v: 255 if v > 14 else 0) \
            .filter(ImageFilter.GaussianBlur(S / 4))


def fill(m, colour):
    """An RGBA layer: `colour` (or a top-to-bottom gradient over the mask's own height) through m."""
    if isinstance(colour[0], tuple):
        box = m.getbbox() or (0, 0, 1, 1)
        t = np.clip((np.arange(m.height) - box[1]) / max(1, box[3] - box[1]), 0, 1)[:, None]
        rows = np.asarray(colour[0]) * (1 - t) + np.asarray(colour[1]) * t
        rgb = Image.fromarray(np.broadcast_to(rows[:, None, :], (m.height, m.width, 3)).astype(np.uint8), "RGB")
    else:
        rgb = Image.new("RGB", m.size, colour)
    layer = rgb.convert("RGBA")
    layer.putalpha(m)
    return layer


def sticker(groups, px, edge, rim_colour):
    """Each group is a list of (mask, colour) painted back to front and gets its own edge of `edge`
    units in `rim_colour`, so a group drawn over another stays apart from it. Returns the sticker at `px`
    pixels per 100 units (the image is larger by PAD on each side)."""
    side = (100 + 2 * PAD) * S
    art = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    whole = Image.new("L", (side, side), 0)
    for group in groups:
        shape = Image.new("L", (side, side), 0)
        for m, _ in group:
            shape = ImageChops.lighter(shape, m)
        rim = grow(shape, edge)
        whole = ImageChops.lighter(whole, rim)
        art.alpha_composite(fill(rim, rim_colour))
        for m, colour in group:
            art.alpha_composite(fill(m, colour))
    shadow = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    soft = ImageChops.offset(whole, 0, int(2.5 * S)).filter(ImageFilter.GaussianBlur(3 * S))
    shadow.putalpha(soft.point(lambda v: int(v * 0.45)))
    shadow.paste((20, 30, 70), (0, 0, side, side), shadow.getchannel("A"))
    shadow.alpha_composite(art)
    out = round(px * (100 + 2 * PAD) / 100)
    return shadow.resize((out, out), Image.LANCZOS)


# ---- the two stickers -----------------------------------------------------------------------------

def floppy(detail):
    """A floppy disk (save) and, with detail, a pencil across its corner (edit)."""
    body_c = ((132, 116, 255), (78, 58, 214))
    body = mask()
    rrect(body, 8, 8, 92, 92, 11)
    poly(body, [(76, 0), (100, 0), (100, 24)], 0)             # the cut corner
    poly(body, [(70, 8), (76, 8), (92, 24), (92, 30), (70, 30)])
    shutter = mask()
    rrect(shutter, 28, 8, 68, 36, 3)
    slot = mask()
    rrect(slot, 53, 13, 62, 31, 2)
    label = mask()
    rrect(label, 22, 52, 78, 92, 5)
    group = [(body, body_c), (shutter, (226, 232, 246)), (slot, (78, 58, 214)), (label, WHITE)]
    if not detail:
        return [group]
    lines = mask()
    rrect(lines, 30, 62, 62, 66, 2)
    rrect(lines, 30, 73, 52, 77, 2)
    group.append((lines, (190, 198, 226)))

    # the pencil: tip bottom left, eraser top right
    tx, ty, ex, ey, half = 45.0, 97.0, 101.0, 41.0, 8.5
    length = math.hypot(ex - tx, ey - ty)
    dx, dy = (ex - tx) / length, (ey - ty) / length

    def at(along, across):
        return tx + dx * along - dy * across, ty + dy * along + dx * across

    def band(a0, a1, w0=half, w1=half):
        m = mask()
        poly(m, [at(a0, -w0), at(a1, -w1), at(a1, w1), at(a0, w0)])
        return m

    cone, lead = band(0, 17, 0, half), band(0, 6.5, 0, half * 6.5 / 17)
    wood, shade = band(17, length - 15), band(17, length - 15, half, 0)   # shade: the lower half only
    shade = ImageChops.multiply(shade, wood)
    ferrule, rubber = band(length - 15, length - 10), mask()
    poly(rubber, [at(length - 10, -half), at(length - 3, -half), at(length, -half + 3), at(length, half - 3),
                  at(length - 3, half), at(length - 10, half)])
    pencil = [(cone, (255, 226, 178)), (lead, (74, 55, 40)), (wood, (255, 204, 64)), (shade, (255, 176, 46)),
              (ferrule, (196, 202, 216)), (rubber, (255, 122, 160))]
    return [group, pencil]


def pack(detail):
    """A lidded box (a pack) with a pair of notes on it (songs)."""
    body = mask()
    rrect(body, 14, 34, 86, 94, 9)
    lid = mask()
    rrect(lid, 7, 14, 93, 40, 8)
    under = mask()                                             # the lid's shadow on the box
    rrect(under, 14, 40, 86, 45, 0)
    notes = mask()
    ellipse(notes, 37, 80, 8.5, 6.5)
    ellipse(notes, 63, 74, 8.5, 6.5)
    rrect(notes, 41, 56, 45.5, 80, 1)
    rrect(notes, 67, 50, 71.5, 74, 1)
    poly(notes, [(41, 52), (71.5, 45.5), (71.5, 56), (41, 62.5)])
    group = [(body, ((255, 112, 176), (226, 52, 138))), (under, (198, 38, 120)),
             (lid, ((255, 150, 196), (255, 96, 164))), (notes, WHITE)]
    if detail:
        shine = mask()
        rrect(shine, 14, 19, 46, 23, 2)
        group.append((shine, (255, 196, 222)))
    return [group]


# ---- backdrops ------------------------------------------------------------------------------------

def recolour(image, shift, keep=1.0):
    """Turns the blue backdrop (hues from cyan to blue) by `shift` degrees and scales its saturation
    by `keep`; the rest is untouched."""
    px = np.asarray(image.convert("RGBA")).astype(np.float32) / 255
    r, g, b = px[..., 0], px[..., 1], px[..., 2]
    hi, lo = px[..., :3].max(-1), px[..., :3].min(-1)
    span = np.where(hi - lo == 0, 1, hi - lo)
    hue = np.where(hi == r, ((g - b) / span) % 6, np.where(hi == g, (b - r) / span + 2, (r - g) / span + 4)) * 60
    sat = np.where(hi == 0, 0, (hi - lo) / np.where(hi == 0, 1, hi))
    # how much of the shift a pixel takes: all of it from 170 to 250 degrees, fading over 25 either side
    weight = np.clip((hue - 145) / 25, 0, 1) * np.clip((275 - hue) / 25, 0, 1)
    # the chocobo's eye is blue too: leave it (a soft disc around EYE, in 256 px terms)
    h, w = hue.shape
    yy, xx = np.mgrid[0:h, 0:w]
    reach = np.hypot((xx + 0.5) * 256 / w - EYE[0], (yy + 0.5) * 256 / h - EYE[1])
    weight = weight * np.clip((reach - EYE[2]) / 4, 0, 1)
    hue = (hue + shift * weight) % 360
    sat = sat * (1 - (1 - keep) * weight)
    c = hi * sat
    x = c * (1 - np.abs((hue / 60) % 2 - 1))
    zero = np.zeros_like(c)
    sector = (hue // 60).astype(int) % 6
    table = [(c, x, zero), (x, c, zero), (zero, c, x), (zero, x, c), (x, zero, c), (c, zero, x)]
    out = np.zeros_like(px)
    for k, parts in enumerate(table):
        for i in range(3):
            out[..., i] = np.where(sector == k, parts[i] + hi - c, out[..., i])
    out[..., 3] = px[..., 3]
    return Image.fromarray(np.round(out * 255).astype(np.uint8), "RGBA")


# ---- the icons ------------------------------------------------------------------------------------

TOOLS = {
    'dlcmaker': {'sticker': pack, 'shift': 62, 'keep': 0.88, 'rim': (74, 12, 66)},        # blue -> violet
    'saveeditor': {'sticker': floppy, 'shift': -62, 'keep': 0.80, 'rim': (30, 20, 92)},   # blue -> green
}


def frame(game, spec, size):
    """One size of a tool's icon, from the game's 256 px icon: the game's icon smaller in the top left,
    and the sticker over its bottom right corner, reaching past it to the corner of the canvas."""
    # the sticker takes more of a small icon, and loses its details there
    share = 0.52 if size >= 64 else 0.56 if size >= 40 else 0.62
    edge = 6.0 if size >= 64 else 7.5 if size >= 32 else 9.0
    tile = round(size * TILE)
    image = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    image.alpha_composite(recolour(game, spec['shift'], spec['keep']).resize((tile, tile), Image.LANCZOS))
    box = round(size * share)
    art = sticker(spec['sticker'](size >= 40), box, edge, spec['rim'])
    # what is drawn (not its shadow) ends on the canvas's right and bottom edges
    drawn = art.getchannel('A').point(lambda v: 255 if v > 128 else 0).getbbox()
    layer = Image.new('RGBA', image.size, (0, 0, 0, 0))
    layer.alpha_composite(art, (size - drawn[2], size - drawn[3] - (1 if size >= 64 else 0)))
    image.alpha_composite(layer)
    return image


def frames(spec):
    game = Image.open(GAME_ICON)
    game.size = (256, 256)
    game = game.convert('RGBA')
    return {size: frame(game, spec, size) for size in SIZES}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--preview', type=Path, help='also write both icons at 256, 48, 32, 24 and 16 px on a dark background')
    a = p.parse_args()
    icons = {}
    for tool, spec in TOOLS.items():
        sized = icons[tool] = frames(spec)
        out = PORT / 'tools' / tool / 'app' / f'theatrhythm-{tool}.ico'
        sized[256].save(out, format='ICO', sizes=[(s, s) for s in SIZES],
                        append_images=[sized[s] for s in SIZES if s != 256])
        print(f'Wrote {out}')
    if a.preview:
        show = (256, 48, 32, 24, 16)
        preview = Image.new('RGBA', (sum(s + 16 for s in show) + 16, 288 * len(icons)), (40, 40, 40, 255))
        for row, sized in enumerate(icons.values()):
            x = 16
            for s in show:
                preview.alpha_composite(sized[s], (x, row * 288 + 16 + 256 - s))
                x += s + 16
        preview.save(a.preview)


if __name__ == '__main__':
    main()
