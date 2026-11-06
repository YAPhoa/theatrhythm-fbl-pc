// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The install itself, in the release folder the setup exe sits in (tools/package.py): prepares the game
// image from the player's dump with prepare/run.py, and writes paths.ini (the dump, the save folder and
// an optional extra DLC folder, read by src/launch.c). The dump is only ever read. Windows (main.cpp) and Linux (main_linux.cpp)
// share it.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace setup {

// Why an install stopped; the text is meant for the user.
struct Error {
    std::wstring message;
};

struct Options {
    std::filesystem::path root; // the release folder: the game exe, prepare/, and what the setup adds
    std::filesystem::path game; // the dump folder or its eboot.bin
    std::filesystem::path save; // saves and the launch log (run.py's --user)
    std::optional<std::filesystem::path> copy_saves_from; // the old save folder, whose saves go to `save`
    std::optional<bool> skip_opening; // settings.ini skip_opening to write; unset keeps what is there
    std::filesystem::path dlc; // optional: one more folder of DLC (e.g. shadPS4's addcont); empty for none
};

// The folders a release folder's paths.ini names.
struct Paths {
    std::filesystem::path game, save;
    std::filesystem::path dlc; // empty when paths.ini names none
};

// Save data in a save folder (what the runtime keeps there apart from the log).
constexpr const wchar_t* kSaveFolders[] = {L"savedata", L"download0"};

using Log = std::function<void(const std::wstring&)>;

#ifdef _WIN32
constexpr wchar_t kGameExe[] = L"theatrhythm-fbl.exe";
constexpr wchar_t kShortcutName[] = L"Theatrhythm Final Bar Line.lnk";
#else
constexpr wchar_t kGameExe[] = L"theatrhythm-fbl";
// A desktop entry (freedesktop.org), in the application menu or on the Desktop.
constexpr wchar_t kShortcutName[] = L"theatrhythm-fbl.desktop";
#endif

// A path as text for the user and paths.ini, and text back as a path. Linux paths are UTF-8 bytes,
// which these convert without depending on the locale.
std::wstring text(const std::filesystem::path& p);
std::filesystem::path path_of(const std::wstring& s);
// UTF-8 to and from the wide text this setup uses.
std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view s);

// The folder this exe is in.
std::filesystem::path exe_folder();
// The dump folder for a path to it or to its eboot.bin (absolute, normalized).
std::filesystem::path game_folder(const std::filesystem::path& path);
// What `root`/paths.ini names, if it names both folders.
std::optional<Paths> read_paths(const std::filesystem::path& root);
// The per-user copy of paths.ini that install() also writes: %APPDATA%\Theatrhythm Final Bar
// Line\paths.ini, or $XDG_CONFIG_HOME/theatrhythm-fbl/paths.ini (~/.config when unset); src/launch.c
// reads it when the release folder has none. Empty when %APPDATA% is unset.
std::filesystem::path user_paths_ini();
// read_paths(root), else the per-user copy: the folders to start from in a release just unpacked.
std::optional<Paths> remembered_paths(const std::filesystem::path& root);
// save/ in the release folder: a release is never zipped with one, so unpacking a newer release over
// the folder keeps the saves.
std::filesystem::path default_save();
// True when `root` is set up for its release: paths.ini names both folders and game/ was prepared
// for this release.txt (what src/launch.c checks before it starts the game).
bool ready(const std::filesystem::path& root);
// True when `save` holds game saves (a savedata folder with something in it).
bool has_saves(const std::filesystem::path& save);
// settings.ini skip_opening as the game reads it: from the save folder's file when there is one,
// else the release folder's; false when unset.
bool skip_opening(const std::filesystem::path& root, const std::filesystem::path& save);
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b);

// Sets up opt.root, reporting each step through `log`. Throws Error. Returns the game exe's path.
std::filesystem::path install(const Options& opt, const Log& log);

// Writes a shortcut at `lnk` that starts `target` in its own folder. Throws Error.
void create_shortcut(const std::filesystem::path& target, const std::filesystem::path& lnk);
// The user's Desktop folder. Throws Error.
std::filesystem::path desktop_folder();
#ifndef _WIN32
// Where desktop entries go to show up in the application menu (and Steam's "Add a Non-Steam Game").
std::filesystem::path applications_folder();
#endif

} // namespace setup
