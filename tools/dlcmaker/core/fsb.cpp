// SPDX-License-Identifier: GPL-2.0-or-later
// The game's FSB5 Vorbis audio (FMOD) as an ordinary Ogg Vorbis file.
//
// FSB5 keeps only the Vorbis audio packets and a CRC32 of the stream's setup header; FMOD fills in the
// headers from a table it carries. The game links FMOD into eboot.bin, so the setup header is found
// there: the '\x05vorbis' packet whose CRC32 matches. The identification and comment headers are
// rebuilt, and the packets are paged into Ogg with libogg and libvorbis (linked in), which ffmpeg then
// reads like any other audio file.
#include "fsb.h"

#include <map>

#include <ogg/ogg.h>
#include <vorbis/codec.h>

namespace dlcmaker::fsb {

namespace {

constexpr uint32_t kModeVorbis = 15;
constexpr uint8_t kBlocksizes = 0xB8; // log2 sizes 256 (short) and 2048 (long), what FMOD encodes with

struct Info {
    uint32_t mode = 0, rate = 0, channels = 0;
    uint64_t samples = 0;
    bool has_crc = false;
    uint32_t crc = 0;
    std::string_view data;
};

Info parse(std::string_view fsb) {
    Reader r(fsb);
    if (fsb.substr(0, 4) != "FSB5")
        throw ValueError("not an FSB5");
    r.p = 4;
    uint32_t ver = r.u32();
    r.u32();
    uint32_t shsize = r.u32(), ntsize = r.u32(), dsize = r.u32();
    Info i;
    i.mode = r.u32();
    size_t hdr = ver == 1 ? 0x3C : 0x40;
    r.p = hdr;
    uint64_t raw = r.u64();
    std::map<uint32_t, std::string_view> chunks;
    for (uint64_t extra = raw & 1; extra;) {
        uint32_t c = r.u32();
        extra = c & 1;
        uint32_t size = c >> 1 & 0xFFFFFF, type = c >> 25 & 0x7F;
        chunks[type] = r.bytes(size);
    }
    static const uint32_t kFreq[] = {0, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    uint32_t code = static_cast<uint32_t>(raw >> 1 & 0xF);
    if (code == 0 || code > 10)
        throw ValueError("FSB5 frequency code " + std::to_string(code));
    i.rate = kFreq[code];
    if (chunks.count(2))
        i.rate = Reader(chunks[2]).u32(); // explicit frequency chunk
    i.channels = static_cast<uint32_t>(raw >> 5 & 3) + 1;
    i.samples = raw >> 34;
    size_t start = hdr + shsize + ntsize;
    if (start > fsb.size())
        throw ValueError("FSB5 data past the end");
    i.data = fsb.substr(start, dsize);
    if (chunks.count(11)) {
        i.has_crc = true;
        i.crc = Reader(chunks[11]).u32();
    }
    return i;
}

// The Vorbis setup packet in FMOD's table (eboot.bin) whose CRC32 is `crc`.
std::string setup_header(const fs::path& eboot, uint32_t crc) {
    static std::map<uint32_t, std::string> found;
    if (found.count(crc))
        return found[crc];
    std::string blob = read_file(eboot);
    const std::string magic("\x05vorbis", 7);
    for (size_t start = blob.find(magic); start != std::string::npos; start = blob.find(magic, start + 1)) {
        uint32_t c = crc32(std::string_view(blob).substr(start, 7));
        size_t limit = std::min(blob.size(), start + 64 * 1024);
        for (size_t end = start + 7; end < limit; ++end) {
            if (end > start + 7)
                c = crc32(std::string_view(blob).substr(end - 1, 1), c);
            if (c == crc)
                return found[crc] = blob.substr(start, end - start);
        }
    }
    char hexcrc[16];
    std::snprintf(hexcrc, sizeof(hexcrc), "%08x", crc);
    throw ValueError(std::string("no Vorbis setup header with CRC ") + hexcrc + " in " + u8(eboot));
}

} // namespace

double to_ogg(std::string_view data, const fs::path& eboot, const fs::path& out) {
    Info info = parse(data);
    if (info.mode != kModeVorbis)
        throw ValueError("FSB5 mode " + std::to_string(info.mode) + " is not Vorbis");
    if (!info.has_crc)
        throw ValueError("FSB5 has no Vorbis setup CRC");
    Writer id;
    id.bytes(std::string("\x01vorbis", 7));
    id.u32(0);
    id.u8(static_cast<uint8_t>(info.channels));
    id.u32(info.rate);
    id.i32(0);
    id.i32(0);
    id.i32(0);
    id.u8(kBlocksizes);
    id.u8(1);
    const std::string vendor = "theatrhythm dlcmaker";
    Writer comment;
    comment.bytes(std::string("\x03vorbis", 7));
    comment.u32(static_cast<uint32_t>(vendor.size()));
    comment.bytes(vendor);
    comment.u32(0);
    comment.u8(1);
    std::string setup = setup_header(eboot, info.crc);

    vorbis_info vi;
    vorbis_comment vc;
    ogg_stream_state stream;
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
    ogg_stream_init(&stream, 0x7468);
    std::string file;
    auto pages = [&](bool flush) {
        ogg_page page;
        while (flush ? ogg_stream_flush(&stream, &page) : ogg_stream_pageout(&stream, &page)) {
            file.append(reinterpret_cast<const char*>(page.header), static_cast<size_t>(page.header_len));
            file.append(reinterpret_cast<const char*>(page.body), static_cast<size_t>(page.body_len));
        }
    };
    auto packet = [](std::string& bytes, int64_t no, int64_t granule = 0, bool bos = false, bool eos = false) {
        ogg_packet p = {};
        p.packet = reinterpret_cast<unsigned char*>(bytes.data());
        p.bytes = static_cast<long>(bytes.size());
        p.b_o_s = bos;
        p.e_o_s = eos;
        p.granulepos = granule;
        p.packetno = no;
        return p;
    };
    std::string headers[3] = {id.d, comment.d, setup};
    try {
        for (int no = 0; no < 3; ++no) {
            ogg_packet p = packet(headers[no], no, 0, no == 0);
            if (vorbis_synthesis_headerin(&vi, &vc, &p) != 0)
                throw ValueError("Vorbis header " + std::to_string(no) + " rejected");
            ogg_stream_packetin(&stream, &p);
        }
        pages(true);
        std::vector<std::pair<size_t, size_t>> sizes;
        for (size_t pos = 0; pos + 2 <= info.data.size();) {
            size_t n = static_cast<unsigned char>(info.data[pos]) | static_cast<size_t>(static_cast<unsigned char>(info.data[pos + 1])) << 8;
            pos += 2;
            if (n == 0)
                break;
            sizes.emplace_back(pos, n);
            pos += n;
        }
        int64_t granule = 0, prev = 0, no = 3;
        for (size_t i = 0; i < sizes.size(); ++i) {
            std::string bytes(info.data.substr(sizes[i].first, sizes[i].second));
            ogg_packet p = packet(bytes, no);
            long bs = vorbis_packet_blocksize(&vi, &p);
            if (bs < 0)
                throw ValueError("bad Vorbis packet " + std::to_string(i));
            granule += prev ? (prev + bs) / 4 : 0;
            prev = bs;
            bool last = i + 1 == sizes.size();
            p.granulepos = last ? static_cast<int64_t>(info.samples) : granule;
            p.e_o_s = last;
            ogg_stream_packetin(&stream, &p);
            ++no;
            pages(false);
        }
        pages(true);
    } catch (...) {
        ogg_stream_clear(&stream);
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
        throw;
    }
    ogg_stream_clear(&stream);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    write_file(out, file);
    return static_cast<double>(info.samples) / info.rate;
}

} // namespace dlcmaker::fsb
