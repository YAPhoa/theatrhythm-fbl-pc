// SPDX-License-Identifier: GPL-2.0-or-later
#include "theatrhythm_settings.h"
#include "theatrhythm_graphics.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>

namespace TheatrhythmSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("THEATRHYTHM_CONFIG");
    return env && env[0] ? env : "settings.ini";
}

// The file read: Path() when it exists, else THEATRHYTHM_CONFIG_DEFAULT (the launcher's: the one the
// setup wrote next to the exe, while Path() is the save folder's). Writes always go to Path().
const char* ReadPath() {
    if (FILE* file = std::fopen(Path(), "r")) {
        std::fclose(file);
        return Path();
    }
    const char* fallback = std::getenv("THEATRHYTHM_CONFIG_DEFAULT");
    return fallback && fallback[0] ? fallback : Path();
}

bool Truthy(std::string_view value) {
    std::string lower;
    for (const char c : value) lower += char(std::tolower(static_cast<unsigned char>(c)));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const int i = std::atoi(value.c_str());
    if (key == "display_mode") {
        v.display_mode = TheatrhythmGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = Truthy(value);
    } else if (key == "tearing") {
        v.tearing = Truthy(value);
    } else if (key == "monitor") {
        v.monitor = TheatrhythmGraphics::ParseMonitor(value);
    } else if (key == "window") {
        const auto [w, h] = TheatrhythmGraphics::ParseWindowSize(value);
        v.window_width = w;
        v.window_height = h;
    } else if (key == "fps_limit") {
        v.fps_limit = value == "unlimited" ? TheatrhythmGraphics::FpsUnlimited : TheatrhythmGraphics::ParseFpsLimit(i);
    } else if (key == "async_shaders") {
        v.async_shaders = Truthy(value);
    } else if (key == "async_shaders_skip_frames") {
        v.async_shaders_skip_frames = value.empty() ? 1 : std::clamp(i, -1, 60);
    } else if (key == "frames_queued") {
        v.frames_queued = TheatrhythmGraphics::ParseFramesQueued(value.empty() ? -1 : i);
    } else if (key == "anisotropy") {
        v.startup_anisotropy = v.anisotropy = TheatrhythmGraphics::ParseAnisotropy(i);
    } else if (key == "vibration") {
        v.vibration = value.empty() ? 100 : std::clamp(i, 0, 100);
    } else if (key == "pad_map") {
        v.pad_map = value;
    } else if (key == "key_map") {
        v.key_map = value;
    } else if (key == "menu_confirm_swap") {
        v.menu_confirm_swap = Truthy(value) || value == "circle" ? 1 : 0;
    } else if (key == "skip_opening") {
        v.skip_opening = Truthy(value);
    } else if (key == "master_volume") {
        v.master_volume = value.empty() ? MasterVolumeDefault : std::clamp(i, MasterVolumeMin, MasterVolumeMax);
    } else if (key == "audio_channels") {
        v.audio_channels = ParseAudioChannels(value);
    } else if (key == "button_icons") {
        const auto* found = std::find(std::begin(ButtonIconKeys), std::end(ButtonIconKeys), value);
        v.button_icons = found == std::end(ButtonIconKeys) ? ButtonIconsAuto : int(found - std::begin(ButtonIconKeys));
        if (found == std::end(ButtonIconKeys) && !value.empty()) {
            std::printf("Settings: button_icons=%s is not auto, ps4, xbox or keyboard; using auto\n", value.c_str());
        }
    } else if (key == "render_resolution") {
        std::string lower;
        for (const char c : value) lower += char(std::tolower(static_cast<unsigned char>(c)));
        v.startup_render_resolution = v.render_resolution = lower == "1080p" ? Render1080p : Render4k;
        if (!lower.empty() && lower != "1080p" && lower != "4k" && lower != "2160p") {
            std::printf("Settings: render_resolution=%s is not 1080p or 4k; using 4k\n", value.c_str());
        }
    } else {
        std::printf("Settings: unknown key %s\n", key.c_str());
    }
}

std::vector<std::pair<std::string, std::string>> DisplayKeys() {
    const auto& v = Get();
    const int monitor = v.monitor;
    return {
        {"display_mode", std::string(TheatrhythmGraphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)])},
        {"monitor", monitor < 0 ? std::string() : std::to_string(monitor)},
        {"window", std::to_string(v.window_width.load()) + "x" + std::to_string(v.window_height.load())},
        {"vsync", v.vsync ? "1" : "0"},
        {"tearing", v.tearing ? "1" : "0"},
        {"fps_limit", v.fps_limit == TheatrhythmGraphics::FpsUnlimited ? "unlimited" : std::to_string(v.fps_limit.load())},
        {"frames_queued", std::to_string(v.frames_queued.load())},
        {"anisotropy", std::to_string(v.anisotropy.load())},
        {"render_resolution", std::string(RenderResolutionKeys[std::clamp(v.render_resolution.load(), 0, RenderResolutionCount - 1)])},
    };
}

