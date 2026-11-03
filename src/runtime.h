#ifndef THEATRHYTHM_RUNTIME_H
#define THEATRHYTHM_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in theatrhythm_longjmp
 * (gpu/shim/theatrhythm_toggles.h uses the same buffer type). */
typedef theatrhythm_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) theatrhythm_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
/* THEATRHYTHM_SLOW_WAITS=<ms>: log guest blocking calls (locks, waits, reads) longer than that.
 * runtime_wait_begin() returns 0 when it is off; runtime_wait_end() then does nothing. */
uint64_t runtime_wait_begin(void);
void runtime_wait_end(const char *what,uint64_t start,const char *detail,long long arg);
/* The floating-point state a PS4 gives its threads (runtime_thread.c). */
void runtime_orbis_fpu(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
/* libc.prx's thread-local destructor runner (_sceKernelSetThreadDtors); NULL until it registers. */
extern GuestCallback runtime_guest_thread_dtors, runtime_guest_thread_atexit_count, runtime_guest_thread_atexit_report;
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
/* Called at the end of runtime_report (the loader adds its own counters). */
void runtime_set_report_hook(void (*hook)(void));
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* Host access to guest memory past page protection, through the backing view: write_backing
 * returns 0 when part of the range is unmapped, and skips GPU tracking (runtime_memory_invalidate). */
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);
void runtime_memory_visit_mapped(void (*visit)(uintptr_t address, const unsigned char *bytes, uint64_t size,
                                               void *context), void *context);
void runtime_memory_invalidate(uintptr_t address, uint64_t size);
/* Button prompt icons (runtime_button_icons.c): mode "ps4", "xbox", "keyboard" or "auto" (follows
 * the device the player last used, reported by the pad code); folder holds prepare's icon files. */
enum { BUTTON_ICONS_PS4, BUTTON_ICONS_XBOX, BUTTON_ICONS_KEYBOARD, BUTTON_ICONS_SETS };
void runtime_button_icons_configure(const char *folder, const char *mode);
void runtime_button_icons_input(int set);
/* Display Settings in the game's Options menu, and all nine display languages on its Language page
 * (runtime_display_page.c): hooks the game's code in the
 * image (still writable), with stubs written to `stubs` (`size` bytes, within rel32 reach of it);
 * `monitor_icon`: the Config atlas bundle with the button's icon is mounted. 0 when installed. */
int runtime_display_page_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size, int monitor_icon);
/* The party's walk on field stages at any frame rate (runtime_field_walk.c): StageCharacter's
 * motion check measures movement in 60ths of a second, with its stub written to `stubs` (`size`
 * bytes, within rel32 reach of the image, which is still writable). 0 when installed. */
int runtime_field_walk_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size);
/* The "DLC songs added" window without an empty bonus row (runtime_dlc_bonus.c): BonusItemDisplay
 * hides itself for a count of 0, with its stub written to `stubs` (`size` bytes, within rel32 reach
 * of the image, which is still writable). 0 when installed. */
int runtime_dlc_bonus_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
/* TLS module ids 2..RUNTIME_TLS_MODULES-1 (link_modules.py numbers every module with TLS). */
#define RUNTIME_TLS_MODULES 32
/* A module of the linked image (TFPROBE6, link_modules.py). Addresses are image offsets. Deferred
 * modules are mapped and relocated at boot but start only when the game loads them. */
#define RUNTIME_MODULE_DEFERRED 1
typedef struct { char nid[16]; uint64_t address, kind; } RuntimeModuleExport;
typedef struct {
    uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module, flags, procparam;
    char name[64];
    uint64_t export_count;
    RuntimeModuleExport *exports;
    int started;
    /* sceKernelGetModuleInfoForUnwind, image offsets (TFPROBE7): .eh_frame_hdr (0: none),
     * .eh_frame and its size, first segment and its size. */
    uint64_t eh_frame_hdr, eh_frame, eh_frame_size, seg0, seg0_size;
} RuntimeModule;
/* The probe hands over its module table once the image is mapped (runtime_module.c), and the
 * eboot's record (base 0, size up to the first module, name and unwind fields). */
