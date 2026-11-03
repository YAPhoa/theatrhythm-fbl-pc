// SPDX-License-Identifier: GPL-2.0-or-later
// Files, paths, processes, hashes and binary streams for the DLC maker's core, on Windows and Linux.
// Paths print with the platform's separators and sort the way Python's pathlib sorts them.
#pragma once

#include "value.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dlcmaker {

namespace fs = std::filesystem;

// --- paths and files ------------------------------------------------------------------------------

fs::path to_path(std::string_view utf8);
std::string u8(const fs::path& p);            // native separators, UTF-8
fs::path py_path(std::string_view utf8);      // Path(text): separators made native, '.' parts and a trailing separator dropped
fs::path resolve(const fs::path& p);          // Path.resolve(): absolute, links followed
std::string name_of(const fs::path& p);       // Path.name
std::string suffix_of(const fs::path& p);     // Path.suffix
std::string stem_of(const fs::path& p);       // Path.stem
std::vector<fs::path> sorted_dir(const fs::path& dir); // sorted(dir.iterdir())
bool path_less(const fs::path& a, const fs::path& b);
bool same_file(const fs::path& a, const fs::path& b);   // a.resolve() == b.resolve()

bool is_file(const fs::path& p);
bool is_dir(const fs::path& p);
bool exists(const fs::path& p);
uint64_t file_size(const fs::path& p);
struct FileTime {
    int64_t sec = 0, nsec = 0;
    double seconds() const { return static_cast<double>(sec) + static_cast<double>(nsec) * 1e-9; } // os.stat().st_mtime
    int64_t ns() const { return sec * 1000000000 + nsec; }
};
FileTime mtime(const fs::path& p);

std::string read_file(const fs::path& p);           // Exit when it cannot be read
std::string read_text(const fs::path& p);           // read_file, checked as UTF-8 (ValueError)
void write_file(const fs::path& p, std::string_view data);
void make_dirs(const fs::path& p);
void remove_tree(const fs::path& p);
void remove_file(const fs::path& p);
void copy_file(const fs::path& from, const fs::path& to);
bool hard_link(const fs::path& from, const fs::path& to);
void replace_file(const fs::path& from, const fs::path& to);
fs::path temp_dir();

std::optional<std::string> env(const char* name);
fs::path which(const std::string& name);  // shutil.which
fs::path exe_path();

// --- processes and output -------------------------------------------------------------------------

// Runs `args` (UTF-8) with this process's stdout and stderr; returns its exit code (-1: it cannot start).
int run(const std::vector<std::string>& args);
struct Captured {
    int code = -1;
    std::string out, err;
};
Captured capture(const std::vector<std::string>& args);
std::string command_text(const std::vector<std::string>& args); // the command as Python's CalledProcessError shows it

void print(std::string_view line);      // stdout, with a newline, flushed
void print_err(std::string_view line);  // stderr, with a newline
void setup_console();                   // UTF-8 binary stdout and stderr (and a console for a GUI exe run from one)
std::vector<std::string> utf8_args(int argc, char** argv); // the arguments after the program name, UTF-8 (Windows: from the wide command line)
bool process_running(std::string_view name_part); // a process whose name holds name_part (case-insensitive)

// --- hashes and ciphers ---------------------------------------------------------------------------

std::string md5_hex(std::string_view data);
std::string sha1(std::string_view data);      // 20 raw bytes
std::string sha1_hex(std::string_view data);
uint32_t crc32(std::string_view data, uint32_t crc = 0); // zlib.crc32
std::string pbkdf2_sha1(std::string_view password, std::string_view salt, int iterations, size_t length);
std::string aes128_cbc_decrypt(std::string_view key, std::string_view iv, std::string_view data);
std::string base64_decode(std::string_view text);
std::string base64_encode(std::string_view data);
std::string hex(std::string_view data);

// --- binary streams -------------------------------------------------------------------------------

struct Reader {
    std::string_view d;
    size_t p = 0;
    bool big = false;

    Reader(std::string_view data, bool big_endian = false, size_t at = 0) : d(data), p(at), big(big_endian) {}
    void need(size_t n) const;
    uint64_t uint(size_t n);
    uint8_t u8() { return static_cast<uint8_t>(uint(1)); }
    uint16_t u16() { return static_cast<uint16_t>(uint(2)); }
    uint32_t u32() { return static_cast<uint32_t>(uint(4)); }
    uint64_t u64() { return uint(8); }
    int8_t i8() { return static_cast<int8_t>(u8()); }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    float f32();
    double f64();
    std::string_view bytes(size_t n);
    std::string cstring();
    void align(size_t n) { p = (p + n - 1) / n * n; }
};

struct Writer {
    std::string d;
    bool big = false;

    explicit Writer(bool big_endian = false) : big(big_endian) {}
    void uint(uint64_t v, size_t n);
    void u8(uint8_t v) { uint(v, 1); }
    void u16(uint16_t v) { uint(v, 2); }
    void u32(uint32_t v) { uint(v, 4); }
    void u64(uint64_t v) { uint(v, 8); }
    void i8(int8_t v) { uint(static_cast<uint8_t>(v), 1); }
    void i16(int16_t v) { uint(static_cast<uint16_t>(v), 2); }
    void i32(int32_t v) { uint(static_cast<uint32_t>(v), 4); }
    void i64(int64_t v) { uint(static_cast<uint64_t>(v), 8); }
    void f32(float v);
    void f64(double v);
    void bytes(std::string_view b) { d.append(b); }
    void cstring(std::string_view s) { d.append(s), d += '\0'; }
    void align(size_t n) { d.append((n - d.size() % n) % n, '\0'); }
    void put_u32(size_t at, uint32_t v); // overwrite in place
    void put_u64(size_t at, uint64_t v);
};

} // namespace dlcmaker