std::mutex save_mutex;
// The display keys as Path() holds them: loaded at start, updated by each save.
std::vector<std::pair<std::string, std::string>> saved_keys;

bool Exists(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (file) std::fclose(file);
    return file != nullptr;
}

// save_mutex held. Writes the display keys into Path() in place, from ReadPath()'s lines.
void WriteDisplay() {
    auto keys = DisplayKeys();
    saved_keys = keys;
    std::vector<std::string> lines;
    if (FILE* file = std::fopen(ReadPath(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            lines.push_back(text);
        }
        std::fclose(file);
    }
    for (std::string& line : lines) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const auto key = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return line.substr(0, eq) == k.first; });
        if (key == keys.end()) continue;
        line = key->first + "=" + key->second;
        keys.erase(key);
    }
    for (const auto& [key, value] : keys) lines.push_back(key + "=" + value);
    FILE* file = std::fopen(Path(), "wb");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    for (const std::string& line : lines) std::fprintf(file, "%s\n", line.c_str());
    std::fclose(file);
    std::printf("Settings: saved display settings to %s\n", Path());
}

// Gives the save folder its own settings.ini at start: the default file copied as it is (the
// environment overrides stay out of it), or the display keys when there is none.
void CopyToSaveFolder() {
    const char* env = std::getenv("THEATRHYTHM_CONFIG"); /* the launcher's or run.py's save folder */
    if (!env || !env[0] || Exists(Path())) return;
    const char* source = ReadPath();
    if (std::string_view(source) == Path()) {
        WriteDisplay();
        return;
    }
    FILE* in = std::fopen(source, "rb");
    if (!in) return;
    std::string data;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), in)) > 0;) data.append(buffer, n);
    std::fclose(in);
    FILE* out = std::fopen(Path(), "wb");
    if (!out) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), out) == data.size();
    if (std::fclose(out) != 0 || !ok) {
        std::printf("Settings: cannot write %s\n", Path());
        std::remove(Path());
        return;
    }
    std::printf("Settings: copied %s to %s\n", source, Path());
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    const char* path = ReadPath();
    if (FILE* file = std::fopen(path, "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", path);
        if (std::string_view(path) != Path()) std::printf("Settings: changes are saved to %s\n", Path());
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"THEATRHYTHM_DISPLAY_MODE", "display_mode"}, {"THEATRHYTHM_VSYNC", "vsync"},
        {"THEATRHYTHM_TEARING", "tearing"},
        {"THEATRHYTHM_MONITOR", "monitor"},           {"THEATRHYTHM_WINDOW", "window"},
        {"THEATRHYTHM_ASYNC_SHADERS", "async_shaders"}, {"THEATRHYTHM_FRAMES_AHEAD", "frames_queued"},
        {"THEATRHYTHM_ASYNC_SHADERS_SKIP_FRAMES", "async_shaders_skip_frames"},
        {"THEATRHYTHM_ANISOTROPY", "anisotropy"},     {"THEATRHYTHM_VIBRATION", "vibration"},
        {"THEATRHYTHM_AUDIO_VOLUME", "master_volume"}, {"THEATRHYTHM_AUDIO_CHANNELS", "audio_channels"},
        {"THEATRHYTHM_MENU_CONFIRM_SWAP", "menu_confirm_swap"}, {"THEATRHYTHM_SKIP_OPENING", "skip_opening"},
        {"THEATRHYTHM_PAD_MAP", "pad_map"},       {"THEATRHYTHM_KEY_MAP", "key_map"},
        {"THEATRHYTHM_BUTTON_ICONS", "button_icons"}, {"THEATRHYTHM_RENDER_RESOLUTION", "render_resolution"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.startup_monitor = v.monitor;
    std::scoped_lock lock{save_mutex};
    saved_keys = DisplayKeys();
    CopyToSaveFolder();
}

void SaveDisplay() {
    std::scoped_lock lock{save_mutex};
    WriteDisplay();
}

void SaveDisplayIfChanged() {
    std::scoped_lock lock{save_mutex};
    if (DisplayKeys() != saved_keys) WriteDisplay();
}

} // namespace TheatrhythmSettings

extern "C" int theatrhythm_gpu_menu_confirm_swap(void) {
    return TheatrhythmSettings::Get().menu_confirm_swap.load(std::memory_order_relaxed);
}

extern "C" const char* theatrhythm_button_icons(void) {
    return TheatrhythmSettings::ButtonIconKeys[TheatrhythmSettings::Get().button_icons].data();
}

extern "C" int theatrhythm_skip_opening(void) {
    return TheatrhythmSettings::Get().skip_opening ? 1 : 0;
}

extern "C" int theatrhythm_render_4k(void) {
    return TheatrhythmSettings::Get().startup_render_resolution == TheatrhythmSettings::Render4k;
}
