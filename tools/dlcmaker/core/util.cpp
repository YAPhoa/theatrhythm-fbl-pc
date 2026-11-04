// SPDX-License-Identifier: GPL-2.0-or-later
#include "util.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace dlcmaker {

// --- paths and files ------------------------------------------------------------------------------

#ifdef _WIN32
namespace {

std::wstring widen(std::string_view s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string u(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), n, nullptr, nullptr);
    return u;
}

// \\?\C:\x -> C:\x, \\?\UNC\server\share -> \\server\share
std::wstring strip_verbatim(std::wstring p) {
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return L"\\\\" + p.substr(8);
    if (p.rfind(L"\\\\?\\", 0) == 0)
        return p.substr(4);
    return p;
}

std::wstring final_path(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
    std::wstring buf(32768, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    return n && n < buf.size() ? strip_verbatim(buf.substr(0, n)) : L"";
}

} // namespace
#endif

fs::path to_path(std::string_view utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string u8(const fs::path& p) {
    fs::path q = p;
    q.make_preferred();
    std::u8string s = q.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

fs::path py_path(std::string_view text) {
    std::string s(text);
#ifdef _WIN32
    const char sep = '\\';
    std::replace(s.begin(), s.end(), '/', '\\');
    std::string prefix;
    if (s.rfind("\\\\", 0) == 0) { // \\server\share
        size_t a = s.find('\\', 2), b = a == std::string::npos ? a : s.find('\\', a + 1);
        prefix = s.substr(0, b);
        s = b == std::string::npos ? "" : s.substr(b);
    } else if (s.size() >= 2 && s[1] == ':') {
        prefix = s.substr(0, 2);
        s = s.substr(2);
    }
#else
    const char sep = '/';
    std::string prefix = s.rfind("//", 0) == 0 && s.rfind("///", 0) != 0 ? "/" : "";
#endif
    bool root = !s.empty() && s[0] == sep;
    std::string out = prefix + (root ? std::string(1, sep) : "");
    bool first = true;
    for (size_t a = 0; a <= s.size();) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos)
            b = s.size();
        std::string part = s.substr(a, b - a);
        if (!part.empty() && part != ".") {
            out += (first ? "" : std::string(1, sep)) + part;
            first = false;
        }
        a = b + 1;
    }
    return to_path(out.empty() ? "." : out);
}

fs::path resolve(const fs::path& p) {
#ifdef _WIN32
    std::wstring in = fs::path(p).make_preferred().wstring();
    std::wstring full(32768, L'\0');
    DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    full = n && n < full.size() ? full.substr(0, n) : in;
    // The longest part that exists, resolved; the rest as it is (os.path.realpath without strict).
    std::wstring head = full, tail;
    for (;;) {
        std::wstring got = final_path(head);
        if (!got.empty())
            return fs::path(got + tail);
        size_t cut = head.find_last_of(L'\\');
        if (cut == std::wstring::npos || cut + 1 >= head.size() || head.size() <= 3)
            return fs::path(full);
        tail = head.substr(cut) + tail;
        head = head.substr(0, cut);
        if (head.size() == 2 && head[1] == L':')
            head += L'\\';
    }
#else
    std::error_code ec;
    fs::path out = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return ec ? fs::absolute(p) : out;
#endif
}

std::string name_of(const fs::path& p) {
    return u8(p.filename());
}

std::string suffix_of(const fs::path& p) {
    std::string name = name_of(p);
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size())
        return "";
    return name.substr(dot);
}

std::string stem_of(const fs::path& p) {
    std::string name = name_of(p), suf = suffix_of(p);
    return name.substr(0, name.size() - suf.size());
}

bool path_less(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    std::wstring x = fs::path(a).make_preferred().wstring(), y = fs::path(b).make_preferred().wstring();
    CharLowerBuffW(x.data(), static_cast<DWORD>(x.size()));
    CharLowerBuffW(y.data(), static_cast<DWORD>(y.size()));
    return x < y;
#else
    return u8(a) < u8(b);
#endif
}

std::vector<fs::path> sorted_dir(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        out.push_back(dir / it->path().filename());
    if (ec && out.empty() && !is_dir(dir))
        throw Exit("[Errno 2] No such file or directory: '" + u8(dir) + "'");
    std::sort(out.begin(), out.end(), path_less);
    return out;
}

