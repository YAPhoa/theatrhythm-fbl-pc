// SPDX-License-Identifier: GPL-2.0-or-later
// theatrhythm-dlcmaker (Linux): the DLC maker's window, drawn with Dear ImGui on SDL3. It is main.cpp's
// window (Windows) with the same fields, buttons and stage editor, and behaves the same way: it edits
// a project folder, the workspace (DLC folders with dlc.json, song folders with song.json, audio, video,
// art, charts) and checks, builds and installs packs. All project logic is the DLC maker's core (../core,
// the command line in this same program): the window runs itself with a command (info, set, add-file,
// new-dlc, new-song, check, build, install, stage-data) and shows what it prints (records.h reads it, as
// on Windows).
//
// Settings (project folder, game folder, save folder, build folder) are kept in dlcmaker.ini next to
// the program. Its release (tools/package.py --dlcmaker) is an AppImage holding this program, ffmpeg and
// the libraries they need. Unpacked inside the game's release folder, the game and save folders default
// to the ones the game's paths.ini names. Files and folders are picked with the desktop's dialogs (SDL:
// the portal or zenity); without one, the path is typed in.

#include "records.h"

#include "../core/commands.h"
#include "../core/util.h"

#include <SDL3/SDL.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "misc/cpp/imgui_stdlib.h"

#define STBI_NO_STDIO // stb_image itself is in the core (core/image.cpp)
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// DejaVu Sans, embedded (gpu/third_party/fonts, Bitstream Vera license), as the game's overlay has it.
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden theatrhythm_font_ttf\n"
    ".global theatrhythm_font_ttf\n"
    "theatrhythm_font_ttf:\n"
    ".incbin \"" THEATRHYTHM_FONT_PATH "\"\n"
    ".hidden theatrhythm_font_ttf_end\n"
    ".global theatrhythm_font_ttf_end\n"
    "theatrhythm_font_ttf_end:\n"
    ".previous\n");
extern "C" const unsigned char theatrhythm_font_ttf[];
extern "C" const unsigned char theatrhythm_font_ttf_end[];

extern char** environ;

namespace fs = std::filesystem;

namespace {

constexpr char kTitle[] = "Theatrhythm Final Bar Line DLC maker";

// --- text and processes ---------------------------------------------------------------------------

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string strip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\""), b = s.find_last_not_of(" \t\r\"");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

fs::path exe_folder() {
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : self.parent_path();
}

// Runs `args` in `cwd`, stdin from /dev/null, and passes each line it prints (stdout and stderr) to
// `line`. Returns the exit code (128 + the signal that ended it), or -1 when it cannot start.
long run(const std::vector<std::string>& args, const fs::path& cwd, const std::function<void(const std::string&)>& line) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    std::vector<char*> argv;
    for (const std::string& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int failed = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (failed) {
        close(fds[0]);
        return -1;
    }
    std::string pending;
    auto emit = [&](std::string_view bytes) {
        std::string l(bytes);
        while (!l.empty() && l.back() == '\r')
            l.pop_back();
        line(l);
    };
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) != 0;) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        pending.append(buf, static_cast<size_t>(n));
        for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1))
            emit(std::string_view(pending).substr(0, end));
    }
    if (!pending.empty())
        emit(pending);
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}

// Starts `args` and leaves it running (xdg-open).
bool start_detached(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const std::string& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
        return false;
    std::thread([pid] {
        int status;
        waitpid(pid, &status, 0);
    }).detach();
    return true;
}

// key=value lines (paths.ini, dlcmaker.ini): UTF-8, # or ; comments.
std::map<std::string, std::string> read_ini(const fs::path& file) {
    std::map<std::string, std::string> out;
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.rfind("\xEF\xBB\xBF", 0) == 0)
        text.erase(0, 3);
    std::istringstream lines(text);
    for (std::string l; std::getline(lines, l);) {
        size_t eq = l.find('=');
        std::string key = lower(strip(l.substr(0, eq)));
        if (eq == std::string::npos || key.empty() || key[0] == ';' || key[0] == '#')
            continue;
        out[key] = strip(l.substr(eq + 1));
    }
    return out;
}

// Names compared as Windows sorts them for people: case aside, digits as numbers ("Ahriman 2" < "Ahriman 10").
bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2])))
                ++i2;
            while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2])))
                ++j2;
            std::string x = a.substr(i, i2 - i), y = b.substr(j, j2 - j);
            x.erase(0, std::min(x.find_first_not_of('0'), x.size() - 1));
            y.erase(0, std::min(y.find_first_not_of('0'), y.size() - 1));
            if (x.size() != y.size())
                return x.size() < y.size();
            if (x != y)
                return x < y;
            i = i2, j = j2;
            continue;
        }
        char x = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i]))),
             y = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
        if (x != y)
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
        ++i, ++j;
    }
    return a.size() - i < b.size() - j;
}

// --- what the commands print (records.h) ----------------------------------------------------------

using Record = dlcmaker::Record<char>;
using Song = dlcmaker::Song<char, int>;
using Dlc = dlcmaker::Dlc<char, int>;
using Model = dlcmaker::Model<char, int>;
using Wave = dlcmaker::Wave;
using GameStage = dlcmaker::GameStage<char>;
using Stages = dlcmaker::Stages<char, SDL_Texture*>; // monster pictures: on the stage editor's renderer
using SongStage = dlcmaker::SongStage<char>;
using dlcmaker::issue_summary, dlcmaker::issue_suffix, dlcmaker::is_stage_record;
using dlcmaker::kWaves, dlcmaker::kWaveSize, dlcmaker::kDefaultStage, dlcmaker::kDefaultField;

// --- fields (as main.cpp's) ----------------------------------------------------------------------

enum class Kind { Text, Stage, ReadOnly, File, Check, Editor };
struct FieldSpec {
    const char* key;   // the value in a dlc/song record
    const char* label;
    Kind kind;
    const char* set;   // the key `dlcmaker set` takes (Text, Stage); the role `add-file` takes (File)
    const char* cue;   // shown while empty (a record key in braces is replaced by its value); a Check's text
};
const FieldSpec kDlcFields[] = {
    {"name", "Pack name:", Kind::Text, "name", nullptr},
    {"label", "Folder label:", Kind::Text, "label", "{auto_label}"},
    {"series", "Series name:", Kind::Text, "series.name", "the pack name"},
    {"package", "Pack number:", Kind::ReadOnly, nullptr, "given on the first build"},
    {"series_id", "Series number:", Kind::ReadOnly, nullptr, "given on the first build"},
    {"logo", "Logo:", Kind::File, "logo", "none (a base series' logo is shown)"},
    {"logo_wide", "Wide logo:", Kind::File, "logo_wide", "none (the logo is used)"},
};
const FieldSpec kSongFields[] = {
    {"title", "Title:", Kind::Text, "title", "the folder name"},
    {"sort_name", "Sort name:", Kind::Text, "sort_name", "{auto_sort_name}"},
    {"stage", "Stage:", Kind::Stage, "stage", nullptr},
    {"copyright", "Copyright:", Kind::Text, "copyright", nullptr},
    {"start", "Start (s):", Kind::Text, "start", "0"},
    {"end", "End (s):", Kind::Text, "end", "end of the audio"},
    {"fade_out", "Fade-out (s):", Kind::Text, "fade_out", "0"},
    {"chart_offset_ms", "Chart offset (ms):", Kind::Text, "chart_offset_ms", "0"},
    {"preview_start", "Preview from (s):", Kind::Text, "preview_start", "{auto_preview_start}"},
    {"preview_end", "Preview to (s):", Kind::Text, "preview_end", "{auto_preview_end}"},
    {"id", "Song number:", Kind::ReadOnly, nullptr, "given on the first build"},
    {"length", "Length (s):", Kind::ReadOnly, nullptr, "known after Check"},
    {"battle_stage", "Battle stage:", Kind::Editor, nullptr, nullptr},
    {"audio", "Audio:", Kind::File, "audio", "none"},
    {"movie", "Video:", Kind::File, "movie", "none"},
    {"audio_from_video", "", Kind::Check, "audio_from_video", "Use audio from the video"},
    {"jacket", "Jacket:", Kind::File, "jacket", "none (the template song's)"},
    {"thumbnail", "Thumbnail:", Kind::File, "thumbnail", "none (the template song's)"},
    {"charts", "Charts:", Kind::File, "chart", "none"},
};
const char* const kStages[] = {"battle", "event", "field"};
const char* const kStageNames[] = {"Battle (enemies, scrolling background)", "Event (the video plays behind the lanes)",
                                   "Field (the party walks; 9 heights, curved holds)"};
constexpr int kNewSongStages = 2; // a new song of your own: battle or event

struct Field {
    const FieldSpec* spec;
    std::string value, cue; // the text shown, and the grey text while it is empty
    bool check = false;
    int stage = 0;
};

// File dialog filters per add-file role (SDL: extensions separated by ;).
struct Filter {
    const char* role;
    const char* name;
    const char* pattern;
};
const Filter kFilters[] = {
    {"audio", "Audio", "mp3;ogg;opus;flac;wav;m4a;aac"},
    {"movie", "Video", "mp4;mkv;webm;mov;avi"},
    {"jacket", "Images", "png;jpg;jpeg;webp"},
    {"thumbnail", "Images", "png;jpg;jpeg;webp"},
    {"logo", "Images", "png;jpg;jpeg;webp"},
    {"logo_wide", "Images", "png;jpg;jpeg;webp"},
    {"chart", "Charts (charts.json, basic/expert/ultimate/supreme.json)", "json"},
};

// --- state ---------------------------------------------------------------------------------------

enum class JobKind { Info, Edit, Run, Stages };
struct Job {
    JobKind kind;
    std::vector<std::string> args; // after this program's name
    std::string title;             // logged when it starts (Edit, Run)
};

// An image shown under its file field (the DLC's logos).
struct Preview {
    const char* key; // the dlc record's field
    std::string shown; // path, size and write time of the texture shown
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
};

// A question in a box over the window: buttons (the first is Enter's, the last Esc's), or a name to
// type (and a stage for a new song). `done` gets the button's index, -1 for Esc.
struct Dialog {
    std::string text;
    std::vector<std::string> buttons;
    std::function<void(int)> done;
    bool input = false, with_stage = false;
    std::string value;
    int stage = 0;
    std::function<void(const std::string&, int)> entered; // input: OK with a name
    ImGuiContext* shown_in = nullptr; // the window whose box it is in
};

// A window with its own ImGui context (the main window, the stage editor).
struct Surface {
    SDL_Window* wnd = nullptr;
    SDL_Renderer* ren = nullptr;
    ImGuiContext* ctx = nullptr;
};

struct Ui {
    Surface main;
    float scale = 1;
    std::string workspace, game, save; // the folder boxes
    std::string dlc;                   // the game's extra DLC folder (paths.ini dlc=): its packs' IDs are skipped
    std::vector<Field> fields;         // every field of kDlcFields and kSongFields
    std::string heading;
    std::vector<std::vector<std::string>> chart_rows, problem_rows;
    Preview previews[2] = {{"logo", "", nullptr, 0, 0}, {"logo_wide", "", nullptr, 0, 0}};
    std::string log;
    bool log_scroll = false;
    fs::path root;                     // the program's folder
    fs::path home;                     // where the player put it: the AppImage's folder, else root
    Model model;
    int sel_dlc = -1, sel_song = -1;
    std::string sel_folder;            // reselected after a reload
    bool dirty = false, busy = false, close_after = false, quit = false;
    bool picking = false;              // a file dialog is open
    std::deque<Job> jobs;
    Stages stages;                     // the game's battle stages and monsters, for the stage editor
    bool open_editor = false;          // open it once they are read
    std::deque<Dialog> dialogs;
    SDL_Cursor* busy_cursor = nullptr;
};
Ui ui;

Uint32 g_wake = 0; // SDL event: something was posted
std::mutex g_posted_mutex;
std::deque<std::function<void()>> g_posted;

// Runs `fn` on the main thread (from a job's thread or a dialog's callback).
void post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(g_posted_mutex);
        g_posted.push_back(std::move(fn));
    }
    SDL_Event e = {};
    e.type = g_wake;
    SDL_PushEvent(&e);
}

void run_posted() {
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(g_posted_mutex);
            if (g_posted.empty())
                return;
            fn = std::move(g_posted.front());
            g_posted.pop_front();
        }
        fn();
    }
}

float px(float v) {
    return std::floor(v * ui.scale + 0.5f);
}

void append_log(const std::string& line) {
    ui.log += line + "\n";
    if (ui.log.size() > 8 * 1024 * 1024)
        ui.log.erase(0, ui.log.find('\n', ui.log.size() - 6 * 1024 * 1024) + 1);
    ui.log_scroll = true;
}

void ask(std::string text, std::vector<std::string> buttons, std::function<void(int)> done = nullptr) {
    Dialog d;
    d.text = std::move(text);
    d.buttons = std::move(buttons);
    d.done = std::move(done);
    ui.dialogs.push_back(std::move(d));
}

void message(const std::string& text) {
    ask(text, {"OK"});
}

void prompt(std::string label, bool with_stage, std::string value, std::function<void(const std::string&, int)> entered) {
    Dialog d;
    d.text = std::move(label);
    d.buttons = {"OK", "Cancel"};
    d.input = true;
    d.with_stage = with_stage;
    d.value = std::move(value);
    d.entered = std::move(entered);
    ui.dialogs.push_back(std::move(d));
}

fs::path ini_path() {
    return ui.home / "dlcmaker.ini";
}

std::string setting(const char* key) {
    auto ini = read_ini(ini_path());
    auto it = ini.find(key);
    return it == ini.end() ? "" : it->second;
}

void set_setting(const char* key, const std::string& value) {
    auto ini = read_ini(ini_path());
    ini[key] = value;
    ini["workspace"] = strip(ui.workspace);
    ini["game"] = strip(ui.game);
    ini["save"] = strip(ui.save);
    std::ofstream out(ini_path(), std::ios::binary);
    out << "; theatrhythm-dlcmaker settings\n";
    for (auto& [k, v] : ini)
        out << k << "=" << v << "\n";
}

void store_settings() {
    set_setting("workspace", strip(ui.workspace));
}

void start_next_job();

// Queues `theatrhythm-dlcmaker <args>`; jobs run one at a time, and a reload follows edits and runs.
void queue(JobKind kind, std::vector<std::string> args, std::string title = "") {
    ui.jobs.push_back({kind, std::move(args), std::move(title)});
    if (!ui.busy)
        start_next_job();
}

// Reads the game's battle stages and monsters for the stage editor (once per game folder).
void load_stages() {
    std::string game = strip(ui.game);
    if (game.empty() || ui.stages.loading || (ui.stages.game == game && !ui.stages.list.empty()))
        return;
    ui.stages.loading = true;
    ui.stages.game = game;
    queue(JobKind::Stages, {"stage-data", game});
}

void reload() {
    std::string ws = strip(ui.workspace);
    if (ws.empty())
        return;
    queue(JobKind::Info, {"info", ws});
}

void on_job_done(JobKind kind, long code, std::vector<std::string>& lines);
void request_close();

void start_next_job() {
    if (ui.jobs.empty()) {
        ui.busy = false;
        if (ui.close_after)
            ui.quit = true;
        return;
    }
    Job job = ui.jobs.front();
    ui.jobs.pop_front();
    ui.busy = true;
    if (!job.title.empty())
        append_log("> " + job.title);
    std::vector<std::string> cmd = {dlcmaker::exe_path().string()};
    cmd.insert(cmd.end(), job.args.begin(), job.args.end());
    JobKind kind = job.kind;
    std::thread([cmd, kind, cwd = ui.root] {
        auto lines = std::make_shared<std::vector<std::string>>();
        long code = run(cmd, cwd, [&](const std::string& line) {
            lines->push_back(line);
            if (kind != JobKind::Info && !is_stage_record(line))
                post([line] { append_log("  " + line); });
        });
        if (code == -1)
            lines->push_back("cannot start " + cmd[0]);
        post([kind, code, lines] { on_job_done(kind, code, *lines); });
    }).detach();
}

// --- showing the model ---------------------------------------------------------------------------

std::string dlc_text(const Dlc& d) {
    std::vector<Record> all = d.issues;
    for (const Song& s : d.songs)
        all.insert(all.end(), s.issues.begin(), s.issues.end());
    std::string name = d.r["name"].empty() ? fs::path(d.r["folder"]).filename().string() : d.r["name"];
    return name + issue_suffix(all);
}

std::string song_text(const Song& s) {
    std::string title = s.r["title"].empty() ? fs::path(s.r["folder"]).filename().string() : s.r["title"];
    return title + "  · " + s.r["stage"] + issue_suffix(s.issues);
}

const Record* selected_record() {
    if (ui.sel_dlc < 0 || ui.sel_dlc >= static_cast<int>(ui.model.dlcs.size()))
        return nullptr;
    const Dlc& d = ui.model.dlcs[static_cast<size_t>(ui.sel_dlc)];
    if (ui.sel_song < 0)
        return &d.r;
    return ui.sel_song < static_cast<int>(d.songs.size()) ? &d.songs[static_cast<size_t>(ui.sel_song)].r : nullptr;
}

const Song* selected_song() {
    if (ui.sel_dlc < 0 || ui.sel_song < 0 || !selected_record())
        return nullptr;
    return &ui.model.dlcs[static_cast<size_t>(ui.sel_dlc)].songs[static_cast<size_t>(ui.sel_song)];
}

bool field_visible(const Field& f) {
    const Record* r = selected_record();
    if (!r)
        return false;
    bool song = ui.sel_song >= 0;
    bool in_dlc = f.spec >= std::begin(kDlcFields) && f.spec < std::end(kDlcFields);
    if (song == in_dlc)
        return false;
    if (song && std::strcmp(f.spec->key, "thumbnail") == 0)
        return (*r)["stage"] == "event";
    if (song && std::strcmp(f.spec->key, "audio_from_video") == 0)
        return (*r)["clone"].empty();
    if (song && f.spec->kind == Kind::Editor)
        return ((*r)["stage"] == "battle" || (*r)["stage"] == "field") && (*r)["clone"].empty();
    return true;
}

std::string cue_of(const FieldSpec& spec, const Record& r) {
    if (!spec.cue)
        return "";
    std::string cue = spec.cue;
    if (cue.size() > 2 && cue.front() == '{' && cue.back() == '}')
        cue = r[cue.substr(1, cue.size() - 2)];
    if (cue.empty())
        cue = "auto";
    return cue;
}

// The text a File field shows: the file's name, or for charts which files hold them.
std::string file_text(const FieldSpec& spec, const Record& r, const Song* song) {
    if (std::strcmp(spec.key, "charts") == 0) {
        std::string files;
        if (song)
            for (const Record& c : song->charts)
                if (files.find(c["file"]) == std::string::npos)
                    files += (files.empty() ? "" : ", ") + c["file"];
        return files;
    }
    std::string v = r[spec.key];
    return v.empty() ? "" : fs::path(v).filename().string();
}

Field* field(const char* key) {
    for (Field& f : ui.fields)
        if (std::strcmp(f.spec->key, key) == 0)
            return &f;
    return nullptr;
}

SongStage song_stage(const Record& r) {
    return dlcmaker::song_stage(r, ui.stages);
}

void show_selection() {
    const Record* r = selected_record();
    const Song* song = selected_song();
    if (!r)
        ui.heading = ui.model.dlcs.empty() ? "No DLC yet: press New DLC... to start one." : "Select a DLC or a song.";
    else if (song) {
        ui.heading = "Song: " + song_text(*song);
        if (!(*r)["clone"].empty())
            ui.heading += "   (reuses game song " + (*r)["clone"] + ((*r)["stage"] == "event" ? "'s audio, charts and video)" : "'s audio and charts)");
    } else {
        std::string folder = fs::path((*r)["folder"]).filename().string();
        ui.heading = "DLC: " + dlc_text(ui.model.dlcs[static_cast<size_t>(ui.sel_dlc)]);
        if (folder != (*r)["name"])
            ui.heading += "   (folder " + folder + ")";
    }
    for (Field& f : ui.fields) {
        if (!field_visible(f))
            continue;
        if (f.spec->kind == Kind::Check) {
            f.check = (*r)[f.spec->key] == "1";
            continue;
        }
        if (f.spec->kind == Kind::Editor)
            continue;
        if (f.spec->kind == Kind::Stage) {
            f.stage = 0;
            for (int k = 0; k < static_cast<int>(std::size(kStages)); ++k)
                if ((*r)["stage"] == kStages[k])
                    f.stage = k;
            continue;
        }
        std::string value = f.spec->kind == Kind::File ? file_text(*f.spec, *r, song) : (*r)[f.spec->key];
        std::string cue = cue_of(*f.spec, *r);
        if (std::string clone = (*r)["clone"]; song && !clone.empty()) { // a game song's parts, from the dump
            if (std::strcmp(f.spec->key, "movie") == 0 && (*r)["stage"] != "event")
                value = "", cue = "none: only event stages play one";
            else if (std::strcmp(f.spec->key, "audio") == 0 || std::strcmp(f.spec->key, "movie") == 0 || std::strcmp(f.spec->key, "charts") == 0)
                value = "", cue = "from game song " + clone;
            else if (std::strcmp(f.spec->key, "length") == 0)
                cue = "game song " + clone + "'s";
        }
        if (value.empty() && f.spec->kind == Kind::File && !(*r)["clone"].empty() && cue.rfind("from ", 0) == 0)
            value = "(" + cue + ")"; // a clone's parts come from the game song; other empty fields stay empty
        f.value = value;
        f.cue = cue;
    }
    // Charts of the song; problems of the selection (all of them with nothing selected).
    ui.chart_rows.clear();
    if (song) {
        for (const char* diff : {"basic", "expert", "ultimate", "supreme"}) {
            const Record* c = nullptr;
            for (const Record& x : song->charts)
                if (x["difficulty"] == diff)
                    c = &x;
            std::vector<Record> own;
            for (const Record& i : song->issues)
                if (i["difficulty"] == diff)
                    own.push_back(i);
            std::string name = diff;
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            if (!c) {
                ui.chart_rows.push_back({name, "-", "-", "-", std::strcmp(diff, "supreme") == 0 ? "optional" : "missing", ""});
                continue;
            }
            std::string probs = issue_summary(own);
            if (probs.empty())
                probs = !song->r["clone"].empty() ? "the game's own chart"
                        : song->r["checked"] != "1" ? "press Check to check it against the audio" : "ok";
            ui.chart_rows.push_back({name, (*c)["level"], (*c)["notes"], (*c)["bpm"], (*c)["file"], probs});
        }
    }
    ui.problem_rows.clear();
    std::vector<Record> shown;
    if (song)
        shown = song->issues;
    else if (r) {
        const Dlc& d = ui.model.dlcs[static_cast<size_t>(ui.sel_dlc)];
        shown = d.issues;
        for (const Song& s : d.songs)
            shown.insert(shown.end(), s.issues.begin(), s.issues.end());
    } else
        shown = ui.model.issues;
    // Errors first, then warnings, each in the order they were found.
    std::stable_sort(shown.begin(), shown.end(), [](const Record& a, const Record& b) {
        return (a["level"] == "error") > (b["level"] == "error");
    });
    for (const Record& i : shown) {
        std::string where = i["where"]; // "DLC/Song [difficulty]": the song part is enough
        if (size_t slash = where.find('/'); slash != std::string::npos && !i["song"].empty())
            where = where.substr(slash + 1);
        ui.problem_rows.push_back({i["level"] == "error" ? "Error" : "Warning", where, i["message"]});
    }
    if (shown.empty())
        ui.problem_rows.push_back({"", "", r ? "No problems found." : ""});
    ui.dirty = false;
}

void select_item(int dlc, int song) {
    ui.sel_dlc = dlc;
    ui.sel_song = song;
    if (const Record* r = selected_record())
        ui.sel_folder = (*r)["folder"];
    show_selection();
}

void show_model() {
    int select_dlc = -1, select_song = -1;
    for (size_t i = 0; i < ui.model.dlcs.size(); ++i) {
        const Dlc& d = ui.model.dlcs[i];
        if (d.r["folder"] == ui.sel_folder)
            select_dlc = static_cast<int>(i), select_song = -1;
        for (size_t j = 0; j < d.songs.size(); ++j)
            if (d.songs[j].r["folder"] == ui.sel_folder)
                select_dlc = static_cast<int>(i), select_song = static_cast<int>(j);
    }
    if (select_dlc < 0 && ui.model.dlcs.size() == 1) // one DLC: start on it
        select_dlc = 0;
    ui.sel_dlc = ui.sel_song = -1;
    if (select_dlc >= 0)
        select_item(select_dlc, select_song);
    else
        show_selection();
}

// --- editing -------------------------------------------------------------------------------------

// `dlcmaker set` arguments for the selection's edited fields.
std::vector<std::string> set_args() {
    const Record* r = selected_record();
    if (!r)
        return {};
    std::vector<std::string> args = {"set", (*r)["folder"]};
    for (const Field& f : ui.fields) {
        if (!field_visible(f) || !f.spec->set || f.spec->kind == Kind::File)
            continue;
        std::string value;
        if (f.spec->kind == Kind::Check)
            value = f.check ? "true" : "false";
        else if (f.spec->kind == Kind::Stage)
            value = kStages[f.stage >= 0 && f.stage < static_cast<int>(std::size(kStages)) ? f.stage : 0];
        else
            value = strip(f.value);
        args.push_back(std::string(f.spec->set) + "=" + value);
    }
    return args;
}

void save_edits() {
    if (!ui.dirty)
        return;
    std::vector<std::string> args = set_args();
    if (args.empty())
        return;
    ui.dirty = false;
    queue(JobKind::Edit, args, "save " + fs::path(args[1]).filename().string());
    reload();
}

// Unsaved edits before something else: `then` runs once they are saved or dropped, not when the
// player stays.
void settle_edits(const char* before, std::function<void()> then) {
    if (!ui.dirty) {
        then();
        return;
    }
    ask(std::string("Save your changes ") + before + "?", {"Yes", "No", "Cancel"}, [then](int answer) {
        if (answer != 0 && answer != 1)
            return;
        if (answer == 0)
            save_edits();
        ui.dirty = false;
        then();
    });
}

SDL_Window* top_window();

// The desktop's file or folder picker; `then` gets the chosen path. Without a picker (no portal and
// no zenity), the path is typed in.
void pick(bool folder, const std::string& title, const std::string& start, const Filter* filter,
          std::function<void(const std::string&)> then) {
    struct Pick {
        std::function<void(const std::string&)> then;
        std::string title, start;
        bool folder;
        SDL_DialogFileFilter filters[2];
    };
    auto* p = new Pick{std::move(then), title, start, folder, {}};
    int nfilters = 0;
    if (filter) {
        p->filters[0] = {filter->name, filter->pattern};
        p->filters[1] = {"All files", "*"};
        nfilters = 2;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, p->title.c_str());
    std::error_code ec;
    if (!start.empty() && fs::is_directory(start, ec)) {
        p->start = (fs::path(start) / "").string(); // a folder to start in, not a file name
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, p->start.c_str());
    }
    if (nfilters) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, p->filters);
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, nfilters);
    }
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, top_window());
    ui.picking = true;
    // The callback may run on another thread.
    auto picked = [](void* data, const char* const* files, int) {
        auto* p = static_cast<Pick*>(data);
        std::string path = files && files[0] ? files[0] : "", error = files ? "" : SDL_GetError();
        post([p, path, error] {
            std::unique_ptr<Pick> own(p);
            ui.picking = false;
            if (!error.empty()) {
                prompt("No " + std::string(p->folder ? "folder" : "file") + " picker is available (" + error +
                           "). Type the path instead:\n" + p->title,
                       false, p->start, [then = p->then](const std::string& typed, int) { then(typed); });
            } else if (!path.empty()) {
                p->then(path);
            }
        });
    };
    SDL_ShowFileDialogWithProperties(folder ? SDL_FILEDIALOG_OPENFOLDER : SDL_FILEDIALOG_OPENFILE, picked, p, props);
    SDL_DestroyProperties(props);
}

void choose_file(const Field& f) {
    if (!selected_record())
        return;
    const FieldSpec* spec = f.spec;
    settle_edits("first", [spec] {
        const Record* r = selected_record();
        if (!r)
            return;
        const Filter* filter = nullptr;
        for (const Filter& x : kFilters)
            if (std::strcmp(x.role, spec->set) == 0)
                filter = &x;
        std::string label = spec->label;
        std::string title = "Choose the " + lower(label.substr(0, label.size() - 1));
        std::string folder = (*r)["folder"];
        pick(false, title, setting("last_file_folder"), filter, [folder, spec](const std::string& file) {
            set_setting("last_file_folder", fs::path(file).parent_path().string());
            queue(JobKind::Edit, {"add-file", folder, spec->set, file}, "add " + fs::path(file).filename().string());
            reload();
        });
    });
}

void new_dlc() {
    std::string ws = strip(ui.workspace);
    if (ws.empty()) {
        message("Choose a project folder first (Browse...).");
        return;
    }
    if (ui.model.single) {
        message("This project folder is itself one DLC. To add another, choose the folder above it.");
        return;
    }
    settle_edits("first", [ws] {
        prompt("Name of the DLC (also its folder name and, at first, its series):", false, "", [ws](const std::string& name, int) {
            ui.sel_folder = (fs::path(ws) / name).string();
            queue(JobKind::Edit, {"new-dlc", ws, name}, "new DLC " + name);
            reload();
        });
    });
}

void new_song() {
    if (ui.sel_dlc < 0 && ui.model.dlcs.size() == 1)
        ui.sel_dlc = 0;
    if (ui.sel_dlc < 0) {
        message("Select the DLC to add the song to.");
        return;
    }
    settle_edits("first", [] {
        if (ui.sel_dlc < 0 || ui.sel_dlc >= static_cast<int>(ui.model.dlcs.size()))
            return;
        std::string dlc = ui.model.dlcs[static_cast<size_t>(ui.sel_dlc)].r["folder"];
        prompt("Title of the song (also its folder name):", true, "", [dlc](const std::string& name, int stage) {
            ui.sel_folder = (fs::path(dlc) / name).string();
            queue(JobKind::Edit, {"new-song", dlc, name, kStages[stage == 1 ? 1 : 0]}, "new song " + name);
            reload();
        });
    });
}

// check / build / install of the whole project.
void run_command(std::string command) {
    std::string ws = strip(ui.workspace);
    if (ws.empty()) {
        message("Choose a project folder first (Browse...).");
        return;
    }
    settle_edits("first", [command, ws] {
        std::vector<std::string> args = {command, ws};
        if (!ui.dlc.empty())
            args.insert(args.end(), {"--dlc", ui.dlc});
        std::string game = strip(ui.game), save = strip(ui.save);
        if (command != "check") {
            if (game.empty()) {
                message("Choose the game folder (your game dump): packs are built from its files.");
                return;
            }
            args.insert(args.end(), {"--game", game});
        }
        auto go = [command, ws](std::vector<std::string> args) {
            store_settings();
            ui.log.clear();
            queue(JobKind::Run, args, command + " " + ws);
            reload();
        };
        if (command == "build") {
            pick(true, "Export the packs into this folder", setting("build_folder"), nullptr, [args, go](const std::string& out) {
                set_setting("build_folder", out);
                std::vector<std::string> a = args;
                a.insert(a.end(), {"--out", out});
                go(a);
            });
            return;
        }
        if (command == "install") {
            if (save.empty()) {
                message("Choose the save folder: packs are installed into its addcont folder.");
                return;
            }
            args.insert(args.end(), {"--user", save});
        }
        go(args);
    });
}

void open_folder() {
    const Record* r = selected_record();
    std::string folder = r ? (*r)["folder"] : strip(ui.workspace);
    if (!folder.empty() && !start_detached({"xdg-open", folder}))
        message("Cannot open " + folder + " (xdg-open is missing).");
}

// --- images --------------------------------------------------------------------------------------

// `path` as RGBA pixels (png, jpg, bmp...; freed with stbi_image_free); null when it cannot be read.
unsigned char* load_rgba(const fs::path& path, int* w, int* h) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return nullptr;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int n = 0;
    return stbi_load_from_memory(reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()), w, h, &n, 4);
}

// `path` as a texture; null when it cannot be read.
SDL_Texture* load_texture(SDL_Renderer* ren, const std::string& path, int* w = nullptr, int* h = nullptr) {
    int iw = 0, ih = 0;
    unsigned char* pixels = load_rgba(path, &iw, &ih);
    if (!pixels)
        return nullptr;
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, iw, ih);
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, pixels, iw * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(pixels);
    if (w)
        *w = iw;
    if (h)
        *h = ih;
    return tex;
}

ImTextureRef tex_ref(SDL_Texture* tex) {
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(tex)));
}

// `tex` (w x h) fitted into the box at `a` (screen coordinates), centred.
void draw_fitted(ImDrawList* dl, SDL_Texture* tex, int w, int h, ImVec2 a, ImVec2 size) {
    if (!tex || w <= 0 || h <= 0)
        return;
    float s = std::min(size.x / static_cast<float>(w), size.y / static_cast<float>(h));
    ImVec2 d(std::floor(w * s), std::floor(h * s));
    ImVec2 o(a.x + std::floor((size.x - d.x) / 2), a.y + std::floor((size.y - d.y) / 2));
    dl->AddImage(tex_ref(tex), o, ImVec2(o.x + d.x, o.y + d.y));
}

// The selection's image for `p`, reloaded only when the file changed.
void update_preview(Preview& p) {
    const Record* r = selected_record();
    std::string path = r && ui.sel_song < 0 ? (*r)[p.key] : "";
    std::error_code ec;
    std::string key;
    if (!path.empty() && fs::is_regular_file(path, ec))
        key = path + "|" + std::to_string(fs::file_size(path, ec)) + "|" +
              std::to_string(static_cast<long long>(fs::last_write_time(path, ec).time_since_epoch().count()));
    if (key == p.shown)
        return;
    p.shown = key;
    if (p.tex)
        SDL_DestroyTexture(p.tex);
    p.tex = key.empty() ? nullptr : load_texture(ui.main.ren, path, &p.w, &p.h);
}

// The text cut to `width` with an ellipsis.
std::string ellipsis(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;
    std::string s = text;
    while (!s.empty()) {
        do
            s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80);
        if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xC0)
            s.pop_back(); // a cut sequence's lead byte
        if (ImGui::CalcTextSize((s + "…").c_str()).x <= width)
            return s + "…";
    }
    return "…";
}

ImU32 rgb(int r, int g, int b) {
    return IM_COL32(r, g, b, 255);
}

// A button at (x, y) in the current window, w x h.
bool button_at(const char* label, float x, float y, float w, float h, bool enabled = true) {
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::Button(label, ImVec2(w, h));
    ImGui::EndDisabled();
    return pressed;
}

void text_at(const char* text, float x, float y) {
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::TextUnformatted(text);
}

// A combo box of `items` at the cursor; true when the choice changed (`sel` holds it).
bool combo(const char* id, const std::vector<std::string>& items, int& sel, float width) {
    ImGui::SetNextItemWidth(width);
    bool changed = false;
    const char* shown = sel >= 0 && sel < static_cast<int>(items.size()) ? items[static_cast<size_t>(sel)].c_str() : "";
    if (ImGui::BeginCombo(id, shown, ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            bool is = i == sel;
            if (ImGui::Selectable((items[static_cast<size_t>(i)] + "##" + std::to_string(i)).c_str(), is) && !is) {
                sel = i;
                changed = true;
            }
            if (is)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// --- windows -------------------------------------------------------------------------------------

void style_context(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(theatrhythm_font_ttf),
                                   static_cast<int>(theatrhythm_font_ttf_end - theatrhythm_font_ttf), 14.0f * scale, &cfg);
    // Japanese and other titles: the desktop's font for them, merged in (glyphs load as they are used).
    static std::string cjk = [] {
        std::string path;
        if (FILE* f = popen("fc-match -f '%{file}' 'sans-serif:lang=ja' 2>/dev/null", "r")) {
            char buf[1024];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            pclose(f);
            path.assign(buf, n);
        }
        std::error_code ec;
        std::string ext = lower(fs::path(path).extension().string());
        return fs::is_regular_file(path, ec) && (ext == ".ttf" || ext == ".otf" || ext == ".ttc") &&
                       lower(path).find("dejavu") == std::string::npos
                   ? path
                   : std::string();
    }();
    if (!cjk.empty()) {
        ImFontConfig merge;
        merge.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(cjk.c_str(), 14.0f * scale, &merge);
    }
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsLight(&style);
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 5);
    style.ItemSpacing = ImVec2(6, 4);
    style.FrameBorderSize = 1;
    style.ChildBorderSize = 1;
    style.FrameRounding = 2;
    style.GrabRounding = 2;
    style.PopupRounding = 2;
    style.WindowRounding = 0;
    style.ScrollbarSize = 14;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.94f, 0.94f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_Border] = ImVec4(0.67f, 0.67f, 0.67f, 1);
    c[ImGuiCol_Button] = ImVec4(0.88f, 0.88f, 0.88f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.90f, 0.94f, 0.99f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.88f, 0.97f, 1);
    c[ImGuiCol_Header] = ImVec4(0.80f, 0.88f, 0.97f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.90f, 0.94f, 0.99f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.75f, 0.85f, 0.97f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.94f, 0.94f, 0.94f, 1);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.25f);
    style.ScaleAllSizes(scale);
}

bool make_surface(Surface& s, const char* title, int w, int h, SDL_WindowFlags flags) {
    s.wnd = SDL_CreateWindow(title, w, h, flags | SDL_WINDOW_HIDDEN);
    if (!s.wnd)
        return false;
    s.ren = SDL_CreateRenderer(s.wnd, nullptr);
    if (!s.ren) {
        SDL_DestroyWindow(s.wnd);
        s.wnd = nullptr;
        return false;
    }
    SDL_SetRenderVSync(s.ren, 1);
    ImGuiContext* previous = ImGui::GetCurrentContext();
    s.ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(s.ctx);
    style_context(ui.scale);
    ImGui_ImplSDL3_InitForSDLRenderer(s.wnd, s.ren);
    ImGui_ImplSDLRenderer3_Init(s.ren);
    if (previous)
        ImGui::SetCurrentContext(previous);
    return true;
}

void destroy_surface(Surface& s) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.ctx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(s.ctx);
    ImGui::SetCurrentContext(previous == s.ctx ? nullptr : previous);
    SDL_DestroyRenderer(s.ren);
    SDL_DestroyWindow(s.wnd);
    s = {};
}

void begin_frame(Surface& s) {
    ImGui::SetCurrentContext(s.ctx);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##window", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
}

void end_frame(Surface& s) {
    ImGui::End();
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(s.ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(s.ren, 240, 240, 240, 255);
    SDL_RenderClear(s.ren);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s.ren);
    SDL_RenderPresent(s.ren);
}

// The front dialog over the current window.
void draw_dialogs() {
    if (ui.dialogs.empty())
        return;
    Dialog& d = ui.dialogs.front();
    const char* id = "##dialog";
    if (d.shown_in != ImGui::GetCurrentContext()) {
        ImGui::OpenPopup(id);
        d.shown_in = ImGui::GetCurrentContext();
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 3), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(px(380), 0), ImVec2(std::max(px(380), io.DisplaySize.x - px(40)), io.DisplaySize.y - px(40)));
    int answer = -2;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(12)));
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(std::max(px(420), std::min(px(640), io.DisplaySize.x - px(80))));
        ImGui::TextUnformatted(d.text.c_str());
        ImGui::PopTextWrapPos();
        bool enter = false;
        if (d.input) {
            ImGui::Spacing();
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(std::max(px(400), ImGui::GetContentRegionAvail().x));
            enter = ImGui::InputText("##value", &d.value, ImGuiInputTextFlags_EnterReturnsTrue);
            if (d.with_stage) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Stage:");
                ImGui::SameLine();
                std::vector<std::string> names(kStageNames, kStageNames + kNewSongStages);
                combo("##stage", names, d.stage, ImGui::GetContentRegionAvail().x);
            }
        }
        ImGui::Spacing();
        ImGui::Spacing();
        float bw = px(90), total = d.buttons.size() * bw + (d.buttons.size() - 1) * px(6);
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total));
        for (size_t i = 0; i < d.buttons.size(); ++i) {
            if (i)
                ImGui::SameLine(0, px(6));
            if (ImGui::Button(d.buttons[i].c_str(), ImVec2(bw, px(28))))
                answer = static_cast<int>(i);
        }
        if (enter || (!d.input && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
            answer = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            answer = d.buttons.size() > 1 ? static_cast<int>(d.buttons.size()) - 1 : 0;
        if (answer != -2)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    if (answer == -2)
        return;
    Dialog done = std::move(ui.dialogs.front());
    ui.dialogs.pop_front();
    if (done.input) {
        std::string name = strip(done.value);
        if (answer == 0 && !name.empty() && done.entered)
            done.entered(name, done.stage);
    } else if (done.done) {
        done.done(answer);
    }
}

// --- stage editor --------------------------------------------------------------------------------
// A battle song's stage in a window of its own (A): the game stage it starts from (a series, then one
// of its songs, or Random), its background (a series, then a background named after a song of that
// series on it) and its enemy list, in a scrolling panel. Clicking a monster, + or Add wave opens the
// monster picker (B), which widens the window to the right without moving A.

constexpr int kEdWidth = 880, kEdHeight = 620, kEdFieldHeight = 330, kSideWidth = 340, kWaveRow = 86, kTile = 92, kImage = 56, kCell = 84;

struct Editor {
    Surface s;
    Record song;
    int from = kDefaultStage;
    std::string background, bg_series_name;
    std::vector<Wave> waves;
    bool field = false;                 // a field song: a background only, no enemies
    int pick_wave = -1, pick_slot = -1; // the picker is open while pick_wave >= 0; pick_wave == waves: a new wave
    int home_x = 0;                     // where A was when the picker opened
    std::string search;
    std::vector<int> shown;             // the picker's monsters, after the search
    bool focus_search = false, grid_top = false, list_top = false;
    bool finished = false, ok = false;
};
std::unique_ptr<Editor> g_ed;

SDL_Window* top_window() {
    return g_ed ? g_ed->s.wnd : ui.main.wnd;
}

// The stages the editor offers: battle stages, or field stages for a field song.
const std::vector<GameStage>& ed_list() {
    return g_ed->field ? ui.stages.fields : ui.stages.list;
}

const GameStage* ed_find(int number) {
    return ui.stages.find(number, g_ed->field);
}

std::string monster_name(int id) {
    auto it = ui.stages.monsters.find(id);
    return it == ui.stages.monsters.end() ? "Monster " + std::to_string(id) : it->second.name;
}

SDL_Texture* monster_texture(int id, int* w, int* h) {
    auto it = ui.stages.monsters.find(id);
    if (it == ui.stages.monsters.end() || it->second.image.empty())
        return nullptr;
    if (!it->second.picture)
        it->second.picture = load_texture(g_ed->s.ren, it->second.image);
    float fw = 0, fh = 0;
    if (it->second.picture)
        SDL_GetTextureSize(it->second.picture, &fw, &fh);
    *w = static_cast<int>(fw);
    *h = static_cast<int>(fh);
    return it->second.picture;
}

bool ed_same() {
    if (g_ed->field)
        return g_ed->from == kDefaultField;
    const GameStage* g = ed_find(g_ed->from);
    return g && g->background == g_ed->background && g->waves == g_ed->waves;
}

// The background's series: the one shown if it has the background, else the stage's own, else the first.
void ed_settle_bg_series() {
    Editor& e = *g_ed;
    const GameStage* g = ed_find(e.from);
    std::string own = g ? g->series : "";
    auto has = [&](const std::string& s) {
        auto b = dlcmaker::series_backgrounds(ed_list(), s);
        return std::any_of(b.begin(), b.end(), [&](auto& x) { return x.first == e.background; });
    };
    if (has(e.bg_series_name))
        return;
    e.bg_series_name = own;
    if (!has(own))
        for (const std::string& s : dlcmaker::stage_series(ed_list()))
            if (has(s)) {
                e.bg_series_name = s;
                break;
            }
}

void ed_filter() {
    Editor& e = *g_ed;
    std::string q = lower(strip(e.search));
    e.shown.clear();
    for (auto& [id, m] : ui.stages.monsters)
        if (lower(m.name).find(q) != std::string::npos)
            e.shown.push_back(id);
    std::sort(e.shown.begin(), e.shown.end(),
              [](int a, int b) { return natural_less(ui.stages.monsters[a].name, ui.stages.monsters[b].name); });
    e.grid_top = true;
}

// The window's width: A alone, or A and the picker. A stays where it is.
void ed_resize(bool side) {
    Editor& e = *g_ed;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(e.s.wnd, &x, &y);
    SDL_GetWindowSize(e.s.wnd, &w, &h);
    int nw = static_cast<int>(px(kEdWidth + (side ? kSideWidth : 0)));
    int nx = side ? x : e.home_x;
    if (side) {
        e.home_x = x;
        SDL_Rect work;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(e.s.wnd), &work) && nx + nw > work.x + work.w)
            nx = std::max(work.x, work.x + work.w - nw); // no room on the right: the screen's edge wins
    }
    SDL_SetWindowSize(e.s.wnd, nw, h);
    if (nx != x)
        SDL_SetWindowPosition(e.s.wnd, nx, y);
}

void ed_pick(int wave, int slot) {
    Editor& e = *g_ed;
    bool open = e.pick_wave >= 0;
    e.pick_wave = wave;
    e.pick_slot = slot;
    if (!open) {
        e.search.clear();
        ed_filter();
        ed_resize(true);
    }
    e.focus_search = true;
}

void ed_pick_close() {
    Editor& e = *g_ed;
    if (e.pick_wave < 0)
        return;
    e.pick_wave = e.pick_slot = -1;
    ed_resize(false);
}

void ed_start(int number) {
    Editor& e = *g_ed;
    const GameStage* g = ed_find(number);
    if (!g)
        return;
    e.from = number;
    e.background = g->background;
    e.bg_series_name = g->series;
    e.waves = g->waves;
    e.list_top = true;
    ed_pick_close();
    ed_settle_bg_series();
}

// A field takes its camera and sky from a game song too: the first one of the series shown on that background.
void ed_field_from() {
    Editor& e = *g_ed;
    if (!e.field)
        return;
    for (const GameStage& s : ed_list())
        if (s.series == e.bg_series_name && s.background == e.background) {
            e.from = s.number;
            return;
        }
}

// A monster picture with its name under it, at screen position (x, y) in a w wide cell; `selected`:
// the one the picker replaces.
void draw_monster(ImDrawList* dl, int id, float x, float y, float w, bool selected) {
    float is = px(kImage);
    ImVec2 a(x + std::floor((w - is) / 2), y), b(a.x + is, a.y + is);
    float f = selected ? px(3) : 1;
    dl->AddRectFilled(ImVec2(a.x - f, a.y - f), ImVec2(b.x + f, b.y + f), selected ? rgb(47, 111, 208) : rgb(204, 202, 196));
    dl->AddRectFilled(a, b, rgb(255, 255, 255));
    int tw = 0, th = 0;
    if (SDL_Texture* tex = monster_texture(id, &tw, &th)) {
        draw_fitted(dl, tex, tw, th, a, ImVec2(is, is));
    } else {
        ImVec2 q = ImGui::CalcTextSize("?");
        dl->AddText(ImVec2(a.x + (is - q.x) / 2, a.y + (is - q.y) / 2), rgb(136, 136, 136), "?");
    }
    std::string name = ellipsis(monster_name(id), w - 4);
    ImVec2 n = ImGui::CalcTextSize(name.c_str());
    dl->AddText(ImVec2(x + std::floor((w - n.x) / 2), b.y + px(3)), rgb(0, 0, 0), name.c_str());
}

// The enemy list: one row per wave, then Add wave.
void ed_list_panel(float width, float height) {
    Editor& e = *g_ed;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("##waves", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleColor();
    if (e.list_top) {
        ImGui::SetScrollY(0);
        e.list_top = false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    enum class Act { None, Pick, Remove, Boss, Up, Delete } act = Act::None;
    int act_wave = 0, act_slot = 0;
    const float gap = px(6), bw = px(70), bh = px(22), W = ImGui::GetContentRegionAvail().x;
    float y = gap;
    for (int wi = 0; wi < static_cast<int>(e.waves.size()); ++wi) {
        const Wave& w = e.waves[static_cast<size_t>(wi)];
        ImGui::PushID(wi);
        ImGui::SetCursorPos(ImVec2(gap, y));
        ImVec2 o = ImGui::GetCursorScreenPos(); // the row's corner on screen
        float rw = W - 2 * gap, rh = px(kWaveRow);
        dl->AddRectFilled(o, ImVec2(o.x + rw, o.y + rh), w.boss ? rgb(163, 65, 45) : rgb(204, 202, 196));
        dl->AddRectFilled(ImVec2(o.x + 1, o.y + 1), ImVec2(o.x + rw - 1, o.y + rh - 1), w.boss ? rgb(251, 233, 227) : rgb(255, 255, 255));
        std::string number = std::to_string(wi + 1);
        ImVec2 ns = ImGui::CalcTextSize(number.c_str());
        dl->AddText(ImVec2(o.x + (px(28) - ns.x) / 2, o.y + (rh - ns.y) / 2), rgb(109, 109, 109), number.c_str());
        if (w.boss) {
            ImVec2 bp(o.x + px(28), o.y + (rh - ns.y) / 2);
            dl->AddText(bp, rgb(163, 65, 45), "Boss");
            dl->AddText(ImVec2(bp.x + 1, bp.y), rgb(163, 65, 45), "Boss"); // bold
        }
        float tx = gap + px(70);
        for (int si = 0; si < static_cast<int>(w.monsters.size()); ++si) {
            ImGui::PushID(si);
            ImGui::SetCursorPos(ImVec2(tx, y));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##monster", ImVec2(px(kTile), rh)))
                act = Act::Pick, act_wave = wi, act_slot = si;
            draw_monster(dl, w.monsters[static_cast<size_t>(si)], o.x - gap + tx, o.y + px(7), px(kTile), e.pick_wave == wi && e.pick_slot == si);
            float ix = tx + (px(kTile) + px(kImage)) / 2;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            if (button_at("×", ix - px(10), y + px(3), px(16), px(16)))
                act = Act::Remove, act_wave = wi, act_slot = si;
            ImGui::PopStyleVar();
            ImGui::PopID();
            tx += px(kTile);
        }
        if (w.monsters.size() < kWaveSize) {
            float ix = tx + (px(kTile) - px(kImage)) / 2;
            bool sel = e.pick_wave == wi && e.pick_slot == static_cast<int>(w.monsters.size());
            if (sel)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (button_at("+", ix, y + px(7), px(kImage), px(kImage)))
                act = Act::Pick, act_wave = wi, act_slot = static_cast<int>(w.monsters.size());
            if (sel)
                ImGui::PopStyleColor();
        }
        float bx = gap + rw - px(6) - bw;
        if (button_at(w.boss ? "Not boss" : "Boss", bx, y + px(6), bw, bh))
            act = Act::Boss, act_wave = wi;
        if (button_at("Up", bx, y + px(32), bw, bh, wi > 0))
            act = Act::Up, act_wave = wi;
        if (button_at("Delete", bx, y + px(58), bw, bh))
            act = Act::Delete, act_wave = wi;
        ImGui::PopID();
        y += rh + gap;
    }
    if (static_cast<int>(e.waves.size()) < kWaves) {
        bool sel = e.pick_wave == static_cast<int>(e.waves.size());
        if (sel)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (button_at("Add wave", gap, y, px(100), px(28)))
            act = Act::Pick, act_wave = static_cast<int>(e.waves.size()), act_slot = 0;
        if (sel)
            ImGui::PopStyleColor();
        y += px(28) + gap;
    }
    ImGui::SetCursorPos(ImVec2(0, y));
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::EndChild();
    if (act == Act::None)
        return;
    if (act == Act::Pick) {
        ed_pick(act_wave, act_slot);
        return;
    }
    auto& waves = e.waves;
    size_t wv = static_cast<size_t>(act_wave);
    if (act == Act::Remove) {
        waves[wv].monsters.erase(waves[wv].monsters.begin() + act_slot);
        if (waves[wv].monsters.empty())
            waves.erase(waves.begin() + act_wave);
    } else if (act == Act::Boss) {
        waves[wv].boss = !waves[wv].boss;
    } else if (act == Act::Up) {
        std::swap(waves[wv], waves[wv - 1]);
    } else if (act == Act::Delete) {
        waves.erase(waves.begin() + act_wave);
    }
    ed_pick_close(); // the waves moved: what the picker was for may be gone
}

// The monster picker's grid: every monster of the search, three to a row.
void ed_grid_panel(float width, float height) {
    Editor& e = *g_ed;
    ImGui::BeginChild("##grid", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (e.grid_top) {
        ImGui::SetScrollY(0);
        e.grid_top = false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cw = std::floor(ImGui::GetContentRegionAvail().x / 3), top = ImGui::GetScrollY(), view = ImGui::GetWindowHeight();
    int picked = -1;
    for (size_t i = 0; i < e.shown.size(); ++i) {
        float x = static_cast<float>(i % 3) * cw, y = px(6) + static_cast<float>(i / 3) * px(kCell);
        ImGui::SetCursorPos(ImVec2(x, y));
        if (y + px(kCell) < top - px(kCell) || y > top + view + px(kCell)) // off screen: only its room
            continue;
        ImGui::PushID(static_cast<int>(i));
        ImVec2 o = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##m", ImVec2(cw, px(kCell))))
            picked = e.shown[i];
        if (ImGui::IsItemHovered())
            dl->AddRectFilled(o, ImVec2(o.x + cw, o.y + px(kCell)), rgb(229, 239, 251));
        draw_monster(dl, e.shown[i], o.x, o.y + px(4), cw, false);
        ImGui::PopID();
    }
    ImGui::SetCursorPos(ImVec2(0, px(6) + static_cast<float>((e.shown.size() + 2) / 3) * px(kCell)));
    ImGui::Dummy(ImVec2(1, 1));
    if (e.shown.empty()) {
        const char* note = "No monster by that name.";
        ImGui::SetCursorPos(ImVec2((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(note).x) / 2, px(20)));
        ImGui::TextDisabled("%s", note);
    }
    ImGui::EndChild();
    if (picked < 0)
        return;
    if (e.pick_wave >= static_cast<int>(e.waves.size()))
        e.waves.push_back({false, {picked}});
    else if (Wave& w = e.waves[static_cast<size_t>(e.pick_wave)]; e.pick_slot >= static_cast<int>(w.monsters.size()))
        w.monsters.push_back(picked);
    else
        w.monsters[static_cast<size_t>(e.pick_slot)] = picked;
    ed_pick_close();
}

void ed_frame() {
    Editor& e = *g_ed;
    begin_frame(e.s);
    bool modal = !ui.dialogs.empty();
    ImGui::BeginDisabled(ui.picking);
    const float A = px(kEdWidth), H = ImGui::GetIO().DisplaySize.y, m = px(12), rh = ImGui::GetFrameHeight(), bh = px(28),
                gap = px(6), lw = std::max(px(86), std::max(ImGui::CalcTextSize("Start from:").x, ImGui::CalcTextSize("Background:").x) + px(8)),
                cw = px(250), rb = px(90);
    float y1 = m, y2 = y1 + rh + px(10), x2 = m + lw + cw + gap, w2 = A - m - rb - gap - x2;
    if (e.field) // the background row first, Random on it, the preview under it
        y2 = y1;
    std::vector<std::string> series = dlcmaker::stage_series(ed_list());
    auto index = [](const auto& v, const auto& x) {
        auto it = std::find(v.begin(), v.end(), x);
        return it == v.end() ? 0 : static_cast<int>(it - v.begin());
    };
    const GameStage* g = ed_find(e.from);
    std::string own = g ? g->series : "";
    if (!e.field) {
        text_at("Start from:", m, y1 + (rh - ImGui::GetTextLineHeight()) / 2);
        ImGui::SetCursorPos(ImVec2(m + lw, y1));
        int si = index(series, own);
        if (combo("##series", series, si, cw)) {
            for (const GameStage& s : ed_list())
                if (s.series == series[static_cast<size_t>(si)]) {
                    ed_start(s.number);
                    break;
                }
        }
        std::vector<std::string> titles;
        std::vector<int> numbers;
        for (const GameStage& s : ed_list())
            if (s.series == own) {
                titles.push_back(s.title);
                numbers.push_back(s.number);
            }
        ImGui::SetCursorPos(ImVec2(x2, y1));
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        int ti = index(numbers, e.from);
        if (combo("##song", titles, ti, w2))
            ed_start(numbers[static_cast<size_t>(ti)]);
    }
    if (button_at("Random", A - m - rb, (e.field ? y2 : y1) - px(2), rb, bh)) {
        static std::mt19937 rng{std::random_device{}()};
        ed_start(ed_list()[std::uniform_int_distribution<size_t>(0, ed_list().size() - 1)(rng)].number);
    }
    text_at("Background:", m, y2 + (rh - ImGui::GetTextLineHeight()) / 2);
    ImGui::SetCursorPos(ImVec2(m + lw, y2));
    int bsi = index(series, e.bg_series_name);
    if (combo("##bgseries", series, bsi, cw)) {
        e.bg_series_name = series[static_cast<size_t>(bsi)];
        auto b = dlcmaker::series_backgrounds(ed_list(), e.bg_series_name);
        if (!b.empty())
            e.background = b.front().first;
        ed_field_from();
        ed_settle_bg_series();
    }
    std::vector<std::string> names, ids;
    for (auto& [id, title] : dlcmaker::series_backgrounds(ed_list(), e.bg_series_name)) {
        ids.push_back(id);
        names.push_back(title);
    }
    ImGui::SetCursorPos(ImVec2(x2, y2));
    int bi = index(ids, e.background);
    if (combo("##bg", names, bi, w2)) {
        e.background = ids[static_cast<size_t>(bi)];
        ed_field_from();
        ed_settle_bg_series();
    }
    float cy = y2 + rh + px(14), fy = H - m - bh, pw = px(220);
    // The background's box: its name (no pictures of backgrounds yet).
    {
        ImGui::SetCursorPos(ImVec2(m, cy));
        ImVec2 o = ImGui::GetCursorScreenPos(), size(e.field ? A - 2 * m : pw, e.field ? fy - px(10) - cy : px(150));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(o, ImVec2(o.x + size.x, o.y + size.y), rgb(160, 160, 160));
        std::string text = dlcmaker::background_name(ed_list(), e.bg_series_name, e.background);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x - px(8));
        ImGui::SetCursorPos(ImVec2(m + px(4), cy + px(8)));
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::Spacing();
        ImGui::SetCursorPosX(m + px(4));
        ImGui::TextDisabled("No picture of this background yet.");
        ImGui::PopTextWrapPos();
    }
    if (!e.field) {
        ImGui::SetCursorPos(ImVec2(m + pw + px(12), cy));
        ed_list_panel(A - m - (m + pw + px(12)), fy - px(10) - cy);
    }
    if (button_at("Restore default", m, fy, px(120), bh, !ed_same()))
        ed_start(e.field ? kDefaultField : e.from);
    if (button_at("Cancel", A - m - 2 * rb - gap, fy, rb, bh))
        e.finished = true;
    if (button_at("Done", A - m - rb, fy, rb, bh))
        e.ok = e.finished = true;
    if (e.pick_wave >= 0) {
        float S = px(kSideWidth), cb = px(80);
        ImVec2 o = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(o.x + A, o.y), ImVec2(o.x + A, o.y + H), rgb(160, 160, 160), 2);
        ImGui::SetCursorPos(ImVec2(A + m, m));
        ImGui::SetNextItemWidth(S - 2 * m - cb - gap);
        if (e.focus_search) {
            ImGui::SetKeyboardFocusHere();
            e.focus_search = false;
        }
        if (ImGui::InputTextWithHint("##search", "Search monsters", &e.search))
            ed_filter();
        if (button_at("Close", A + S - m - cb, m - px(2), cb, bh))
            ed_pick_close();
        ImGui::SetCursorPos(ImVec2(A + m, m + rh + px(10)));
        if (e.pick_wave >= 0)
            ed_grid_panel(S - 2 * m, H - m - (m + rh + px(10)));
    }
    // Esc closes the picker first, then the editor; Enter is Done.
    if (!modal && !ui.picking && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (e.pick_wave >= 0)
            ed_pick_close();
        else
            e.finished = true;
    }
    if (!modal && !ui.picking && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        e.ok = e.finished = true;
    ImGui::EndDisabled();
    draw_dialogs();
    end_frame(e.s);
}

void close_stage_editor() {
    Editor& e = *g_ed;
    for (auto& [id, m] : ui.stages.monsters)
        if (m.picture) {
            SDL_DestroyTexture(m.picture);
            m.picture = nullptr;
        }
    destroy_surface(e.s);
    std::unique_ptr<Editor> done = std::move(g_ed);
    SDL_RaiseWindow(ui.main.wnd);
    if (!done->ok)
        return;
    // song.json keeps the game stage it starts from, and the background and enemies only where they differ.
    const GameStage* g = ui.stages.find(done->from, done->field);
    bool own_bg = !done->field && (!g || done->background != g->background), own_waves = !done->field && (!g || done->waves != g->waves);
    queue(JobKind::Edit,
          {"set", done->song["folder"], "stage_from=" + std::to_string(done->from), "background=" + (own_bg ? done->background : ""),
           "enemy_waves=" + (own_waves ? dlcmaker::waves_text<char>(done->waves) : ""), "enemies="},
          "save the stage of " + fs::path(done->song["folder"]).filename().string());
    reload();
}

// Edits the selected battle or field song's stage; Done saves it to its song.json.
void open_stage_editor() {
    const Record* sel = selected_record();
    if (!sel || ui.sel_song < 0 || g_ed)
        return;
    std::string game = strip(ui.game);
    if ((ui.stages.list.empty() && ui.stages.fields.empty()) || ui.stages.game != game) {
        if (game.empty()) {
            message("Choose the game folder first: the stage editor shows the game's own stages and monsters.");
            return;
        }
        ui.open_editor = true; // opens when they are read
        load_stages();
        return;
    }
    settle_edits("first", [] {
        const Record* sel = selected_record();
        if (!sel || ui.sel_song < 0 || g_ed)
            return;
        auto e = std::make_unique<Editor>();
        e->song = *sel;
        e->field = e->song["stage"] == "field";
        g_ed = std::move(e);
        Editor& ed = *g_ed;
        if (ed_list().empty()) {
            g_ed.reset();
            message("No stages of this kind were found in the game folder.");
            return;
        }
        SongStage s = song_stage(ed.song);
        ed.from = ed_find(s.from) ? s.from : ed_list().front().number;
        ed.background = s.background.empty() ? ed_find(ed.from)->background : s.background;
        ed.waves = s.waves;
        ed.bg_series_name = ed_find(ed.from)->series;
        ed_settle_bg_series();
        SDL_Rect work = {0, 0, 1920, 1080};
        SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(ui.main.wnd), &work);
        int w = static_cast<int>(px(kEdWidth)), h = std::min(static_cast<int>(px(ed.field ? kEdFieldHeight : kEdHeight)), work.h - static_cast<int>(px(60)));
        std::string title = "Stage editor — " + (ed.song["title"].empty() ? fs::path(ed.song["folder"]).filename().string() : ed.song["title"]);
        if (!make_surface(ed.s, title.c_str(), w, h, 0)) {
            g_ed.reset();
            message(std::string("Cannot open the stage editor's window (") + SDL_GetError() + ").");
            return;
        }
        int ox = 0, oy = 0, ow = 0, oh = 0;
        SDL_GetWindowPosition(ui.main.wnd, &ox, &oy);
        SDL_GetWindowSize(ui.main.wnd, &ow, &oh);
        SDL_SetWindowParent(ed.s.wnd, ui.main.wnd);
        SDL_SetWindowModal(ed.s.wnd, true);
        SDL_SetWindowPosition(ed.s.wnd, std::clamp(ox + (ow - w) / 2, work.x, std::max(work.x, work.x + work.w - w)),
                              std::clamp(oy + (oh - h) / 2, work.y, std::max(work.y, work.y + work.h - h)));
        SDL_ShowWindow(ed.s.wnd);
        SDL_RaiseWindow(ed.s.wnd);
    });
}

// --- the main window -----------------------------------------------------------------------------

// Layout constants in 96-dpi units, as main.cpp's.
constexpr int kMargin = 12, kButton = 96, kGap = 6, kLabel = 118, kTreeWidth = 260, kBottom = 150;

void table(const char* id, std::initializer_list<std::pair<const char*, float>> cols, const std::vector<std::vector<std::string>>& rows,
           ImVec2 size) {
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                            ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(id, static_cast<int>(cols.size()), flags, size))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    int i = 0;
    for (auto& [name, width] : cols)
        ImGui::TableSetupColumn(name, width > 0 ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch,
                                width > 0 ? px(width) : 0, i++);
    ImGui::TableHeadersRow();
    for (const auto& row : rows) {
        ImGui::TableNextRow();
        for (size_t c = 0; c < row.size() && c < cols.size(); ++c) {
            ImGui::TableSetColumnIndex(static_cast<int>(c));
            ImGui::TextUnformatted(row[c].c_str());
            if (c + 1 == cols.size() && ImGui::IsItemHovered() && ImGui::CalcTextSize(row[c].c_str()).x > ImGui::GetColumnWidth())
                ImGui::SetTooltip("%s", row[c].c_str());
        }
    }
    ImGui::EndTable();
}

// A path box with its label and a Browse... button; true when a folder was typed (Enter) or picked.
void folder_row(const char* label, std::string& value, const char* hint, const char* title, float y, float W, float lx,
                std::function<void()> changed) {
    const float m = px(kMargin), bw = px(kButton), bh = px(28), rh = ImGui::GetFrameHeight();
    text_at(label, m, y + (rh - ImGui::GetTextLineHeight()) / 2);
    ImGui::SetCursorPos(ImVec2(m + lx, y));
    ImGui::SetNextItemWidth(W - 2 * m - lx - bw - px(kGap));
    ImGui::PushID(label);
    if (ImGui::InputTextWithHint("##path", hint, &value, ImGuiInputTextFlags_EnterReturnsTrue))
        changed();
    if (ImGui::IsItemDeactivatedAfterEdit())
        store_settings();
    if (button_at("Browse...", W - m - bw, y - (bh - rh) / 2, bw, bh))
        pick(true, title, strip(value), nullptr, [&value, changed](const std::string& folder) {
            value = folder;
            changed();
        });
    ImGui::PopID();
}

void main_frame() {
    begin_frame(ui.main);
    bool locked = ui.busy || ui.picking || g_ed != nullptr;
    bool blocked = ui.picking || g_ed != nullptr || !ui.dialogs.empty(); // nothing behind a box or a picker takes input
    ImGui::BeginDisabled(ui.picking || g_ed != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y, m = px(kMargin), bh = px(28), rh = ImGui::GetFrameHeight(), gap = px(kGap);
    const float bw = std::max(px(kButton), ImGui::CalcTextSize("Export packs...").x + 2 * ImGui::GetStyle().FramePadding.x);
    const float lx = std::max(px(112), std::max({ImGui::CalcTextSize("Project folder:").x, ImGui::CalcTextSize("Game folder:").x,
                                                 ImGui::CalcTextSize("Save folder:").x}) + px(8));
    const float by_off = (bh - rh) / 2; // buttons sit centred on their row's boxes

    // Top: the project folder.
    float y = m;
    text_at("Project folder:", m, y + (rh - ImGui::GetTextLineHeight()) / 2);
    float bx = W - m - bw;
    ImGui::BeginDisabled(ui.busy);
    if (button_at("Open folder", bx, y - by_off, bw, bh))
        open_folder();
    if (button_at("Reload", bx -= bw + gap, y - by_off, bw, bh))
        settle_edits("first", [] {
            store_settings();
            reload();
        });
    if (button_at("Browse...", bx -= bw + gap, y - by_off, bw, bh))
        settle_edits("first", [] {
            pick(true, "Select the project folder (DLC folders, or one DLC)", strip(ui.workspace), nullptr, [](const std::string& folder) {
                ui.workspace = folder;
                ui.sel_folder.clear();
                store_settings();
                reload();
            });
        });
    ImGui::SetCursorPos(ImVec2(m + lx, y));
    ImGui::SetNextItemWidth(bx - gap - m - lx);
    if (ImGui::InputTextWithHint("##workspace", "a folder of DLC folders, or one DLC folder", &ui.workspace,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) // Enter in the project folder reloads
        settle_edits("first", [] {
            store_settings();
            reload();
        });
    ImGui::EndDisabled();

    // Bottom: buttons, the game and save folders, then the problems and the log.
    float fb = H - m - bh;
    ImGui::BeginDisabled(ui.busy);
    if (button_at("Check", W - m - 4 * bw - 3 * gap, fb, bw, bh))
        run_command("check");
    if (button_at("Export packs...", W - m - 3 * bw - 2 * gap, fb, bw, bh))
        run_command("build");
    if (button_at("Install", W - m - 2 * bw - gap, fb, bw, bh))
        run_command("install");
    if (button_at("Exit", W - m - bw, fb, bw, bh))
        request_close();
    float sy = fb - px(10) - rh, gy = sy - gap - rh;
    folder_row("Game folder:", ui.game, "your game dump (needed to build)", "Select the game folder (the one with eboot.bin)", gy, W, lx, [] {
        store_settings();
        load_stages();
    });
    folder_row("Save folder:", ui.save, "the game's save folder (Install puts packs in its addcont folder)", "Select the save folder", sy, W,
               lx, [] { store_settings(); });
    ImGui::EndDisabled();
    // The problems and the log are kBottom high, less what a song's charts need to show all four rows (down
    // to half of that) under the selection's fields.
    float top = y + bh + px(10), label_h = ImGui::GetTextLineHeightWithSpacing() + px(6), bottom = px(kBottom);
    if (selected_song()) {
        int rows[2] = {0, 0};
        for (const Field& f : ui.fields)
            if (field_visible(f))
                ++rows[f.spec->kind == Kind::File || f.spec->kind == Kind::Check || f.spec->kind == Kind::Editor];
        float fields_end = top + px(28) + static_cast<float>((rows[0] + 1) / 2 + (rows[1] + 1) / 2) * (rh + gap) + px(8) + px(4);
        float need = 5 * (ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().CellPadding.y) + px(6);
        float room = gy - px(12) - px(kBottom) - label_h - fields_end;
        bottom = std::clamp(px(kBottom) - (need - room), std::floor(px(kBottom) / 2), px(kBottom));
    }
    float ly = gy - px(12) - bottom, half = std::floor((W - 3 * m) * 3 / 5);
    text_at("Problems:", m, ly - ImGui::GetTextLineHeightWithSpacing());
    ImGui::SetCursorPos(ImVec2(m, ly));
    table("##problems", {{"Type", 64}, {"Location", 170}, {"Description", 0}}, ui.problem_rows, ImVec2(half, bottom));
    text_at("Log:", 2 * m + half, ly - ImGui::GetTextLineHeightWithSpacing());
    ImGui::SetCursorPos(ImVec2(2 * m + half, ly));
    ImGui::BeginChild("##log", ImVec2(W - 3 * m - half, bottom), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(ui.log.c_str(), ui.log.c_str() + ui.log.size());
    if (ui.log_scroll) {
        ImGui::SetScrollHereY(1.0f);
        ui.log_scroll = false;
    }
    if (ImGui::BeginPopupContextWindow("##logmenu")) {
        if (ImGui::MenuItem("Copy the log"))
            SDL_SetClipboardText(ui.log.c_str());
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    // Middle: the tree, and the selection's heading with Save beside it, its fields and a song's charts.
    float mid_bottom = ly - label_h;
    float tw = px(kTreeWidth);
    ImGui::SetCursorPos(ImVec2(m, top));
    ImGui::BeginDisabled(ui.busy);
    ImGui::BeginChild("##tree", ImVec2(tw, mid_bottom - top - bh - gap), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    int click_dlc = -2, click_song = -2;
    for (size_t i = 0; i < ui.model.dlcs.size(); ++i) {
        const Dlc& d = ui.model.dlcs[i];
        ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (ui.sel_dlc == static_cast<int>(i) && ui.sel_song < 0)
            f |= ImGuiTreeNodeFlags_Selected;
        bool open = ImGui::TreeNodeEx((dlc_text(d) + "###dlc" + d.r["folder"]).c_str(), f);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            click_dlc = static_cast<int>(i), click_song = -1;
        if (!open)
            continue;
        for (size_t j = 0; j < d.songs.size(); ++j) {
            ImGuiTreeNodeFlags sf = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (ui.sel_dlc == static_cast<int>(i) && ui.sel_song == static_cast<int>(j))
                sf |= ImGuiTreeNodeFlags_Selected;
            ImGui::TreeNodeEx((song_text(d.songs[j]) + "###song" + d.songs[j].r["folder"]).c_str(), sf);
            if (ImGui::IsItemClicked())
                click_dlc = static_cast<int>(i), click_song = static_cast<int>(j);
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();
    if (click_dlc >= 0 && (click_dlc != ui.sel_dlc || click_song != ui.sel_song))
        settle_edits("before switching", [click_dlc, click_song] {
            if (click_dlc < static_cast<int>(ui.model.dlcs.size()))
                select_item(click_dlc, click_song);
        });
    if (button_at("New DLC...", m, mid_bottom - bh, std::floor((tw - gap) / 2), bh))
        new_dlc();
    if (button_at("New song...", m + std::floor((tw + gap) / 2), mid_bottom - bh, tw - std::floor((tw + gap) / 2), bh))
        new_song();
    ImGui::EndDisabled();

    float rx = m + tw + m, rw = W - rx - m;
    {
        ImGui::SetCursorPos(ImVec2(rx, top));
        ImVec2 o = ImGui::GetCursorScreenPos();
        std::string heading = ellipsis(ui.heading, rw - bw - gap);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(o, ImGui::GetColorU32(ImGuiCol_Text), heading.c_str());
        dl->AddText(ImVec2(o.x + 1, o.y), ImGui::GetColorU32(ImGuiCol_Text), heading.c_str()); // bold
    }
    float lw = px(kLabel);
    for (const Field& f : ui.fields)
        lw = std::max(lw, ImGui::CalcTextSize(f.spec->label).x + px(8));
    float fy = top + px(28), col = std::floor((rw - px(16)) / 2), cb = px(80);
    int i = 0;
    const Record* r = selected_record();
    bool from_video = false;
    if (Field* box = field("audio_from_video"); box && r && ui.sel_song >= 0 && (*r)["clone"].empty())
        from_video = box->check;
    ImGui::BeginDisabled(ui.busy);
    bool save_pressed = button_at("Save", rx + rw - bw, top - px(4), bw, bh, ui.dirty);
    for (Kind pass : {Kind::Text, Kind::File}) { // values in two columns, then files in two columns
        i = 0;
        for (size_t k = 0; k < ui.fields.size(); ++k) {
            Field& f = ui.fields[k];
            bool file = f.spec->kind == Kind::File || f.spec->kind == Kind::Check || f.spec->kind == Kind::Editor;
            if (!field_visible(f) || file != (pass == Kind::File))
                continue;
            ImGui::PushID(static_cast<int>(k));
            float x = rx + static_cast<float>(i % 2) * (col + px(16)), row = fy + static_cast<float>(i / 2) * (rh + gap);
            text_at(f.spec->label, x, row + (rh - ImGui::GetTextLineHeight()) / 2);
            ImGui::SetCursorPos(ImVec2(x + lw, row));
            if (f.spec->kind == Kind::Editor) { // the button alone
                if (button_at("Stage editor...", x + lw, row - px(1), px(110), rh + px(2)))
                    open_stage_editor();
            } else if (f.spec->kind == Kind::Check) {
                bool movie = r && !(*r)["movie"].empty();
                ImGui::BeginDisabled(!(f.check || movie)); // the box needs a video to tick
                if (ImGui::Checkbox(f.spec->cue, &f.check))
                    ui.dirty = true;
                ImGui::EndDisabled();
            } else if (file) {
                bool audio_off = std::strcmp(f.spec->key, "audio") == 0 && from_video; // greyed out while the video's is used
                ImGui::BeginDisabled(audio_off);
                ImGui::SetNextItemWidth(col - lw - cb - gap);
                ImGui::InputTextWithHint("##v", f.cue.c_str(), &f.value, ImGuiInputTextFlags_ReadOnly);
                if (button_at("Choose...", x + col - cb, row - px(1), cb, rh + px(2)))
                    choose_file(f);
                ImGui::EndDisabled();
            } else if (f.spec->kind == Kind::Stage) {
                std::vector<std::string> names(std::begin(kStageNames), std::end(kStageNames));
                if (combo("##v", names, f.stage, col - lw))
                    ui.dirty = true;
            } else {
                ImGui::SetNextItemWidth(col - lw);
                bool read_only = f.spec->kind == Kind::ReadOnly;
                if (read_only)
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
                if (ImGui::InputTextWithHint("##v", f.cue.c_str(), &f.value,
                                             read_only ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_EnterReturnsTrue))
                    save_pressed = true; // Enter in a field saves
                if (!read_only && ImGui::IsItemEdited())
                    ui.dirty = true;
                if (read_only)
                    ImGui::PopStyleColor();
            }
            ImGui::PopID();
            ++i;
        }
        fy += static_cast<float>((i + 1) / 2) * (rh + gap) + (pass == Kind::Text ? px(8) : 0);
    }
    bool dlc = r && ui.sel_song < 0;
    if (dlc) { // each logo under its field
        float ph = std::clamp(mid_bottom - fy - px(4), px(60), px(260));
        for (int k = 0; k < 2; ++k) {
            Preview& p = ui.previews[k];
            update_preview(p);
            float x = rx + static_cast<float>(k) * (col + px(16)) + lw, pw = col - lw;
            ImGui::SetCursorPos(ImVec2(x, fy - px(2)));
            ImVec2 o = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRect(ImVec2(o.x - 1, o.y - 1), ImVec2(o.x + pw + 1, o.y + ph + 1), rgb(160, 160, 160));
            if (p.tex) { // logos are mostly transparent: on a checkerboard
                float c8 = px(8);
                dl->PushClipRect(o, ImVec2(o.x + pw, o.y + ph), true);
                for (int cy = 0; cy * c8 < ph; ++cy)
                    for (int cx = 0; cx * c8 < pw; ++cx)
                        dl->AddRectFilled(ImVec2(o.x + cx * c8, o.y + cy * c8), ImVec2(o.x + (cx + 1) * c8, o.y + (cy + 1) * c8),
                                          (cx + cy) % 2 ? rgb(200, 200, 200) : rgb(156, 156, 156));
                draw_fitted(dl, p.tex, p.w, p.h, o, ImVec2(pw, ph));
                dl->PopClipRect();
            }
        }
        fy += ph + gap;
    }
    ImGui::EndDisabled();
    fy += px(4);
    if (selected_song()) {
        ImGui::SetCursorPos(ImVec2(rx, fy));
        table("##charts", {{"Difficulty", 90}, {"Level", 50}, {"Notes", 60}, {"BPM", 60}, {"File", 120}, {"Check", 0}}, ui.chart_rows,
              ImVec2(rw, std::max(px(60), mid_bottom - fy)));
    }
    if (save_pressed && !locked)
        save_edits();
    ImGui::EndDisabled();
    if (!g_ed)
        draw_dialogs();
    // The busy cursor over the window while a job runs.
    if (ui.busy && !blocked) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_SetCursor(ui.busy_cursor);
    } else if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    }
    end_frame(ui.main);
}

void on_job_done(JobKind kind, long code, std::vector<std::string>& lines) {
    if (kind == JobKind::Info) {
        if (code != 0) {
            append_log("Could not read the project folder:");
            for (const std::string& l : lines)
                append_log("  " + l);
        } else {
            if (const Record* r = selected_record(); r && ui.sel_folder.empty())
                ui.sel_folder = (*r)["folder"];
            ui.model = dlcmaker::parse_model<char, int>(lines);
            show_model();
        }
    } else if (kind == JobKind::Edit && code != 0) {
        // The edit's output is in the log; drop what was queued after it, but still reload.
        ui.jobs.clear();
        ui.jobs.push_back({JobKind::Info, {"info", strip(ui.workspace)}, ""});
        message(lines.empty() ? "That did not work." : lines.back());
    } else if (kind == JobKind::Run) {
        append_log(code == 0 ? "Done." : "Stopped with problems (see above and the Problems list).");
    } else if (kind == JobKind::Stages) {
        ui.stages.loading = false;
        if (code != 0) {
            ui.stages.game.clear();
            ui.open_editor = false;
            append_log("Could not read the game's stages:");
            for (const std::string& l : lines)
                if (!is_stage_record(l))
                    append_log("  " + l);
        } else {
            dlcmaker::parse_stages(ui.stages, lines);
            if (ui.open_editor)
                post(open_stage_editor);
            ui.open_editor = false;
        }
    }
    start_next_job();
}

void request_close() {
    if (ui.dirty) {
        ask("Save your changes before closing?", {"Yes", "No", "Cancel"}, [](int answer) {
            if (answer == 0) {
                save_edits();
                ui.close_after = true;
            } else if (answer == 1) {
                ui.dirty = false;
                request_close();
            }
        });
        return;
    }
    if (ui.busy) {
        ask("A job is still running. Close anyway? (It keeps running in the background.)", {"Yes", "No"}, [](int answer) {
            if (answer == 0) {
                store_settings();
                ui.quit = true;
            }
        });
        return;
    }
    store_settings();
    ui.quit = true;
}

void create_controls() {
    for (const FieldSpec& s : kDlcFields)
        ui.fields.push_back({&s, "", "", false, 0});
    for (const FieldSpec& s : kSongFields)
        ui.fields.push_back({&s, "", "", false, 0});
    // Settings: dlcmaker.ini, else the release's paths.ini (the folders theatrhythm-setup chose).
    auto ini = read_ini(ini_path());
    std::map<std::string, std::string> paths;
    for (fs::path p = ui.home; !p.empty(); p = p.parent_path()) {
        std::error_code ec;
        if (fs::is_regular_file(p / "paths.ini", ec)) {
            paths = read_ini(p / "paths.ini");
            for (auto& [k, v] : paths)
                if (!v.empty() && fs::path(v).is_relative())
                    v = (p / v).lexically_normal().string();
            break;
        }
        if (p == p.parent_path())
            break;
    }
    auto value = [&](const char* key) { return !ini[key].empty() ? ini[key] : paths[key]; };
    ui.workspace = ini["workspace"];
    ui.game = value("game");
    ui.save = value("save");
    ui.dlc = paths["dlc"];
    show_selection();

    // A release ships ffmpeg next to this program (the commands look there and on PATH themselves).
    if (access((ui.root / "ffmpeg").c_str(), X_OK) == 0 && !std::getenv("THEATRHYTHM_FFMPEG"))
        setenv("THEATRHYTHM_FFMPEG", (ui.root / "ffmpeg").c_str(), 1);
    if (ini["workspace"].empty())
        append_log("Choose a project folder (Browse...), or make an empty one and press New DLC...");
    else
        reload();
    load_stages();
}

void set_icon(SDL_Window* wnd) {
    for (const fs::path& png : {ui.root / "theatrhythm-fbl.png", ui.root.parent_path() / "theatrhythm-fbl.png"}) {
        int w = 0, h = 0;
        unsigned char* pixels = load_rgba(png, &w, &h);
        if (!pixels)
            continue;
        if (SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4)) {
            SDL_SetWindowIcon(wnd, s);
            SDL_DestroySurface(s);
        }
        stbi_image_free(pixels);
        return;
    }
}

} // namespace

int main(int argc, char** argv) {
    // With a command it is the command line (the window runs itself so), not the window.
    std::vector<std::string> args = dlcmaker::utf8_args(argc, argv);
    if (!args.empty() && dlcmaker::is_command(args[0]))
        return dlcmaker::cli_main(args);
    // The release is an AppImage (tools/package.py --dlcmaker): it runs from a read-only mount, so its
    // settings and the game release's paths.ini are looked for beside the AppImage file itself.
    ui.root = exe_folder();
    const char* appimage = std::getenv("APPIMAGE");
    ui.home = appimage && *appimage ? fs::path(appimage).parent_path() : ui.root;
    SDL_SetAppMetadata(kTitle, nullptr, "theatrhythm-dlcmaker");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "theatrhythm-dlcmaker: no display (%s)\n", SDL_GetError());
        return 2;
    }
    g_wake = SDL_RegisterEvents(1);
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    ui.scale = scale >= 0.5f && scale <= 4 ? scale : 1;
    // 1060 x 760 at 96 dpi, within the work area.
    SDL_Rect work = {0, 0, 1920, 1080};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &work);
    int w = std::min(static_cast<int>(px(1060)), work.w), h = std::min(static_cast<int>(px(760)), work.h - static_cast<int>(px(40)));
    if (!make_surface(ui.main, kTitle, w, h, SDL_WINDOW_RESIZABLE)) {
        std::fprintf(stderr, "theatrhythm-dlcmaker: cannot open its window (%s)\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    SDL_SetWindowMinimumSize(ui.main.wnd, w, h); // the fixed size, or maximized (resizable only so it can be)
    set_icon(ui.main.wnd);
    ui.busy_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_PROGRESS);
    ImGui::SetCurrentContext(ui.main.ctx);
    create_controls();
    SDL_ShowWindow(ui.main.wnd);

    int frames = 3; // ImGui settles hover and popups over a few frames after each event
    while (!ui.quit) {
        SDL_Event e;
        bool got = SDL_WaitEventTimeout(&e, frames > 0 ? 1 : 250);
        while (got) {
            frames = 3;
            if (e.type == g_wake) {
                // run_posted below
            } else if (e.type == SDL_EVENT_QUIT) {
                if (!g_ed)
                    request_close();
            } else if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (g_ed && e.window.windowID == SDL_GetWindowID(g_ed->s.wnd))
                    g_ed->finished = true;
                else if (!g_ed)
                    request_close();
            } else {
                SDL_WindowID id = 0;
                if (SDL_Window* ew = SDL_GetWindowFromEvent(&e))
                    id = SDL_GetWindowID(ew);
                if (g_ed && id == SDL_GetWindowID(g_ed->s.wnd)) {
                    ImGui::SetCurrentContext(g_ed->s.ctx);
                    ImGui_ImplSDL3_ProcessEvent(&e);
                } else {
                    // The main window takes no input while the stage editor is open (it is modal).
                    bool input = (e.type >= SDL_EVENT_KEY_DOWN && e.type < SDL_EVENT_JOYSTICK_AXIS_MOTION);
                    if (!(g_ed && input)) {
                        ImGui::SetCurrentContext(ui.main.ctx);
                        ImGui_ImplSDL3_ProcessEvent(&e);
                    }
                }
            }
            got = SDL_PollEvent(&e);
        }
        run_posted();
        main_frame();
        if (g_ed) {
            ed_frame();
            if (g_ed->finished && ui.dialogs.empty())
                close_stage_editor();
        }
        --frames;
    }
    if (g_ed) {
        g_ed->ok = false;
        close_stage_editor();
    }
    for (Preview& p : ui.previews)
        if (p.tex)
            SDL_DestroyTexture(p.tex);
    destroy_surface(ui.main);
    SDL_DestroyCursor(ui.busy_cursor);
    SDL_Quit();
    return 0;
}
