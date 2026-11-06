// SPDX-License-Identifier: GPL-2.0-or-later
// The system UI the PS4 draws over a game (save list, text entry, trophy and screenshot notices) and
// the shader-cache notes, drawn with Dear ImGui by the presenter after the game frame.
#include "theatrhythm_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "theatrhythm_graphics.h"
#include "theatrhythm_settings.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "../theatrhythm_gpu.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans, embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global theatrhythm_font_ttf\n"
    "theatrhythm_font_ttf:\n"
    ".incbin \"" THEATRHYTHM_FONT_PATH "\"\n"
    ".global theatrhythm_font_ttf_end\n"
    "theatrhythm_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden theatrhythm_font_ttf\n"
    ".global theatrhythm_font_ttf\n"
    "theatrhythm_font_ttf:\n"
    ".incbin \"" THEATRHYTHM_FONT_PATH "\"\n"
    ".hidden theatrhythm_font_ttf_end\n"
    ".global theatrhythm_font_ttf_end\n"
    "theatrhythm_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char theatrhythm_font_ttf[];
extern "C" const unsigned char theatrhythm_font_ttf_end[];

namespace TheatrhythmOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
float base_scale = 1.0f;

namespace Ink {
constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
constexpr ImVec4 Bg = Rgb(0x10, 0x12, 0x16), Panel = Rgb(0x18, 0x1b, 0x21), Card = Rgb(0x1f, 0x23, 0x2b);
constexpr ImVec4 Line = Rgb(0x2e, 0x34, 0x3f), Text = Rgb(0xe8, 0xea, 0xee), Muted = Rgb(0x8c, 0x93, 0xa0);
constexpr ImVec4 Accent = Rgb(0x3d, 0x7c, 0xd9), AccentDeep = Rgb(0x1c, 0x33, 0x55);
} // namespace Ink

std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
std::atomic<u32> prefetch_done{0}, prefetch_total{0};
std::atomic<s64> prefetch_until{0}; // "ready" stays this long after the last program
// Shader translations: in progress, and started since the note last appeared.
std::atomic<u32> compile_pending{0}, compile_count{0}, compile_waits{0};
std::atomic<s64> compile_until{0};
// A short message in the lower right corner (trophy, screenshot), shown until notice_until.
std::mutex notice_mutex;
std::string notice_text;
std::atomic<s64> notice_until{0}; // steady_clock milliseconds
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::string choice_preview_label;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<TheatrhythmPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
// The start-up picture (SetSplash): decoded at start, made a texture on the present thread (both
// guarded by imgui_mutex), retired through choice_retired once faded out.
enum SplashState : int { SplashNone, SplashShown, SplashFading, SplashDone };
constexpr s64 kSplashFadeMs = 400, kSplashMaxMs = 60000;
TheatrhythmPng::Image splash_picture;
ImTextureData* splash_texture = nullptr;
std::atomic<int> splash_state{SplashNone}, splash_lit_frames{0};
std::atomic<s64> splash_since{0}, splash_fade_from{0};
std::chrono::steady_clock::time_point last_present{};
// The Display Settings panel (F10, or the Options menu's Display Settings button). The row and
// dirty flag are guarded by imgui_mutex; the values live in TheatrhythmSettings.
std::atomic<bool> display_open{false};
std::atomic<bool> exit_requested{false}; // F4: the game shows "Exit the game?" (runtime_exit_prompt.c)
int display_row = 0;
bool display_dirty = false;
enum DisplayRow : int { RowMode, RowResolution, RowMonitor, RowVsync, RowFps, RowQueued, RowAnisotropy, RowRender, RowCount };

