// SPDX-License-Identifier: GPL-2.0-or-later
#include "media.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>

namespace dlcmaker::media {

const std::vector<std::string> AUDIO_EXT = {".mp3", ".ogg", ".opus", ".flac", ".wav", ".m4a", ".aac"};
const std::vector<std::string> VIDEO_EXT = {".mp4", ".mkv", ".webm", ".mov", ".avi"};

std::optional<std::string> try_ffmpeg() {
#ifdef _WIN32
    const char* name = "ffmpeg.exe";
#else
    const char* name = "ffmpeg";
#endif
    std::vector<fs::path> candidates;
    if (auto e = env("THEATRHYTHM_FFMPEG"); e && !e->empty())
        candidates.push_back(to_path(*e));
    candidates.push_back(exe_path().parent_path() / name); // a release's own
    candidates.push_back(which("ffmpeg"));
#ifdef _WIN32
    candidates.push_back(to_path("C:\\msys64\\clang64\\bin\\ffmpeg.exe"));
#endif
    for (const fs::path& c : candidates)
        if (!c.empty() && is_file(c))
            return u8(c);
    return std::nullopt;
}

std::string find_ffmpeg(const std::string& hint) {
    if (!hint.empty() && is_file(to_path(hint)))
        return hint;
    if (auto f = try_ffmpeg())
        return *f;
    throw Exit("ffmpeg not found: install it, put it on PATH, or pass --ffmpeg PATH");
}

void run_checked(const std::vector<std::string>& cmd) {
    int code = run(cmd);
    if (code == -1)
        throw Exit("cannot run " + cmd[0]);
    if (code != 0)
        throw ProcessError(command_text(cmd) + " returned non-zero exit status " + std::to_string(code) + ".");
}

namespace {

// cache/<name> made by make(path) unless the same params already made it.
fs::path cached(const fs::path& cache, const std::string& name, const Value& params, const std::function<void(const fs::path&)>& make) {
    make_dirs(cache);
    fs::path out = cache / to_path(name), stamp = cache / to_path(name + ".json");
    JsonStyle sorted;
    sorted.sort_keys = true;
    std::string key = sha1_hex(json_dump(params, sorted));
    if (is_file(out) && is_file(stamp)) {
        try {
            if (json_parse(read_file(stamp)).get("key") == Value(key))
                return out;
        } catch (const ValueError&) {
        }
    }
    make(out);
    Value s = Value::of_map();
    s["key"] = key;
    s["params"] = params;
    write_file(stamp, json_dump(s));
    return out;
}

Value sig(const fs::path& p) {
    return Value::of_list({Value(u8(resolve(p))), Value(static_cast<int64_t>(dlcmaker::file_size(p))),
                           Value(static_cast<int64_t>(std::floor(mtime(p).seconds())))});
}

std::string probe(const std::string& ffmpeg, const fs::path& src) {
    return capture({ffmpeg, "-hide_banner", "-i", u8(src)}).err;
}

// True when `src` is already what the game plays: H.264 Baseline 1920x1080 30 fps, no audio.
bool game_ready(const std::string& ffmpeg, const fs::path& src) {
    std::string info = probe(ffmpeg, src);
    std::vector<std::string> video;
    size_t a = 0;
    while (a <= info.size()) {
        size_t b = info.find('\n', a);
        if (b == std::string::npos)
            b = info.size();
        std::string line = info.substr(a, b - a);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find("Video:") != std::string::npos)
            video.push_back(line);
        a = b + 1;
    }
    return video.size() == 1 && video[0].find("h264") != std::string::npos && video[0].find("Baseline") != std::string::npos &&
           video[0].find("1920x1080") != std::string::npos && video[0].find(" 30 fps") != std::string::npos &&
           info.find("Audio:") == std::string::npos;
}

} // namespace

fs::path decode_audio(const std::string& ffmpeg, const fs::path& src, const fs::path& cache, double start, const Value& end, double fade_out) {
    Value params = Value::of_map();
    params["src"] = sig(src);
    params["start"] = start;
    params["end"] = end;
    params["fade"] = fade_out;
    return cached(cache, "song.wav", params, [&](const fs::path& out) {
        std::vector<std::string> cmd = {ffmpeg, "-hide_banner", "-v", "error", "-y", "-ss", float_repr(start), "-i", u8(src), "-vn"};
        if (!end.is_null())
            cmd.insert(cmd.end(), {"-t", float_repr(py_float(end) - start)});
        if (fade_out != 0 && !end.is_null())
            cmd.insert(cmd.end(), {"-af", "afade=t=out:st=" + float_repr(py_float(end) - start - fade_out) + ":d=" + float_repr(fade_out)});
        cmd.insert(cmd.end(), {"-ac", "2", "-ar", "48000", "-c:a", "pcm_s16le", u8(out)});
        run_checked(cmd);
    });
}

fs::path encode_movie(const std::string& ffmpeg, const fs::path& src, const fs::path& cache, double start, double length_s) {
    Value params = Value::of_map();
    params["src"] = sig(src);
    params["start"] = start;
    params["length"] = length_s;
    return cached(cache, "movie.mp4", params, [&](const fs::path& out) {
        std::string t = float_repr(length_s + MOVIE_TAIL_S);
        if (start == 0 && game_ready(ffmpeg, src)) {
            run_checked({ffmpeg, "-hide_banner", "-v", "error", "-y", "-i", u8(src), "-t", t, "-c", "copy", u8(out)});
            return;
        }
        run_checked({ffmpeg, "-hide_banner", "-v", "error", "-y", "-ss", float_repr(start), "-i", u8(src), "-t", t, "-an", "-vf",
                     "fps=30,scale=1920:1080:force_original_aspect_ratio=decrease:flags=lanczos,"
                     "pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p",
                     "-c:v", "libx264", "-profile:v", "baseline", "-level", "4.0", "-preset", "slow", "-b:v", "6M", "-maxrate", "8M",
                     "-bufsize", "12M", "-g", "30", u8(out)});
    });
}

std::optional<double> duration(const std::string& ffmpeg, const fs::path& src) {
    std::string info = probe(ffmpeg, src);
    std::smatch m;
    static const std::regex re(R"(Duration: (\d+):(\d+):(\d+(?:\.\d+)?))");
    if (!std::regex_search(info, m, re))
        return std::nullopt;
    return std::stoll(m[1]) * 3600 + std::stoll(m[2]) * 60 + std::strtod(m[3].str().c_str(), nullptr);
}

void frame(const std::string& ffmpeg, const fs::path& src, double at_s, const fs::path& out) {
    run_checked({ffmpeg, "-hide_banner", "-v", "error", "-y", "-ss", float_repr(at_s), "-i", u8(src), "-frames:v", "1", u8(out)});
}

namespace {

struct Wav {
    int rate = 0, channels = 0, width = 0;
    std::string_view data;
};

// The RIFF chunks Python's wave module reads: fmt (PCM, or the extensible form of it) and data.
Wav parse_wav(std::string_view b, const fs::path& path) {
    Reader r(b);
    if (b.size() < 12 || b.substr(0, 4) != "RIFF" || b.substr(8, 4) != "WAVE")
        throw ValueError(u8(path) + ": file does not start with RIFF id");
    r.p = 12;
    Wav w;
    bool fmt = false;
    while (r.p + 8 <= b.size()) {
        std::string_view id = r.bytes(4);
        uint32_t size = r.u32();
        size_t at = r.p, n = std::min<size_t>(size, b.size() - at);
        if (id == "fmt ") {
            Reader f(b.substr(at, n));
            uint16_t tag = f.u16();
            w.channels = f.u16();
            w.rate = static_cast<int>(f.u32());
            f.u32();
            f.u16();
            w.width = (f.u16() + 7) / 8;
            if (tag != 1 && tag != 0xFFFE)
                throw ValueError("unknown format: " + std::to_string(tag));
            fmt = true;
        } else if (id == "data") {
            if (!fmt)
                throw ValueError("data chunk before fmt chunk");
            w.data = b.substr(at, n);
            return w;
        }
        r.p = at + n + (size & 1);
    }
    throw ValueError(u8(path) + ": fmt chunk and/or data chunk missing");
}

} // namespace

