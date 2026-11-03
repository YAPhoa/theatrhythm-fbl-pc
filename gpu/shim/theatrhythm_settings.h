// SPDX-License-Identifier: GPL-2.0-or-later
// User settings, read once at start from settings.ini (THEATRHYTHM_CONFIG overrides the path) and then
// from environment variables, which win. One "key=value" per line, '#' starts a comment.
#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "theatrhythm_graphics.h"

namespace TheatrhythmSettings {

/// Speaker layout the game's 7.1 audio is mixed down to (src/runtime_audio.c): the playback
/// device's own, stereo, 5.1 or 7.1. settings.ini and THEATRHYTHM_AUDIO_CHANNELS name them as below.
enum AudioChannels : int { AudioChannelsAuto = 0, AudioStereo, Audio51, Audio71, AudioChannelsCount };
inline constexpr std::string_view AudioChannelKeys[AudioChannelsCount] = {"auto", "stereo", "5.1", "7.1"};
/// A layout key (also "2.0", "6" and "8"); anything else is automatic.
inline int ParseAudioChannels(std::string_view value) {
    for (int c = 0; c < AudioChannelsCount; ++c) {
        if (value == AudioChannelKeys[c]) return c;
    }
    if (value == "2.0" || value == "2") return AudioStereo;
    if (value == "6") return Audio51;
    if (value == "8") return Audio71;
    return AudioChannelsAuto;
}
inline constexpr int MasterVolumeMin = 0, MasterVolumeMax = 200, MasterVolumeDefault = 100;
/// Button icon set the game's prompts show (key button_icons): the device last used (auto), the
/// game's own PS4 icons, or a sheet from assets/button_icons (src/runtime_button_icons.c).
enum ButtonIcons : int { ButtonIconsAuto = 0, ButtonIconsPs4, ButtonIconsXbox, ButtonIconsKeyboard, ButtonIconsCount };
inline constexpr std::string_view ButtonIconKeys[ButtonIconsCount] = {"auto", "ps4", "xbox", "keyboard"};
/// Size the game renders at (key render_resolution): the PS4's 1080p, or the PS4 Pro's 2160p
/// (PS4 Pro mode and a 3840x2160 video out, probe.c mount_boot_config_4k).
enum RenderResolution : int { Render1080p = 0, Render4k, RenderResolutionCount };
inline constexpr std::string_view RenderResolutionKeys[RenderResolutionCount] = {"1080p", "4k"};

struct Values {
    /// TheatrhythmGraphics::DisplayMode (key display_mode: windowed, borderless, fullscreen);
    /// borderless by default, so the picture is scaled to the desktop resolution.
    std::atomic<int> display_mode{TheatrhythmGraphics::Borderless};
    std::atomic<bool> vsync{true};
    /// With VSync off: present at once, tearing allowed (Vulkan Immediate), instead of the newest
    /// frame at each refresh (Mailbox, which some drivers hold to twice the refresh rate).
    std::atomic<bool> tearing{false};
    /// SDL display index the window opens on, -1 the primary (key monitor, THEATRHYTHM_MONITOR).
    std::atomic<int> monitor{-1};
    int startup_monitor = -1;
    std::vector<int> monitors; ///< SDL indices, populated before the guest starts
    std::vector<std::string> monitor_names; ///< SDL's name of each of `monitors`
    /// Resolution (key window, "1920x1080"): the window's size when windowed, the screen mode in
    /// exclusive fullscreen; borderless uses the desktop's. The window follows changes.
    std::atomic<int> window_width{1920}, window_height{1080};
    /// Sizes the start display offers (largest first), listed before the guest starts.
    std::vector<std::pair<int, int>> resolutions;
    /// Present rate limit, 0 paced by the vblank, FpsUnlimited none (TheatrhythmGraphics::FpsLimits).
    std::atomic<int> fps_limit{0};
    std::atomic<bool> async_shaders{true};
    /// GPU frames a draw may be skipped while its shader or pipeline compiles asynchronously; then
    /// the GPU thread waits for it. 0 always waits; -1 is the earlier behaviour: skipped until the
    /// compile finishes, one permutation per program at a time, failed builds not retried
    /// (key async_shaders_skip_frames).
    std::atomic<int> async_shaders_skip_frames{1};
    std::atomic<int> frames_queued{1};
    /// TheatrhythmGraphics::Anisotropies; the samplers read it once.
    int startup_anisotropy = 0;
    /// What the Display Settings panel saves for the next start.
    std::atomic<int> anisotropy{0};
    /// Controller vibration strength in percent; 0 turns it off (runtime_pad.c).
    std::atomic<int> vibration{100};
    /// Button remapping for the gamepad and the keyboard/mouse (runtime_pad.c apply_button_map);
    /// set before the guest starts, read once at its first pad sample.
    std::string pad_map, key_map;
    /// Dialog confirmation button: 0 Cross confirms, Circle cancels; 1 the other way round.
    std::atomic<int> menu_confirm_swap{0};
    /// Master volume of the game's audio in percent, 0 to 200: above 100 boosts the mix behind a
    /// soft limiter (src/runtime_audio.c).
    std::atomic<int> master_volume{MasterVolumeDefault};
    /// Speaker layout of the game's audio (AudioChannels), read when the game opens its ports.
    std::atomic<int> audio_channels{AudioChannelsAuto};
    /// ButtonIcons; read once, before the game loads its UI.
    int button_icons = ButtonIconsAuto;
    /// RenderResolution, 4k by default; read once, before the game starts.
    int startup_render_resolution = Render4k;
    /// What the Display Settings pages save for the next start.
    std::atomic<int> render_resolution{Render4k};
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();

/// Writes the display keys (display_mode, monitor, window, vsync, tearing, fps_limit, frames_queued,
/// anisotropy, render_resolution) into Path() in place: their lines are replaced, missing ones
/// appended, the rest kept (from the default file until Path() exists).
void SaveDisplay();

} // namespace TheatrhythmSettings