bool same_file(const fs::path& a, const fs::path& b) {
    return u8(resolve(a)) == u8(resolve(b));
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

uint64_t file_size(const fs::path& p) {
    std::error_code ec;
    uint64_t n = fs::file_size(p, ec);
    return ec ? 0 : n;
}

FileTime mtime(const fs::path& p) {
    FileTime t;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(fs::path(p).make_preferred().c_str(), GetFileExInfoStandard, &a)) {
        int64_t ticks = static_cast<int64_t>(static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime) -
                        116444736000000000LL;
        t.sec = ticks / 10000000;
        t.nsec = ticks % 10000000 * 100;
        if (t.nsec < 0)
            t.sec -= 1, t.nsec += 1000000000;
    }
#else
    struct stat st;
    if (stat(p.c_str(), &st) == 0)
        t.sec = st.st_mtim.tv_sec, t.nsec = st.st_mtim.tv_nsec;
#endif
    return t;
}

namespace {

FILE* open_file(const fs::path& p, bool write) {
#ifdef _WIN32
    return _wfopen(fs::path(p).make_preferred().c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(p.c_str(), write ? "wb" : "rb");
#endif
}

std::string os_error(int e, const fs::path& p) {
    std::string what = e == ENOENT ? "No such file or directory" : e == EACCES ? "Permission denied" : std::strerror(e);
    return "[Errno " + std::to_string(e) + "] " + what + ": '" + u8(p) + "'";
}

} // namespace

std::string read_file(const fs::path& p) {
    FILE* f = open_file(p, false);
    if (!f)
        throw Exit(os_error(errno, p));
    std::string out;
    char buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

std::string read_text(const fs::path& p) {
    std::string s = read_file(p);
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 && c >= 0xC2 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 && c <= 0xF4 ? 4 : 0;
        bool ok = n && i + n <= s.size();
        for (size_t k = 1; ok && k < n; ++k)
            ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (!ok) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "'utf-8' codec can't decode byte 0x%02x in position %zu: %s", c, i,
                          n ? "invalid continuation byte" : "invalid start byte");
            throw ValueError(buf);
        }
        i += n;
    }
    return s;
}

void write_file(const fs::path& p, std::string_view data) {
    FILE* f = open_file(p, true);
    if (!f)
        throw Exit(os_error(errno, p));
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok)
        throw Exit("cannot write " + u8(p));
}

void make_dirs(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec && !is_dir(p))
        throw Exit("cannot create " + u8(p) + ": " + ec.message());
}

void remove_tree(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec)
        throw Exit("cannot remove " + u8(p) + ": " + ec.message());
}

void remove_file(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}

void copy_file(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec)
        throw Exit("cannot copy " + u8(from) + " to " + u8(to) + ": " + ec.message());
}

bool hard_link(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_hard_link(from, to, ec);
    return !ec;
}

void replace_file(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(fs::path(from).make_preferred().c_str(), fs::path(to).make_preferred().c_str(), MOVEFILE_REPLACE_EXISTING))
        throw Exit("cannot move " + u8(from) + " to " + u8(to));
#else
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec)
        throw Exit("cannot move " + u8(from) + " to " + u8(to) + ": " + ec.message());
#endif
}

fs::path temp_dir() {
    std::error_code ec;
    fs::path t = fs::temp_directory_path(ec);
    if (ec)
        t = fs::current_path(ec);
    return py_path(u8(t));
}

std::optional<std::string> env(const char* name) {
#ifdef _WIN32
    std::wstring w = widen(name);
    DWORD n = GetEnvironmentVariableW(w.c_str(), nullptr, 0);
    if (!n)
        return std::nullopt;
    std::wstring buf(n, L'\0');
    n = GetEnvironmentVariableW(w.c_str(), buf.data(), n);
    return narrow(buf.substr(0, n));
#else
    const char* v = std::getenv(name);
    return v ? std::optional<std::string>(v) : std::nullopt;
#endif
}

