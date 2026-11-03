/* Double-click start (Windows Explorer, a Linux file manager or menu entry, a shortcut). A release
 * folder (tools/package.py) holds this exe, its libraries, release.txt and theatrhythm-setup; the
 * setup adds the prepared game image in game/, settings.ini and paths.ini:
 *     [paths]
 *     game=<the dump: eboot.bin, sce_module, sce_sys, Media>
 *     save=<saves and the launch log: what run.py calls --user>
 *     cache=<optional: shader caches; by default the folder every copy shares, as run.py's>
 * Narrow strings are UTF-8 (Windows: the manifest's activeCodePage). */
#include "launch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define PATH_CHARS (MAX_PATH * 4)
#define SEP "\\"
#define SETUP "theatrhythm-setup.exe"
#define strcasecmp _stricmp
#else
#include <errno.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>
#define PATH_CHARS 4096
#define SEP "/"
#define SETUP "theatrhythm-setup"
#endif

#define TITLE "Theatrhythm Final Bar Line"
#define RUN_SETUP "Run " SETUP " (in the game's folder) to set the game up."

static char exe_dir[PATH_CHARS], game[PATH_CHARS], save[PATH_CHARS], cache[PATH_CHARS];
static char boot[PATH_CHARS], profile[PATH_CHARS], isa[PATH_CHARS], log_path[PATH_CHARS];
static char *args[64];
static int unattended; /* started from Explorer, a file manager or a shortcut: nobody reads the output */

static void complain(const char *message) {
    fprintf(stderr, "ERROR: %s\n", message);
#ifdef _WIN32
    if (unattended) MessageBoxA(NULL, message, TITLE, MB_OK | MB_ICONERROR);
#else
    if (unattended) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, message, NULL);
#endif
}

#ifdef _WIN32
static int exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
static int make_dir(const char *path) { return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }
static int is_absolute(const char *path) { return (path[0] && path[1] == ':') || path[0] == '\\' || path[0] == '/'; }
#else
static int exists(const char *path) { struct stat st; return !stat(path, &st); }
static int make_dir(const char *path) { return !mkdir(path, 0755) || errno == EEXIST; }
static int is_absolute(const char *path) { return path[0] == '/'; }
#endif

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    size_t n = strlen(s);
    while (n && strchr(" \t\r\n", s[n - 1])) s[--n] = 0;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s[n - 1] = 0; ++s; }
    return s;
}

/* A relative value is relative to the exe's folder. */
static void set_path(char *out, const char *value) {
    if (is_absolute(value)) snprintf(out, PATH_CHARS, "%s", value);
    else snprintf(out, PATH_CHARS, "%s" SEP "%s", exe_dir, value);
}

/* run.py's shared_cache_dir: one folder per PC for every copy of the game. */
static void shared_cache(char *out) {
#ifdef _WIN32
    const char *local = getenv("LOCALAPPDATA");
    if (local && local[0]) snprintf(out, PATH_CHARS, "%s\\Theatrhythm Final Bar Line", local);
#else
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(out, PATH_CHARS, "%s/theatrhythm-fbl", xdg);
    else if (home && home[0]) snprintf(out, PATH_CHARS, "%s/.cache/theatrhythm-fbl", home);
#endif
    else snprintf(out, PATH_CHARS, "%s" SEP "cache", exe_dir);
}

/* game=, save= and cache= of paths.ini; 1 when game and save are there. */
static int read_paths(const char *ini) {
    FILE *f = fopen(ini, "rb");
    if (!f) return 0;
    char line[PATH_CHARS + 64];
    while (fgets(line, sizeof line, f)) {
        char *key = line;
        if (!memcmp(key, "\xEF\xBB\xBF", 3)) key += 3;
        char *eq = strchr(key, '=');
        key = trim(key);
        if (!eq || *key == ';' || *key == '#' || *key == '[') continue;
        *eq = 0;
        key = trim(key);
        char *value = trim(eq + 1);
        if (!value[0]) continue;
        if (!strcasecmp(key, "game")) set_path(game, value);
        else if (!strcasecmp(key, "save")) set_path(save, value);
        else if (!strcasecmp(key, "cache")) set_path(cache, value);
    }
    fclose(f);
    if (!cache[0]) shared_cache(cache);
    return game[0] && save[0];
}

/* The first line of a small text file, or "" when it is missing. */
static void first_line(const char *path, char *out, size_t size) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (!fgets(out, (int)size, f)) out[0] = 0;
    fclose(f);
    char *text = trim(out);
    memmove(out, text, strlen(text) + 1);
}

/* Creates the folder and its missing parents. */
static int make_dirs(const char *path) {
    char part[PATH_CHARS];
    snprintf(part, sizeof part, "%s", path);
    for (char *p = part + 1; *p; ++p) {
        if (*p != '\\' && *p != '/') continue;
        if (p[-1] == ':') continue;
        char c = *p;
        *p = 0;
        make_dir(part);
        *p = c;
    }
    return make_dir(part);
}

/* The folder of this exe (UTF-8, no trailing separator); 0 when it cannot be found. */
static int find_exe_dir(void) {
#ifdef _WIN32
    wchar_t wide[PATH_CHARS];
    DWORD n = GetModuleFileNameW(NULL, wide, PATH_CHARS);
    if (!n || n == PATH_CHARS || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, exe_dir, sizeof exe_dir, NULL, NULL)) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof exe_dir - 1);
    if (n <= 0 || n == (ssize_t)sizeof exe_dir - 1) return 0;
    exe_dir[n] = 0;
