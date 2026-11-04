// SPDX-License-Identifier: GPL-2.0-or-later
#include "image.h"

#include "media.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include "ProcessDxtc.hpp"
#include "bc7enc.h"
#pragma GCC diagnostic pop

namespace dlcmaker::image {

namespace {

Image from_memory(std::string_view data) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()), static_cast<int>(data.size()), &w, &h, &n, 4);
    Image im;
    if (!px)
        return im;
    im.w = w, im.h = h;
    im.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return im;
}

} // namespace

Image load(const fs::path& path) {
    Image im = from_memory(read_file(path));
    if (im.w)
        return im;
    // What stb_image does not read (WebP): ffmpeg decodes it to a PNG.
    if (auto ffmpeg = media::try_ffmpeg()) {
        Captured c = capture({*ffmpeg, "-hide_banner", "-v", "error", "-i", u8(path), "-frames:v", "1", "-f", "image2pipe", "-c:v", "png",
                              "-pix_fmt", "rgba", "-"});
        if (c.code == 0)
            im = from_memory(c.out);
    }
    if (!im.w)
        throw Exit("cannot identify image file '" + u8(path) + "'");
    return im;
}

// --- Pillow's resampling (libImaging/Resample.c, 8 bits per channel) ------------------------------

namespace {

constexpr int kPrecision = 32 - 8 - 2;
constexpr double kPi = 3.14159265358979323846; // M_PI

double sinc(double x) {
    if (x == 0.0)
        return 1.0;
    x = x * kPi;
    return std::sin(x) / x;
}

double lanczos(double x) {
    if (-3.0 <= x && x < 3.0)
        return sinc(x) * sinc(x / 3);
    return 0.0;
}

struct Coeffs {
    int ksize = 0;
    std::vector<int> bounds; // per output pixel: first input, count
    std::vector<int32_t> kk;
};

Coeffs precompute(int in_size, double in0, double in1, int out_size) {
    Coeffs c;
    double scale = (in1 - in0) / out_size, filterscale = scale < 1.0 ? 1.0 : scale;
    double support = 3.0 * filterscale;
    c.ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
    std::vector<double> kk(static_cast<size_t>(out_size) * c.ksize);
    c.bounds.resize(static_cast<size_t>(out_size) * 2);
    for (int xx = 0; xx < out_size; ++xx) {
        double center = in0 + (xx + 0.5) * scale, ww = 0.0, ss = 1.0 / filterscale;
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0)
            xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > in_size)
            xmax = in_size;
        xmax -= xmin;
        double* k = &kk[static_cast<size_t>(xx) * c.ksize];
        int x = 0;
        for (; x < xmax; ++x) {
            double w = lanczos((x + xmin - center + 0.5) * ss);
            k[x] = w;
            ww += w;
        }
        for (x = 0; x < xmax; ++x)
            if (ww != 0.0)
                k[x] /= ww;
        for (; x < c.ksize; ++x)
            k[x] = 0;
        c.bounds[static_cast<size_t>(xx) * 2] = xmin;
        c.bounds[static_cast<size_t>(xx) * 2 + 1] = xmax;
    }
    c.kk.resize(kk.size());
    for (size_t x = 0; x < kk.size(); ++x)
        c.kk[x] = kk[x] < 0 ? static_cast<int32_t>(-0.5 + kk[x] * (1 << kPrecision)) : static_cast<int32_t>(0.5 + kk[x] * (1 << kPrecision));
    return c;
}

uint8_t clip8(int32_t v) {
    if (v >= (1 << kPrecision << 8))
        return 255;
    if (v <= 0)
        return 0;
    return static_cast<uint8_t>(v >> kPrecision);
}

uint8_t muldiv255(unsigned a, unsigned b) {
    unsigned tmp = a * b + 128;
    return static_cast<uint8_t>(((tmp >> 8) + tmp) >> 8);
}

} // namespace

