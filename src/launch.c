/* Double-click start (Windows Explorer, a Linux file manager or menu entry, a shortcut). A release
 * folder (tools/package.py) holds this exe, its libraries, release.txt and theatrhythm-setup; the
 * setup adds the prepared game image in game/, settings.ini and paths.ini:
 *     [paths]
 *     game=<the dump: eboot.bin, sce_module, sce_sys, Media>
 *     save=<saves and the launch log: what run.py calls --user>
 *     cache=<optional: shader caches; by default the folder every copy shares, as run.py's>
 *     dlc=<optional: one more folder of DLC, read next to save's addcont, e.g. shadPS4's addcont>
 * Without paths.ini here, the per-user copy the setup also writes is read (user_paths_ini).
 * Narrow strings are UTF-8 (Windows: the manifest's activeCodePage). */
#include "launch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define PATH_CHARS (MAX_PATH * 4)
#define SEP "\\"
#define SETUP "theatrhythm-setup.exe"
#define strcasecmp _stricmp
#else
#include <errno.h>
#include <signal.h>
#include <strings.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <SDL3/SDL.h>
#define PATH_CHARS 4096
#define SEP "/"
#define SETUP "theatrhythm-setup"
#endif

#define TITLE "Theatrhythm Final Bar Line"
#define RUN_SETUP "Run " SETUP " (in the game's folder) to set the game up."

static char exe_dir[PATH_CHARS], game[PATH_CHARS], save[PATH_CHARS], cache[PATH_CHARS], dlc[PATH_CHARS];
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

/* The per-user copy of paths.ini the setup also writes, so a release unpacked into a new folder
 * finds the folders the last setup chose: %APPDATA%\Theatrhythm Final Bar Line\paths.ini, or
 * $XDG_CONFIG_HOME/theatrhythm-fbl/paths.ini (~/.config when unset). "" when neither is known. */
static void user_paths_ini(char *out) {
    out[0] = 0;
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0]) snprintf(out, PATH_CHARS, "%s\\Theatrhythm Final Bar Line\\paths.ini", appdata);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(out, PATH_CHARS, "%s/theatrhythm-fbl/paths.ini", xdg);
    else if (home && home[0]) snprintf(out, PATH_CHARS, "%s/.config/theatrhythm-fbl/paths.ini", home);
#endif
}

static int top_hits_off;