#endif
    *strrchr(exe_dir, SEP[0]) = 0;
    return 1;
}

static void set_env(const char *name, const char *value) {
    if (getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

int launch_from_paths_ini(int *argc, char ***argv) {
    int console = 0;
    for (int i = 1; i < *argc; ++i) console |= !strcmp((*argv)[i], "--console");
#ifdef _WIN32
    /* The exe is a GUI program (SDL's -mwindows): started from Explorer it has no console and no
     * standard handles; run.py and shells that redirect give it theirs. */
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    unattended = (!out || out == INVALID_HANDLE_VALUE) && !GetConsoleWindow();
    if (console && AllocConsole() && freopen("CONOUT$", "w", stdout) && freopen("CONOUT$", "w", stderr)) {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
#else
    /* A file manager, menu entry or Steam gives no terminal; --console keeps the output where it is. */
    unattended = !isatty(STDOUT_FILENO) && !isatty(STDERR_FILENO);
#endif
    if (!find_exe_dir()) {
        complain("Cannot find the folder of theatrhythm-fbl.");
        return 1;
    }

    char ini[PATH_CHARS], message[3 * PATH_CHARS];
    snprintf(ini, sizeof ini, "%s" SEP "paths.ini", exe_dir);
    if (!read_paths(ini)) {
        complain("The game is not set up yet.\n\n" RUN_SETUP);
        return 1;
    }
    snprintf(boot, sizeof boot, "%s" SEP "game" SEP "boot-linked.bin", exe_dir);
    snprintf(profile, sizeof profile, "%s" SEP "game" SEP "content.bin", exe_dir);
    snprintf(isa, sizeof isa, "%s" SEP "game" SEP "isa-rewrite.bin", exe_dir);
    /* The image is built by this release's scripts for this exe: after a new release is unpacked
     * over an old one, the setup has to prepare it again. */
    char release[PATH_CHARS], prepared_for[256], current[256];
    snprintf(release, sizeof release, "%s" SEP "game" SEP "release.txt", exe_dir);
    first_line(release, prepared_for, sizeof prepared_for);
    snprintf(release, sizeof release, "%s" SEP "release.txt", exe_dir);
    first_line(release, current, sizeof current);
    if (!exists(boot) || !exists(profile) || strcmp(prepared_for, current)) {
        complain("This version of the game is not set up yet.\n\n" RUN_SETUP);
        return 1;
    }
    char eboot[PATH_CHARS];
    snprintf(eboot, sizeof eboot, "%s" SEP "eboot.bin", game);
    if (!exists(eboot)) {
        snprintf(message, sizeof message, "The game folder is gone or has no eboot.bin:\n%s\n\n" RUN_SETUP, game);
        complain(message);
        return 1;
    }
    if (!make_dirs(save)) {
        snprintf(message, sizeof message, "Cannot create the save folder:\n%s\n\n" RUN_SETUP, save);
        complain(message);
        return 1;
    }

    if (unattended && !console) {
        char log_dir[PATH_CHARS];
        snprintf(log_dir, sizeof log_dir, "%s" SEP "log", save);
        snprintf(log_path, sizeof log_path, "%s" SEP "theatrhythm.log", log_dir);
        FILE *log = make_dirs(log_dir) ? fopen(log_path, "w") : NULL;
        if (log) {
            fclose(log);
            /* Append mode for both, so their lines interleave instead of overwriting each other. */
            if (freopen(log_path, "a", stdout) && freopen(log_path, "a", stderr)) {
                setvbuf(stdout, NULL, _IONBF, 0);
                setvbuf(stderr, NULL, _IONBF, 0);
            }
        }
    }

    /* settings.ini: the save folder's, so the options move with the saves (the in-game pages write
     * there); until it has one, the one the setup wrote next to the exe is read. */
    char config[PATH_CHARS], config_default[PATH_CHARS];
    snprintf(config, sizeof config, "%s" SEP "settings.ini", save);
    snprintf(config_default, sizeof config_default, "%s" SEP "settings.ini", exe_dir);
    set_env("THEATRHYTHM_CONFIG", config);
    set_env("THEATRHYTHM_CONFIG_DEFAULT", config_default);
    /* Before anything asks the GPU library for its folders (shadps4 common/path_util.cpp). */
    set_env("THEATRHYTHM_GPU_USER_DIR", cache);

    int count = 0;
    args[count++] = (*argv)[0];
    args[count++] = boot;
    args[count++] = "--content-profile"; args[count++] = profile;
    args[count++] = "--app0"; args[count++] = game;
    args[count++] = "--user"; args[count++] = save;
    args[count++] = "--timeout"; args[count++] = "0";
    if (exists(isa)) { args[count++] = "--isa-rewrite"; args[count++] = isa; }
    for (int i = 1; i < *argc && count < (int)(sizeof args / sizeof *args) - 1; ++i)
        if (strcmp((*argv)[i], "--console")) args[count++] = (*argv)[i];
    args[count] = NULL;
    printf("Launch: %s (%s)\nLaunch: game %s\nLaunch: save %s\nLaunch: cache %s\n", ini, current, game, save, cache);
    *argc = count;
    *argv = args;
    return 0;
}