namespace {

// ResampleHorizontal_8bpc: rows offset.. of `in`, each to out.w pixels.
Image pass_horizontal(const Image& in, int w, int rows, int offset, const Coeffs& c) {
    Image out;
    out.w = w, out.h = rows;
    out.rgba.resize(static_cast<size_t>(w) * rows * 4);
    for (int yy = 0; yy < rows; ++yy) {
        const uint8_t* row = &in.rgba[static_cast<size_t>(yy + offset) * in.w * 4];
        for (int xx = 0; xx < w; ++xx) {
            int xmin = c.bounds[static_cast<size_t>(xx) * 2], xmax = c.bounds[static_cast<size_t>(xx) * 2 + 1];
            const int32_t* k = &c.kk[static_cast<size_t>(xx) * c.ksize];
            for (int ch = 0; ch < 4; ++ch) {
                int32_t ss = 1 << (kPrecision - 1);
                for (int x = 0; x < xmax; ++x)
                    ss += row[(x + xmin) * 4 + ch] * k[x];
                out.rgba[(static_cast<size_t>(yy) * w + xx) * 4 + ch] = clip8(ss);
            }
        }
    }
    return out;
}

// ResampleVertical_8bpc: every column of `in` to h pixels.
Image pass_vertical(const Image& in, int h, const Coeffs& c) {
    Image out;
    out.w = in.w, out.h = h;
    out.rgba.resize(static_cast<size_t>(in.w) * h * 4);
    for (int yy = 0; yy < h; ++yy) {
        const int32_t* k = &c.kk[static_cast<size_t>(yy) * c.ksize];
        int ymin = c.bounds[static_cast<size_t>(yy) * 2], ymax = c.bounds[static_cast<size_t>(yy) * 2 + 1];
        for (int xx = 0; xx < in.w; ++xx)
            for (int ch = 0; ch < 4; ++ch) {
                int32_t ss = 1 << (kPrecision - 1);
                for (int y = 0; y < ymax; ++y)
                    ss += in.rgba[(static_cast<size_t>(y + ymin) * in.w + xx) * 4 + ch] * k[y];
                out.rgba[(static_cast<size_t>(yy) * in.w + xx) * 4 + ch] = clip8(ss);
            }
    }
    return out;
}

} // namespace

Image resize_lanczos(const Image& src, int w, int h) {
    if (src.w == w && src.h == h)
        return src; // Pillow returns a copy, without the premultiplied round trip
    // RGBA -> RGBa (premultiplied), resample, back to RGBA, as Image.resize does for RGBA.
    Image cur = src;
    for (size_t p = 0; p < cur.rgba.size(); p += 4)
        for (int c = 0; c < 3; ++c)
            cur.rgba[p + c] = muldiv255(cur.rgba[p + c], cur.rgba[p + 3]);
    // ImagingResampleInner: the vertical pass first when the height shrinks more than twice the width.
    bool need_h = w != src.w, need_v = h != src.h;
    bool horizontal_first = !((src.h - h) > 0 && (src.h - h) > (src.w - w) * 2);
    Coeffs vert = precompute(src.h, 0, src.h, h), horiz = precompute(src.w, 0, src.w, w);
    if (horizontal_first) {
        if (need_h) {
            int first = vert.bounds[0], last = vert.bounds[static_cast<size_t>(h) * 2 - 2] + vert.bounds[static_cast<size_t>(h) * 2 - 1];
            for (int i = 0; i < h; ++i)
                vert.bounds[static_cast<size_t>(i) * 2] -= first;
            cur = pass_horizontal(cur, w, last - first, first, horiz);
        }
        if (need_v)
            cur = pass_vertical(cur, h, vert);
    } else {
        if (need_v)
            cur = pass_vertical(cur, h, vert);
        if (need_h)
            cur = pass_horizontal(cur, w, h, 0, horiz);
    }
    for (size_t p = 0; p < cur.rgba.size(); p += 4) {
        unsigned a = cur.rgba[p + 3];
        if (a != 255 && a != 0)
            for (int c = 0; c < 3; ++c)
                cur.rgba[p + c] = static_cast<uint8_t>(std::min(255u, 255u * cur.rgba[p + c] / a));
    }
    return cur;
}

Image flip_vertical(const Image& im) {
    Image out = im;
    size_t row = static_cast<size_t>(im.w) * 4;
    for (int y = 0; y < im.h; ++y)
        std::memcpy(&out.rgba[static_cast<size_t>(y) * row], &im.rgba[static_cast<size_t>(im.h - 1 - y) * row], row);
    return out;
}

Image crop(const Image& im, int x0, int y0, int x1, int y1) {
    Image out;
    out.w = x1 - x0, out.h = y1 - y0;
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            int sx = x + x0, sy = y + y0;
            if (sx < 0 || sy < 0 || sx >= im.w || sy >= im.h)
                continue;
            std::memcpy(&out.rgba[(static_cast<size_t>(y) * out.w + x) * 4], &im.rgba[(static_cast<size_t>(sy) * im.w + sx) * 4], 4);
        }
    return out;
}

bool bbox(const Image& im, int box[4]) {
    int x0 = im.w, y0 = im.h, x1 = -1, y1 = -1;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const uint8_t* p = &im.rgba[(static_cast<size_t>(y) * im.w + x) * 4];
            if (p[3]) { // RGBA: Pillow trims transparent pixels (getbbox(alpha_only=True))
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
        }
    if (x1 < 0)
        return false;
    box[0] = x0, box[1] = y0, box[2] = x1 + 1, box[3] = y1 + 1;
    return true;
}

Image thumbnail(const Image& im, int size) {
    if (im.w <= size && im.h <= size)
        return im;
    double scale = std::min(static_cast<double>(size) / im.w, static_cast<double>(size) / im.h);
    int w = std::max(1, static_cast<int>(std::lround(im.w * scale))), h = std::max(1, static_cast<int>(std::lround(im.h * scale)));
    return resize_lanczos(im, std::min(w, size), std::min(h, size));
}

std::string png(const Image& im, bool alpha) {
    std::vector<uint8_t> px;
    const uint8_t* data = im.rgba.data();
    if (!alpha) {
        px.resize(static_cast<size_t>(im.w) * im.h * 3);
        for (size_t p = 0, q = 0; p < im.rgba.size(); p += 4, q += 3)
            px[q] = im.rgba[p], px[q + 1] = im.rgba[p + 1], px[q + 2] = im.rgba[p + 2];
        data = px.data();
    }
    int n = alpha ? 4 : 3, len = 0;
    unsigned char* out = stbi_write_png_to_mem(data, im.w * n, im.w, im.h, n, &len);
    std::string s(reinterpret_cast<char*>(out), static_cast<size_t>(len));
    STBIW_FREE(out);
    return s;
}

// --- PS4 textures ---------------------------------------------------------------------------------

namespace {

int padded(int n) {
    return (n + 31) / 32 * 32;
}

void morton(int t, int& x, int& y) {
    x = y = 0;
    for (int bit = 0; bit < 3; ++bit) {
        x |= ((t >> (2 * bit)) & 1) << bit;
        y |= ((t >> (2 * bit + 1)) & 1) << bit;
    }
}

std::string tile(std::string_view linear, int bw, int bh, int size) {
    std::string out;
    out.reserve(linear.size());
    for (int ty = 0; ty < bh / 8; ++ty)
        for (int tx = 0; tx < bw / 8; ++tx)
            for (int t = 0; t < 64; ++t) {
                int mx, my;
                morton(t, mx, my);
                size_t i = (static_cast<size_t>(ty * 8 + my) * bw + tx * 8 + mx) * size;
                out.append(linear.substr(i, static_cast<size_t>(size)));
            }
    return out;
}

std::string untile(std::string_view tiled, int bw, int bh, int size) {
    std::string out(tiled.size(), '\0');
    size_t src = 0;
    for (int ty = 0; ty < bh / 8; ++ty)
        for (int tx = 0; tx < bw / 8; ++tx)
            for (int t = 0; t < 64; ++t) {
                int mx, my;
                morton(t, mx, my);
                size_t i = (static_cast<size_t>(ty * 8 + my) * bw + tx * 8 + mx) * size;
                if (src + size <= tiled.size() && i + size <= out.size())
                    std::memcpy(&out[i], &tiled[src], static_cast<size_t>(size));
                src += static_cast<size_t>(size);
            }
    return out;
}

} // namespace