/* game=, save=, cache=, dlc= and top_hits= of paths.ini; 1 when game and save are there. */
static int read_paths(const char *ini) {
    game[0] = save[0] = cache[0] = dlc[0] = 0;
    top_hits_off = 0;
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
        else if (!strcasecmp(key, "dlc")) set_path(dlc, value);
        else if (!strcasecmp(key, "top_hits")) top_hits_off = !strcmp(value, "0");
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

/* The crash watch (THEATRHYTHM_CRASH_WATCH, on unless 0): an unattended start runs the game as a
 * child of this process and waits for it. Whatever ends the game with an exit code other than 0 (a
 * fault the runtime reports, an error exit, or an end no handler sees: a fast fail, abort(), a
 * driver ending the process) is added to the log, the log is kept as log/crash-<time>.log and the
 * player is told where it is. The next start no longer overwrites the only record of a crash, and
 * every crash keeps its own copy (none is ever deleted here: the player may delete them). */

/* What an exit code means, "" when nothing is known about it. */
static void describe_exit(unsigned long code, char *out, size_t size) {
    const char *name = "";
    switch (code) {
    case 1: name = "an error, see the ERROR: line"; break;
    case 3: name = "a fault the runtime reported, see the Fault: lines, or abort()"; break;
    case 20: name = "the runtime stopped, see the STOP: line"; break;
#ifdef _WIN32
    case 0xC0000005: name = "access violation"; break;
    case 0xC000001D: name = "illegal instruction"; break;
    case 0xC0000094: name = "integer divide by zero"; break;
    case 0xC0000096: name = "privileged instruction"; break;
    case 0xC00000FD: name = "stack overflow"; break;
    case 0xC0000135: name = "a DLL is missing"; break;
    case 0xC0000142: name = "a DLL failed to start"; break;
    case 0xC0000374: name = "heap corruption"; break;
    case 0xC0000409: name = "fast fail: a security check or abort()"; break;
    case 0x80000003: name = "breakpoint"; break;
#else
    default:
        if (code > 128 && code < 128 + 65) {
            snprintf(out, size, "signal %lu (%s)", code - 128, strsignal((int)(code - 128)));
            return;
        }
#endif
    }
    snprintf(out, size, "%s", name);
}

/* The game ended with `code`: the end goes at the bottom of the log, the log is copied to
 * log/crash-<time>.log and a message box names that file. */
static void report_crash(const char *log_dir, unsigned long code) {
    char meaning[160], stamp[32], crash[PATH_CHARS], message[3 * PATH_CHARS];
    describe_exit(code, meaning, sizeof meaning);
    time_t now = time(NULL);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    make_dirs(log_dir);
    FILE *log = fopen(log_path, "a");
    if (log) {
        fprintf(log, "Launcher: the game ended with exit code %lu (0x%08lx)%s%s\n", code, code, meaning[0] ? ": " : "", meaning);
        fclose(log);
    }
    snprintf(crash, sizeof crash, "%s" SEP "crash-%s.log", log_dir, stamp);
    FILE *from = fopen(log_path, "rb"), *to = from ? fopen(crash, "wb") : NULL;
    if (to) {
        char buffer[65536];
        for (size_t n; (n = fread(buffer, 1, sizeof buffer, from));) fwrite(buffer, 1, n, to);
        fclose(to);
    }
    if (from) fclose(from);
    if (!to) snprintf(crash, sizeof crash, "%s", log_path);
    snprintf(message, sizeof message,
             "The game closed unexpectedly (exit code 0x%08lx%s%s).\n\nIts log was kept as:\n%s\n\n"
             "Please attach that file when you report the problem at\n"
             "https://github.com/YAPhoa/theatrhythm-fbl-pc/issues",
             code, meaning[0] ? ": " : "", meaning, crash);
    fprintf(stderr, "ERROR: %s\n", message);
#ifdef _WIN32
    strncat(message, "\n\nOpen the log's folder now?", sizeof message - strlen(message) - 1);
    if (MessageBoxA(NULL, message, TITLE, MB_YESNO | MB_ICONERROR) == IDYES) {
        char command[PATH_CHARS + 64];
        snprintf(command, sizeof command, "explorer.exe /select,\"%s\"", crash);
        STARTUPINFOA startup = {.cb = sizeof startup};
        PROCESS_INFORMATION explorer;
        if (CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &explorer)) {
            CloseHandle(explorer.hThread);
            CloseHandle(explorer.hProcess);
        }
    }
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, message, NULL);
#endif
}

/* Starts the game as a child and waits for it: 1 with its exit code in *code once it has ended,
 * 0 when this process is to run the game itself (the child, or no child could be started). */
