// theatrhythm: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    bool IsMinimized() const;
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Any thread: shows the window, created hidden so that it does not stand black while Vulkan
    /// starts (the presenter calls it once it can draw).
    void Show() { show_requested.store(true, std::memory_order_release); }
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::atomic<bool> is_minimized{false};
    std::atomic<bool> show_requested{false};
    bool shown{};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int display_mode = -1; ///< applied TheatrhythmSettings display mode (window thread)
    void ApplyDisplayMode(int mode);
    int monitor = -1; ///< applied TheatrhythmSettings monitor (window thread)
    void ApplyMonitor(int index);
    std::pair<int, int> resolution{1920, 1080}; ///< applied TheatrhythmSettings window size (window thread)
    void ApplyResolution(std::pair<int, int> size);
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend
