// theatrhythm: SDL3 window for the Vulkan swapchain.
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "theatrhythm_overlay.h"
#include "theatrhythm_graphics.h"
#include "theatrhythm_settings.h"

namespace Frontend {

// theatrhythm: the display the window opens on. THEATRHYTHM_MONITOR is an index into SDL_GetDisplays()
// (the order SDL lists them in); absent or empty means the primary display. An invalid,
// negative or out-of-range value falls back to the primary with a diagnostic. SDL_WINDOWPOS_CENTERED
// is SDL_WINDOWPOS_CENTERED_DISPLAY(0), so the primary stays the safe default on every path.
static SDL_DisplayID MonitorDisplay() {
    auto& settings = TheatrhythmSettings::Get();
    int detected = 0;
    if (SDL_DisplayID* list = SDL_GetDisplays(&detected)) {
        settings.monitors.clear();
        settings.monitor_names.clear();
        for (int i = 0; i < detected; ++i) {
            SDL_Rect bounds{};
            if (SDL_GetDisplayBounds(list[i], &bounds) && bounds.w > 0 && bounds.h > 0) {
                settings.monitors.push_back(i);
                const char* name = SDL_GetDisplayName(list[i]);
                settings.monitor_names.push_back(name && name[0] ? name : "Display " + std::to_string(i + 1));
            }
        }
        SDL_free(list);
    }
    const char* value = std::getenv("THEATRHYTHM_MONITOR");
    const int index = TheatrhythmGraphics::ParseMonitor(value ? value : "");
    if (index == TheatrhythmGraphics::PrimaryMonitor) {
        if (value && value[0]) {
            LOG_WARNING(Frontend, "THEATRHYTHM_MONITOR \"{}\" is not a monitor index; using the primary display", value);
        }
        return SDL_GetPrimaryDisplay();
    }
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays || count == 0) {
        LOG_WARNING(Frontend, "No displays available; using the primary display");
        SDL_free(displays);
        return SDL_GetPrimaryDisplay();
    }
    if (index < 0 || index >= count || std::find(settings.monitors.begin(), settings.monitors.end(), index) == settings.monitors.end()) {
        settings.monitor = settings.startup_monitor = -1;
        LOG_WARNING(Frontend, "THEATRHYTHM_MONITOR {} is out of range for {} display(s); using the primary display", value, count);
        SDL_free(displays);
        return SDL_GetPrimaryDisplay();
    }
    const SDL_DisplayID chosen = displays[index];
    SDL_free(displays);
    return chosen;
}

// The display's fullscreen sizes of the desktop's shape (within 1%: 1366x768 counts as 16:9),
// largest first, without repeats (refresh rates and pixel densities collapse), for the Display
// Settings page; the size from settings.ini is added when missing.
static void ListResolutions(SDL_DisplayID display) {
    auto& settings = TheatrhythmSettings::Get();
    std::vector<std::pair<int, int>> sizes;
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    const double shape = desktop && desktop->h > 0 ? double(desktop->w) / desktop->h : 0.0;
    int count = 0;
    if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count)) {
        for (int i = 0; i < count; ++i) {
            const double ratio = double(modes[i]->w) / modes[i]->h;
            if (modes[i]->w >= 640 && modes[i]->h >= 360 && (!shape || std::abs(ratio / shape - 1.0) <= 0.01))
                sizes.emplace_back(modes[i]->w, modes[i]->h);
        }
        SDL_free(modes);
    }
    sizes.emplace_back(settings.window_width.load(), settings.window_height.load());
    std::sort(sizes.begin(), sizes.end(), std::greater<>());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    settings.resolutions = std::move(sizes);
}

// theatrhythm: a window opens at the size in settings.ini (window), fitted to the chosen display's
// usable area (with room for the title bar); one too large opens maximized, restoring to that size
// shrunk to fit. The picture is scaled to the window either way. True when shrunk.
static bool StartSize(s32& width, s32& height, SDL_DisplayID display) {
    constexpr int title_room = 48;
    const int w = TheatrhythmSettings::Get().window_width, h = TheatrhythmSettings::Get().window_height;
    double scale = 1.0;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 0 && usable.h > title_room) {
        scale = std::min({1.0, double(usable.w) / w, double(usable.h - title_room) / h});
    }
    width = std::max(320, int(w * scale));
    height = std::max(180, int(h * scale));
    return scale < 1.0;
}

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    const SDL_DisplayID display = MonitorDisplay();
    ListResolutions(display);
    resolution = {TheatrhythmSettings::Get().window_width.load(), TheatrhythmSettings::Get().window_height.load()};
    const bool too_large = StartSize(width_, height_, display);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MAXIMIZED_BOOLEAN, too_large);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true); // until Show()
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    // theatrhythm: THEATRHYTHM_WINDOW_ICON names a BMP (SDL's built-in format) for the window icon.
    if (const char* icon_path = std::getenv("THEATRHYTHM_WINDOW_ICON")) {
        if (SDL_Surface* icon = SDL_LoadBMP(icon_path)) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        }
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    monitor = TheatrhythmSettings::Get().monitor;
    ApplyDisplayMode(TheatrhythmSettings::Get().display_mode);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

// Windowed, borderless (desktop-sized, no mode change) or exclusive fullscreen at the display mode
// closest to the chosen resolution (the desktop's when none is close). Window thread only (SDL).
// The swapchain follows the size events.
void WindowSDL::ApplyDisplayMode(int mode) {
    display_mode = mode;
    if (mode == TheatrhythmGraphics::Windowed) {
        SDL_SetWindowFullscreen(window, false);
    } else {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        SDL_DisplayMode chosen{};
        const SDL_DisplayMode* target = SDL_GetDesktopDisplayMode(display);
        if (mode == TheatrhythmGraphics::Fullscreen &&
            SDL_GetClosestFullscreenDisplayMode(display, resolution.first, resolution.second, 0.0f, false, &chosen)) {
            target = &chosen;
        }
        const bool exclusive = mode == TheatrhythmGraphics::Fullscreen && target;
        if (!SDL_SetWindowFullscreenMode(window, exclusive ? target : nullptr)) {
            LOG_WARNING(Frontend, "Fullscreen mode: {}", SDL_GetError());
        }
        SDL_SetWindowFullscreen(window, true);
    }
    SDL_SyncWindow(window);
    std::printf("Window: %s\n", TheatrhythmGraphics::DisplayModeNames[mode]);
}

// A new resolution: a window takes that size (shrunk to fit the display's usable area), centred;
// exclusive fullscreen switches to the closest mode; borderless keeps the desktop's.
void WindowSDL::ApplyResolution(std::pair<int, int> size) {
    resolution = size;
    if (display_mode == TheatrhythmGraphics::Fullscreen) {
        ApplyDisplayMode(display_mode);
    } else if (display_mode == TheatrhythmGraphics::Windowed) {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        s32 w = 0, h = 0;
        StartSize(w, h, display);
        SDL_RestoreWindow(window);
        SDL_SetWindowSize(window, w, h);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        SDL_SyncWindow(window);
    }
    std::printf("Window: resolution %dx%d\n", size.first, size.second);
}

// Moves the window to another display (an index into SDL_GetDisplays(), or PrimaryMonitor),
// centred; a fullscreen window leaves fullscreen for the move and re-enters it there.
void WindowSDL::ApplyMonitor(int index) {
    monitor = index;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    int count = 0;
    if (SDL_DisplayID* displays = SDL_GetDisplays(&count)) {
        if (index >= 0 && index < count) display = displays[index];
        SDL_free(displays);
    }
    if (SDL_GetDisplayForWindow(window) == display) return;
    if (display_mode != TheatrhythmGraphics::Windowed) {
        SDL_SetWindowFullscreen(window, false);
        SDL_SyncWindow(window);
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SyncWindow(window);
    if (display_mode != TheatrhythmGraphics::Windowed) ApplyDisplayMode(display_mode);
    std::printf("Window: moved to display %d\n", index);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    TheatrhythmOverlay::SetTextEntry(text_active, text_prompt, text);
}

#ifdef _WIN32
// DWM cloaking: a cloaked window is shown, sized and drawn into, but not composed on screen.
bool WindowSDL::Cloak(bool on) {
    const BOOL value = on;
    return window_info.type == WindowSystemType::Windows &&
           SUCCEEDED(DwmSetWindowAttribute(static_cast<HWND>(window_info.render_surface), DWMWA_CLOAK,
                                           &value, sizeof(value)));
}
#endif

bool WindowSDL::PollEvents() {
    if (!shown && show_requested.load(std::memory_order_acquire)) {
        shown = true;
#ifdef _WIN32
        // Shown cloaked: it takes its borderless/fullscreen size and the presenter draws the start-up
        // picture into it before anyone sees it, where a plain show shows it black for a moment.
        cloaked = Cloak(true);
        shown_at = SDL_GetTicks();
#endif
        SDL_ShowWindow(window);
        SDL_SyncWindow(window);
        int w = 0, h = 0; // a window created hidden takes its borderless/fullscreen size only now
        SDL_GetWindowSizeInPixels(window, &w, &h);
        width = w;
        height = h;
        shown_done.store(true, std::memory_order_release);
#ifdef _WIN32
        if (!cloaked)
#endif
            revealed.store(true, std::memory_order_release);
    }
#ifdef _WIN32
    // Revealed once the presenter has drawn into it, or after a second whatever happened.
    if (cloaked && (reveal_requested.load(std::memory_order_acquire) || SDL_GetTicks() - shown_at > 1000)) {
        Cloak(false);
        cloaked = false;
        revealed.store(true, std::memory_order_release);
    }
#endif
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (const int mode = TheatrhythmSettings::Get().display_mode; mode != display_mode) {
        ApplyDisplayMode(mode);
    }
    if (const int index = TheatrhythmSettings::Get().monitor; index != monitor) {
        ApplyMonitor(index);
    }
    if (const std::pair<int, int> size{TheatrhythmSettings::Get().window_width.load(),
                                       TheatrhythmSettings::Get().window_height.load()};
        size != resolution) {
        ApplyResolution(size);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels (or swapped).
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            const bool swapped = TheatrhythmSettings::Get().menu_confirm_swap.load(std::memory_order_relaxed) != 0;
            const u8 confirm_btn = swapped ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
            text_state = event.gbutton.button == confirm_btn ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (TheatrhythmOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_OCCLUDED:
            is_minimized.store(true, std::memory_order_release);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_EXPOSED: {
            is_minimized.store(false, std::memory_order_release);
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            if (w > 0 && h > 0) {
                width = w;
                height = h;
            }
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    if (window) {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        const bool min = (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) != 0;
        const bool was_min = is_minimized.exchange(min, std::memory_order_acq_rel);
        if (was_min && !min) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            if (w > 0 && h > 0) {
                width = w;
                height = h;
            }
        }
    }
    return is_open;
}

bool WindowSDL::IsMinimized() const {
    if (is_minimized.load(std::memory_order_acquire)) {
        return true;
    }
    return width.load(std::memory_order_relaxed) <= 0 || height.load(std::memory_order_relaxed) <= 0;
}

} // namespace Frontend
