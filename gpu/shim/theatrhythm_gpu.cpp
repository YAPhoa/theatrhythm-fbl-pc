#include "theatrhythm_write_log.h"
// theatrhythm: glue between the C loader and the vendored shadPS4 video core.
#include "theatrhythm_overlay.h"
#include "theatrhythm_png.h"
#include "theatrhythm_settings.h"
#include "theatrhythm_graphics.h"
#include "theatrhythm_copy.h"
#ifdef _WIN32
#include <dxgi.h>
#else
#include <sys/resource.h>
#endif
#include "theatrhythm_toggles.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../theatrhythm_gpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
uint64_t runtime_read_tsc(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
// runtime.c: exit counters (and the ISA site report), as on the guest's own exit
void runtime_report(void);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
// Upstream HLE libraries for hardware and services a PC does not have (VR, Move, remote play,
// browser, disc map, SSL): they report "not connected" the way shadPS4 does.
namespace Libraries::Hmd { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::HmdSetupDialog { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VrTracker { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Move { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Remoteplay { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::WebBrowserDialog { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::DiscMap { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Ssl { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::GameLiveStreaming { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::ScreenShot { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::SharePlay { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoRecording { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
// commondialog.cpp's state; the C runtime implements sceCommonDialogInitialize.
namespace Libraries::CommonDialog {
bool g_isInitialized = false;
bool g_isUsed = false;
} // namespace Libraries::CommonDialog
extern "C" void theatrhythm_gpu_common_dialog_initialized(void) {
    Libraries::CommonDialog::g_isInitialized = true;
}
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const TheatrhythmGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
// Through the backing view: with THEATRHYTHM_READBACKS=2 (precise) GPU-written pages are also
// read-protected, and a copy thread faulting there would wait for the GPU thread waiting for it.
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    runtime_memory_read_backing(source, dest, size);
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // theatrhythm: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !TheatrhythmCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    TheatrhythmCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    TheatrhythmWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, TheatrhythmWriteLog::Backing);
    const bool written = runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
    // The write skipped page tracking: textures over it must reload.
    // ponytail: fence and label writes (<= 8 bytes) never hold texels, so they are left out;
    // every one would send all texture bindings through the locked path.
    if (written && rasterizer && size > 8) {
        rasterizer->GetTextureCache().NoteBackingWrite(reinterpret_cast<VAddr>(address), size);
    }
    return written;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    TheatrhythmStats::Timer timer{TheatrhythmStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    TheatrhythmStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    TheatrhythmStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        TheatrhythmStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        TheatrhythmStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return runtime_read_tsc(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return theatrhythm_render_4k(); } // PS4 Pro with render_resolution=4k
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 TheatrhythmDisplayRefreshHz() {
    static const u32 hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
    }();
    return hz;
}

double TheatrhythmWindowRefreshRate() {
    SDL_DisplayID display = g_window ? SDL_GetDisplayForWindow(g_window->GetSDLWindow()) : 0;
    if (!display) display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    if (!mode) return 0.0;
    if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0) {
        return double(mode->refresh_rate_numerator) / mode->refresh_rate_denominator;
    }
    return mode->refresh_rate > 0 ? mode->refresh_rate : 0.0;
}

// The DXGI output of the window's monitor, per calling thread, found again when the window moves.
bool TheatrhythmWaitForVblank() {
#ifdef _WIN32
    thread_local IDXGIOutput* output = nullptr;
    thread_local HMONITOR monitor = nullptr;
    HWND hwnd = g_window ? static_cast<HWND>(SDL_GetPointerProperty(
                               SDL_GetWindowProperties(g_window->GetSDLWindow()),
                               SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
                         : nullptr;
    if (!hwnd) return false;
    const HMONITOR current = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (current != monitor) {
        if (output) output->Release();
        output = nullptr;
        monitor = current;
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            IDXGIOutput* candidate = nullptr;
            for (UINT o = 0; !output && adapter->EnumOutputs(o, &candidate) != DXGI_ERROR_NOT_FOUND; ++o) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == current) {
                    output = candidate;
                } else {
                    candidate->Release();
                }
            }
            adapter->Release();
        }
        factory->Release();
    }
    return output && SUCCEEDED(output->WaitForVBlank());
#else
    return false;
#endif
}

extern "C" int theatrhythm_gpu_init(const TheatrhythmGpuConfig* config) {
    TheatrhythmSettings::Load();
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("THEATRHYTHM_GPU_USER_DIR")) _putenv_s("THEATRHYTHM_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("THEATRHYTHM_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    // The window carries the project name; the dump's own title stays in ElfInfo (above).
    const std::string title = "Theatrhythm Final Bar Line";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("runner:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        TheatrhythmSettings::SaveDisplayIfChanged();
        runtime_report();
        std::fflush(stdout);
        std::_Exit(0);
    });
    g_window_thread.detach();
    // The PS4 shows the game's pic1.png while it starts; decoded while the window opens.
    if (config->splash) {
        TheatrhythmPng::Image splash;
        if (TheatrhythmPng::LoadRgba(config->splash, splash)) {
            TheatrhythmOverlay::SetSplash(std::move(splash));
        }
    }
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void theatrhythm_gpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
    Libraries::Hmd::RegisterLib(&resolver);
    Libraries::HmdSetupDialog::RegisterLib(&resolver);
    Libraries::VrTracker::RegisterLib(&resolver);
    Libraries::Move::RegisterLib(&resolver);
    Libraries::Remoteplay::RegisterLib(&resolver);
    Libraries::WebBrowserDialog::RegisterLib(&resolver);
    Libraries::DiscMap::RegisterLib(&resolver);
    Libraries::Ssl::RegisterLib(&resolver);
    Libraries::GameLiveStreaming::RegisterLib(&resolver);
    Libraries::ScreenShot::RegisterLib(&resolver);
    Libraries::SharePlay::RegisterLib(&resolver);
    Libraries::VideoRecording::RegisterLib(&resolver);
}

extern "C" uintptr_t theatrhythm_gpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int theatrhythm_gpu_handle_fault(void* ucontext, void* address) {
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

extern "C" unsigned theatrhythm_gpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

extern "C" unsigned long long theatrhythm_gpu_copy_yields(void) {
    return TheatrhythmCopy::Yields();
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("theatrhythm:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port. Screenshots (F12) are taken by the presenter:
// the game's frame without the overlay.
namespace VideoCore {
namespace {
std::atomic<u32> screenshot_requests{0};
}
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest request) {
    if (request != ScreenshotRequest::None) {
        screenshot_requests.fetch_add(1, std::memory_order_relaxed);
    }
}
u32 ConsumeGameOnlyScreenshotRequests() {
    return screenshot_requests.exchange(0, std::memory_order_acq_rel);
}
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() {
    return {.game_only_count = ConsumeGameOnlyScreenshotRequests(), .with_overlays_count = 0};
}
} // namespace VideoCore

extern "C" int theatrhythm_gpu_vibration_percent(void) {
    return TheatrhythmSettings::Get().vibration.load(std::memory_order_relaxed);
}

extern "C" const char* theatrhythm_pad_map(int keyboard) {
    const auto& v = TheatrhythmSettings::Get();
    return keyboard ? v.key_map.c_str() : v.pad_map.c_str();
}

extern "C" int theatrhythm_gpu_audio_volume_percent(void) {
    return TheatrhythmSettings::Get().master_volume.load(std::memory_order_relaxed);
}

extern "C" int theatrhythm_gpu_audio_channels(void) {
    return TheatrhythmSettings::Get().audio_channels.load(std::memory_order_relaxed);
}

extern "C" int theatrhythm_gpu_overlay_captures_input(void) {
    return TheatrhythmOverlay::CapturesInput() ? 1 : 0;
}

extern "C" void theatrhythm_display_settings_open(void) {
    TheatrhythmOverlay::OpenDisplaySettings();
}

extern "C" int theatrhythm_display_settings_is_open(void) {
    return TheatrhythmOverlay::DisplaySettingsOpen() ? 1 : 0;
}

extern "C" int theatrhythm_gpu_take_exit_request(void) {
    return TheatrhythmOverlay::TakeExitRequest() ? 1 : 0;
}

extern "C" void theatrhythm_gpu_request_quit(void) {
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT; // the window thread ends the process as when the window is closed
    SDL_PushEvent(&event);
}

extern "C" int theatrhythm_display_get(int setting) {
    const auto& s = TheatrhythmSettings::Get();
    switch (setting) {
    case THEATRHYTHM_DISPLAY_MODE: return s.display_mode;
    case THEATRHYTHM_DISPLAY_RESOLUTION: { // the first listed size not larger than the window
        const std::pair<int, int> now{s.window_width.load(), s.window_height.load()};
        int at = 0;
        while (at + 1 < int(s.resolutions.size()) && s.resolutions[at] > now) ++at;
        return at;
    }
    case THEATRHYTHM_DISPLAY_VSYNC: return s.vsync ? 1 : 0;
    case THEATRHYTHM_DISPLAY_FPS_LIMIT: return s.fps_limit;
    case THEATRHYTHM_DISPLAY_RENDER_4K: return s.render_resolution == TheatrhythmSettings::Render4k ? 1 : 0;
    default: return 0;
    }
}

extern "C" void theatrhythm_display_set(int setting, int value) {
    auto& s = TheatrhythmSettings::Get();
    switch (setting) {
    case THEATRHYTHM_DISPLAY_MODE:
        if (value >= 0 && value < TheatrhythmGraphics::DisplayModeCount) s.display_mode = value;
        break;
    case THEATRHYTHM_DISPLAY_RESOLUTION:
        if (value >= 0 && value < int(s.resolutions.size())) {
            s.window_width = s.resolutions[value].first;
            s.window_height = s.resolutions[value].second;
        }
        break;
    case THEATRHYTHM_DISPLAY_VSYNC: s.vsync = value != 0; break;
    case THEATRHYTHM_DISPLAY_FPS_LIMIT: s.fps_limit = value; break;
    case THEATRHYTHM_DISPLAY_RENDER_4K:
        s.render_resolution = value ? TheatrhythmSettings::Render4k : TheatrhythmSettings::Render1080p;
        break;
    default: return;
    }
}

extern "C" int theatrhythm_display_resolution(int index, int* width, int* height) {
    const auto& sizes = TheatrhythmSettings::Get().resolutions;
    if (index >= 0 && index < int(sizes.size())) {
        *width = sizes[index].first;
        *height = sizes[index].second;
    }
    return int(sizes.size());
}

extern "C" void theatrhythm_display_save(void) {
    TheatrhythmSettings::SaveDisplay();
}

extern "C" int theatrhythm_gpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int theatrhythm_gpu_choice_begin(const char* title, const char* const* items, const char* const* images,
                                    int count, int focus) {
    if (!g_window || count <= 0) return 0;
    // Pictures are decoded here, on the game's thread, before the list is shown.
    std::vector<TheatrhythmPng::Image> pictures(count);
    for (int i = 0; images && i < count; ++i) {
        if (images[i] && !TheatrhythmPng::LoadRgba(images[i], pictures[i])) {
            LOG_WARNING(Lib_SaveDataDialog, "save list: no picture from {}", images[i]);
        }
    }
    TheatrhythmOverlay::BeginChoice(title ? title : "", std::vector<std::string>(items, items + count),
                             std::move(pictures), focus);
    return 1;
}

extern "C" int theatrhythm_gpu_choice_poll(void) {
    return TheatrhythmOverlay::PollChoice();
}

extern "C" int theatrhythm_gpu_choice_confirm(const char* title, const char* const* items, int count, int focus,
                                           const unsigned char* png, uint64_t size, const char* caption, const char* unavailable) {
    if (!g_window || !items || count <= 0) return 0;
    TheatrhythmPng::Image preview;
    std::string label = caption ? caption : "";
    if (!png || !size || size > 4u * 1024u * 1024u || !TheatrhythmPng::DecodeRgba(png, size, preview)) {
        label += "\n";
        label += unavailable ? unavailable : "";
    }
    TheatrhythmOverlay::BeginChoice(title ? title : "", std::vector<std::string>(items, items + count),
                               std::vector<TheatrhythmPng::Image>(count), focus, label, std::move(preview));
    return 1;
}

extern "C" int theatrhythm_gpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