fs::path which(const std::string& name) {
    std::string path = env("PATH").value_or("");
#ifdef _WIN32
    const char sep = ';';
    std::vector<std::string> names = {name};
    if (suffix_of(to_path(name)).empty())
        names = {name + ".exe", name + ".bat", name + ".cmd", name};
#else
    const char sep = ':';
    std::vector<std::string> names = {name};
#endif
    for (size_t a = 0; a <= path.size();) {
        size_t b = path.find(sep, a);
        if (b == std::string::npos)
            b = path.size();
        std::string dir = path.substr(a, b - a);
        a = b + 1;
        if (dir.empty())
            continue;
        for (const std::string& n : names) {
            fs::path c = py_path(dir) / to_path(n);
#ifdef _WIN32
            if (is_file(c))
                return c;
#else
            if (is_file(c) && access(c.c_str(), X_OK) == 0)
                return c;
#endif
        }
    }
    return {};
}

fs::path exe_path() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return fs::path(buf.substr(0, n));
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

// --- processes and output -------------------------------------------------------------------------

std::string command_text(const std::vector<std::string>& args) {
    Value l = Value::of_list();
    for (const std::string& a : args)
        l.list.push_back(Value(a));
    return "Command '" + py_repr(l) + "'";
}

#ifdef _WIN32
namespace {

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

HANDLE inheritable(HANDLE h) {
    HANDLE dup = nullptr;
    if (!h || h == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return nullptr;
    return dup;
}

HANDLE null_device() {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    return CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
}

int spawn(const std::vector<std::string>& args, HANDLE out, HANDLE err) {
    std::wstring cmd;
    for (const std::string& a : args)
        cmd += (cmd.empty() ? L"" : L" ") + quote(widen(a));
    HANDLE in = null_device();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (in != INVALID_HANDLE_VALUE)
        CloseHandle(in);
    if (!ok)
        return -1;
    CloseHandle(pi.hThread);
    return static_cast<int>(reinterpret_cast<intptr_t>(pi.hProcess)); // a handle, waited on by finish()
}

int finish(int handle) {
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<intptr_t>(handle));
    WaitForSingleObject(h, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return static_cast<int>(code);
}

} // namespace

int run(const std::vector<std::string>& args) {
    std::fflush(stdout);
    std::fflush(stderr);
    HANDLE out = inheritable(GetStdHandle(STD_OUTPUT_HANDLE)), err = inheritable(GetStdHandle(STD_ERROR_HANDLE));
    int h = spawn(args, out, err);
    if (out)
        CloseHandle(out);
    if (err)
        CloseHandle(err);
    return h == -1 ? -1 : finish(h);
}

Captured capture(const std::vector<std::string>& args) {
    Captured c;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE r[2], w[2];
    for (int k = 0; k < 2; ++k) {
        CreatePipe(&r[k], &w[k], &sa, 0);
        SetHandleInformation(r[k], HANDLE_FLAG_INHERIT, 0);
    }
    int h = spawn(args, w[0], w[1]);
    CloseHandle(w[0]);
    CloseHandle(w[1]);
    auto drain = [](HANDLE from, std::string* to) {
        char buf[1 << 16];
        DWORD n;
        while (ReadFile(from, buf, sizeof(buf), &n, nullptr) && n)
            to->append(buf, n);
    };
    std::thread t(drain, r[1], &c.err);
    drain(r[0], &c.out);
    t.join();
    CloseHandle(r[0]);
    CloseHandle(r[1]);
    c.code = h == -1 ? -1 : finish(h);
    return c;
}

void setup_console() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!out || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

std::vector<std::string> utf8_args(int, char**) {
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> out;
    for (int i = 1; i < n; ++i)
        out.push_back(narrow(w[i]));
    LocalFree(w);
    return out;
}

bool process_running(std::string_view part) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W e = {};
    e.dwSize = sizeof(e);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = ascii_lower(narrow(e.szExeFile)).find(ascii_lower(std::string(part))) != std::string::npos;
    CloseHandle(snap);
    return found;
}

#else

namespace {

pid_t spawn(const std::vector<std::string>& args, int out, int err) {
    std::vector<char*> argv;
    for (const std::string& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out >= 0)
        posix_spawn_file_actions_adddup2(&fa, out, 1);
    if (err >= 0)
        posix_spawn_file_actions_adddup2(&fa, err, 2);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? -1 : pid;
}

int finish(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? -WTERMSIG(status) : 1;
}

} // namespace

int run(const std::vector<std::string>& args) {
    std::fflush(stdout);
    std::fflush(stderr);
    pid_t pid = spawn(args, -1, -1);
    return pid < 0 ? -1 : finish(pid);
}

