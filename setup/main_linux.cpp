// SPDX-License-Identifier: GPL-2.0-or-later
// theatrhythm-setup (Linux): sets up the release folder it sits in (tools/package.py) from the player's
// Theatrhythm Final Bar Line dump, so the game starts from theatrhythm-fbl or its menu entry.
//
// With no arguments it asks for the game folder (the dump), the save folder, whether to skip the
// opening movie (settings.ini skip_opening) and an optional extra DLC folder, installs, and offers
// a menu entry and to start the game: in the terminal it was started from, or with the desktop's
// dialogs (SDL: the folder picker and message boxes) when it has none, as from a file manager. Run it
// again to change either folder, or after unpacking a newer release over the folder.
// With arguments it installs from the command line:
//
//     theatrhythm-setup --game <dump or eboot.bin> [--save <folder>] [--copy-saves]
//                       [--dlc <folder>|--no-dlc] [--skip-opening|--play-opening]
//                       [--shortcut <folder>|desktop|menu]

#include "setup.h"

#include <SDL3/SDL.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr char kTitle[] = "Theatrhythm Final Bar Line setup";

// The old save folder to copy from when the save folder changes and only the old one has saves.
std::optional<fs::path> saves_to_move(const fs::path& root, const fs::path& save) {
    auto old = setup::remembered_paths(root);
    if (!old || setup::same_path(old->save, save) || !setup::has_saves(old->save) || setup::has_saves(save))
        return std::nullopt;
    return old->save;
}

void out(const std::wstring& s) {
    std::cout << setup::narrow(s) << std::endl;
}

void err(const std::wstring& s) {
    std::cerr << setup::narrow(s) << std::endl;
}

// --- command line -------------------------------------------------------------------------------

constexpr wchar_t kUsage[] =
    L"usage: theatrhythm-setup --game <dump or eboot.bin> [--save <folder>] [--copy-saves]\n"
    L"                         [--dlc <folder>|--no-dlc] [--skip-opening|--play-opening]\n"
    L"                         [--shortcut <folder>|desktop|menu]\n"
    L"  --game        the decrypted game dump (eboot.bin, sce_module, sce_sys, Media); only ever read\n"
    L"  --save        where your saves go (default: the one paths.ini names, else save/ here)\n"
    L"  --copy-saves  when --save changes, copy the saves from the old save folder\n"
    L"  --dlc         one more folder of DLC to read, e.g. shadPS4's addcont (default: the one\n"
    L"                paths.ini names); --no-dlc reads only the save folder's addcont\n"
    L"  --skip-opening, --play-opening\n"
    L"                start at the title screen without the opening movie, or play it again\n"
    L"                (settings.ini skip_opening; default: leave it as it is)\n"
    L"  --shortcut    also write the game's desktop entry into this folder, onto the Desktop or into\n"
    L"                the application menu\n"
    L"Sets up the folder this program is in. With no arguments, asks for the folders.";

fs::path shortcut_folder(const std::wstring& where) {
    if (where == L"desktop")
        return setup::desktop_folder();
    if (where == L"menu")
        return setup::applications_folder();
    return setup::path_of(where);
}

int run_cli(const std::vector<std::wstring>& args) {
    std::wstring game, save, shortcut, dlc;
    bool copy_saves = false, no_dlc = false;
    std::optional<bool> skip_opening;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        std::wstring* value = a == L"--game"       ? &game
                              : a == L"--save"     ? &save
                              : a == L"--shortcut" ? &shortcut
                              : a == L"--dlc"      ? &dlc
                                                   : nullptr;
        if (value && i + 1 < args.size()) {
            *value = args[++i];
        } else if (a == L"--copy-saves") {
            copy_saves = true;
        } else if (a == L"--skip-opening" || a == L"--play-opening") {
            skip_opening = a == L"--skip-opening";
        } else if (a == L"--no-dlc") {
            no_dlc = true;
        } else {
            err(a == L"--help" || a == L"-h" ? kUsage : std::wstring(kUsage) + L"\n\nunexpected: " + a);
            return 2;
        }
    }
    if (game.empty()) {
        err(std::wstring(kUsage) + L"\n\n--game is required");
        return 2;
    }
    try {
        setup::Options opt;
        opt.root = setup::exe_folder();
        opt.game = setup::path_of(game);
        auto old = setup::remembered_paths(opt.root);
        opt.save = !save.empty() ? setup::path_of(save) : old ? old->save : setup::default_save();
        opt.dlc = no_dlc ? fs::path() : !dlc.empty() ? setup::path_of(dlc) : old ? old->dlc : fs::path();
        if (copy_saves)
            opt.copy_saves_from = saves_to_move(opt.root, opt.save);
        opt.skip_opening = skip_opening;
        fs::path exe = setup::install(opt, out);
        if (!shortcut.empty()) {
            fs::path entry = shortcut_folder(shortcut) / setup::kShortcutName;
            setup::create_shortcut(exe, entry);
            out(L"shortcut " + setup::text(entry));
        }
        out(L"\nDone. Run " + setup::text(exe));
        return 0;
    } catch (const setup::Error& e) {
        err(e.message);
        return 1;
    }
}

// --- asking -------------------------------------------------------------------------------------

// How the no-argument setup asks its questions.
class Asker {
public:
    virtual ~Asker() = default;
    // A folder (or the dump's eboot.bin), starting from `current`; nullopt when the player cancels.
    virtual std::optional<fs::path> folder(const std::wstring& title, const fs::path& current) = 0;
    // The index of the answer picked (the first is the default); -1 when the player cancels.
    virtual int choose(const std::wstring& question, const std::vector<std::wstring>& answers) = 0;
    virtual void tell(const std::wstring& message, bool error) = 0;
    // A line of the install's output.
    virtual void log(const std::wstring& line) = 0;
};

// Questions in the terminal the setup was started from.
class TerminalAsker : public Asker {
public:
    std::optional<fs::path> folder(const std::wstring& title, const fs::path& current) override {
        for (;;) {
            out(title + (current.empty() ? L"" : L"\n  (Enter keeps " + setup::text(current) + L")"));
            std::cout << "> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line))
                return std::nullopt;
            std::wstring value = setup::widen(line);
            size_t a = value.find_first_not_of(L" \t'\""), b = value.find_last_not_of(L" \t'\"");
            value = a == std::wstring::npos ? L"" : value.substr(a, b - a + 1);
            if (value.empty() && !current.empty())
                return current;
            if (value.empty())
                continue;
            if (value.rfind(L"~/", 0) == 0)
                if (const char* home = std::getenv("HOME"))
                    value = setup::widen(home) + value.substr(1);
            return setup::path_of(value);
        }
    }
    int choose(const std::wstring& question, const std::vector<std::wstring>& answers) override {
        for (;;) {
            out(question);
            for (size_t i = 0; i < answers.size(); ++i)
                out(L"  " + std::to_wstring(i + 1) + L") " + answers[i] + (i ? L"" : L" (Enter)"));
            std::cout << "> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line))
                return -1;
            if (line.empty())
                return 0;
            char* end = nullptr;
            long n = std::strtol(line.c_str(), &end, 10);
            if (end != line.c_str() && n >= 1 && n <= static_cast<long>(answers.size()))
                return static_cast<int>(n - 1);
        }
    }
    void tell(const std::wstring& message, bool error) override { (error ? err : out)(L"\n" + message + L"\n"); }
    void log(const std::wstring& line) override { out(line); }
};

// The desktop's dialogs, through SDL.
class DialogAsker : public Asker {
public:
    std::optional<fs::path> folder(const std::wstring& title, const fs::path& current) override {
        struct Pick {
            std::atomic<bool> done{false};
            std::optional<fs::path> path;
            std::string error;
        } pick;
        std::string title_text = setup::narrow(title), location = current.native();
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title_text.c_str());
        std::error_code ec;
        fs::path start = current.empty() ? fs::path() : setup::game_folder(current);
        if (!start.empty() && fs::is_directory(start, ec))
            SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, start.c_str());
        // The callback may run on another thread; `done` is set last.
        auto picked = [](void* data, const char* const* files, int) {
            auto* p = static_cast<Pick*>(data);
            if (!files)
                p->error = SDL_GetError();
            else if (files[0])
                p->path = fs::path(files[0]);
            p->done = true;
        };
        SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, picked, &pick, props);
        while (!pick.done) {
            SDL_Event e;
            SDL_WaitEventTimeout(&e, 50);
        }
        SDL_DestroyProperties(props);
        if (!pick.error.empty())
            tell(L"No folder picker is available (" + setup::widen(pick.error) +
                     L").\n\nRun theatrhythm-setup in a terminal, or with --game <folder>.",
                 true);
        return pick.path;
    }
    int choose(const std::wstring& question, const std::vector<std::wstring>& answers) override {
        std::string message = setup::narrow(question);
        std::vector<std::string> labels;
        for (const std::wstring& a : answers)
            labels.push_back(setup::narrow(a));
        std::vector<SDL_MessageBoxButtonData> buttons;
        for (size_t i = 0; i < labels.size(); ++i) {
            SDL_MessageBoxButtonFlags flags = i == 0                    ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT
                                              : i + 1 == labels.size() ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT
                                                                       : 0;
            buttons.push_back({flags, static_cast<int>(i), labels[i].c_str()});
        }
        SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, nullptr, kTitle, message.c_str(),
                                  static_cast<int>(buttons.size()), buttons.data(), nullptr};
        int picked = -1;
        return SDL_ShowMessageBox(&box, &picked) ? picked : -1;
    }
    void tell(const std::wstring& message, bool error) override {
        (error ? err : out)(message);
        std::wstring text = message;
        if (error && !lines_.empty()) {
            text += L"\n\nThe last lines of the setup's output:";
            for (const std::wstring& line : lines_)
                text += L"\n" + line;
        }
        SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, kTitle,
                                 setup::narrow(text).c_str(), nullptr);
    }
    void log(const std::wstring& line) override {
        out(line);
        lines_.push_back(line);
        if (lines_.size() > 12)
            lines_.pop_front();
    }

