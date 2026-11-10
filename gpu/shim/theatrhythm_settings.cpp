// SPDX-License-Identifier: GPL-2.0-or-later
#include "theatrhythm_settings.h"
#include "theatrhythm_graphics.h"
#include "../theatrhythm_gpu.h"

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
        v.next_skip_opening = v.skip_opening;
    } else if (key == "unlock_songs") {
        v.startup_unlock_songs = Truthy(value);
        v.unlock_songs = v.startup_unlock_songs;
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

// The cheats as the [cheats] section holds them.
std::vector<std::pair<std::string, std::string>> CheatKeys() {
    return {{"unlock_songs", Get().unlock_songs ? "1" : "0"}, {"skip_opening", Get().next_skip_opening ? "1" : "0"}};
}

// The name of a "[name]" line; nullptr for any other line.
const char* SectionLine(const std::string& line, std::string& name) {
    const auto end = line.find(']');
    if (line.empty() || line[0] != '[' || end == std::string::npos) return nullptr;
    name = line.substr(1, end - 1);
    return name.c_str();
}

std::mutex save_mutex;
// The display keys as Path() holds them: loaded at start, updated by each save.
std::vector<std::pair<std::string, std::string>> saved_keys;

bool Exists(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (file) std::fclose(file);
    return file != nullptr;
}

// save_mutex held. Writes the display keys and the cheats into Path() in place, from ReadPath()'s
// lines. A key's line is replaced wherever it is; a cheat that [cheats] does not hold is added there
// (its line outside the section, from an older file, stays and says the same).
void WriteDisplay() {
    auto keys = DisplayKeys();
    const auto cheats = CheatKeys();
    std::vector<bool> in_section(cheats.size());
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
    const size_t none = std::string::npos;
    size_t first_section = none, cheats_end = none; // where missing display keys and cheats go
    bool in_cheats = false;
    std::string section;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string& line = lines[i];
        if (SectionLine(line, section)) {
            if (first_section == none) first_section = i;
            in_cheats = section == "cheats";
            if (in_cheats) cheats_end = i + 1;
            continue;
        }
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        if (in_cheats) cheats_end = i + 1;
        const auto named = [&](const auto& k) { return line.substr(0, eq) == k.first; };
        if (const auto cheat = std::find_if(cheats.begin(), cheats.end(), named); cheat != cheats.end()) {
            line = cheat->first + "=" + cheat->second;
            if (in_cheats) in_section[cheat - cheats.begin()] = true;
        } else if (const auto key = std::find_if(keys.begin(), keys.end(), named); key != keys.end()) {
            line = key->first + "=" + key->second;
            keys.erase(key);
        }
    }
    size_t display_at = first_section == none ? lines.size() : first_section;
    if (cheats_end == none) {
        if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
        lines.emplace_back("# Cheats, set on the game's Options > Cheats page. They apply at the next start.");
        lines.emplace_back("[cheats]");
        cheats_end = lines.size();
    }
    for (size_t i = 0; i < cheats.size(); ++i) {
        if (!in_section[i]) lines.insert(lines.begin() + cheats_end++, cheats[i].first + "=" + cheats[i].second);
    }
    for (const auto& [key, value] : keys) lines.insert(lines.begin() + display_at++, key + "=" + value);
    FILE* file = std::fopen(Path(), "wb");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    for (const std::string& line : lines) std::fprintf(file, "%s\n", line.c_str());
    std::fclose(file);
    std::printf("Settings: saved to %s\n", Path());
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
        std::string section;
        std::vector<std::string> from_cheats; // the keys [cheats] set
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (SectionLine(text, section) || text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            // Every key is read wherever it is (a writer that appends to the file puts it after the
            // last section), but what [cheats] sets is not changed by a line outside it: a cheat's
            // line from before the section (skip_opening) still counts in a file without the new one.
            const std::string key = text.substr(0, eq);
            const bool set_by_cheats = std::find(from_cheats.begin(), from_cheats.end(), key) != from_cheats.end();
            if (section == "cheats") {
                if (!set_by_cheats) from_cheats.push_back(key);
            } else if (set_by_cheats) {
                continue;
            }
            Set(v, key, text.substr(eq + 1));
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
        {"THEATRHYTHM_UNLOCK_SONGS", "unlock_songs"},
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

void SaveCheats() {
    std::scoped_lock lock{save_mutex};
    WriteDisplay();
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

extern "C" int theatrhythm_unlock_songs(void) {
    return TheatrhythmSettings::Get().startup_unlock_songs ? 1 : 0;
}

extern "C" int theatrhythm_cheat_get(int cheat) {
    const auto& v = TheatrhythmSettings::Get();
    return (cheat == THEATRHYTHM_CHEAT_UNLOCK_SONGS && v.unlock_songs) ||
           (cheat == THEATRHYTHM_CHEAT_SKIP_OPENING && v.next_skip_opening) ? 1 : 0;
}

extern "C" void theatrhythm_cheat_set(int cheat, int value) {
    auto& v = TheatrhythmSettings::Get();
    if (cheat == THEATRHYTHM_CHEAT_UNLOCK_SONGS) {
        v.unlock_songs = value != 0;
    } else if (cheat == THEATRHYTHM_CHEAT_SKIP_OPENING) {
        v.next_skip_opening = value != 0;
    }
}

extern "C" void theatrhythm_cheats_save(void) {
    TheatrhythmSettings::SaveCheats();
}

extern "C" int theatrhythm_render_4k(void) {
    return TheatrhythmSettings::Get().startup_render_resolution == TheatrhythmSettings::Render4k;
}
