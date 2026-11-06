// SPDX-License-Identifier: GPL-2.0-or-later
#include "setup.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#endif

#include <cwctype>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace setup {
namespace {

// What run.py reads from the dump to prepare the image (IMAGE_INPUTS), and the title's param.sfo.
constexpr const wchar_t* kGameFiles[] = {
    L"eboot.bin",
    L"sce_sys/param.sfo",
    L"sce_module/libc.prx",
    L"sce_module/libSceFios2.prx",
    L"Media/Modules/PS4Util.prx",
    L"Media/Modules/Il2CppUserAssemblies.prx",
};
constexpr wchar_t kIncomplete[] = L"\n\nThis folder is incomplete: unpack the whole release again.";
#ifdef _WIN32
constexpr wchar_t kSeparator[] = L"\\";
constexpr char kNewline[] = "\r\n";
#else
constexpr wchar_t kSeparator[] = L"/";
constexpr char kNewline[] = "\n";
#endif

[[noreturn]] void fail(std::wstring message) {
    throw Error{std::move(message)};
}

// Absolute and normalized, without a trailing separator.
fs::path clean(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec).lexically_normal();
    while (!a.has_filename() && a.has_relative_path())
        a = a.parent_path();
    return a;
}

// Windows paths are case-insensitive; Linux paths are not.
std::wstring lower(std::wstring s) {
#ifdef _WIN32
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
#endif
    return s;
}

// `inner` is `outer` or a folder inside it.
bool inside(const fs::path& inner, const fs::path& outer) {
    std::wstring i = lower(text(clean(inner))), o = lower(text(clean(outer)));
    return i == o || i.rfind(o + kSeparator, 0) == 0;
}

std::wstring join(const std::vector<std::wstring>& items) {
    std::wstring s;
    for (const std::wstring& item : items)
        s += (s.empty() ? L"" : L", ") + item;
    return s;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        fail(L"cannot read " + text(p));
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f)
        fail(L"cannot write " + setup::text(p) + L"\nIf the game is running, close it and try again.");
}

// --- child processes ----------------------------------------------------------------------------

// Passes each line of a child's output to `log` (and `output`, if given).
class LineSink {
public:
    LineSink(const Log& log, std::wstring* output) : log_(log), output_(output) {}
    void add(const char* bytes, size_t n) {
        pending_.append(bytes, n);
        for (size_t end; (end = pending_.find('\n')) != std::string::npos; pending_.erase(0, end + 1))
            emit(std::string_view(pending_).substr(0, end));
    }
    void finish() {
        if (!pending_.empty())
            emit(pending_);
    }

private:
    void emit(std::string_view bytes) {
        std::wstring line = widen(bytes);
        while (!line.empty() && line.back() == L'\r')
            line.pop_back();
        log_(L"  " + line);
        if (output_)
            *output_ += line + L"\n";
    }
    const Log& log_;
    std::wstring* output_;
    std::string pending_;
};

#ifdef _WIN32
// One argument as CommandLineToArgvW reads it back.
std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}

// Runs `args` in `cwd` without a console window and passes each line it prints to `log`.
// Returns the exit code; `output` (if given) collects the text.
unsigned long run(const std::vector<std::wstring>& args, const fs::path& cwd, const Log& log, std::wstring* output = nullptr) {
    std::wstring line;
    for (const std::wstring& a : args)
        line += (line.empty() ? L"" : L" ") + quote(a);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0))
        fail(L"cannot start " + args[0]);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi = {};
    BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                  cwd.c_str(), &si, &pi);
    CloseHandle(write);
    if (!started) {
        CloseHandle(read);
        fail(L"cannot start " + line);
    }
    LineSink sink(log, output);
    char buf[4096];
    DWORD n;
    while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n)
        sink.add(buf, n);
    sink.finish();
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

fs::path search_path(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = SearchPathW(nullptr, name, nullptr, static_cast<DWORD>(std::size(buf)), buf, nullptr);
    return n && n < std::size(buf) ? fs::path(std::wstring(buf, n)) : fs::path();
}