// The colours of the game's own settings pages (Settings > MS Settings).
namespace PageInk {
constexpr ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
constexpr ImU32 Page = Rgb(0x0b, 0x2a, 0x82), PageLow = Rgb(0x09, 0x21, 0x6c);
constexpr ImU32 Header = Rgb(0x17, 0x7c, 0xdc), HeaderLow = Rgb(0x14, 0x5e, 0xc6), HeaderRule = Rgb(0xa8, 0xc8, 0xf4);
constexpr ImU32 Row = Rgb(0x1b, 0x3c, 0x9e), RowRim = Rgb(0x36, 0x68, 0xd8), Focus = Rgb(0x1a, 0xb4, 0xd8);
constexpr ImU32 FocusSpeck = Rgb(0x5e, 0xd4, 0xee, 150), Pill = Rgb(0x0a, 0x1f, 0x62), Chevron = Rgb(0x4a, 0x7a, 0xe6);
constexpr ImU32 Text = Rgb(0xff, 0xff, 0xff), Outline = Rgb(0x0a, 0x18, 0x48), Help = Rgb(0xc8, 0xd8, 0xf8);
constexpr ImU32 Hand = Rgb(0xff, 0xff, 0xff), HandLine = Rgb(0x2a, 0x2a, 0x3a), Yellow = Rgb(0xff, 0xd8, 0x4a);
} // namespace PageInk

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool Swapped() {
    return TheatrhythmSettings::Get().menu_confirm_swap.load(std::memory_order_relaxed) != 0;
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(Ink::Accent, "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled(Swapped() ? "Controller: Circle (B) = OK, Cross (A) = cancel"
                                  : "Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

// One save (or one answer) of the game's save list as a card: its picture on the left when the list
// has pictures, then its lines: the first is the name, the rest in grey. `draw` false only
// measures. Returns the card's height.
float SaveCard(ImVec2 at, float width, const std::string& text, ImTextureData* picture, bool pictures, bool selected,
               bool draw) {
    const float px = base_scale;
    const float pad = 12.0f * px, gap = 16.0f * px;
    const ImVec2 picture_size(192.0f * px, 108.0f * px);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize(), name_size = size * 1.15f;
    const float text_x = pad + (pictures ? picture_size.x + gap : 0.0f);
    const float wrap = std::max(width - text_x - pad, 40.0f * px);
    std::vector<std::string_view> lines;
    for (size_t start = 0; start <= text.size();) {
        const size_t end = std::min(text.find('\n', start), text.size());
        lines.emplace_back(text.data() + start, end - start);
        start = end + 1;
    }
    const auto line_gap = [&](size_t i) { return i == 0 && lines.size() > 1 ? 4.0f * px : 1.0f * px; };
    float text_h = 0.0f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const char* begin = lines[i].data();
        text_h += font->CalcTextSizeA(i == 0 ? name_size : size, FLT_MAX, wrap, begin, begin + lines[i].size()).y +
                  line_gap(i);
    }
    const float height = std::max(pictures ? picture_size.y : 0.0f, text_h) + pad * 2.0f;
    if (!draw) {
        return height;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(at.x + width, at.y + height);
    const float rounding = 6.0f * px;
    dl->AddRectFilled(at, max, ImGui::GetColorU32(selected ? Ink::AccentDeep : Ink::Card), rounding);
    dl->AddRect(at, max, ImGui::GetColorU32(selected ? Ink::Accent : Ink::Line), rounding, 0,
                (selected ? 2.0f : 1.0f) * px);
    if (pictures) {
        const ImVec2 p0(at.x + pad, at.y + (height - picture_size.y) * 0.5f);
        const ImVec2 p1(p0.x + picture_size.x, p0.y + picture_size.y);
        if (picture) {
            dl->AddImage(picture->GetTexRef(), p0, p1);
        } else { // the new save: no picture yet
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(Ink::Panel), 4.0f * px);
        }
        dl->AddRect(p0, p1, ImGui::GetColorU32(Ink::Line), 4.0f * px, 0, 1.0f * px);
    }
    float y = at.y + (height - text_h) * 0.5f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const float s = i == 0 ? name_size : size;
        const char* begin = lines[i].data();
        const char* end = begin + lines[i].size();
        dl->AddText(font, s, ImVec2(at.x + text_x, y), ImGui::GetColorU32(i == 0 ? Ink::Text : Ink::Muted), begin,
                    end, wrap);
        y += font->CalcTextSizeA(s, FLT_MAX, wrap, begin, end).y + line_gap(i);
    }
    return height;
}