static int watch_game(unsigned long *code) {
#ifdef _WIN32
    /* The same command line again, in a job that also takes in the process a restart starts
     * (runtime_restart, whose exit is then the one that counts) and that ends with this process. */
    HANDLE job = CreateJobObjectW(NULL, NULL), port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!job || !port) return 0;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT link = {.CompletionKey = job, .CompletionPort = port};
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &link, sizeof link)) return 0;
    _putenv_s("THEATRHYTHM_CRASH_WATCH", "0");
    STARTUPINFOW startup = {.cb = sizeof startup};
    PROCESS_INFORMATION child;
    wchar_t *line = _wcsdup(GetCommandLineW());
    if (!line || !CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &child)) {
        _putenv_s("THEATRHYTHM_CRASH_WATCH", "");
        return 0;
    }
    if (!AssignProcessToJobObject(job, child.hProcess)) {
        TerminateProcess(child.hProcess, 0);
        _putenv_s("THEATRHYTHM_CRASH_WATCH", "");
        return 0;
    }
    AllowSetForegroundWindow(child.dwProcessId);
    ResumeThread(child.hThread);
    CloseHandle(child.hThread);
    HANDLE last = child.hProcess;
    DWORD message;
    ULONG_PTR key;
    LPOVERLAPPED data;
    while (GetQueuedCompletionStatus(port, &message, &key, &data, INFINITE)) {
        DWORD pid = (DWORD)(ULONG_PTR)data;
        if (message == JOB_OBJECT_MSG_NEW_PROCESS && pid != child.dwProcessId) {
            HANDLE next = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (next) last = next;
        } else if (message == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) break;
    }
    WaitForSingleObject(last, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(last, &exit_code);
    *code = exit_code;
    return 1;
#else
    /* The child returns to run the game and ends with this process (a restart execs in place). */
    setenv("THEATRHYTHM_CRASH_WATCH", "0", 1);
    pid_t pid = fork();
    if (pid < 0) unsetenv("THEATRHYTHM_CRASH_WATCH");
    if (pid <= 0) {
        if (!pid) prctl(PR_SET_PDEATHSIG, SIGTERM);
        return 0;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    *code = WIFSIGNALED(status) ? 128 + (unsigned long)WTERMSIG(status) : (unsigned long)WEXITSTATUS(status);
    return 1;
#endif
}

/* An exit code that is not a crash: 0, or the player or the system ending the game. */
static int quiet_exit(unsigned long code) {
#ifdef _WIN32
    return !code || code == 0xC000013A /* STATUS_CONTROL_C_EXIT */;
#else
    return !code || code == 128 + SIGTERM || code == 128 + SIGINT;
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
        user_paths_ini(ini);
        if (!ini[0] || !read_paths(ini)) {
            complain("The game is not set up yet.\n\n" RUN_SETUP);
            return 1;
        }
    }
    /* THEATRHYTHM_SAVE_DIR overrides save= (a relative one is relative to the exe's folder). */
    const char *save_override = getenv("THEATRHYTHM_SAVE_DIR");
    if (save_override && save_override[0]) set_path(save, save_override);
    /* THEATRHYTHM_DLC_DIR overrides dlc= the same way. */
    const char *dlc_override = getenv("THEATRHYTHM_DLC_DIR");
    if (dlc_override && dlc_override[0]) set_path(dlc, dlc_override);
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
        const char *watch = getenv("THEATRHYTHM_CRASH_WATCH");
        unsigned long code = 0;
        if ((!watch || strcmp(watch, "0")) && watch_game(&code)) {
            if (!quiet_exit(code)) report_crash(log_dir, code);
            exit((int)code);
        }
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
    if (top_hits_off) set_env("THEATRHYTHM_TOP_HITS", "0"); /* the setup's Patches box (src/probe.c) */

    int count = 0;
    args[count++] = (*argv)[0];
    args[count++] = boot;
    args[count++] = "--content-profile"; args[count++] = profile;
    args[count++] = "--app0"; args[count++] = game;
    args[count++] = "--user"; args[count++] = save;
    if (dlc[0]) { args[count++] = "--dlc"; args[count++] = dlc; }
    args[count++] = "--timeout"; args[count++] = "0";
    if (exists(isa)) { args[count++] = "--isa-rewrite"; args[count++] = isa; }
    for (int i = 1; i < *argc && count < (int)(sizeof args / sizeof *args) - 1; ++i)
        if (strcmp((*argv)[i], "--console")) args[count++] = (*argv)[i];
    args[count] = NULL;
    printf("Launch: %s (%s)\nLaunch: game %s\nLaunch: save %s\nLaunch: cache %s\n", ini, current, game, save, cache);
    if (dlc[0]) printf("Launch: dlc %s\n", dlc);
    *argc = count;
    *argv = args;
    return 0;
}
