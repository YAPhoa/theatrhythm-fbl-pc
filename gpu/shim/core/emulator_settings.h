// theatrhythm: shadPS4 settings used by the video core, read once from THEATRHYTHM_* environment
// variables (defaults match shadPS4 except the pipeline cache, which is on).
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include "common/types.h"

u32 TheatrhythmDisplayRefreshHz(); // theatrhythm_gpu.cpp: primary display refresh rate, 60 when unknown
double TheatrhythmWindowRefreshRate(); // theatrhythm_gpu.cpp: exact rate of the window's display, 0 when unknown
bool TheatrhythmWaitForVblank(); // theatrhythm_gpu.cpp: waits for the window's display to start a refresh; false when it cannot (not Windows)
extern "C" int theatrhythm_render_4k(void); // theatrhythm_settings.cpp: render_resolution=4k
inline bool TheatrhythmRender4k() { return theatrhythm_render_4k() != 0; }

enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance() { static EmulatorSettingsImpl s; return &s; }
    static bool Flag(const char* name, bool fallback) {
        const char* v = std::getenv(name);
        return v ? (v[0] == '1' || v[0] == 'y' || v[0] == 't') : fallback;
    }
    static long Number(const char* name, long fallback) {
        const char* v = std::getenv(name);
        return v ? std::strtol(v, nullptr, 10) : fallback;
    }
    s32 GetGpuId() { static const auto value = s32(Number("THEATRHYTHM_GPU_ID", -1)); return value; }
    // theatrhythm: the video out size, 3840x2160 with render_resolution=4k (theatrhythm_render_4k).
    u32 GetInternalScreenWidth() { static const auto value = u32(Number("THEATRHYTHM_INTERNAL_WIDTH", TheatrhythmRender4k() ? 3840 : 1920)); return value; }
    u32 GetInternalScreenHeight() { static const auto value = u32(Number("THEATRHYTHM_INTERNAL_HEIGHT", TheatrhythmRender4k() ? 2160 : 1080)); return value; }
    // theatrhythm: Relaxed by default: without readbacks, guest code that reads GPU-written
    // vertices sees stale data; Relaxed costs no measurable frame rate.
    u32 GetReadbacksMode() { static const auto value = u32(Number("THEATRHYTHM_READBACKS", GpuReadbacksMode::Relaxed)); return value; }
    // theatrhythm: THEATRHYTHM_VBLANK_HZ=0 (uncapped presets): vblank runs at 480 Hz so a finished frame is
    // shown within ~2 ms instead of waiting for the next display-rate vblank (below the display
    // rate frames alternated 10/20 ms at 100 Hz: judder), and GetFrameLimit() caps the rate at
    // the display refresh, at most 120 (above ~120 FPS movement timing breaks: running slows).
    // An explicit THEATRHYTHM_VBLANK_HZ is used as given, without a limit (measurements).
    u32 GetVblankFrequency() {
        static const u32 value = [] {
            const long hz = Number("THEATRHYTHM_VBLANK_HZ", 60);
            return hz > 0 ? u32(hz) : 480u;
        }();
        return value;
    }
    // The vblank period. A display within 0.5 Hz of the vblank rate (a 59.95 Hz panel at the
    // default 60) sets it exactly: at 60.00 Hz the game made 0.05 frames a second more than the
    // display shows, so after ~35 s the VSync (Fifo) queue was full and the GPU thread waited on
    // the display for 4.9 s of every 5 (frames of extra latency, 33 ms frames). The display is
    // the one the window opened on. An explicit THEATRHYTHM_VBLANK_HZ is used as given.
    std::chrono::nanoseconds GetVblankPeriod() {
        static const auto value = DisplayPeriod(GetVblankFrequency(), std::getenv("THEATRHYTHM_VBLANK_HZ") == nullptr);
        return value;
    }
    // The present thread's slot for a frame limit, snapped the same way: the 60 FPS limit paces
    // the flips (they pass between vblanks), so at 60.00 it filled the queue as well.
    // THEATRHYTHM_FPS_LIMIT is used as given.
    std::chrono::nanoseconds GetFramePeriod(u32 limit) {
        return limit ? DisplayPeriod(limit, std::getenv("THEATRHYTHM_FPS_LIMIT") == nullptr)
                     : std::chrono::nanoseconds(0);
    }
    static std::chrono::nanoseconds DisplayPeriod(double hz, bool snap) {
        static const double display = TheatrhythmWindowRefreshRate();
        return std::chrono::nanoseconds(s64(1e9 / (snap && std::abs(display - hz) < 0.5 ? display : hz) + 0.5));
    }
    /// Frames per second the present thread lets through; 0 = no limit. THEATRHYTHM_FPS_LIMIT overrides.
    u32 GetFrameLimit() {
        static const u32 value = [] {
            const long limit = Number("THEATRHYTHM_FPS_LIMIT", -1);
            if (limit >= 0) return u32(limit);
            return Number("THEATRHYTHM_VBLANK_HZ", 60) > 0 ? 0u : std::min<u32>(TheatrhythmDisplayRefreshHz(), 120);
        }();
        return value;
    }
    bool IsCopyGpuBuffers() { static const auto value = Flag("THEATRHYTHM_COPY_GPU_BUFFERS", false); return value; }
    bool IsDirectMemoryAccessEnabled() { static const auto value = Flag("THEATRHYTHM_DIRECT_MEMORY_ACCESS", false); return value; }
    bool IsDumpShaders() { static const auto value = Flag("THEATRHYTHM_DUMP_SHADERS", false); return value; }
    bool IsHdrAllowed() { static const auto value = Flag("THEATRHYTHM_HDR", false); return value; }
    bool IsNullGPU() { static const auto value = Flag("THEATRHYTHM_NULL_GPU", false); return value; }
    bool IsPatchShaders() { return false; }
    bool IsPipelineCacheArchived() { return false; }
    bool IsPipelineCacheEnabled() { static const auto value = Flag("THEATRHYTHM_PIPELINE_CACHE", true); return value; }
    bool IsReadbackLinearImagesEnabled() { static const auto value = Flag("THEATRHYTHM_READBACK_LINEAR", false); return value; }
    bool IsRenderdocEnabled() { return false; }
    bool IsShaderCollect() { return false; }
    bool IsVkCrashDiagnosticEnabled() { return false; }
    bool IsVkGuestMarkersEnabled() { static const auto value = Flag("THEATRHYTHM_VK_MARKERS", false); return value; }
    bool IsVkHostMarkersEnabled() { static const auto value = Flag("THEATRHYTHM_VK_MARKERS", false); return value; }
    bool IsVkValidationCoreEnabled() { return true; }
    bool IsVkValidationEnabled() { static const auto value = Flag("THEATRHYTHM_VK_VALIDATION", false); return value; }
    bool IsVkValidationGpuEnabled() { return false; }
    bool IsVkValidationSyncEnabled() { static const auto value = Flag("THEATRHYTHM_VK_VALIDATION_SYNC", false); return value; }
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