// The game's save list (sceSaveDataDialog): the action as the heading, then a card per save.
// Asked to overwrite, the new save is shown first (its picture and the details of the one it
// replaces), then the answers.
void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float px = base_scale;
    const float width = std::min(760.0f * px, viewport->WorkSize.x * 0.92f);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const bool confirm = !choice_preview_label.empty();
    const size_t details = confirm ? choice_title.find("\n\n") : std::string::npos;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextColored(Ink::Text, "%s", choice_title.substr(0, details).c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    const float inner = ImGui::GetContentRegionAvail().x;
    const float card_gap = 8.0f * px;
    if (confirm) {
        std::string text = choice_preview_label;
        if (details != std::string::npos) text += "\n" + choice_title.substr(details + 2);
        ImTextureData* preview = choice_textures.size() > choice_items.size() ? choice_textures.back() : nullptr;
        const float h = SaveCard(ImGui::GetCursorScreenPos(), inner, text, preview, true, false, true);
        ImGui::Dummy(ImVec2(inner, h + card_gap));
    }
    const int count = int(choice_items.size());
    const size_t listed = std::min(choice_textures.size(), choice_items.size());
    const bool pictures = !confirm && std::any_of(choice_textures.begin(), choice_textures.begin() + listed,
                                                  [](ImTextureData* tex) { return tex != nullptr; });
    const auto picture_of = [&](int i) { return pictures && i < int(listed) ? choice_textures[i] : nullptr; };
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        total += SaveCard(ImVec2(), inner, choice_items[i], picture_of(i), pictures, false, false) +
                 (i + 1 < count ? card_gap : 0.0f);
    }
    // A long list scrolls in its own area, between the heading and the keys.
    const float most = viewport->WorkSize.y * 0.86f - ImGui::GetCursorPosY() - ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::BeginChild("##saves", ImVec2(inner, std::min(total, std::max(most, 120.0f * px))), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    for (int i = 0; i < count; ++i) {
        const bool selected = i == choice_index;
        const float h = SaveCard(ImGui::GetCursorScreenPos(), inner, choice_items[i], picture_of(i), pictures,
                                 selected, true);
        ImGui::Dummy(ImVec2(inner, h));
        if (selected) {
            ImGui::SetScrollHereY(0.5f);
        }
        if (i + 1 < count) {
            ImGui::Dummy(ImVec2(inner, card_gap));
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::TextDisabled(Swapped() ? "Up/Down or D-pad: choose   Enter or Circle (B): OK   Esc or Cross (A): cancel"
                                  : "Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel");
    ImGui::End();
}

template <typename T, std::size_t N>
int StepValue(const std::array<T, N>& values, int current, int delta) {
    const int i = TheatrhythmGraphics::IndexOf(values, current, 0);
    return values[(i + delta + int(N)) % int(N)];
}

// imgui_mutex held: the focused row's value one step forward (delta 1) or back (-1).
void ChangeDisplayRow(int row, int delta) {
    namespace G = TheatrhythmGraphics;
    auto& s = TheatrhythmSettings::Get();
    switch (row) {
    case RowMode: s.display_mode = (s.display_mode + delta + G::DisplayModeCount) % G::DisplayModeCount; break;
    case RowResolution: { // listed largest first: right steps up to a larger size
        const auto& sizes = s.resolutions;
        if (sizes.empty()) return;
        const std::pair<int, int> now{s.window_width.load(), s.window_height.load()};
        int at = 0;
        while (at + 1 < int(sizes.size()) && sizes[at] > now) ++at;
        at = std::clamp(at - delta, 0, int(sizes.size()) - 1);
        s.window_width = sizes[at].first;
        s.window_height = sizes[at].second;
        break;
    }
    case RowMonitor: { // the primary, then each listed display
        const int count = int(s.monitors.size()) + 1;
        int at = 0;
        for (int i = 0; i < int(s.monitors.size()); ++i) {
            if (s.monitors[i] == s.monitor) at = i + 1;
        }
        at = (at + delta + count) % count;
        s.monitor = at == 0 ? G::PrimaryMonitor : s.monitors[at - 1];
        break;
    }
    case RowVsync: { // On, Off, Off with tearing
        const int at = (s.vsync ? 0 : s.tearing ? 2 : 1) + delta;
        const int next = (at + 3) % 3;
        s.vsync = next == 0;
        s.tearing = next == 2;
        break;
    }
    case RowFps: s.fps_limit = StepValue(G::FpsLimits, s.fps_limit, delta); break;
    case RowQueued: s.frames_queued = StepValue(G::FramesQueued, s.frames_queued, delta); break;
    case RowAnisotropy: s.anisotropy = StepValue(G::Anisotropies, s.anisotropy, delta); break;
    case RowRender:
        s.render_resolution = s.render_resolution == TheatrhythmSettings::Render4k ? TheatrhythmSettings::Render1080p
                                                                                   : TheatrhythmSettings::Render4k;
        break;
    default: return;
    }
    display_dirty = true;
}

// imgui_mutex held. Saving happens once, on close.
void CloseDisplaySettings() {
    display_open = false;
    if (display_dirty) {
        display_dirty = false;
        TheatrhythmSettings::SaveDisplay();
    }
}

struct DisplayRowText {
    const char* label;
    std::string value;
    const char* help;
};

DisplayRowText DisplayRowAt(int row) {
    auto& s = TheatrhythmSettings::Get();
    switch (row) {
    case RowMode: {
        static const char* const names[] = {"Windowed", "Borderless windowed", "Fullscreen"};
        return {"Window mode", names[std::clamp(s.display_mode.load(), 0, 2)],
                "Windowed, a borderless window covering the screen, or exclusive fullscreen."};
    }
    case RowResolution: {
        std::string text = std::to_string(s.window_width.load()) + " x " + std::to_string(s.window_height.load());
        if (s.display_mode == TheatrhythmGraphics::Borderless) text += "  (desktop in borderless)";
        return {"Resolution", text,
                "The window's size when windowed, the screen mode in fullscreen. Borderless always uses the "
                "desktop resolution."};
    }
    case RowMonitor: {
        std::string name = "Primary display";
        for (size_t i = 0; i < s.monitors.size() && i < s.monitor_names.size(); ++i) {
            if (s.monitors[i] == s.monitor) name = std::to_string(s.monitors[i] + 1) + ": " + s.monitor_names[i];
        }
        return {"Monitor", name, "The display the game window is on."};
    }
    case RowVsync:
        return {"VSync", s.vsync ? "On" : s.tearing ? "Off, tearing allowed" : "Off",
                "On waits for the monitor: no tearing, slightly more input delay. Off shows the newest frame "
                "at each refresh, still without tearing. Off, tearing allowed shows each frame at once: the "
                "least delay, and the only way past twice the refresh rate."};
    case RowFps: {
        const int fps = s.fps_limit;
        return {"Frame rate limit",
                fps == TheatrhythmGraphics::FpsUnlimited ? "Unlimited" : fps ? std::to_string(fps) + " FPS" : "Display rate",
                "Caps how often frames are shown. Display rate follows the monitor's refresh. Unlimited is as "
                "fast as the GPU goes; pair it with VSync off, tearing allowed."};
    }
    case RowQueued: {
        const int queued = s.frames_queued;
        return {"Frames queued", queued == 1 ? "1 (lowest latency)" : queued ? std::to_string(queued) : "Unbounded",
                "Frames the CPU may prepare ahead of the GPU. Fewer means lower input latency; more can smooth "
                "out a slow GPU."};
    }
    case RowAnisotropy: {
        const int value = s.anisotropy;
        std::string text = value ? std::to_string(value) + "x" : "Game default";
        if (value != s.startup_anisotropy) text += "  (after restart)";
        return {"Anisotropic filtering", text, "Sharper textures seen at an angle. Applies after a restart."};
    }
    case RowRender: {
        const int value = s.render_resolution;
        std::string text = value == TheatrhythmSettings::Render4k ? "4K (3840 x 2160)" : "1080p";
        if (value != s.startup_render_resolution) text += "  (after restart)";
        return {"Render resolution", text,
                "4K as on PS4 Pro, filtered down to the window: sharper. 1080p as on PS4: lighter on the GPU. "
                "Applies after a restart."};
    }
    default: return {"", "", ""};
    }
}

// Text as the game's pages draw it: white with a dark rim, a little heavier than the plain face.
void PageText(ImDrawList* dl, ImFont* font, float size, ImVec2 at, const char* text, ImU32 color = PageInk::Text) {
    const float r = std::max(1.5f, size * 0.06f);
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * 0.785398f;
        dl->AddText(font, size, ImVec2(at.x + std::cos(a) * r, at.y + std::sin(a) * r), PageInk::Outline, text);
    }
    dl->AddText(font, size, at, color, text);
    dl->AddText(font, size, ImVec2(at.x + 0.6f, at.y), color, text);
}

// The game's chevron (">" for dir 1, "<" for -1) as one thick stroke, centred on `at`.
void Chevron(ImDrawList* dl, ImVec2 at, float size, int dir, ImU32 color, float thickness) {
    const float w = size * 0.28f * float(dir), h = size * 0.5f;
    const ImVec2 points[3] = {ImVec2(at.x - w, at.y - h), ImVec2(at.x + w, at.y), ImVec2(at.x - w, at.y + h)};
    dl->AddPolyline(points, 3, color, ImDrawFlags_None, thickness);
}

// The white glove that points at the focused row, its fingertip at `tip`.
void PointingHand(ImDrawList* dl, ImVec2 tip, float s) {
    const float line = std::max(1.5f, s * 0.05f);
    const ImVec2 finger0(tip.x - s * 0.95f, tip.y - s * 0.14f), finger1(tip.x, tip.y + s * 0.14f);
    const ImVec2 palm0(tip.x - s * 1.55f, tip.y - s * 0.32f), palm1(tip.x - s * 0.72f, tip.y + s * 0.5f);
    dl->AddRectFilled(ImVec2(palm0.x - line, palm0.y - line), ImVec2(palm1.x + line, palm1.y + line), PageInk::HandLine,
                      s * 0.24f);
    dl->AddRectFilled(ImVec2(finger0.x - line, finger0.y - line), ImVec2(finger1.x + line, finger1.y + line),
                      PageInk::HandLine, s * 0.14f + line);
    dl->AddRectFilled(palm0, palm1, PageInk::Hand, s * 0.22f);
    dl->AddRectFilled(finger0, finger1, PageInk::Hand, s * 0.14f);
    for (const float f : {0.18f, 0.34f}) { // the folded fingers
        dl->AddLine(ImVec2(palm1.x - s * 0.28f, tip.y + s * f), ImVec2(palm1.x - s * 0.04f, tip.y + s * f),
                    PageInk::HandLine, line);
    }
}

// The settings gear of the page header.
void Gear(ImDrawList* dl, ImVec2 at, float r) {
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * 0.785398f, c = std::cos(a), s = std::sin(a);
        dl->AddLine(ImVec2(at.x + c * r * 0.6f, at.y + s * r * 0.6f), ImVec2(at.x + c * r * 1.05f, at.y + s * r * 1.05f),
                    PageInk::Outline, r * 0.42f);
        dl->AddLine(ImVec2(at.x + c * r * 0.6f, at.y + s * r * 0.6f), ImVec2(at.x + c * r * 0.98f, at.y + s * r * 0.98f),
                    PageInk::Text, r * 0.28f);
    }
    dl->AddCircleFilled(at, r * 0.8f, PageInk::Text);
    dl->AddCircle(at, r * 0.8f, PageInk::Outline, 0, r * 0.1f);
    dl->AddCircleFilled(at, r * 0.36f, PageInk::Header);
}