Captured capture(const std::vector<std::string>& args) {
    Captured c;
    int po[2], pe[2];
    if (pipe(po) || pipe(pe))
        return c;
    pid_t pid = spawn(args, po[1], pe[1]);
    close(po[1]);
    close(pe[1]);
    struct pollfd fds[2] = {{po[0], POLLIN, 0}, {pe[0], POLLIN, 0}};
    std::string* to[2] = {&c.out, &c.err};
    for (int open = 2; open;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int k = 0; k < 2; ++k) {
            if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char buf[1 << 16];
            ssize_t n = read(fds[k].fd, buf, sizeof(buf));
            if (n > 0)
                to[k]->append(buf, static_cast<size_t>(n));
            else {
                close(fds[k].fd);
                fds[k].fd = -1;
                --open;
            }
        }
    }
    c.code = pid < 0 ? -1 : finish(pid);
    return c;
}

void setup_console() {}

std::vector<std::string> utf8_args(int argc, char** argv) { return std::vector<std::string>(argv + 1, argv + argc); }

bool process_running(std::string_view part) {
    DIR* d = opendir("/proc");
    if (!d)
        return false;
    bool found = false;
    std::string want = ascii_lower(std::string(part)).substr(0, 15); // comm holds 15 characters
    while (dirent* e = readdir(d)) {
        if (found || e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        FILE* f = std::fopen((std::string("/proc/") + e->d_name + "/comm").c_str(), "rb");
        if (!f)
            continue;
        char buf[64] = {};
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        found = ascii_lower(std::string(buf, n)).find(want) != std::string::npos;
    }
    closedir(d);
    return found;
}

#endif

void print(std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void print_err(std::string_view line) {
    std::fflush(stdout);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// --- hashes and ciphers ---------------------------------------------------------------------------

namespace {

uint32_t rol(uint32_t x, int n) {
    return x << n | x >> (32 - n);
}

// Merkle-Damgard padding: the message, 0x80, zeros, the bit length (little- or big-endian).
std::string padded(std::string_view data, bool big) {
    std::string m(data);
    uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    m += '\x80';
    while (m.size() % 64 != 56)
        m += '\0';
    for (int k = 0; k < 8; ++k)
        m += static_cast<char>(big ? bits >> (56 - 8 * k) : bits >> (8 * k));
    return m;
}

std::array<uint32_t, 4> md5(std::string_view data) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
        0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
        0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
        0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::array<uint32_t, 4> h = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::string m = padded(data, false);
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[16];
        for (int k = 0; k < 16; ++k)
            w[k] = static_cast<uint32_t>(static_cast<unsigned char>(m[off + 4 * k])) | static_cast<unsigned char>(m[off + 4 * k + 1]) << 8 |
                   static_cast<unsigned char>(m[off + 4 * k + 2]) << 16 | static_cast<uint32_t>(static_cast<unsigned char>(m[off + 4 * k + 3])) << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int k = 0; k < 64; ++k) {
            uint32_t f;
            int g;
            if (k < 16)
                f = (b & c) | (~b & d), g = k;
            else if (k < 32)
                f = (d & b) | (~d & c), g = (5 * k + 1) % 16;
            else if (k < 48)
                f = b ^ c ^ d, g = (3 * k + 5) % 16;
            else
                f = c ^ (b | ~d), g = (7 * k) % 16;
            uint32_t t = d;
            d = c;
            c = b;
            b = b + rol(a + f + K[k] + w[g], R[k]);
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    }
    return h;
}

} // namespace

std::string hex(std::string_view data) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : data)
        out += digits[c >> 4], out += digits[c & 15];
    return out;
}

std::string md5_hex(std::string_view data) {
    std::array<uint32_t, 4> h = md5(data);
    std::string raw;
    for (uint32_t v : h)
        for (int k = 0; k < 4; ++k)
            raw += static_cast<char>(v >> (8 * k));
    return hex(raw);
}

