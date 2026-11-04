// SPDX-License-Identifier: GPL-2.0-or-later
// Images: loading (stb_image; ffmpeg for what it does not read, such as WebP), Pillow's LANCZOS
// resize to the pixel, PNG output, and PS4 Unity textures (BC7 / DXT1) as the game stores them: rows
// bottom-up, BC blocks in 8x8-block tiles (Morton order inside), each side padded to whole tiles
// (32 px). Same layout as the runner's tools/encode_button_icons.py.
#pragma once

#include "util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dlcmaker::image {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba; // w * h * 4, top row first
};

Image load(const fs::path& path);                 // as Image.open(path).convert('RGBA'); Exit when unreadable
Image resize_lanczos(const Image& im, int w, int h); // Image.resize((w, h), Image.LANCZOS) of an RGBA image
Image flip_vertical(const Image& im);
Image crop(const Image& im, int x0, int y0, int x1, int y1);
bool bbox(const Image& im, int box[4]);            // Image.getbbox() of RGBA: the non-transparent area
Image thumbnail(const Image& im, int size);        // fitted within size x size, aspect kept
std::string png(const Image& im, bool alpha = true);

// Textures (m_TextureFormat 25 BC7, 10 DXT1).
int block_bytes(int format);
std::string encode_texture(const Image& im, int width, int height, int format);
Image decode_texture(std::string_view data, int width, int height, int format);

} // namespace dlcmaker::image