// The Display Settings page, drawn like the game's own settings pages (Settings > MS Settings):
// a blue header with the path, then one blue row per setting with its value in a navy pill. The
// focused row turns cyan on the left and the glove points at it.
void DisplaySettingsBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float px = viewport->Size.y / 1080.0f;
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##display_settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 o = viewport->Pos, size = viewport->Size;
    const float right = o.x + size.x, bottom = o.y + size.y;

    dl->AddRectFilledMultiColor(o, ImVec2(right, bottom), PageInk::Page, PageInk::Page, PageInk::PageLow,
                                PageInk::PageLow);
    const float header_h = 92.0f * px;
    dl->AddRectFilledMultiColor(o, ImVec2(right, o.y + header_h), PageInk::Header, PageInk::HeaderLow,
                                PageInk::HeaderLow, PageInk::Header);
    dl->AddRectFilled(ImVec2(o.x, o.y + header_h), ImVec2(right, o.y + header_h + 4.0f * px), PageInk::HeaderRule);
    Gear(dl, ImVec2(o.x + 76.0f * px, o.y + header_h * 0.48f), 24.0f * px);
    const float title = 54.0f * px, crumb = 36.0f * px;
    PageText(dl, font, title, ImVec2(o.x + 118.0f * px, o.y + (header_h - title) * 0.5f - 2.0f * px), "Settings");
    const float title_w = font->CalcTextSizeA(title, FLT_MAX, 0.0f, "Settings").x;
    const float crumb_x = o.x + 118.0f * px + title_w + 22.0f * px, crumb_mid = o.y + header_h * 0.5f;
    dl->AddTriangleFilled(ImVec2(crumb_x, crumb_mid - 10.0f * px), ImVec2(crumb_x + 13.0f * px, crumb_mid),
                          ImVec2(crumb_x, crumb_mid + 10.0f * px), PageInk::Text);
    PageText(dl, font, crumb, ImVec2(crumb_x + 28.0f * px, crumb_mid - crumb * 0.55f), "Display Settings");

    const float x0 = o.x + std::max(140.0f * px, size.x * 0.075f), x1 = right - std::max(80.0f * px, size.x * 0.042f);
    const float row_h = 80.0f * px, gap = 18.0f * px, label = 36.0f * px, value = 34.0f * px;
    float y = o.y + header_h + 34.0f * px;
    const char* help = "";
    for (int row = 0; row < RowCount; ++row) {
        const DisplayRowText text = DisplayRowAt(row);
        const bool focus = row == display_row;
        const ImVec2 a(x0, y), b(x1, y + row_h);
        const float mid = y + row_h * 0.5f, rounding = 14.0f * px;
        dl->AddRectFilled(a, b, PageInk::Row, rounding);
        if (focus) {
            const float split = x0 + (x1 - x0) * 0.44f, tip = row_h * 0.22f, inset = 5.0f * px;
            const ImVec2 band[5] = {ImVec2(x0 + inset, y + inset), ImVec2(split, y + inset), ImVec2(split + tip, mid),
                                    ImVec2(split, b.y - inset), ImVec2(x0 + inset, b.y - inset)};
            dl->AddConvexPolyFilled(band, 5, PageInk::Focus);
            const float t = 8.0f * px, span = split - x0 - 90.0f * px;
            for (int i = 0; i < 16; ++i) { // the band's faint triangle pattern
                const float tx = x0 + 60.0f * px + span * float((i * 37) % 100) / 100.0f;
                const float ty = y + 12.0f * px + (row_h - 30.0f * px) * float((i * 61) % 100) / 100.0f;
                if (i & 1) {
                    dl->AddTriangleFilled(ImVec2(tx, ty), ImVec2(tx + t, ty), ImVec2(tx + t * 0.5f, ty + t), PageInk::FocusSpeck);
                } else {
                    dl->AddTriangleFilled(ImVec2(tx, ty + t), ImVec2(tx + t, ty + t), ImVec2(tx + t * 0.5f, ty), PageInk::FocusSpeck);
                }
            }
            help = text.help;
        }
        dl->AddRect(a, b, PageInk::RowRim, rounding, 0, 3.0f * px);
        PageText(dl, font, label, ImVec2(x0 + 36.0f * px, mid - label * 0.55f), text.label);

        const float pill0 = x0 + (x1 - x0) * 0.49f, pill1 = x0 + (x1 - x0) * 0.87f, pill_h = row_h * 0.52f;
        dl->AddRectFilled(ImVec2(pill0, mid - pill_h * 0.5f), ImVec2(pill1, mid + pill_h * 0.5f), PageInk::Pill,
                          pill_h * 0.5f);
        const float vw = font->CalcTextSizeA(value, FLT_MAX, 0.0f, text.value.c_str()).x;
        PageText(dl, font, value, ImVec2((pill0 + pill1 - vw) * 0.5f, mid - value * 0.55f), text.value.c_str());
        const float chevron = row_h * 0.42f, stroke = 4.0f * px;
        Chevron(dl, ImVec2(x1 - 34.0f * px, mid), chevron, 1, focus ? PageInk::Yellow : PageInk::Chevron, stroke);
        if (focus) {
            Chevron(dl, ImVec2(pill0 - 30.0f * px, mid), chevron, -1, PageInk::Yellow, stroke);
            PointingHand(dl, ImVec2(x0 + 8.0f * px, mid + 4.0f * px), 52.0f * px);
        }
        y += row_h + gap;
    }

    const float help_size = 28.0f * px;
    PageText(dl, font, help_size, ImVec2(x0 + 8.0f * px, y + 6.0f * px), help, PageInk::Help);
    const char* keys = Swapped() ? "Up/Down: choose     Left/Right or Circle (B): change     Esc or Cross (A): back"
                                 : "Up/Down: choose     Left/Right or Cross (A): change     Esc or Circle (B): back";
    const float key_size = 26.0f * px, kw = font->CalcTextSizeA(key_size, FLT_MAX, 0.0f, keys).x;
    PageText(dl, font, key_size, ImVec2(right - kw - 60.0f * px, bottom - key_size - 30.0f * px), keys, PageInk::Help);
    ImGui::End();
    ImGui::PopStyleVar();
}

// The panel takes every key and button while it is open (the game sees neutral input).
bool HandleDisplayEvent(const SDL_Event& event) {
    int move = 0, change = 0;
    bool close = false;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        change = key == SDLK_LEFT || key == SDLK_A ? -1
                 : key == SDLK_RIGHT || key == SDLK_D || key == SDLK_RETURN || key == SDLK_KP_ENTER ||
                         key == SDLK_SPACE
                     ? 1
                     : 0;
        close = !event.key.repeat && (key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_F10);
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        const u8 confirm_btn = Swapped() ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
        const u8 cancel_btn = Swapped() ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        change = button == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? -1
                 : button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || button == confirm_btn ? 1
                                                                                      : 0;
        close = button == cancel_btn || button == SDL_GAMEPAD_BUTTON_START;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    if (move) {
        display_row = (display_row + move + RowCount) % RowCount;
    }
    if (change) {
        ChangeDisplayRow(display_row, change);
    }
    if (close) {
        CloseDisplaySettings();
    }
    return true;
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total, u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

bool NoticeActive() {
    return notice_until.load(std::memory_order_relaxed) > NowMs();
}

// Lower-right notes stack upward from `bottom`: the notice, then the shader notes.
template <typename Body>
void CornerNote(const char* id, float& bottom, float alpha, Body&& body) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, bottom), ImGuiCond_Always,
                            ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(alpha);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    body();
    bottom -= ImGui::GetWindowHeight() + pad / 2;
    ImGui::End();
}