int block_bytes(int format) {
    if (format == 25)
        return 16;
    if (format == 10)
        return 8;
    throw ValueError("texture format " + std::to_string(format) + " is not supported");
}

std::string encode_texture(const Image& im, int width, int height, int format) {
    int size = block_bytes(format), pw = padded(width), ph = padded(height);
    // Seen top-down the texture sits bottom-left of the padded canvas; Unity stores rows bottom-up.
    Image fitted = resize_lanczos(im, width, height);
    Image canvas;
    canvas.w = pw, canvas.h = ph;
    canvas.rgba.assign(static_cast<size_t>(pw) * ph * 4, 0);
    for (int y = 0; y < height; ++y)
        std::memcpy(&canvas.rgba[(static_cast<size_t>(ph - height + y) * pw) * 4], &fitted.rgba[static_cast<size_t>(y) * width * 4],
                    static_cast<size_t>(width) * 4);
    Image flipped = flip_vertical(canvas);
    std::vector<uint32_t> px(static_cast<size_t>(pw) * ph);
    std::memcpy(px.data(), flipped.rgba.data(), px.size() * 4);
    uint32_t blocks = static_cast<uint32_t>(pw * ph / 16);
    std::string linear(static_cast<size_t>(blocks) * size, '\0');
    if (format == 25) {
        bc7enc_compress_block_init();
        bc7enc_compress_block_params params;
        std::memset(&params, 0, sizeof(params));
        bc7enc_compress_block_params_init(&params);
        bc7enc_compress_block_params_init_linear_weights(&params);
        CompressBc7(px.data(), reinterpret_cast<uint64_t*>(linear.data()), blocks, static_cast<size_t>(pw), &params);
    } else {
        for (uint32_t& p : px)
            p |= 0xFF000000u; // convert('RGB').convert('RGBA')
        CompressBc1(px.data(), reinterpret_cast<uint64_t*>(linear.data()), blocks, static_cast<size_t>(pw));
    }
    return tile(linear, pw / 4, ph / 4, size);
}

Image decode_texture(std::string_view data, int width, int height, int format) {
    int size = block_bytes(format), pw = padded(width), ph = padded(height);
    std::string linear = untile(data, pw / 4, ph / 4, size);
    linear.resize(static_cast<size_t>(pw / 4) * (ph / 4) * size, '\0');
    Image full;
    full.w = pw, full.h = ph;
    full.rgba.assign(static_cast<size_t>(pw) * ph * 4, 0);
    for (int by = 0; by < ph / 4; ++by)
        for (int bx = 0; bx < pw / 4; ++bx) {
            const void* block = &linear[(static_cast<size_t>(by) * (pw / 4) + bx) * size];
            uint8_t* dst = &full.rgba[(static_cast<size_t>(by) * 4 * pw + bx * 4) * 4];
            if (format == 25)
                bcdec_bc7(block, dst, pw * 4);
            else
                bcdec_bc1(block, dst, pw * 4);
        }
    Image up = flip_vertical(full);
    return crop(up, 0, ph - height, width, ph);
}

} // namespace dlcmaker::image