void runtime_module_register(unsigned char *image, RuntimeModule *modules, uint64_t count);
void runtime_module_set_main(const RuntimeModule *main);
/* Candidate guest return addresses on this thread's stack as "Module.prx+0xoffset" (traces). */
void runtime_guest_backtrace(char *out, size_t size, int max);
/* THEATRHYTHM_THROW_TRACE=1: log guest C++ throws (after runtime_module_set_main). */
void runtime_throw_trace_install(void);
/* THEATRHYTHM_VERBOSE=1: log routine events too (runtime.c). */
int runtime_verbose(void);
uintptr_t runtime_module_resolve(const char *name);
void runtime_module_report(void);
/* Starts a linked module by file name ("libSceNpToolkit2.prx") unless it already runs: its
 * handle (>0) and, for a new start, the start routine's result; or a negative SCE error. */
int32_t runtime_module_start(const char *file, uint64_t args, const void *argp, int32_t *result);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
/* Counts a guest sched_yield (zero_sleep 0) or a zero-length sleep (1) for the report. */
void runtime_thread_count_yield(int zero_sleep);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
/* The stack the calling thread runs guest code on, when it is not the one the host gave it (Linux:
 * the guest main stack below 1 TiB): scePthreadAttrGet reports it, and the IL2CPP collector scans it. */
void runtime_thread_set_stack(void *low, uint64_t size);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
uintptr_t runtime_eventflag_resolve(const char *name);
void runtime_eventflag_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
/* param.sfo ATTRIBUTE of the title (sceKernelGetCpumode). */
void runtime_kernel_set_psf_attributes(uint32_t attributes);
void runtime_kernel_set_neo_mode(int enabled);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
/* Offline networking (POSIX sockets, libSceHttp extras), PSN libraries and their dialogs, and
 * devices the PC lacks (camera, Audio3d, social screen): runtime_net.c, runtime_np.c, runtime_devices.c. */
uintptr_t runtime_net_resolve(const char *name);
uintptr_t runtime_np_resolve(const char *name);
uintptr_t runtime_devices_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
/* Zeroed, 64-byte aligned host records the guest sees by address, below 1 TiB (THEATRHYTHM_LOW_HANDLES). */
void *runtime_low_alloc(size_t size);
void runtime_low_free(void *pointer);
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
/* AjmDecMp3ParseFrame (guest layout). */
typedef struct {
    uint64_t frame_size;
    uint32_t num_channels, samples_per_channel, bitrate, sample_rate, encoder_delay, num_frames, total_samples, ofl_type;
} Mp3Frame;
typedef struct Mp3Decoder Mp3Decoder;
int runtime_mp3_parse(const unsigned char *data, uint32_t size, int parse_ofl, int layer3_only, Mp3Frame *frame);
Mp3Decoder *runtime_mp3_open(void);
void runtime_mp3_close(Mp3Decoder *decoder);
void runtime_mp3_reset(Mp3Decoder *decoder);
int runtime_mp3_decode(Mp3Decoder *decoder, const unsigned char *data, int size, float *pcm, int max_samples, int *channels);
uintptr_t runtime_videodec_resolve(const char *name);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
/* probe --gamepad: prints one "Gamepad:" line for the first pad found (returns 1) or none (0). */
int runtime_pad_describe(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
/* OrbisSaveDataParam (1328 bytes) of a save directory of the current title. */
#define RUNTIME_SAVE_PARAM_SIZE 1328
int runtime_savedata_param(int32_t user, const char *dir, void *param);
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size);
int64_t runtime_savedata_mtime(int32_t user, const char *dir);
void runtime_savedata_preview_path(const char *path);
void runtime_thread_attach_host(const char *name);
#endif