void Notice(float& bottom) {
    std::string text;
    {
        std::scoped_lock lock{notice_mutex};
        text = notice_text;
    }
    CornerNote("##notice", bottom, 0.6f, [&] { ImGui::TextUnformatted(text.c_str()); });
}

// THEATRHYTHM_SHADER_NOTES=1: shader cache and compile notes in the corner.
const bool shader_notes = [] {
    const char* env = std::getenv("THEATRHYTHM_SHADER_NOTES");
    return env && *env != '0';
}();

bool PrefetchActive() {
    const u32 total = prefetch_total;
    return shader_notes && total && (prefetch_done < total || prefetch_until > NowMs());
}

void ShaderPrefetch(float& bottom) {
    const u32 done = prefetch_done, total = std::max(prefetch_total.load(), 1u);
    CornerNote("##shader_prefetch", bottom, 0.35f, [&] {
        if (done >= total) {
            ImGui::TextDisabled("Shader cache ready (%u)", total);
            return;
        }
        ImGui::TextDisabled("Reading shader cache %u%% (%u/%u)", u32(u64(done) * 100 / total), done, total);
        ImGui::ProgressBar(float(done) / float(total), ImVec2(ImGui::GetContentRegionAvail().x, 4.0f * base_scale),
                           "");
    });
}