// The Python that runs prepare\run.py: the release's own (prepare\python, the python.org embeddable
// package), else the player's: the py launcher, else python.exe (not the Store's WindowsApps alias).
std::vector<std::wstring> python_command(const fs::path& prepare) {
    std::error_code ec;
    if (fs::path own = prepare / L"python" / L"python.exe"; fs::is_regular_file(own, ec))
        return {own.wstring()};
    if (fs::path py = search_path(L"py.exe"); !py.empty())
        return {py.wstring(), L"-3"};
    if (fs::path python = search_path(L"python.exe");
        !python.empty() && lower(python.wstring()).find(L"\\windowsapps\\") == std::wstring::npos)
        return {python.wstring()};
    fail(L"Setup needs Python 3.10 or newer to prepare the game. Install it from python.org, then run setup again.");
}
#else
// Runs `args` in `cwd` and passes each line it prints to `log`. Returns the exit code (128 + the
// signal when it was killed); `output` (if given) collects the text.
unsigned long run(const std::vector<std::wstring>& args, const fs::path& cwd, const Log& log, std::wstring* output = nullptr) {
    std::vector<std::string> narrow_args;
    for (const std::wstring& a : args)
        narrow_args.push_back(narrow(a));
    std::vector<char*> argv;
    for (std::string& a : narrow_args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    int fds[2];
    if (pipe2(fds, O_CLOEXEC))
        fail(L"cannot start " + args[0]);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        if (!chdir(cwd.c_str()))
            execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        fail(L"cannot start " + args[0]);
    }
    LineSink sink(log, output);
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) != 0;) {
        if (n > 0)
            sink.add(buf, static_cast<size_t>(n));
        else if (errno != EINTR)
            break;
    }
    sink.finish();
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

fs::path search_path(const char* name) {
    const char* path = std::getenv("PATH");
    std::string_view dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
    while (!dirs.empty()) {
        size_t colon = dirs.find(':');
        std::string_view dir = dirs.substr(0, colon);
        dirs = colon == std::string_view::npos ? std::string_view() : dirs.substr(colon + 1);
        fs::path candidate = fs::path(std::string(dir.empty() ? "." : dir)) / name;
        if (!access(candidate.c_str(), X_OK))
            return candidate;
    }
    return {};
}

// The Python that runs prepare/run.py: the release's own (prepare/python/bin/python3) if it has one,
// else the system's python3.
std::vector<std::wstring> python_command(const fs::path& prepare) {
    std::error_code ec;
    if (fs::path own = prepare / "python" / "bin" / "python3"; fs::is_regular_file(own, ec))
        return {text(own)};
    for (const char* name : {"python3", "python"})
        if (fs::path python = search_path(name); !python.empty())
            return {text(python)};
    fail(L"Setup needs Python 3.10 or newer to prepare the game. Install python3 with your distribution's "
         L"package manager, then run setup again.");
}
#endif

// --- paths.ini ----------------------------------------------------------------------------------

std::wstring strip(const std::wstring& s) {
    constexpr wchar_t ws[] = L" \t\r\n\"";
    size_t a = s.find_first_not_of(ws);
    if (a == std::wstring::npos)
        return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

void write_paths(const fs::path& ini, const fs::path& game, const fs::path& save, const fs::path& dlc) {
#ifdef _WIN32
    const std::string setup = "theatrhythm-setup.exe", cache = "%LOCALAPPDATA%\\Theatrhythm Final Bar Line";
#else
    const std::string setup = "theatrhythm-setup", cache = "~/.cache/theatrhythm-fbl";
#endif
    const std::string nl = kNewline;
    write_text(ini, "; Written by " + setup + ". Run it again to change these folders." + nl +
                    "; game: your dump (eboot.bin, sce_module, sce_sys, Media). It is only ever read." + nl +
                    "; save: your saves, and theatrhythm.log in its log folder." + nl +
                    "; dlc: optional, one more folder of DLC (e.g. shadPS4's addcont), next to save's addcont." + nl +
                    "; Shader caches are shared by every copy of the game: " + cache + nl +
                    "; (cache=<folder> moves them)." + nl +
                    "[paths]" + nl +
                    "game=" + narrow(text(game)) + nl +
                    "save=" + narrow(text(save)) + nl +
                    (dlc.empty() ? std::string() : "dlc=" + narrow(text(dlc)) + nl));
}

// The key of a settings.ini line ("key=value", gpu/shim/theatrhythm_settings.cpp), lower case; empty
// for comments and other lines.
std::wstring setting_key(std::string_view line) {
    size_t eq = line.find('=');
    std::wstring key = eq == std::string_view::npos ? std::wstring() : lower(strip(widen(line.substr(0, eq))));
    return !key.empty() && key[0] != L'#' && key[0] != L';' ? key : std::wstring();
}

// `key`'s value in a settings.ini: the last line that sets it wins, as in the game.
std::optional<std::wstring> read_setting(const fs::path& ini, const std::wstring& key) {
    std::error_code ec;
    if (!fs::is_regular_file(ini, ec))
        return std::nullopt;
    std::string text = read_text(ini);
    std::optional<std::wstring> value;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        std::string_view line = std::string_view(text).substr(start, end == std::string::npos ? end : end - start);
        start = end == std::string::npos ? text.size() : end + 1;
        if (setting_key(line) == key)
            value = strip(widen(line.substr(line.find('=') + 1)));
    }
    return value;
}