private:
    std::deque<std::wstring> lines_;
};

// Replaces this process with the game, started in its folder as its menu entry would.
void play(Asker& ask, const fs::path& root) {
    fs::path exe = root / setup::kGameExe;
    SDL_Quit();
    if (!chdir(root.c_str()))
        execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
    ask.tell(L"Cannot start " + setup::text(exe), true);
}

int run_asking(Asker& ask) {
    fs::path root = setup::exe_folder();
    std::optional<setup::Paths> paths;
    try {
        paths = setup::remembered_paths(root);
    } catch (const setup::Error&) {
    }
    if (setup::ready(root)) {
        int answer = ask.choose(L"The game is set up.\n\nGame folder: " + setup::text(paths->game) +
                                    L"\nSave folder: " + setup::text(paths->save) +
                                    (paths->dlc.empty() ? L"" : L"\nExtra DLC folder: " + setup::text(paths->dlc)),
                                {L"Play Game", L"Set up again", L"Exit"});
        if (answer == 0) {
            play(ask, root);
            return 1;
        }
        if (answer != 1)
            return 0;
    }

    std::optional<fs::path> game =
        ask.folder(L"Select the game folder (your game dump, the one with eboot.bin)", paths ? paths->game : fs::path());
    if (!game)
        return 1;
    fs::path save = paths ? paths->save : setup::default_save();
    int keep = ask.choose(L"Keep your saves in this folder?\n\n" + setup::text(save),
                          {L"Yes", L"Choose another folder", L"Cancel"});
    if (keep == 1) {
        std::optional<fs::path> picked = ask.folder(L"Select the save folder", save);
        if (!picked)
            return 1;
        save = *picked;
    } else if (keep != 0) {
        return 1;
    }
    // The current choice (settings.ini skip_opening) is the default answer; Install writes the new one.
    const bool skipping = setup::skip_opening(root, save);
    std::vector<std::wstring> answers = {L"Skip it", L"Play it"};
    if (!skipping)
        std::swap(answers[0], answers[1]);
    int skip = ask.choose(L"Skip the opening movie (set skip_opening=0 in settings.ini to bring it back)?", answers);
    if (skip < 0)
        return 1;

    // An extra DLC folder, read next to the save folder's addcont (e.g. shadPS4's addcont).
    fs::path dlc = paths ? paths->dlc : fs::path();
    int extra = dlc.empty()
                    ? ask.choose(L"Also read DLC from another folder (e.g. shadPS4's addcont)?\n\n"
                                 L"DLC in the save folder's addcont folder is always read.",
                                 {L"No", L"Choose a folder", L"Cancel"})
                    : ask.choose(L"Also read DLC from this folder?\n\n" + setup::text(dlc),
                                 {L"Yes", L"Choose another folder", L"No", L"Cancel"});
    if (extra < 0 || extra == (dlc.empty() ? 2 : 3))
        return 1;
    if (extra == 1) {
        std::optional<fs::path> picked = ask.folder(L"Select a folder of DLC (e.g. shadPS4's addcont)", dlc);
        if (!picked)
            return 1;
        dlc = *picked;
    } else if (!dlc.empty() && extra == 2) {
        dlc.clear();
    }
    setup::Options opt;
    opt.root = root;
    opt.game = *game;
    opt.save = save;
    opt.skip_opening = (skip == 0) == skipping;
    opt.dlc = dlc;
    try {
        if (auto old = saves_to_move(opt.root, opt.save)) {
            int answer = ask.choose(L"Copy your saves to the new save folder?\n\nFrom: " + setup::text(*old) +
                                        L"\nTo: " + setup::text(save),
                                    {L"Yes", L"No", L"Cancel"});
            if (answer < 0 || answer == 2)
                return 1;
            if (answer == 0)
                opt.copy_saves_from = old;
        }
        fs::path exe = setup::install(opt, [&](const std::wstring& line) { ask.log(line); });
        if (ask.choose(L"Done!\n\nAdd the game to your application menu?", {L"Yes", L"No"}) == 0) {
            fs::path entry = setup::applications_folder() / setup::kShortcutName;
            setup::create_shortcut(exe, entry);
            ask.log(L"Menu entry " + setup::text(entry));
        }
    } catch (const setup::Error& e) {
        ask.tell(e.message, true);
        return 1;
    }
    if (ask.choose(L"Start the game now?", {L"Play Game", L"Exit"}) == 0) {
        play(ask, root);
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(setup::widen(argv[i]));
    if (!args.empty())
        return run_cli(args);
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        TerminalAsker ask;
        out(L"Theatrhythm Final Bar Line setup (Ctrl+C or Ctrl+D cancels)\n");
        return run_asking(ask);
    }
    // SIGTERM and SIGINT end the setup as usual (SDL would turn them into a quit event nobody reads).
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        err(std::wstring(kUsage) + L"\n\nNo terminal and no display (" + setup::widen(SDL_GetError()) + L").");
        return 2;
    }
    DialogAsker ask;
    int rc = run_asking(ask);
    SDL_Quit();
    return rc;
}