std::string read_wav(const fs::path& path) {
    std::string b = read_file(path);
    Wav w = parse_wav(b, path);
    if (w.rate != 48000 || w.channels != 2 || w.width != 2)
        throw ValueError(u8(path) + ": need 48 kHz stereo 16-bit");
    return std::string(w.data.substr(0, w.data.size() / 4 * 4));
}

std::optional<double> wav_seconds(const fs::path& path) {
    if (!is_file(path))
        return std::nullopt;
    try {
        std::string b = read_file(path);
        Wav w = parse_wav(b, path);
        size_t frame = static_cast<size_t>(w.channels * w.width);
        if (!frame || !w.rate)
            return std::nullopt;
        return static_cast<double>(w.data.size() / frame) / w.rate;
    } catch (const ValueError&) {
        return std::nullopt;
    }
}

double seconds(const std::string& frames) {
    return static_cast<double>(frames.size()) / 4 / 48000;
}

std::string cut(const std::string& frames, double start_s, double end_s, double fade_s) {
    auto at = [&](double s) {
        int64_t v = static_cast<int64_t>(std::trunc(s * 48000)) * 4;
        int64_t n = static_cast<int64_t>(frames.size());
        return static_cast<size_t>(std::clamp<int64_t>(v < 0 ? n + v : v, 0, n));
    };
    size_t a = at(start_s), b = at(end_s);
    std::string part = b > a ? frames.substr(a, (b - a) / 2 * 2) : std::string();
    std::vector<int16_t> s(part.size() / 2);
    std::memcpy(s.data(), part.data(), s.size() * 2);
    int64_t n = static_cast<int64_t>(s.size()) / 2;
    int64_t f = std::min<int64_t>(static_cast<int64_t>(std::trunc(fade_s * 48000)), n / 2);
    for (int64_t i = 0; i < f; ++i) {
        double g = static_cast<double>(i) / static_cast<double>(f);
        for (int64_t j : {2 * i, 2 * i + 1, 2 * (n - 1 - i), 2 * (n - 1 - i) + 1})
            s[static_cast<size_t>(j)] = static_cast<int16_t>(std::trunc(s[static_cast<size_t>(j)] * g));
    }
    std::string out(s.size() * 2, '\0');
    std::memcpy(out.data(), s.data(), out.size());
    return out;
}

std::string fsb5_pcm16(const std::string& frames, int rate, int channels) {
    static const std::pair<int, int> kFreq[] = {{8000, 1}, {11000, 2}, {11025, 3}, {16000, 4}, {22050, 5},
                                                {24000, 6}, {32000, 7}, {44100, 8}, {48000, 9}, {96000, 10}};
    uint64_t freq = 0;
    for (auto [r, code] : kFreq)
        if (r == rate)
            freq = static_cast<uint64_t>(code);
    std::string data = frames + std::string((32 - frames.size() % 32) % 32, '\0');
    uint64_t count = frames.size() / (2 * static_cast<size_t>(channels));
    uint64_t sample = freq << 1 | (channels == 2 ? 1ull : 0ull) << 5 | count << 34;
    Writer w;
    w.bytes("FSB5");
    for (uint32_t v : {1u, 1u, 8u, 0u, static_cast<uint32_t>(data.size()), 2u})
        w.u32(v);
    w.u64(1);
    w.bytes(std::string(24, '\0'));
    w.u64(sample);
    w.bytes(data);
    return std::move(w.d);
}

} // namespace dlcmaker::media