std::string sha1(std::string_view data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = padded(data, true);
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int k = 0; k < 16; ++k)
            w[k] = static_cast<uint32_t>(static_cast<unsigned char>(m[off + 4 * k])) << 24 | static_cast<unsigned char>(m[off + 4 * k + 1]) << 16 |
                   static_cast<unsigned char>(m[off + 4 * k + 2]) << 8 | static_cast<unsigned char>(m[off + 4 * k + 3]);
        for (int k = 16; k < 80; ++k)
            w[k] = rol(w[k - 3] ^ w[k - 8] ^ w[k - 14] ^ w[k - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int k = 0; k < 80; ++k) {
            uint32_t f, kk;
            if (k < 20)
                f = (b & c) | (~b & d), kk = 0x5A827999;
            else if (k < 40)
                f = b ^ c ^ d, kk = 0x6ED9EBA1;
            else if (k < 60)
                f = (b & c) | (b & d) | (c & d), kk = 0x8F1BBCDC;
            else
                f = b ^ c ^ d, kk = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + kk + w[k];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::string out;
    for (uint32_t v : h)
        for (int k = 0; k < 4; ++k)
            out += static_cast<char>(v >> (24 - 8 * k));
    return out;
}

std::string sha1_hex(std::string_view data) {
    return hex(sha1(data));
}

uint32_t crc32(std::string_view data, uint32_t crc) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (unsigned char b : data)
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

namespace {

std::string hmac_sha1(std::string_view key, std::string_view msg) {
    std::string k = key.size() > 64 ? sha1(key) : std::string(key);
    k.resize(64, '\0');
    std::string ipad = k, opad = k;
    for (int i = 0; i < 64; ++i)
        ipad[i] = static_cast<char>(ipad[i] ^ 0x36), opad[i] = static_cast<char>(opad[i] ^ 0x5c);
    return sha1(opad + sha1(ipad + std::string(msg)));
}

struct Aes {
    uint8_t sbox[256], inv[256];
    Aes() {
        // The S-box from the multiplicative inverse and the affine map (FIPS-197 5.1.1).
        uint8_t p = 1, q = 1;
        do {
            p = static_cast<uint8_t>(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
            q ^= static_cast<uint8_t>(q << 1);
            q ^= static_cast<uint8_t>(q << 2);
            q ^= static_cast<uint8_t>(q << 4);
            if (q & 0x80)
                q ^= 0x09;
            uint8_t x = static_cast<uint8_t>(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
            sbox[p] = x ^ 0x63;
        } while (p != 1);
        sbox[0] = 0x63;
        for (int k = 0; k < 256; ++k)
            inv[sbox[k]] = static_cast<uint8_t>(k);
    }
    static uint8_t mul(uint8_t a, uint8_t b) {
        uint8_t r = 0;
        while (b) {
            if (b & 1)
                r ^= a;
            a = static_cast<uint8_t>(a << 1 ^ (a & 0x80 ? 0x1B : 0));
            b >>= 1;
        }
        return r;
    }
};

} // namespace

std::string pbkdf2_sha1(std::string_view password, std::string_view salt, int iterations, size_t length) {
    std::string out;
    for (uint32_t block = 1; out.size() < length; ++block) {
        std::string s(salt);
        for (int k = 3; k >= 0; --k)
            s += static_cast<char>(block >> (8 * k));
        std::string u = hmac_sha1(password, s), t = u;
        for (int i = 1; i < iterations; ++i) {
            u = hmac_sha1(password, u);
            for (size_t k = 0; k < t.size(); ++k)
                t[k] = static_cast<char>(t[k] ^ u[k]);
        }
        out += t;
    }
    return out.substr(0, length);
}

std::string aes128_cbc_decrypt(std::string_view key, std::string_view iv, std::string_view data) {
    static const Aes aes;
    uint8_t w[176];
    std::memcpy(w, key.data(), 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
        if (i % 16 == 0) {
            uint8_t a = t[0];
            t[0] = static_cast<uint8_t>(aes.sbox[t[1]] ^ rcon);
            t[1] = aes.sbox[t[2]];
            t[2] = aes.sbox[t[3]];
            t[3] = aes.sbox[a];
            rcon = Aes::mul(rcon, 2);
        }
        for (int k = 0; k < 4; ++k)
            w[i + k] = static_cast<uint8_t>(w[i - 16 + k] ^ t[k]);
    }
    std::string out(data.size(), '\0');
    uint8_t chain[16];
    std::memcpy(chain, iv.data(), 16);
    for (size_t off = 0; off + 16 <= data.size(); off += 16) {
        uint8_t s[16];
        std::memcpy(s, data.data() + off, 16);
        for (int k = 0; k < 16; ++k)
            s[k] ^= w[160 + k];
        for (int round = 9; round >= 0; --round) {
            uint8_t t[16];
            for (int c = 0; c < 4; ++c) // InvShiftRows and InvSubBytes
                for (int r = 0; r < 4; ++r)
                    t[4 * ((c + r) % 4) + r] = aes.inv[s[4 * c + r]];
            for (int k = 0; k < 16; ++k)
                s[k] = static_cast<uint8_t>(t[k] ^ w[16 * round + k]);
            if (round == 0)
                break;
            for (int c = 0; c < 4; ++c) { // InvMixColumns
                uint8_t* m = s + 4 * c;
                uint8_t a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3];
                m[0] = Aes::mul(a0, 14) ^ Aes::mul(a1, 11) ^ Aes::mul(a2, 13) ^ Aes::mul(a3, 9);
                m[1] = Aes::mul(a0, 9) ^ Aes::mul(a1, 14) ^ Aes::mul(a2, 11) ^ Aes::mul(a3, 13);
                m[2] = Aes::mul(a0, 13) ^ Aes::mul(a1, 9) ^ Aes::mul(a2, 14) ^ Aes::mul(a3, 11);
                m[3] = Aes::mul(a0, 11) ^ Aes::mul(a1, 13) ^ Aes::mul(a2, 9) ^ Aes::mul(a3, 14);
            }
        }
        for (int k = 0; k < 16; ++k)
            out[off + k] = static_cast<char>(s[k] ^ chain[k]);
        std::memcpy(chain, data.data() + off, 16);
    }
    return out;
}

std::string base64_decode(std::string_view text) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (c == '=')
            break;
        if (v < 0)
            continue;
        acc = acc << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>(acc >> bits & 0xFF);
        }
    }
    return out;
}

