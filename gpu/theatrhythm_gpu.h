/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef THEATRHYTHM_GPU_H
#define THEATRHYTHM_GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
    const char *splash;         /* start-up picture (the dump's sce_sys/pic1.png), NULL: none */
} TheatrhythmGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void theatrhythm_gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int theatrhythm_gpu_init(const TheatrhythmGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t theatrhythm_gpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int theatrhythm_gpu_handle_fault(void *ucontext, void *address);
/* Windows guest red-zone protection (shadps4/core/redzone_patches.cpp): one executable segment,
 * its functions' start offsets (sorted, from .eh_frame_hdr) and the starts of functions to leave
 * alone. Offsets are relative to the image. */
typedef struct {
    uint64_t address, size;
    const uint64_t *function_starts; uint64_t function_count;
    const uint64_t *skip_starts; uint64_t skip_count;
} TheatrhythmRedZoneSegment;
typedef struct {
    uint64_t function_count, skipped_function_count, instruction_count, red_zone_function_count,
        indirect_red_zone_function_count, memory_instruction_count, patched_memory_instruction_count,
        short_memory_instruction_count, stack_dependent_memory_instruction_count,
        control_flow_memory_instruction_count, unrelocatable_memory_instruction_count,
        trampoline_bytes, trampoline_exhausted;
} TheatrhythmRedZoneResult;
/* Moves each guest memory access made while red-zone data is live to a trampoline (in
 * `trampolines`, within rel32 reach of the image) that runs it with rsp 128 bytes lower. The image
 * must still be writable. 0 on success. */
int theatrhythm_redzone_patch(unsigned char *image, unsigned long long image_size, unsigned char *trampolines,
                              unsigned long long trampolines_size, const TheatrhythmRedZoneSegment *segments,
                              unsigned long long segment_count, TheatrhythmRedZoneResult *result);
/* The trampoline copy of a guest instruction the last theatrhythm_redzone_patch moved, or NULL. */
unsigned char *theatrhythm_redzone_copy(const unsigned char *instruction);
/* THEATRHYTHM_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void theatrhythm_gpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int theatrhythm_gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int theatrhythm_gpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int theatrhythm_gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int theatrhythm_gpu_choice_poll(void);
/* Confirmation with a separate preview of the new save; PNG data is decoded before returning. */
int theatrhythm_gpu_choice_confirm(const char *title, const char *const *items, int count, int focus,
                                 const unsigned char *png, uint64_t size, const char *caption, const char *unavailable);
/* 1 while a dialog (save list, text entry) is shown: the game's pad input is held neutral. */
int theatrhythm_gpu_overlay_captures_input(void);
/* Opens the Display Settings panel (the Options menu's Display Settings button); 1 while it is open. */
void theatrhythm_display_settings_open(void);
int theatrhythm_display_settings_is_open(void);
/* 1 once per press of F4 (the game's "Exit the game?" window, runtime_exit_prompt.c); any thread. */
int theatrhythm_gpu_take_exit_request(void);
/* Ends the game as closing its window does (any thread). */
void theatrhythm_gpu_request_quit(void);
/* The Options menu's Display Settings page (runtime_display_page.c). get: a setting's value (window
 * mode 0 windowed, 1 borderless, 2 fullscreen; resolution: index into the sizes, largest first;
 * VSync 0/1, off keeping the tearing choice; frame rate limit in FPS, 0 one per vblank, -1 unlimited).
 * set applies at once (the window follows on its next
 * event poll). resolution: the count of sizes, and size `index` when it exists. save: settings.ini. */
/* THEATRHYTHM_DISPLAY_RENDER_4K: render_resolution for the next start, 1 for 4k. */
enum { THEATRHYTHM_DISPLAY_MODE, THEATRHYTHM_DISPLAY_RESOLUTION, THEATRHYTHM_DISPLAY_VSYNC, THEATRHYTHM_DISPLAY_FPS_LIMIT,
       THEATRHYTHM_DISPLAY_RENDER_4K };
int theatrhythm_display_get(int setting);
void theatrhythm_display_set(int setting, int value);
int theatrhythm_display_resolution(int index, int *width, int *height);
void theatrhythm_display_save(void);
/* Controller vibration strength (settings.ini vibration), 0 (off) to 100 (the game's own). */
int theatrhythm_gpu_vibration_percent(void);
/* Button remapping (settings.ini pad_map, or key_map when keyboard is 1; THEATRHYTHM_PAD_MAP and
 * THEATRHYTHM_KEY_MAP override them): "" when unset. */
const char *theatrhythm_pad_map(int keyboard);
/* Dialog confirm button: 0 Cross confirms, Circle cancels; 1 Circle confirms, Cross cancels. */
int theatrhythm_gpu_menu_confirm_swap(void);
/* Button icon set (settings.ini button_icons, THEATRHYTHM_BUTTON_ICONS): "auto" (the device last
 * used), "ps4" (the game's own), "xbox" or "keyboard". Valid once theatrhythm_gpu_init returned. */
const char *theatrhythm_button_icons(void);
/* 1 when settings.ini render_resolution (THEATRHYTHM_RENDER_RESOLUTION) is 4k: the game runs in PS4
 * Pro mode with a 3840x2160 video out. Valid once theatrhythm_gpu_init returned. */
int theatrhythm_render_4k(void);
/* 1 when settings.ini skip_opening (THEATRHYTHM_SKIP_OPENING) is on: the start goes to the title
 * screen without the start-up logos and the opening movie. Valid once theatrhythm_gpu_init returned. */
int theatrhythm_skip_opening(void);
/* sceCommonDialogInitialize ran (runtime_services.c): the upstream dialog libraries check it. */
void theatrhythm_gpu_common_dialog_initialized(void);
/* Master volume of the game's audio (settings.ini master_volume), 0 to 200 percent (100: the
 * game's own mix; more is boosted behind a soft limiter). Read for every audio buffer. */
int theatrhythm_gpu_audio_volume_percent(void);
/* Speaker layout for the game's 7.1 audio (settings.ini audio_channels): 0 automatic (the
 * device's), 1 stereo, 2 5.1, 3 7.1. Read when an audio port is opened. */
int theatrhythm_gpu_audio_channels(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned theatrhythm_gpu_symbol_count(void);
/* Yields of threads waiting for guest memory copies (CPU report). */
unsigned long long theatrhythm_gpu_copy_yields(void);
/* Frames handed to the presenter since start (hang dump). */
unsigned long long theatrhythm_gpu_flips(void);
/* A notice in the corner for `milliseconds` (any thread; the runtime's messages to the player). */
void theatrhythm_gpu_notice(const char *text, int milliseconds);
/* Trophy unlocked (any thread): its name as a notice in the corner. The other arguments are kept
 * for the runtime's call: id, description and PNG icon. */
void theatrhythm_gpu_toast_trophy(int id, const char *title, const char *detail, const unsigned char *png,
                                 uint64_t png_size);
#ifdef __cplusplus
}
#endif
#endif
