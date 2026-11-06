// SPDX-License-Identifier: GPL-2.0-or-later
// The system UI drawn over the game (Dear ImGui), by the presenter into the swapchain image after
// the game frame: the save list and text entry dialogs, notices and the shader-cache notes.

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "theatrhythm_png.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;

namespace Vulkan {
class Instance;
}

namespace TheatrhythmOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the save list consumed it.
bool HandleEvent(const SDL_Event& event);

/// Start-up loading screen (pipeline cache warm-up): `total` 0 shows no count.
void SetLoading(bool active, u32 done, u32 total);
/// Background cache warm-up: a small corner note while the game runs; `total` 0 hides it.
void SetBackgroundLoading(u32 done, u32 total);
/// Saved shaders read into memory at start (any thread): a lower-right note; `total` 0 hides it.
void SetShaderPrefetch(u32 done, u32 total);
/// GPU thread, when shader translations start or finish: `pending` still running, `started`
/// new since the last call, `waits` times a draw waited for a build since the last call (skip
/// budget spent). A lower-right note shows while any run and 2 s after.
void NoteShaderCompiles(u32 pending, u32 started, u32 waits);

/// Before the presenter exists: the game's start-up picture (sce_sys/pic1.png), drawn under the
/// rest until the game's frames show something, then faded out, as the PS4 does while a game starts.
void SetSplash(TheatrhythmPng::Image picture);
/// The start-up picture is up and waits for a game frame that is not black.
bool SplashWaiting();
/// Any thread, for each game frame read back while SplashWaiting: whether it shows a picture.
void NoteSplashFrame(bool picture);

/// Whether anything is drawn this frame.
bool Visible();

/// Any thread: a short message in the lower right corner (a trophy, a saved screenshot).
void ShowNotice(const std::string& text, int milliseconds = 3000);

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// A dialog is shown: the game's input is held neutral.
bool CapturesInput();

/// Any thread: the Display Settings panel (also F10). It saves settings.ini when it closes.
void OpenDisplaySettings();
bool DisplaySettingsOpen();

/// Any thread: F4 was pressed since the last call (the game's "Exit the game?" window).
bool TakeExitRequest();

/// Window thread: the game's text dialog (sceImeDialog). Shown as a box on screen until
/// `active` is false; typing goes to the window (sdl_window), the box only displays it.
void SetTextEntry(bool active, const std::string& prompt, const std::string& text);

/// The game's save list: shown until the player chooses (arrows/D-pad, Enter/Cross) or
/// cancels (Esc/Circle). `pictures`: one per item (empty = none), drawn beside its text.
/// PollChoice: -1 while open, -2 cancelled, else the chosen index.
void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<TheatrhythmPng::Image> pictures, int focus,
                 const std::string& preview_label = {}, TheatrhythmPng::Image preview = {});
int PollChoice();

} // namespace TheatrhythmOverlay
