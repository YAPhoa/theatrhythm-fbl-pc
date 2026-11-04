// SPDX-License-Identifier: GPL-2.0-or-later
// Display options of settings.ini (theatrhythm_settings.h) and how their values are parsed.
#pragma once

#include <array>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace TheatrhythmGraphics {

enum DisplayMode : int { Windowed = 0, Borderless = 1, Fullscreen = 2, DisplayModeCount };
inline constexpr std::array<std::string_view, DisplayModeCount> DisplayModeKeys = {
    "windowed", "borderless", "fullscreen"};
inline constexpr std::array<const char*, DisplayModeCount> DisplayModeNames = {
    "Windowed", "Borderless", "Fullscreen"};

/// Present rate limits; FpsUnlimited = none (as fast as the game makes frames), 0 = paced by the vblank.
inline constexpr int FpsUnlimited = -1;
inline constexpr std::array<int, 9> FpsLimits = {30, 60, 100, 120, 144, 180, 240, FpsUnlimited, 0};

template <typename T, std::size_t N>
constexpr int IndexOf(const std::array<T, N>& values, const T& value, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (values[i] == value) return int(i);
    }
    return fallback;
}

/// A listed mode, else borderless (the default).
inline int ParseDisplayMode(std::string_view text) {
    return IndexOf(DisplayModeKeys, text, Borderless);
}
/// The primary display, the default. -1 keeps it distinct from an explicit index 0, which names
/// the first display SDL3 lists even when it is not the primary.
inline constexpr int PrimaryMonitor = -1;
/// The display the window opens on: an index into SDL_GetDisplays(), or PrimaryMonitor. Empty,
/// negative, non-numeric or too large for an int is PrimaryMonitor.
inline int ParseMonitor(std::string_view text) {
    if (text.empty()) return PrimaryMonitor;
    int index = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') return PrimaryMonitor;
        if (index > (std::numeric_limits<int>::max() - (digit - '0')) / 10) return PrimaryMonitor;
        index = index * 10 + (digit - '0');
    }
    return index;
}
/// A listed limit, else 0.
inline int ParseFpsLimit(int value) {
    return IndexOf(FpsLimits, value, -1) >= 0 ? value : 0;
}
/// The window size "<width>x<height>"; anything else is the game's 1920x1080.
inline std::pair<int, int> ParseWindowSize(std::string_view text) {
    const std::size_t x = text.find('x');
    if (x != std::string_view::npos) {
        const int w = std::atoi(std::string(text.substr(0, x)).c_str());
        const int h = std::atoi(std::string(text.substr(x + 1)).c_str());
        if (w >= 320 && h >= 180) return {w, h};
    }
    return {1920, 1080};
}

/// Anisotropic filtering forced on the game's mipmapped textures; 0 = the game's own. Samplers
/// are cached, so it applies on restart.
inline constexpr std::array<int, 5> Anisotropies = {0, 2, 4, 8, 16};
inline int ParseAnisotropy(int value) {
    return IndexOf(Anisotropies, value, -1) >= 0 ? value : 0;
}

/// Frames the GPU command thread may queue ahead of the GPU; 0 = unbounded.
inline constexpr std::array<int, 3> FramesQueued = {1, 2, 0};
inline int ParseFramesQueued(int value) {
    return IndexOf(FramesQueued, value, -1) >= 0 ? value : 1;
}

} // namespace TheatrhythmGraphics
