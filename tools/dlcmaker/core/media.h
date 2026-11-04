// SPDX-License-Identifier: GPL-2.0-or-later
// Audio and video through ffmpeg; FSB5 PCM16 for the game's AudioClips.
//
// Every input format ffmpeg reads works (mp3, ogg, flac, wav, m4a; the audio track of a video).
// The decoded 48 kHz WAV is the timing reference: charts are in ms from its first sample.
// Results are cached in the build folder, keyed by the inputs and settings (the keys earlier (Python)
// releases used, so their caches stay valid).
#pragma once

#include "util.h"
#include "value.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dlcmaker::media {

extern const std::vector<std::string> AUDIO_EXT, VIDEO_EXT;
constexpr double MOVIE_TAIL_S = 3.0; // the game's event videos run 3 s past their music

// ffmpeg failed (subprocess.CalledProcessError).
struct ProcessError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string find_ffmpeg(const std::string& hint = ""); // Exit when there is none
std::optional<std::string> try_ffmpeg();

fs::path decode_audio(const std::string& ffmpeg, const fs::path& src, const fs::path& cache, double start, const Value& end, double fade_out);
fs::path encode_movie(const std::string& ffmpeg, const fs::path& src, const fs::path& cache, double start, double length_s);
std::optional<double> duration(const std::string& ffmpeg, const fs::path& src); // from the header
void frame(const std::string& ffmpeg, const fs::path& src, double at_s, const fs::path& out);
void run_checked(const std::vector<std::string>& cmd);

std::string read_wav(const fs::path& path);                // 48 kHz stereo 16-bit frames (ValueError otherwise)
std::optional<double> wav_seconds(const fs::path& path);   // wave.open(...).getnframes() / framerate
double seconds(const std::string& frames);
std::string cut(const std::string& frames, double start_s, double end_s, double fade_s = 0.5);
std::string fsb5_pcm16(const std::string& frames, int rate = 48000, int channels = 2);

} // namespace dlcmaker::media