// Sets `key` in a settings.ini: its first line is replaced and any later ones dropped, or the line is
// added at the end. The other lines and their line endings stay.
void write_setting(const fs::path& ini, const std::wstring& key, const std::wstring& value) {
    std::string text = read_text(ini), out, line_text = narrow(key) + "=" + narrow(value);
    bool done = false;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        std::string_view line = std::string_view(text).substr(start, end == std::string::npos ? end : end - start);
        start = end == std::string::npos ? text.size() : end + 1;
        bool cr = !line.empty() && line.back() == '\r';
        if (setting_key(line) == key) {
            if (done)
                continue;
            out += line_text + (cr ? "\r" : "");
            done = true;
        } else {
            out += line;
        }
        if (end != std::string::npos)
            out += '\n';
    }
    if (!done) {
        if (!out.empty() && out.back() != '\n')
            out += kNewline;
        out += line_text + kNewline;
    }
    write_text(ini, out);
}

#ifndef _WIN32
// A desktop entry's quoted argument: ", `, $ and \ escaped inside the quotes, \ doubled again for
// the string value itself, and % doubled (field codes).
std::string desktop_quote(const std::string& arg) {
    std::string q = "\"";
    for (char c : arg) {
        if (c == '\\')
            q += "\\\\\\\\";
        else if (c == '"' || c == '`' || c == '$')
            q += std::string("\\") + c;
        else if (c == '%')
            q += "%%";
        else
            q += c;
    }
    return q + "\"";
}

// $HOME, or the current folder when it is unset.
fs::path home() {
    const char* h = std::getenv("HOME");
    return h && h[0] ? fs::path(h) : fs::current_path();
}
#endif

} // namespace

#ifdef _WIN32
std::wstring widen(std::string_view s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), n, nullptr, nullptr);
    return u;
}

std::wstring text(const fs::path& p) {
    return p.wstring();
}

fs::path path_of(const std::wstring& s) {
    return fs::path(s);
}
#else
// UTF-8 to UTF-32 (wchar_t); a malformed byte becomes U+FFFD.
std::wstring widen(std::string_view s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        char32_t code = extra == 0 ? c : extra == 1 ? c & 0x1F : extra == 2 ? c & 0x0F : c & 0x07;
        bool ok = extra >= 0 && i + static_cast<size_t>(extra) < s.size();
        for (int k = 1; ok && k <= extra; ++k) {
            unsigned char next = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            ok = (next & 0xC0) == 0x80;
            code = (code << 6) | (next & 0x3F);
        }
        if (!ok) {
            w += L'�';
            ++i;
            continue;
        }
        w += static_cast<wchar_t>(code);
        i += static_cast<size_t>(extra) + 1;
    }
    return w;
}