bool CompileNoteActive() {
    return shader_notes && (compile_pending || compile_until > NowMs());
}

void ShaderCompiles(float& bottom) {
    const u32 pending = compile_pending, count = compile_count, waits = compile_waits;
    CornerNote("##shader_compiles", bottom, 0.35f, [&] {
        if (pending) {
            ImGui::TextDisabled("Compiling shaders (%u left)", pending);
        } else {
            ImGui::TextDisabled("Compiled %u new shader%s", count, count == 1 ? "" : "s");
        }
        if (waits) {
            ImGui::TextDisabled("Waited for %u build%s", waits, waits == 1 ? "" : "s");
        }
    });
}

void ConfigureContext() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.BackendPlatformName = "theatrhythm";
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.WindowPadding = ImVec2(20.0f, 16.0f);
    style.ItemSpacing = ImVec2(10.0f, 7.0f);
    style.WindowBorderSize = 1.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Ink::Text;
    c[ImGuiCol_TextDisabled] = Ink::Muted;
    c[ImGuiCol_WindowBg] = ImVec4(Ink::Bg.x, Ink::Bg.y, Ink::Bg.z, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = Ink::Line;
    c[ImGuiCol_Separator] = Ink::Line;
    c[ImGuiCol_FrameBg] = Ink::Card;
    c[ImGuiCol_PlotHistogram] = Ink::Accent;
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(theatrhythm_font_ttf),
                                   int(theatrhythm_font_ttf_end - theatrhythm_font_ttf), 18.0f, &font_config);
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const TheatrhythmPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

void StartSplashFade(const char* why) {
    int shown = SplashShown;
    splash_fade_from = NowMs();
    if (splash_state.compare_exchange_strong(shown, SplashFading)) {
        std::printf("Splash: %s after %.1f s\n", why, double(splash_fade_from - splash_since) / 1000.0);
    }
}

// imgui_mutex held, before NewFrame and UpdateChoiceTextures: the picture becomes a texture, and
// one faded out is retired.
void UpdateSplashTexture() {
    if (!splash_picture.rgba.empty()) {
        splash_texture = IM_NEW(ImTextureData)();
        splash_texture->Create(ImTextureFormat_RGBA32, splash_picture.width, splash_picture.height);
        std::memcpy(splash_texture->GetPixels(), splash_picture.rgba.data(), splash_picture.rgba.size());
        ImGui::RegisterUserTexture(splash_texture);
        splash_picture = {};
    }
    const int state = splash_state;
    if (state == SplashShown && NowMs() - splash_since > kSplashMaxMs) {
        StartSplashFade("no game picture yet, hidden");
    }
    if (state == SplashFading && NowMs() - splash_fade_from >= kSplashFadeMs) {
        splash_state = SplashDone;
        if (splash_texture) {
            splash_texture->WantDestroyNextFrame = true;
            choice_retired.push_back(splash_texture);
            choice_retiring = true;
            splash_texture = nullptr;
        }
    }
}