std::string base64_encode(std::string_view data) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        uint32_t v = static_cast<unsigned char>(data[i]) << 16 | static_cast<unsigned char>(data[i + 1]) << 8 | static_cast<unsigned char>(data[i + 2]);
        out += abc[v >> 18], out += abc[v >> 12 & 63], out += abc[v >> 6 & 63], out += abc[v & 63];
    }
    if (size_t rest = data.size() - i) {
        uint32_t v = static_cast<unsigned char>(data[i]) << 16 | (rest == 2 ? static_cast<unsigned char>(data[i + 1]) << 8 : 0);
        out += abc[v >> 18], out += abc[v >> 12 & 63];
        out += rest == 2 ? abc[v >> 6 & 63] : '=';
        out += '=';
    }
    return out;
}

// --- binary streams -------------------------------------------------------------------------------

void Reader::need(size_t n) const {
    if (p + n > d.size() || p + n < p)
        throw ValueError("unexpected end of data");
}

uint64_t Reader::uint(size_t n) {
    need(n);
    uint64_t v = 0;
    for (size_t k = 0; k < n; ++k) {
        uint64_t b = static_cast<unsigned char>(d[p + k]);
        v |= big ? b << (8 * (n - 1 - k)) : b << (8 * k);
    }
    p += n;
    return v;
}

float Reader::f32() {
    uint32_t u = u32();
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

double Reader::f64() {
    uint64_t u = u64();
    double f;
    std::memcpy(&f, &u, 8);
    return f;
}

std::string_view Reader::bytes(size_t n) {
    need(n);
    std::string_view out = d.substr(p, n);
    p += n;
    return out;
}

std::string Reader::cstring() {
    size_t end = d.find('\0', p);
    if (end == std::string_view::npos)
        throw ValueError("unterminated string");
    std::string out(d.substr(p, end - p));
    p = end + 1;
    return out;
}

void Writer::uint(uint64_t v, size_t n) {
    for (size_t k = 0; k < n; ++k)
        d += static_cast<char>(big ? v >> (8 * (n - 1 - k)) : v >> (8 * k));
}

void Writer::f32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u32(u);
}

void Writer::f64(double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    u64(u);
}

void Writer::put_u32(size_t at, uint32_t v) {
    for (size_t k = 0; k < 4; ++k)
        d[at + k] = static_cast<char>(big ? v >> (8 * (3 - k)) : v >> (8 * k));
}

void Writer::put_u64(size_t at, uint64_t v) {
    for (size_t k = 0; k < 8; ++k)
        d[at + k] = static_cast<char>(big ? v >> (8 * (7 - k)) : v >> (8 * k));
}

} // namespace dlcmaker