std::string narrow(std::wstring_view s) {
    std::string u;
    for (wchar_t wc : s) {
        auto c = static_cast<char32_t>(wc);
        if (c < 0x80) {
            u += static_cast<char>(c);
        } else if (c < 0x800) {
            u += static_cast<char>(0xC0 | (c >> 6));
            u += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            u += static_cast<char>(0xE0 | (c >> 12));
            u += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            u += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            u += static_cast<char>(0xF0 | (c >> 18));
            u += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            u += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            u += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return u;
}

std::wstring text(const fs::path& p) {
    return widen(p.native());
}

fs::path path_of(const std::wstring& s) {
    return fs::path(narrow(s));
}
#endif

fs::path exe_folder() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
#else
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : exe.parent_path();
#endif
}

bool same_path(const fs::path& a, const fs::path& b) {
    return lower(text(clean(a))) == lower(text(clean(b)));
}

fs::path game_folder(const fs::path& path) {
    std::error_code ec;
    fs::path p = clean(path);
    return fs::is_regular_file(p, ec) ? p.parent_path() : p;
}

fs::path user_paths_ini() {
#ifdef _WIN32
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    return appdata && appdata[0] ? fs::path(appdata) / L"Theatrhythm Final Bar Line" / L"paths.ini" : fs::path();
#else
    const char* config = std::getenv("XDG_CONFIG_HOME");
    return (config && config[0] ? fs::path(config) : home() / ".config") / "theatrhythm-fbl" / "paths.ini";
#endif
}

namespace {

// game=, save= and dlc= of `ini`; relative values are relative to `root`.
std::optional<Paths> read_paths_file(const fs::path& ini, const fs::path& root) {
    std::error_code ec;
    if (ini.empty() || !fs::is_regular_file(ini, ec))
        return std::nullopt;
    std::string text = read_text(ini);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0)
        text.erase(0, 3);
    Paths paths;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        std::wstring line = widen(std::string_view(text).substr(start, end == std::string::npos ? end : end - start));
        start = end == std::string::npos ? text.size() : end + 1;
        size_t eq = line.find(L'=');
        std::wstring key = lower(strip(line.substr(0, eq)));
        if (eq == std::wstring::npos || key.empty() || key[0] == L';' || key[0] == L'#')
            continue;
        std::wstring value = strip(line.substr(eq + 1));
        if (value.empty())
            continue;
        if (key == L"game")
            paths.game = clean(root / path_of(value));
        else if (key == L"save")
            paths.save = clean(root / path_of(value));
        else if (key == L"dlc")
            paths.dlc = clean(root / path_of(value));
    }
    if (paths.game.empty() || paths.save.empty())
        return std::nullopt;
    return paths;
}

} // namespace

std::optional<Paths> read_paths(const fs::path& root) {
    return read_paths_file(root / L"paths.ini", root);
}

std::optional<Paths> remembered_paths(const fs::path& root) {
    if (auto paths = read_paths(root))
        return paths;
    try {
        return read_paths_file(user_paths_ini(), root);
    } catch (const Error&) {
        return std::nullopt;
    }
}

fs::path default_save() {
    return exe_folder() / L"save";
}

bool ready(const fs::path& root) {
    std::error_code ec;
    fs::path stamp = root / L"game" / L"release.txt", release = root / L"release.txt";
    try {
        return read_paths(root) && fs::is_regular_file(root / kGameExe, ec) && fs::is_regular_file(stamp, ec) &&
               fs::is_regular_file(release, ec) && read_text(stamp) == read_text(release);
    } catch (const Error&) {
        return false;
    }
}

bool skip_opening(const fs::path& root, const fs::path& save) {
    std::error_code ec;
    fs::path ini = save / L"settings.ini";
    if (!fs::is_regular_file(ini, ec))
        ini = root / L"settings.ini";
    try {
        std::optional<std::wstring> value = read_setting(ini, L"skip_opening");
        std::wstring v = value ? lower(*value) : L"";
        return v == L"1" || v == L"true" || v == L"yes" || v == L"on";
    } catch (const Error&) {
        return false;
    }
}

bool has_saves(const fs::path& save) {
    std::error_code ec;
    fs::path data = save / L"savedata";
    return fs::is_directory(data, ec) && !fs::is_empty(data, ec);
}

fs::path install(const Options& opt, const Log& log) {
    fs::path root = clean(opt.root), game = game_folder(opt.game), save = clean(opt.save);
    fs::path dlc = opt.dlc.empty() ? fs::path() : clean(opt.dlc);
    fs::path prepare = root / L"prepare", image = root / L"game", exe = root / kGameExe;
    if (inside(save, game))
        fail(L"Choose a save folder outside the game folder. Setup never writes into the game folder.");
    if (inside(root, game) || inside(game, root))
        fail(L"Keep the game folder and this folder apart (neither inside the other).");
    std::error_code dlc_ec;
    if (!dlc.empty() && !fs::is_directory(dlc, dlc_ec))
        fail(L"The DLC folder does not exist:\n" + text(dlc) + L"\n\nChoose another one, or leave it empty.");
    if (!dlc.empty() && same_path(dlc, save / L"addcont"))
        dlc.clear(); // the save folder's addcont is always read

    std::error_code ec;
    std::vector<std::wstring> missing;
    for (const wchar_t* f : kGameFiles)
        if (!fs::is_regular_file(game / f, ec))
            missing.push_back(text(fs::path(f).make_preferred()));
    if (!missing.empty())
        fail(L"This folder isn't a decrypted Theatrhythm Final Bar Line dump:\n" + text(game) +
             L"\n\nMissing: " + join(missing));
    for (const fs::path& f : {exe, prepare / L"run.py", root / L"release.txt"})
        if (!fs::is_regular_file(f, ec))
            fail(text(f) + L" is missing." + kIncomplete);
    std::vector<std::wstring> python = python_command(prepare);

    log(L"Preparing the game from " + text(game));
    fs::remove(image / L"release.txt", ec); // the image is this release's only once it is complete
    std::vector<std::wstring> command = python;
    command.insert(command.end(), {text(prepare / L"run.py"), L"prepare", L"--game", text(game),
                                   L"--out", text(image)});
#ifdef _WIN32
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
#else
    setenv("PYTHONIOENCODING", "utf-8", 1);
#endif
    if (unsigned long code = run(command, prepare, log))
        fail(L"Preparing the game failed (exit code " + std::to_wstring(code) + L"). The log says why.");

    // On a CPU without some instruction sets the game uses, the sites that use them are rewritten
    // (run.py's play does this at every start; here once).
    std::wstring cpu;
    unsigned long code = run({text(exe), L"--cpu-features"}, root, log, &cpu);
    if (code == 20)
        fail(L"This PC's CPU cannot run the game:\n" + cpu);
    if (code)
        fail(L"The game does not start (exit code " + std::to_wstring(code) + L")." + kIncomplete);
    if (cpu.find(L"CPU: missing") != std::wstring::npos) {
        command = python;
        command.insert(command.end(), {text(prepare / L"scripts" / L"isa_rewrite.py"),
                                       text(image / L"boot-linked.bin"), L"--out", text(image)});
        if (run(command, prepare, log))
            fail(L"This CPU lacks instructions the game uses, and rewriting them failed. That step needs the "
                 L"Python package iced-x86 (pip install iced-x86); the log says more.");
    } else {
        fs::remove(image / L"isa-rewrite.bin", ec);
    }
    write_text(image / L"release.txt", read_text(root / L"release.txt"));
    log(L"prepared " + text(image));

    fs::path settings = root / L"settings.ini";
    if (!fs::exists(settings, ec)) {
        if (!fs::copy_file(prepare / L"settings.example.ini", settings, ec))
            fail(L"cannot write " + text(settings));
        log(L"wrote    settings.ini");
    }

    fs::create_directories(save, ec);
    if (!fs::is_directory(save, ec))
        fail(L"Cannot create the save folder " + text(save));
    if (opt.copy_saves_from && !same_path(*opt.copy_saves_from, save)) {
        for (const wchar_t* f : kSaveFolders) {
            fs::path from = *opt.copy_saves_from / f;
            if (!fs::is_directory(from, ec))
                continue;
            fs::copy(from, save / f, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
            if (ec)
                fail(L"Cannot copy " + text(from) + L" to " + text(save) + L": " + widen(ec.message()));
        }
        log(L"copied   saves from " + text(*opt.copy_saves_from));
    }
    // The game reads the save folder's settings.ini once it has one (the in-game Display Settings
    // write it), else this folder's: set both.
    if (opt.skip_opening) {
        const std::wstring value = *opt.skip_opening ? L"1" : L"0";
        for (const fs::path& ini : {settings, save / L"settings.ini"}) {
            if (!fs::is_regular_file(ini, ec))
                continue;
            write_setting(ini, L"skip_opening", value);
            log(L"set      skip_opening=" + value + L" in " + text(ini));
        }
    }
    write_paths(root / L"paths.ini", game, save, dlc);
    log(L"wrote    paths.ini (game " + text(game) + L", save " + text(save) +
        (dlc.empty() ? L"" : L", dlc " + text(dlc)) + L")");
    // The per-user copy, for the next release unpacked elsewhere; the game runs without it.
    if (fs::path user = user_paths_ini(); !user.empty()) {
        try {
            fs::create_directories(user.parent_path(), ec);
            write_paths(user, clean(game), clean(save), dlc);
            log(L"wrote    " + text(user));
        } catch (const Error& e) {
            log(L"skipped  " + text(user) + L" (" + e.message + L")");
        }
    }
    return exe;
}

#ifdef _WIN32
void create_shortcut(const fs::path& target, const fs::path& lnk) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(target.parent_path().c_str());
        link->SetIconLocation(target.c_str(), 0);
        link->SetDescription(L"Theatrhythm Final Bar Line");
        hr = link->QueryInterface(IID_PPV_ARGS(&file));
    }
    if (SUCCEEDED(hr))
        hr = file->Save(lnk.c_str(), TRUE);
    if (file)
        file->Release();
    if (link)
        link->Release();
    if (SUCCEEDED(init))
        CoUninitialize();
    if (FAILED(hr)) {
        wchar_t code[16];
        swprintf(code, std::size(code), L"%08X", static_cast<unsigned>(hr));
        fail(L"Cannot create the shortcut " + lnk.wstring() + L" (error 0x" + code + L")");
    }
}