// The start-up picture fitted to the window (black bars like the game's frame), under the rest.
void Splash() {
    const int state = splash_state;
    if (!splash_texture || (state != SplashShown && state != SplashFading)) {
        return;
    }
    float alpha = 1.0f;
    if (state == SplashFading) {
        alpha = std::clamp(1.0f - float(NowMs() - splash_fade_from) / float(kSplashFadeMs), 0.0f, 1.0f);
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float w = float(splash_texture->Width), h = float(splash_texture->Height);
    const float scale = std::min(viewport->Size.x / w, viewport->Size.y / h);
    const ImVec2 size(w * scale, h * scale);
    const ImVec2 p0(viewport->Pos.x + (viewport->Size.x - size.x) * 0.5f,
                    viewport->Pos.y + (viewport->Size.y - size.y) * 0.5f);
    ImGui::GetBackgroundDrawList()->AddImage(splash_texture->GetTexRef(), p0, ImVec2(p0.x + size.x, p0.y + size.y),
                                             ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32(255, 255, 255, int(alpha * 255.0f + 0.5f)));
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        const u8 confirm_btn = Swapped() ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
        const u8 cancel_btn = Swapped() ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
        finish = button == confirm_btn ? 1 : button == cancel_btn ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

void DrawWindows() {
    Splash();
    if (loading) {
        LoadingScreen();
    }
    float corner = ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y - 12.0f * base_scale;
    if (NoticeActive()) {
        Notice(corner);
    }
    if (PrefetchActive() && !loading) {
        ShaderPrefetch(corner);
    }
    if (CompileNoteActive() && !loading) {
        ShaderCompiles(corner);
    }
    if (background_total && !loading) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active) {
        ChoiceBox();
    }
    if (display_open && !choice_active) {
        DisplaySettingsBox();
    }
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureContext();

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(*static_cast<const vk::Instance*>(user),
                                                                       name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // The font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 80;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active) {
        return HandleChoiceEvent(event);
    }
    if (display_open) {
        return HandleDisplayEvent(event);
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10 && !event.key.repeat && !text_entry_active) {
        display_row = 0;
        display_open = true;
        return true;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F4 && !event.key.repeat && !text_entry_active &&
        !(event.key.mod & SDL_KMOD_ALT)) { // Alt+F4 still closes the window at once
        exit_requested = true;
        return true;
    }
    return false;
}

bool TakeExitRequest() {
    return exit_requested.exchange(false);
}

void OpenDisplaySettings() {
    std::scoped_lock lock{imgui_mutex};
    if (!display_open) {
        display_row = 0;
        display_open = true;
    }
}

bool DisplaySettingsOpen() {
    return display_open;
}

void ShowNotice(const std::string& text, int milliseconds) {
    {
        std::scoped_lock lock{notice_mutex};
        notice_text = text;
    }
    notice_until.store(NowMs() + milliseconds, std::memory_order_relaxed);
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetShaderPrefetch(u32 done, u32 total) {
    if (total && done >= total) {
        prefetch_until = NowMs() + 2000;
    }
    prefetch_done = done;
    prefetch_total = total;
}

void NoteShaderCompiles(u32 pending, u32 started, u32 waits) {
    const s64 now = NowMs();
    if (!compile_pending && compile_until <= now) {
        compile_count = 0; // a new burst
        compile_waits = 0;
    }
    compile_count += started;
    compile_waits += waits;
    compile_pending = pending;
    compile_until = now + 2000;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

void SetSplash(TheatrhythmPng::Image picture) {
    std::scoped_lock lock{imgui_mutex};
    if (picture.rgba.empty() || splash_state != SplashNone) {
        return;
    }
    splash_picture = std::move(picture);
    splash_since = NowMs();
    splash_state = SplashShown;
}

bool SplashWaiting() {
    return splash_state == SplashShown;
}

// Two picture frames in a row: a single stray one (a buffer not yet cleared) keeps the splash.
void NoteSplashFrame(bool picture) {
    if (!picture) {
        splash_lit_frames = 0;
    } else if (++splash_lit_frames >= 2) {
        StartSplashFade("game picture");
    }
}

bool Visible() {
    const int splash = splash_state;
    return initialized && (splash == SplashShown || splash == SplashFading || loading ||background_total || PrefetchActive() || CompileNoteActive() ||
                           text_entry_active || choice_active || choice_retiring || NoticeActive() || display_open);
}

bool CapturesInput() {
    return text_entry_active || choice_active || display_open;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<TheatrhythmPng::Image> pictures, int focus, const std::string& preview_label,
                 TheatrhythmPng::Image preview) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_preview_label = preview_label;
    if (!preview_label.empty()) choice_pictures.push_back(std::move(preview));
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateSplashTexture();
    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    DrawWindows();
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace TheatrhythmOverlay

// A message from the runtime to the player (src/runtime_display_page.c: Display Settings is on F10
// alone in a game version it cannot patch).
extern "C" void theatrhythm_gpu_notice(const char* text, int milliseconds) {
    if (text && text[0]) {
        TheatrhythmOverlay::ShowNotice(text, milliseconds);
    }
}

// Called by the runtime (src/runtime_services.c) when the game unlocks a trophy: its name as a
// notice in the corner.
extern "C" void theatrhythm_gpu_toast_trophy(int id, const char* title, const char* detail, const unsigned char* png,
                                            uint64_t png_size) {
    (void)id;
    (void)detail;
    (void)png;
    (void)png_size;
    if (title && title[0]) {
        TheatrhythmOverlay::ShowNotice(std::string("Trophy: ") + title, 5000);
    }
}