fs::path desktop_folder() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) {
        CoTaskMemFree(p);
        fail(L"Cannot find the Desktop folder");
    }
    fs::path desktop = p;
    CoTaskMemFree(p);
    return desktop;
}
#else
// A desktop entry that starts the game in its folder; executable, which desktops ask of entries on
// the Desktop before they trust them.
void create_shortcut(const fs::path& target, const fs::path& lnk) {
    std::error_code ec;
    fs::path folder = target.parent_path();
    std::string entry = "[Desktop Entry]\n"
                        "Type=Application\n"
                        "Name=Theatrhythm Final Bar Line\n"
                        "Exec=" + desktop_quote(target.native()) + "\n"
                        "Path=" + folder.native() + "\n"
                        "Terminal=false\n"
                        "Categories=Game;\n";
    if (fs::path icon = folder / "theatrhythm-fbl.png"; fs::is_regular_file(icon, ec))
        entry += "Icon=" + icon.native() + "\n";
    fs::create_directories(lnk.parent_path(), ec);
    write_text(lnk, entry);
    fs::permissions(lnk, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
}

// XDG_DESKTOP_DIR of ~/.config/user-dirs.dirs (xdg-user-dirs), else ~/Desktop.
fs::path desktop_folder() {
    const char* config = std::getenv("XDG_CONFIG_HOME");
    fs::path dirs = (config && config[0] ? fs::path(config) : home() / ".config") / "user-dirs.dirs";
    std::ifstream f(dirs);
    for (std::string line; std::getline(f, line);) {
        constexpr std::string_view key = "XDG_DESKTOP_DIR=\"";
        if (line.rfind(key, 0) != 0 || line.back() != '"')
            continue;
        std::string value = line.substr(key.size(), line.size() - key.size() - 1);
        if (value.rfind("$HOME", 0) == 0)
            return home() / fs::path(value.substr(5)).relative_path();
        if (!value.empty() && value[0] == '/')
            return value;
    }
    return home() / "Desktop";
}

fs::path applications_folder() {
    const char* data = std::getenv("XDG_DATA_HOME");
    return (data && data[0] ? fs::path(data) : home() / ".local" / "share") / "applications";
}
#endif

} // namespace setup
