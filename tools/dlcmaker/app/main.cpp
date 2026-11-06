// SPDX-License-Identifier: GPL-2.0-or-later
// theatrhythm-dlcmaker.exe: the DLC maker's window, in the style of theatrhythm-setup.exe. It edits a
// project folder, the workspace (DLC folders with dlc.json, song folders with song.json, audio,
// video, art, charts) and checks, builds and installs packs. All project logic is the DLC maker's core
// (../core, the command line in this same exe): the window runs itself with a command (info, set,
// add-file, new-dlc, new-song, check, build, install) and shows what it prints.
//
// Settings (project folder, game folder, save folder, build folder) are kept in dlcmaker.ini next to
// the exe. Its release (tools/package.py --dlcmaker) is this exe with ffmpeg.exe appended, unpacked on
// the first start. Unpacked inside the game's release folder, the game and save folders default to the
// ones the game's paths.ini names (what theatrhythm-setup.exe chose).

#include "records.h"

#include "../core/commands.h"
#include "../core/util.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Theatrhythm Final Bar Line DLC maker";

// --- text and processes ---------------------------------------------------------------------------

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

std::wstring lower(std::wstring s) {
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring strip(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\""), b = s.find_last_not_of(L" \t\r\"");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

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

fs::path exe_file() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return fs::path(buf.substr(0, n));
}

fs::path exe_folder() {
    return exe_file().parent_path();
}

// Runs `args` in `cwd` without a console window and passes each line it prints (stdout and stderr,
// UTF-8) to `line`. Returns the exit code, or -1 when it cannot start.
long run(const std::vector<std::wstring>& args, const fs::path& cwd, const std::function<void(const std::wstring&)>& line) {
    std::wstring cmd;
    for (const std::wstring& a : args)
        cmd += (cmd.empty() ? L"" : L" ") + quote(a);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0))
        return -1;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi = {};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                  cwd.c_str(), &si, &pi);
    CloseHandle(write);
    if (!started) {
        CloseHandle(read);
        return -1;
    }
    std::string pending;
    auto emit = [&](std::string_view bytes) {
        std::wstring w = widen(bytes);
        while (!w.empty() && w.back() == L'\r')
            w.pop_back();
        line(w);
    };
    char buf[4096];
    DWORD n;
    while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n) {
        pending.append(buf, n);
        for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1))
            emit(std::string_view(pending).substr(0, end));
    }
    if (!pending.empty())
        emit(pending);
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<long>(code);
}

// key=value lines (paths.ini, dlcmaker.ini): UTF-8, # or ; comments.
std::map<std::wstring, std::wstring> read_ini(const fs::path& file) {
    std::map<std::wstring, std::wstring> out;
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.rfind("\xEF\xBB\xBF", 0) == 0)
        text.erase(0, 3);
    std::istringstream lines(text);
    for (std::string raw; std::getline(lines, raw);) {
        std::wstring l = widen(raw);
        size_t eq = l.find(L'=');
        std::wstring key = lower(strip(l.substr(0, eq)));
        if (eq == std::wstring::npos || key.empty() || key[0] == L';' || key[0] == L'#')
            continue;
        out[key] = strip(l.substr(eq + 1));
    }
    return out;
}

// --- what the commands print (records.h) ---------- ------------------------------------------------

using Record = dlcmaker::Record<wchar_t>;
using Song = dlcmaker::Song<wchar_t, HTREEITEM>;
using Dlc = dlcmaker::Dlc<wchar_t, HTREEITEM>;
using Model = dlcmaker::Model<wchar_t, HTREEITEM>;
using Wave = dlcmaker::Wave;
using GameStage = dlcmaker::GameStage<wchar_t>;
using Stages = dlcmaker::Stages<wchar_t, HBITMAP>; // monster pictures: rendered when first shown
using SongStage = dlcmaker::SongStage<wchar_t>;
using dlcmaker::issue_summary, dlcmaker::issue_suffix, dlcmaker::is_stage_record;
using dlcmaker::kWaves, dlcmaker::kWaveSize, dlcmaker::kDefaultStage, dlcmaker::kDefaultField;

Model parse_model(const std::vector<std::wstring>& lines) {
    return dlcmaker::parse_model<wchar_t, HTREEITEM>(lines);
}

void parse_stages(Stages& out, const std::vector<std::wstring>& lines) {
    for (auto& [id, m] : out.monsters)
        if (m.picture)
            DeleteObject(m.picture);
    dlcmaker::parse_stages(out, lines);
}

// --- fields --------------------------------------------------------------------------------------

enum class Kind { Text, Stage, ReadOnly, File, Check, Editor };
struct FieldSpec {
    const wchar_t* key;   // the value in a dlc/song record
    const wchar_t* label;
    Kind kind;
    const wchar_t* set;   // the key `dlcmaker set` takes (Text, Stage); the role `add-file` takes (File)
    const wchar_t* cue;   // shown while empty (a record key in braces is replaced by its value); a Check's text
};
const FieldSpec kDlcFields[] = {
    {L"name", L"Pack name:", Kind::Text, L"name", nullptr},
    {L"label", L"Folder label:", Kind::Text, L"label", L"{auto_label}"},
    {L"series", L"Series name:", Kind::Text, L"series.name", L"the pack name"},
    {L"package", L"Pack number:", Kind::ReadOnly, nullptr, L"given on the first build"},
    {L"series_id", L"Series number:", Kind::ReadOnly, nullptr, L"given on the first build"},
    {L"logo", L"Logo:", Kind::File, L"logo", L"none (a base series' logo is shown)"},
    {L"logo_wide", L"Wide logo:", Kind::File, L"logo_wide", L"none (the logo is used)"},
};
const FieldSpec kSongFields[] = {
    {L"title", L"Title:", Kind::Text, L"title", L"the folder name"},
    {L"sort_name", L"Sort name:", Kind::Text, L"sort_name", L"{auto_sort_name}"},
    {L"stage", L"Stage:", Kind::Stage, L"stage", nullptr},
    {L"copyright", L"Copyright:", Kind::Text, L"copyright", nullptr},
    {L"start", L"Start (s):", Kind::Text, L"start", L"0"},
    {L"end", L"End (s):", Kind::Text, L"end", L"end of the audio"},
    {L"fade_out", L"Fade-out (s):", Kind::Text, L"fade_out", L"0"},
    {L"chart_offset_ms", L"Chart offset (ms):", Kind::Text, L"chart_offset_ms", L"0"},
    {L"preview_start", L"Preview from (s):", Kind::Text, L"preview_start", L"{auto_preview_start}"},
    {L"preview_end", L"Preview to (s):", Kind::Text, L"preview_end", L"{auto_preview_end}"},
    {L"id", L"Song number:", Kind::ReadOnly, nullptr, L"given on the first build"},
    {L"length", L"Length (s):", Kind::ReadOnly, nullptr, L"known after Check"},
    {L"battle_stage", L"Battle stage:", Kind::Editor, nullptr, nullptr},
    {L"audio", L"Audio:", Kind::File, L"audio", L"none"},
    {L"movie", L"Video:", Kind::File, L"movie", L"none"},
    {L"audio_from_video", L"", Kind::Check, L"audio_from_video", L"Use audio from the video"},
    {L"jacket", L"Jacket:", Kind::File, L"jacket", L"none (the template song's)"},
    {L"thumbnail", L"Thumbnail:", Kind::File, L"thumbnail", L"none (the template song's)"},
    {L"charts", L"Charts:", Kind::File, L"chart", L"none"},
};
const wchar_t* const kStages[] = {L"battle", L"event", L"field"};
const wchar_t* const kStageNames[] = {L"Battle (enemies, scrolling background)", L"Event (the video plays behind the lanes)",
                                      L"Field (the party walks; 9 heights, curved holds)"};
constexpr int kNewSongStages = 2; // a new song of your own: battle or event

struct Field {
    const FieldSpec* spec;
    HWND label = nullptr, ctrl = nullptr, button = nullptr;
};

// File dialog filters per add-file role.
struct Filter {
    const wchar_t* role;
    const wchar_t* name;
    const wchar_t* pattern;
};
const Filter kFilters[] = {
    {L"audio", L"Audio", L"*.mp3;*.ogg;*.opus;*.flac;*.wav;*.m4a;*.aac"},
    {L"movie", L"Video", L"*.mp4;*.mkv;*.webm;*.mov;*.avi"},
    {L"jacket", L"Images", L"*.png;*.jpg;*.jpeg;*.webp"},
    {L"thumbnail", L"Images", L"*.png;*.jpg;*.jpeg;*.webp"},
    {L"logo", L"Images", L"*.png;*.jpg;*.jpeg;*.webp"},
    {L"logo_wide", L"Images", L"*.png;*.jpg;*.jpeg;*.webp"},
    {L"chart", L"Charts (charts.json, basic/expert/ultimate/supreme.json)", L"*.json"},
};

// --- window --------------------------------------------------------------------------------------

enum : int {
    kWorkspaceEdit = 101,
    kWorkspaceBrowse,
    kReload,
    kOpenFolder,
    kTree,
    kNewDlc,
    kNewSong,
    kSave,
    kCharts,
    kProblems,
    kLog,
    kGameEdit,
    kGameBrowse,
    kSaveEdit,
    kSaveBrowse,
    kCheck,
    kBuild,
    kInstall,
    kExit,
    kFieldBase = 1000, // + index: the field's control; + 500: its Choose... button
};
enum : UINT {
    kMsgLine = WM_APP + 1, // lParam: std::wstring* line
    kMsgDone,              // wParam: exit code; lParam: std::vector<std::wstring>* captured lines
    kMsgOpenEditor,        // the game's stages are read: open the stage editor asked for before
};

enum class JobKind { Info, Edit, Run, Stages };
struct Job {
    JobKind kind;
    std::vector<std::wstring> args; // after this exe's name
    std::wstring title;             // logged when it starts (Edit, Run)
};

// An image shown under its file field (the DLC's logos), rendered to the box's size.
struct Preview {
    const wchar_t* key; // the dlc record's field
    HWND wnd = nullptr;
    std::wstring shown; // path, size, write time and box size of the bitmap shown
    HBITMAP bmp = nullptr;
};

struct Ui {
    HWND wnd = nullptr;
    Preview previews[2] = {{L"logo", nullptr, L"", nullptr}, {L"logo_wide", nullptr, L"", nullptr}};
    HWND workspace = nullptr, tree = nullptr, heading = nullptr, charts = nullptr, problems = nullptr, log = nullptr;
    HWND game = nullptr, save = nullptr, save_button = nullptr;
    std::vector<HWND> labels;   // static texts laid out with their controls
    std::vector<HWND> inputs;   // disabled while a job runs
    std::vector<Field> fields;  // every field of kDlcFields and kSongFields
    std::map<int, HWND> by_id;
    HFONT font = nullptr, bold = nullptr;
    UINT dpi = 96;
    fs::path root, tool;        // exe folder; the unpacked payload (ffmpeg.exe), if any
    std::wstring dlc;           // the game's extra DLC folder (paths.ini dlc=): its packs' IDs are skipped
    Model model;
    int sel_dlc = -1, sel_song = -1;
    std::wstring sel_folder;    // reselected after a reload
    bool populating = false, dirty = false, busy = false, close_after = false;
    std::deque<Job> jobs;
    Stages stages;              // the game's battle stages and monsters, for the stage editor
    bool open_editor = false;   // open it once they are read
};
Ui ui;

int px(int v) {
    return MulDiv(v, static_cast<int>(ui.dpi), 96);
}

std::wstring text_of(HWND h) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1);
    return s;
}

void append_log(const std::wstring& line) {
    int end = GetWindowTextLengthW(ui.log);
    SendMessageW(ui.log, EM_SETSEL, end, end);
    SendMessageW(ui.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>((line + L"\r\n").c_str()));
}

void message(const std::wstring& text, UINT icon = MB_ICONWARNING) {
    MessageBoxW(ui.wnd, text.c_str(), kTitle, MB_OK | icon);
}

fs::path ini_path() {
    return ui.root / L"dlcmaker.ini";
}

std::wstring setting(const wchar_t* key) {
    auto ini = read_ini(ini_path());
    auto it = ini.find(key);
    return it == ini.end() ? L"" : it->second;
}

void set_setting(const wchar_t* key, const std::wstring& value) {
    auto ini = read_ini(ini_path());
    ini[key] = value;
    ini[L"workspace"] = strip(text_of(ui.workspace));
    ini[L"game"] = strip(text_of(ui.game));
    ini[L"save"] = strip(text_of(ui.save));
    std::ofstream out(ini_path(), std::ios::binary);
    out << "; theatrhythm-dlcmaker.exe settings\n";
    for (auto& [k, v] : ini)
        out << narrow(k) << "=" << narrow(v) << "\n";
}

void store_settings() {
    set_setting(L"workspace", strip(text_of(ui.workspace)));
}

void apply_audio_state();

void set_busy(bool busy) {
    ui.busy = busy;
    for (HWND h : ui.inputs)
        EnableWindow(h, !busy);
    for (const Field& f : ui.fields) {
        if (f.spec->kind != Kind::ReadOnly)
            EnableWindow(f.ctrl, !busy);
        if (f.button)
            EnableWindow(f.button, !busy);
    }
    EnableWindow(ui.save_button, !busy && ui.dirty);
    apply_audio_state();
    SetCursor(LoadCursorW(nullptr, busy ? IDC_APPSTARTING : IDC_ARROW));
}

void start_next_job();

// Queues `theatrhythm-dlcmaker.exe <args>`; jobs run one at a time, and a reload follows edits and runs.
void queue(JobKind kind, std::vector<std::wstring> args, std::wstring title = L"") {
    ui.jobs.push_back({kind, std::move(args), std::move(title)});
    if (!ui.busy)
        start_next_job();
}

// Reads the game's battle stages and monsters for the stage editor (once per game folder).
void load_stages() {
    std::wstring game = strip(text_of(ui.game));
    if (game.empty() || ui.stages.loading || (ui.stages.game == game && !ui.stages.list.empty()))
        return;
    ui.stages.loading = true;
    ui.stages.game = game;
    queue(JobKind::Stages, {L"stage-data", game});
}

void reload() {
    std::wstring ws = strip(text_of(ui.workspace));
    if (ws.empty())
        return;
    queue(JobKind::Info, {L"info", ws});
}

void start_next_job() {
    if (ui.jobs.empty()) {
        set_busy(false);
        if (ui.close_after)
            DestroyWindow(ui.wnd);
        return;
    }
    Job job = ui.jobs.front();
    ui.jobs.pop_front();
    set_busy(true);
    if (!job.title.empty())
        append_log(L"> " + job.title);
    std::vector<std::wstring> cmd = {exe_file().wstring()};
    cmd.insert(cmd.end(), job.args.begin(), job.args.end());
    JobKind kind = job.kind;
    std::thread([cmd, kind, cwd = ui.root, wnd = ui.wnd] {
        auto lines = std::make_unique<std::vector<std::wstring>>();
        long code = run(cmd, cwd, [&](const std::wstring& line) {
            lines->push_back(line);
            if (kind != JobKind::Info && !is_stage_record(line))
                PostMessageW(wnd, kMsgLine, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
        });
        if (code == -1)
            lines->push_back(L"cannot start " + cmd[0]);
        lines->push_back(kind == JobKind::Info ? L"I" : kind == JobKind::Edit ? L"E" : kind == JobKind::Stages ? L"S" : L"R"); // the job's kind, last
        PostMessageW(wnd, kMsgDone, static_cast<WPARAM>(code), reinterpret_cast<LPARAM>(lines.release()));
    }).detach();
}

// --- showing the model ---------------------------------------------------------------------------

std::wstring dlc_text(const Dlc& d) {
    std::vector<Record> all = d.issues;
    for (const Song& s : d.songs)
        all.insert(all.end(), s.issues.begin(), s.issues.end());
    std::wstring name = d.r[L"name"].empty() ? fs::path(d.r[L"folder"]).filename().wstring() : d.r[L"name"];
    return name + issue_suffix(all);
}

std::wstring song_text(const Song& s) {
    std::wstring title = s.r[L"title"].empty() ? fs::path(s.r[L"folder"]).filename().wstring() : s.r[L"title"];
    return title + L"  · " + s.r[L"stage"] + issue_suffix(s.issues);
}

HTREEITEM add_tree(HTREEITEM parent, const std::wstring& text, LPARAM param) {
    TVINSERTSTRUCTW ins = {};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = const_cast<wchar_t*>(text.c_str());
    ins.item.lParam = param;
    return reinterpret_cast<HTREEITEM>(SendMessageW(ui.tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&ins)));
}

LPARAM tree_param(int dlc, int song) {
    return static_cast<LPARAM>(dlc) * 100000 + song + 1;
}

void list_columns(HWND list, std::initializer_list<std::pair<const wchar_t*, int>> cols) {
    int i = 0;
    for (auto& [name, width] : cols) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(name);
        c.cx = px(width);
        SendMessageW(list, LVM_INSERTCOLUMNW, i++, reinterpret_cast<LPARAM>(&c));
    }
}

// A list's header row with visible borders: Windows 11's theme draws hardly any line under it or between
// its columns. The header's custom-draw notices go to the list, hence the subclass.
LRESULT CALLBACK header_borders(HWND list, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
        HWND header = reinterpret_cast<HWND>(SendMessageW(list, LVM_GETHEADER, 0, 0));
        if (cd->hdr.hwndFrom == header && cd->hdr.code == NM_CUSTOMDRAW) {
            if (cd->dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYPOSTPAINT;
            if (cd->dwDrawStage == CDDS_POSTPAINT) {
                RECT rc;
                GetClientRect(header, &rc);
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160)), old = static_cast<HPEN>(SelectObject(cd->hdc, pen));
                MoveToEx(cd->hdc, rc.left, rc.bottom - 1, nullptr);
                LineTo(cd->hdc, rc.right, rc.bottom - 1);
                int n = static_cast<int>(SendMessageW(header, HDM_GETITEMCOUNT, 0, 0));
                for (int i = 0; i < n; ++i) {
                    RECT item;
                    SendMessageW(header, HDM_GETITEMRECT, i, reinterpret_cast<LPARAM>(&item));
                    MoveToEx(cd->hdc, item.right - 1, item.top, nullptr);
                    LineTo(cd->hdc, item.right - 1, item.bottom);
                }
                DeleteObject(SelectObject(cd->hdc, old));
                return CDRF_DODEFAULT;
            }
        }
    }
    return DefSubclassProc(list, msg, wp, lp);
}

void list_row(HWND list, const std::vector<std::wstring>& cells) {
    LVITEMW it = {};
    it.mask = LVIF_TEXT;
    it.iItem = static_cast<int>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
    it.pszText = const_cast<wchar_t*>(cells[0].c_str());
    int row = static_cast<int>(SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it)));
    for (size_t i = 1; i < cells.size(); ++i) {
        LVITEMW sub = {};
        sub.iSubItem = static_cast<int>(i);
        sub.pszText = const_cast<wchar_t*>(cells[i].c_str());
        SendMessageW(list, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&sub));
    }
}

const Record* selected_record() {
    if (ui.sel_dlc < 0 || ui.sel_dlc >= static_cast<int>(ui.model.dlcs.size()))
        return nullptr;
    const Dlc& d = ui.model.dlcs[ui.sel_dlc];
    if (ui.sel_song < 0)
        return &d.r;
    return ui.sel_song < static_cast<int>(d.songs.size()) ? &d.songs[ui.sel_song].r : nullptr;
}

const Song* selected_song() {
    if (ui.sel_dlc < 0 || ui.sel_song < 0)
        return nullptr;
    return &ui.model.dlcs[ui.sel_dlc].songs[ui.sel_song];
}

bool field_visible(const Field& f) {
    const Record* r = selected_record();
    if (!r)
        return false;
    bool song = ui.sel_song >= 0;
    bool in_dlc = f.spec >= std::begin(kDlcFields) && f.spec < std::end(kDlcFields);
    if (song == in_dlc)
        return false;
    if (song && wcscmp(f.spec->key, L"thumbnail") == 0)
        return (*r)[L"stage"] == L"event";
    if (song && wcscmp(f.spec->key, L"audio_from_video") == 0)
        return (*r)[L"clone"].empty();
    if (song && f.spec->kind == Kind::Editor)
        return ((*r)[L"stage"] == L"battle" || (*r)[L"stage"] == L"field") && (*r)[L"clone"].empty();
    return true;
}

void layout();

std::wstring cue_of(const FieldSpec& spec, const Record& r) {
    if (!spec.cue)
        return L"";
    std::wstring cue = spec.cue;
    if (cue.size() > 2 && cue.front() == L'{' && cue.back() == L'}')
        cue = r[cue.substr(1, cue.size() - 2)];
    if (cue.empty())
        cue = L"auto";
    return cue;
}

// The text a File field shows: the file's name, or for charts which files hold them.
std::wstring file_text(const FieldSpec& spec, const Record& r, const Song* song) {
    if (wcscmp(spec.key, L"charts") == 0) {
        std::wstring files;
        if (song)
            for (const Record& c : song->charts)
                if (files.find(c[L"file"]) == std::wstring::npos)
                    files += (files.empty() ? L"" : L", ") + c[L"file"];
        return files;
    }
    std::wstring v = r[spec.key];
    return v.empty() ? L"" : fs::path(v).filename().wstring();
}

Field* field(const wchar_t* key) {
    for (Field& f : ui.fields)
        if (wcscmp(f.spec->key, key) == 0)
            return &f;
    return nullptr;
}

// "Use audio from the video": the Audio field greys out; the box needs a video to tick.
void apply_audio_state() {
    const Record* r = selected_record();
    Field *audio = field(L"audio"), *box = field(L"audio_from_video");
    if (!r || ui.sel_song < 0 || !(*r)[L"clone"].empty())
        return;
    bool from_video = SendMessageW(box->ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(box->ctrl, !ui.busy && (from_video || !(*r)[L"movie"].empty()));
    EnableWindow(audio->ctrl, !from_video);
    EnableWindow(audio->button, !ui.busy && !from_video);
    std::wstring file = (*r)[L"audio"].empty() ? L"" : fs::path((*r)[L"audio"]).filename().wstring();
    std::wstring text = file; // greyed out, not reworded, while the video's is used
    bool was = ui.populating;
    ui.populating = true; // not an edit
    SetWindowTextW(audio->ctrl, text.c_str());
    ui.populating = was;
}

SongStage song_stage(const Record& r) {
    return dlcmaker::song_stage(r, ui.stages);
}

void show_selection() {
    ui.populating = true;
    const Record* r = selected_record();
    const Song* song = selected_song();
    std::wstring heading;
    if (!r)
        heading = ui.model.dlcs.empty() ? L"No DLC yet: press New DLC... to start one." : L"Select a DLC or a song.";
    else if (song) {
        heading = L"Song: " + song_text(*song);
        if (!(*r)[L"clone"].empty())
            heading += L"   (reuses game song " + (*r)[L"clone"] + ((*r)[L"stage"] == L"event" ? L"'s audio, charts and video)" : L"'s audio and charts)");
    }
    else {
        std::wstring folder = fs::path((*r)[L"folder"]).filename().wstring();
        heading = L"DLC: " + dlc_text(ui.model.dlcs[ui.sel_dlc]);
        if (folder != (*r)[L"name"])
            heading += L"   (folder " + folder + L")";
    }
    SetWindowTextW(ui.heading, heading.c_str());
    for (Field& f : ui.fields) {
        bool visible = field_visible(f);
        for (HWND h : {f.label, f.ctrl, f.button})
            if (h)
                ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
        if (!visible)
            continue;
        if (f.spec->kind == Kind::Check) {
            SendMessageW(f.ctrl, BM_SETCHECK, (*r)[f.spec->key] == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
            continue;
        }
        if (f.spec->kind == Kind::Editor)
            continue;
        if (f.spec->kind == Kind::Stage) {
            int i = 0;
            for (int k = 0; k < static_cast<int>(std::size(kStages)); ++k)
                if ((*r)[L"stage"] == kStages[k])
                    i = k;
            SendMessageW(f.ctrl, CB_SETCURSEL, i, 0);
            continue;
        }
        std::wstring value = f.spec->kind == Kind::File ? file_text(*f.spec, *r, song) : (*r)[f.spec->key];
        std::wstring cue = cue_of(*f.spec, *r);
        if (std::wstring clone = (*r)[L"clone"]; song && !clone.empty()) { // a game song's parts, from the dump
            if (wcscmp(f.spec->key, L"movie") == 0 && (*r)[L"stage"] != L"event")
                value = L"", cue = L"none: only event stages play one";
            else if (wcscmp(f.spec->key, L"audio") == 0 || wcscmp(f.spec->key, L"movie") == 0 || wcscmp(f.spec->key, L"charts") == 0)
                value = L"", cue = L"from game song " + clone;
            else if (wcscmp(f.spec->key, L"length") == 0)
                cue = L"game song " + clone + L"'s";
        }
        if (value.empty() && f.spec->kind == Kind::File && !(*r)[L"clone"].empty() && cue.rfind(L"from ", 0) == 0)
            value = L"(" + cue + L")"; // a clone's parts come from the game song; other empty fields stay empty
        SetWindowTextW(f.ctrl, value.c_str());
        SendMessageW(f.ctrl, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    }
    // Charts of the song; problems of the selection (all of them with nothing selected).
    SendMessageW(ui.charts, LVM_DELETEALLITEMS, 0, 0);
    ShowWindow(ui.charts, song ? SW_SHOW : SW_HIDE);
    if (song) {
        for (const wchar_t* diff : {L"basic", L"expert", L"ultimate", L"supreme"}) {
            const Record* c = nullptr;
            for (const Record& x : song->charts)
                if (x[L"difficulty"] == diff)
                    c = &x;
            std::vector<Record> own;
            for (const Record& i : song->issues)
                if (i[L"difficulty"] == diff)
                    own.push_back(i);
            std::wstring name = diff;
            name[0] = towupper(name[0]);
            if (!c) {
                list_row(ui.charts, {name, L"-", L"-", L"-", wcscmp(diff, L"supreme") == 0 ? L"optional" : L"missing", L""});
                continue;
            }
            std::wstring probs = issue_summary(own);
            if (probs.empty())
                probs = !song->r[L"clone"].empty() ? L"the game's own chart"
                        : song->r[L"checked"] != L"1" ? L"press Check to check it against the audio" : L"ok";
            list_row(ui.charts, {name, (*c)[L"level"], (*c)[L"notes"], (*c)[L"bpm"], (*c)[L"file"], probs});
        }
    }
    SendMessageW(ui.problems, LVM_DELETEALLITEMS, 0, 0);
    std::vector<Record> shown;
    if (song)
        shown = song->issues;
    else if (r) {
        const Dlc& d = ui.model.dlcs[ui.sel_dlc];
        shown = d.issues;
        for (const Song& s : d.songs)
            shown.insert(shown.end(), s.issues.begin(), s.issues.end());
    } else
        shown = ui.model.issues;
    // Errors first, then warnings, each in the order they were found.
    std::stable_sort(shown.begin(), shown.end(), [](const Record& a, const Record& b) {
        return (a[L"level"] == L"error") > (b[L"level"] == L"error");
    });
    for (const Record& i : shown) {
        std::wstring where = i[L"where"]; // "DLC/Song [difficulty]": the song part is enough
        if (size_t slash = where.find(L'/'); slash != std::wstring::npos && !i[L"song"].empty())
            where = where.substr(slash + 1);
        list_row(ui.problems, {i[L"level"] == L"error" ? L"Error" : L"Warning", where, i[L"message"]});
    }
    if (shown.empty())
        list_row(ui.problems, {L"", L"", r ? L"No problems found." : L""});
    ui.populating = false;
    ui.dirty = false;
    EnableWindow(ui.save_button, FALSE);
    for (Field& f : ui.fields) // fields enabled again for the new selection, then the audio box's rule
        if (f.spec->kind != Kind::ReadOnly) {
            EnableWindow(f.ctrl, !ui.busy);
            if (f.button)
                EnableWindow(f.button, !ui.busy);
        }
    apply_audio_state();
    layout();
}

void show_model() {
    ui.populating = true; // deleting the selected item notifies a selection change
    SendMessageW(ui.tree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));
    HTREEITEM select = nullptr;
    ui.sel_dlc = ui.sel_song = -1;
    for (size_t i = 0; i < ui.model.dlcs.size(); ++i) {
        Dlc& d = ui.model.dlcs[i];
        d.item = add_tree(TVI_ROOT, dlc_text(d), tree_param(static_cast<int>(i), -1));
        if (d.r[L"folder"] == ui.sel_folder)
            select = d.item;
        for (size_t j = 0; j < d.songs.size(); ++j) {
            Song& s = d.songs[j];
            s.item = add_tree(d.item, song_text(s), tree_param(static_cast<int>(i), static_cast<int>(j)));
            if (s.r[L"folder"] == ui.sel_folder)
                select = s.item;
        }
        SendMessageW(ui.tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(d.item));
    }
    ui.populating = false;
    if (!select && ui.model.dlcs.size() == 1) // one DLC: start on it
        select = ui.model.dlcs[0].item;
    if (select)
        SendMessageW(ui.tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(select));
    else
        show_selection();
}

// --- editing -------------------------------------------------------------------------------------

// `dlcmaker set` arguments for the selection's edited fields.
std::vector<std::wstring> set_args() {
    const Record* r = selected_record();
    if (!r)
        return {};
    std::vector<std::wstring> args = {L"set", (*r)[L"folder"]};
    for (const Field& f : ui.fields) {
        if (!field_visible(f) || !f.spec->set || f.spec->kind == Kind::File)
            continue;
        std::wstring value;
        if (f.spec->kind == Kind::Check) {
            value = SendMessageW(f.ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"true" : L"false";
        } else if (f.spec->kind == Kind::Stage) {
            LRESULT i = SendMessageW(f.ctrl, CB_GETCURSEL, 0, 0);
            value = kStages[i >= 0 && i < static_cast<LRESULT>(std::size(kStages)) ? i : 0];
        } else {
            value = strip(text_of(f.ctrl));
        }
        args.push_back(std::wstring(f.spec->set) + L"=" + value);
    }
    return args;
}

void save_edits() {
    if (!ui.dirty)
        return;
    std::vector<std::wstring> args = set_args();
    if (args.empty())
        return;
    ui.dirty = false;
    EnableWindow(ui.save_button, FALSE);
    queue(JobKind::Edit, args, L"save " + fs::path(args[1]).filename().wstring());
    reload();
}

// Unsaved edits before something else: true to go on (saved or dropped), false to stay.
bool settle_edits(const wchar_t* before) {
    if (!ui.dirty)
        return true;
    std::wstring ask = std::wstring(L"Save your changes ") + before + L"?";
    int answer = MessageBoxW(ui.wnd, ask.c_str(), kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL)
        return false;
    if (answer == IDYES)
        save_edits();
    ui.dirty = false;
    return true;
}

// The shell's file or folder picker. Returns the chosen path, or empty.
std::wstring pick(bool folder, const wchar_t* title, const std::wstring& start, const Filter* filter = nullptr) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return L"";
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetTitle(title);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (filter) {
        COMDLG_FILTERSPEC specs[] = {{filter->name, filter->pattern}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, specs);
    }
    std::error_code ec;
    IShellItem* item = nullptr;
    if (!start.empty() && fs::is_directory(start, ec) &&
        SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dlg->SetFolder(item);
        item->Release();
    }
    std::wstring out;
    IShellItem* result = nullptr;
    if (SUCCEEDED(dlg->Show(ui.wnd)) && SUCCEEDED(dlg->GetResult(&result))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out = path;
            CoTaskMemFree(path);
        }
        result->Release();
    }
    dlg->Release();
    return out;
}

void choose_file(const Field& f) {
    const Record* r = selected_record();
    if (!r || !settle_edits(L"first"))
        return;
    const Filter* filter = nullptr;
    for (const Filter& x : kFilters)
        if (wcscmp(x.role, f.spec->set) == 0)
            filter = &x;
    std::wstring start = setting(L"last_file_folder");
    std::wstring title = std::wstring(L"Choose the ") + lower(std::wstring(f.spec->label).substr(0, wcslen(f.spec->label) - 1));
    std::wstring file = pick(false, title.c_str(), start, filter);
    if (file.empty())
        return;
    set_setting(L"last_file_folder", fs::path(file).parent_path().wstring());
    queue(JobKind::Edit, {L"add-file", (*r)[L"folder"], f.spec->set, file}, L"add " + fs::path(file).filename().wstring());
    reload();
}

// A small modal prompt: a name, and a stage for a new song. Returns the name (empty when cancelled).
struct Prompt {
    HWND wnd = nullptr, edit = nullptr, stage = nullptr;
    bool done = false, ok = false;
    std::wstring name;
    int stage_index = 0;
};
Prompt* g_prompt = nullptr;

LRESULT CALLBACK prompt_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && g_prompt && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        g_prompt->ok = LOWORD(wp) == IDOK;
        g_prompt->name = strip(text_of(g_prompt->edit));
        if (g_prompt->stage)
            g_prompt->stage_index = static_cast<int>(SendMessageW(g_prompt->stage, CB_GETCURSEL, 0, 0));
        g_prompt->done = true;
        return 0;
    }
    if (msg == WM_CLOSE && g_prompt) {
        g_prompt->done = true;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

std::optional<std::pair<std::wstring, int>> prompt(const wchar_t* title, const wchar_t* label, bool with_stage) {
    Prompt p;
    g_prompt = &p;
    const int w = 420, h = with_stage ? 170 : 130;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r = {0, 0, px(w), px(h)};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, ui.dpi);
    RECT owner;
    GetWindowRect(ui.wnd, &owner);
    int x = owner.left + ((owner.right - owner.left) - (r.right - r.left)) / 2;
    int y = owner.top + ((owner.bottom - owner.top) - (r.bottom - r.top)) / 3;
    p.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"theatrhythm_dlcmaker_prompt", title, style, x, y, r.right - r.left,
                            r.bottom - r.top, ui.wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int cx, int cy, int cw, int ch, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | st, px(cx), px(cy), px(cw), px(ch), p.wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
        return c;
    };
    add(WC_STATICW, label, 0, 12, 12, w - 24, 18, 0);
    p.edit = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, 12, 34, w - 24, 24, 100, WS_EX_CLIENTEDGE);
    int by = 76;
    if (with_stage) {
        add(WC_STATICW, L"Stage:", 0, 12, 74, 60, 18, 0);
        p.stage = add(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 72, 70, w - 84, 200, 101);
        for (int k = 0; k < kNewSongStages; ++k)
            SendMessageW(p.stage, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kStageNames[k]));
        SendMessageW(p.stage, CB_SETCURSEL, 0, 0);
        by = 116;
    }
    add(WC_BUTTONW, L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, w - 12 - 180 - 6, by, 90, 28, IDOK);
    add(WC_BUTTONW, L"Cancel", WS_TABSTOP, w - 12 - 90, by, 90, 28, IDCANCEL);
    EnableWindow(ui.wnd, FALSE);
    ShowWindow(p.wnd, SW_SHOW);
    SetFocus(p.edit);
    MSG msg;
    while (!p.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(p.wnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(ui.wnd, TRUE);
    DestroyWindow(p.wnd);
    SetForegroundWindow(ui.wnd);
    g_prompt = nullptr;
    if (!p.ok || p.name.empty())
        return std::nullopt;
    return std::make_pair(p.name, p.stage_index);
}

void new_dlc() {
    std::wstring ws = strip(text_of(ui.workspace));
    if (ws.empty()) {
        message(L"Choose a project folder first (Browse...).");
        return;
    }
    if (ui.model.single) {
        message(L"This project folder is itself one DLC. To add another, choose the folder above it.");
        return;
    }
    if (!settle_edits(L"first"))
        return;
    auto name = prompt(L"New DLC", L"Name of the DLC (also its folder name and, at first, its series):", false);
    if (!name)
        return;
    ui.sel_folder = (fs::path(ws) / name->first).wstring();
    queue(JobKind::Edit, {L"new-dlc", ws, name->first}, L"new DLC " + name->first);
    reload();
}

void new_song() {
    if (ui.sel_dlc < 0 && ui.model.dlcs.size() == 1)
        ui.sel_dlc = 0;
    if (ui.sel_dlc < 0) {
        message(L"Select the DLC to add the song to.");
        return;
    }
    if (!settle_edits(L"first"))
        return;
    auto name = prompt(L"New song", L"Title of the song (also its folder name):", true);
    if (!name)
        return;
    std::wstring dlc = ui.model.dlcs[ui.sel_dlc].r[L"folder"];
    ui.sel_folder = (fs::path(dlc) / name->first).wstring();
    queue(JobKind::Edit, {L"new-song", dlc, name->first, kStages[name->second == 1 ? 1 : 0]}, L"new song " + name->first);
    reload();
}

// check / build / install of the whole project.
void run_command(const wchar_t* command) {
    std::wstring ws = strip(text_of(ui.workspace));
    if (ws.empty()) {
        message(L"Choose a project folder first (Browse...).");
        return;
    }
    if (!settle_edits(L"first"))
        return;
    std::vector<std::wstring> args = {command, ws};
    std::wstring game = strip(text_of(ui.game)), save = strip(text_of(ui.save));
    if (wcscmp(command, L"check") != 0) {
        if (game.empty()) {
            message(L"Choose the game folder (your game dump): packs are built from its files.");
            return;
        }
        args.insert(args.end(), {L"--game", game});
    }
    if (wcscmp(command, L"build") == 0) {
        std::wstring out = pick(true, L"Export the packs into this folder", setting(L"build_folder"));
        if (out.empty())
            return;
        set_setting(L"build_folder", out);
        args.insert(args.end(), {L"--out", out});
    } else if (wcscmp(command, L"install") == 0) {
        if (save.empty()) {
            message(L"Choose the save folder: packs are installed into its addcont folder.");
            return;
        }
        args.insert(args.end(), {L"--user", save});
    }
    if (!ui.dlc.empty())
        args.insert(args.end(), {L"--dlc", ui.dlc});
    store_settings();
    SetWindowTextW(ui.log, L"");
    queue(JobKind::Run, args, std::wstring(command) + L" " + ws);
    reload();
}

void open_folder() {
    const Record* r = selected_record();
    std::wstring folder = r ? (*r)[L"folder"] : strip(text_of(ui.workspace));
    if (!folder.empty())
        ShellExecuteW(ui.wnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- image previews ------------------------------------------------------------------------------

// `path` fitted into a w x h 24-bit bitmap on a checkerboard (logos are mostly transparent) or on white,
// with WIC (png, jpg; webp where Windows has its codec). Null when it cannot be read.
HBITMAP render_image(const std::wstring& path, int w, int h, bool checker) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    HBITMAP bmp = nullptr;
    UINT iw = 0, ih = 0;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&iw, &ih)) && iw && ih) {
        double scale = std::min(static_cast<double>(w) / iw, static_cast<double>(h) / ih);
        UINT tw = std::max(1u, static_cast<UINT>(iw * scale)), th = std::max(1u, static_cast<UINT>(ih * scale));
        std::vector<BYTE> src(static_cast<size_t>(tw) * th * 4);
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(conv->CopyPixels(nullptr, tw * 4, static_cast<UINT>(src.size()), src.data()))) {
            BITMAPINFO bi = {};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 24, BI_RGB, 0, 0, 0, 0, 0};
            void* bits = nullptr;
            bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bmp) {
                const int stride = (w * 3 + 3) & ~3, ox = (w - static_cast<int>(tw)) / 2, oy = (h - static_cast<int>(th)) / 2;
                auto* out = static_cast<BYTE*>(bits);
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        BYTE bg = !checker ? 0xFF : ((x / 8 + y / 8) % 2) ? 0xC8 : 0x9C;
                        BYTE* d = out + y * stride + x * 3;
                        int sx = x - ox, sy = y - oy;
                        if (sx < 0 || sy < 0 || sx >= static_cast<int>(tw) || sy >= static_cast<int>(th)) {
                            d[0] = d[1] = d[2] = bg;
                            continue;
                        }
                        const BYTE* p = &src[(static_cast<size_t>(sy) * tw + sx) * 4]; // premultiplied BGRA
                        for (int c = 0; c < 3; ++c)
                            d[c] = static_cast<BYTE>(p[c] + bg * (255 - p[3]) / 255);
                    }
                }
            }
        }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{conv, scaler, frame, dec, wic})
        if (u)
            u->Release();
    return bmp;
}

// Shows the selection's image for `p` in a w x h box, re-rendering only when the file or size changed.
void update_preview(Preview& p, int w, int h) {
    const Record* r = selected_record();
    std::wstring path = r && ui.sel_song < 0 ? (*r)[p.key] : L"";
    std::error_code ec;
    std::wstring key;
    if (!path.empty() && fs::is_regular_file(path, ec))
        key = path + L"|" + std::to_wstring(static_cast<unsigned long long>(fs::file_size(path, ec))) + L"|" +
              std::to_wstring(static_cast<long long>(fs::last_write_time(path, ec).time_since_epoch().count())) + L"|" +
              std::to_wstring(w) + L"x" + std::to_wstring(h);
    if (key == p.shown)
        return;
    p.shown = key;
    HBITMAP bmp = key.empty() || w < 8 || h < 8 ? nullptr : render_image(path, w, h, true);
    SendMessageW(p.wnd, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bmp));
    if (p.bmp)
        DeleteObject(p.bmp);
    p.bmp = bmp;
}

void place(HWND h, int x, int y, int w, int hgt) {
    MoveWindow(h, x, y, w < 0 ? 0 : w, hgt < 0 ? 0 : hgt, FALSE);
}

// --- stage editor --------------------------------------------------------------------------------
// A battle song's stage in a window of its own (A): the game stage it starts from (a series, then one
// of its songs, or Random), its background (a series, then a background named after a song of that
// series on it) and its enemy list, drawn in a scrolling panel. Clicking a monster, + or Add wave
// opens the monster picker (B), which widens the window to the right without moving A.

constexpr int kEdWidth = 880, kEdHeight = 620, kEdFieldHeight = 330, kSideWidth = 340, kWaveRow = 86, kTile = 92, kImage = 56, kCell = 84;
enum : int { kEdSeries = 200, kEdSong, kEdRandom, kEdBgSeries, kEdBg, kEdRestore, kEdSearch, kEdClose };
enum class HitWhat { Monster, Remove, Add, Boss, Up, Delete, AddWave, Pick };
struct Hit {
    RECT r;
    HitWhat what;
    int wave, slot; // for Pick: slot is the monster
};

struct Editor {
    HWND wnd = nullptr, series = nullptr, song = nullptr, random = nullptr, bg_series = nullptr, bg = nullptr,
         preview = nullptr, list = nullptr, restore = nullptr, cancel = nullptr, done = nullptr, divider = nullptr,
         search = nullptr, close = nullptr, grid = nullptr, from_label = nullptr, bg_label = nullptr;
    int from = kDefaultStage;
    std::wstring background, bg_series_name;
    std::vector<Wave> waves;
    bool field = false;                 // a field song: a background only, no enemies
    int pick_wave = -1, pick_slot = -1; // the picker is open while pick_wave >= 0; pick_wave == waves: a new wave
    int home_x = 0;                     // where A was when the picker opened
    std::vector<int> shown;             // the picker's monsters, after the search
    std::vector<int> song_numbers;      // the song list's stages
    std::vector<std::wstring> bg_ids;   // the background list's backgrounds
    int list_top = 0, grid_top = 0;     // scroll positions
    std::vector<Hit> list_hits, grid_hits;
    bool populating = false, finished = false, ok = false;
};
Editor* g_ed = nullptr;

// The stages the editor offers: battle stages, or field stages for a field song.
const std::vector<GameStage>& ed_list() {
    return g_ed->field ? ui.stages.fields : ui.stages.list;
}

const GameStage* ed_find(int number) {
    return ui.stages.find(number, g_ed->field);
}

std::vector<std::wstring> stage_series() {
    return dlcmaker::stage_series(ed_list());
}

std::vector<std::pair<std::wstring, std::wstring>> series_backgrounds(const std::wstring& series) {
    return dlcmaker::series_backgrounds(ed_list(), series);
}

std::wstring background_name(const std::wstring& bg) {
    return dlcmaker::background_name(ed_list(), g_ed->bg_series_name, bg);
}

std::wstring monster_name(int id) {
    auto it = ui.stages.monsters.find(id);
    return it == ui.stages.monsters.end() ? L"Monster " + std::to_wstring(id) : it->second.name;
}

HBITMAP monster_bitmap(int id) {
    auto it = ui.stages.monsters.find(id);
    if (it == ui.stages.monsters.end() || it->second.image.empty())
        return nullptr;
    if (!it->second.picture)
        it->second.picture = render_image(it->second.image, px(kImage), px(kImage), false);
    return it->second.picture;
}

bool ed_same() {
    if (g_ed->field)
        return g_ed->from == kDefaultField;
    const GameStage* g = ed_find(g_ed->from);
    return g && g->background == g_ed->background && g->waves == g_ed->waves;
}

int list_height() {
    int n = static_cast<int>(g_ed->waves.size());
    return px(6) + n * (px(kWaveRow) + px(6)) + (n < kWaves ? px(28) + px(6) : 0);
}

int grid_height() {
    return px(6) + static_cast<int>((g_ed->shown.size() + 2) / 3) * px(kCell);
}

void set_scroll(HWND h, int& top, int content) {
    RECT rc;
    GetClientRect(h, &rc);
    top = std::clamp(top, 0, std::max(0, content - static_cast<int>(rc.bottom)));
    SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, 0, std::max(0, content - 1),
                     static_cast<UINT>(rc.bottom), top, 0};
    SetScrollInfo(h, SB_VERT, &si, TRUE);
}

void on_vscroll(HWND h, int& top, int content, WPARAM wp) {
    RECT rc;
    GetClientRect(h, &rc);
    switch (LOWORD(wp)) {
    case SB_LINEUP: top -= px(40); break;
    case SB_LINEDOWN: top += px(40); break;
    case SB_PAGEUP: top -= rc.bottom; break;
    case SB_PAGEDOWN: top += rc.bottom; break;
    case SB_TOP: top = 0; break;
    case SB_BOTTOM: top = content; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        GetScrollInfo(h, SB_VERT, &si);
        top = si.nTrackPos;
        break;
    }
    }
    set_scroll(h, top, content);
    InvalidateRect(h, nullptr, FALSE);
}

void draw_button(HDC dc, HWND h, RECT r, const wchar_t* text, bool enabled, bool pressed = false) {
    if (HTHEME th = OpenThemeData(h, L"BUTTON")) {
        DrawThemeBackground(th, dc, BP_PUSHBUTTON, !enabled ? PBS_DISABLED : pressed ? PBS_PRESSED : PBS_NORMAL, &r, nullptr);
        CloseThemeData(th);
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (enabled ? 0 : DFCS_INACTIVE) | (pressed ? DFCS_PUSHED : 0));
    }
    SetTextColor(dc, GetSysColor(enabled ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_rect(HDC dc, const RECT& r, COLORREF fill, COLORREF line, int width = 1) {
    HBRUSH b = CreateSolidBrush(line);
    FillRect(dc, &r, b);
    DeleteObject(b);
    RECT in = {r.left + width, r.top + width, r.right - width, r.bottom - width};
    b = CreateSolidBrush(fill);
    FillRect(dc, &in, b);
    DeleteObject(b);
}

// A monster picture with its name under it; `selected`: the one the picker replaces.
void draw_monster(HDC dc, int id, int x, int y, int w, bool selected) {
    RECT img = {x + (w - px(kImage)) / 2, y, x + (w - px(kImage)) / 2 + px(kImage), y + px(kImage)};
    RECT frame = {img.left - 1, img.top - 1, img.right + 1, img.bottom + 1};
    if (selected)
        frame = {img.left - 3, img.top - 3, img.right + 3, img.bottom + 3};
    draw_rect(dc, frame, RGB(255, 255, 255), selected ? RGB(47, 111, 208) : RGB(204, 202, 196), selected ? 3 : 1);
    if (HBITMAP bmp = monster_bitmap(id)) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(dc, img.left, img.top, px(kImage), px(kImage), mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    } else {
        SetTextColor(dc, RGB(136, 136, 136));
        DrawTextW(dc, L"?", -1, &img, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RECT name = {x + 2, y + px(kImage) + px(3), x + w - 2, y + px(kImage) + px(19)};
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    std::wstring n = monster_name(id);
    DrawTextW(dc, n.c_str(), -1, &name, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void paint_list(HWND h) {
    Editor& e = *g_ed;
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP buf = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ old_bmp = SelectObject(dc, buf);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    HGDIOBJ old_font = SelectObject(dc, ui.font);
    SetBkMode(dc, TRANSPARENT);
    e.list_hits.clear();
    const int W = rc.right, gap = px(6), bw = px(70), bh = px(22);
    int y = gap - e.list_top;
    for (int wi = 0; wi < static_cast<int>(e.waves.size()); ++wi) {
        const Wave& w = e.waves[static_cast<size_t>(wi)];
        RECT row = {gap, y, W - gap, y + px(kWaveRow)};
        draw_rect(dc, row, w.boss ? RGB(251, 233, 227) : RGB(255, 255, 255), w.boss ? RGB(163, 65, 45) : RGB(204, 202, 196));
        RECT num = {row.left, y, row.left + px(28), y + px(kWaveRow)};
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, std::to_wstring(wi + 1).c_str(), -1, &num, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (w.boss) {
            RECT tag = {row.left + px(28), y, row.left + px(68), y + px(kWaveRow)};
            SetTextColor(dc, RGB(163, 65, 45));
            SelectObject(dc, ui.bold);
            DrawTextW(dc, L"Boss", -1, &tag, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, ui.font);
        }
        int tx = row.left + px(70);
        for (int si = 0; si < static_cast<int>(w.monsters.size()); ++si) {
            draw_monster(dc, w.monsters[static_cast<size_t>(si)], tx, y + px(7), px(kTile), e.pick_wave == wi && e.pick_slot == si);
            e.list_hits.push_back({{tx, y, tx + px(kTile), y + px(kWaveRow)}, HitWhat::Monster, wi, si});
            int ix = tx + (px(kTile) + px(kImage)) / 2;
            RECT x = {ix - px(10), y + px(3), ix + px(6), y + px(19)};
            draw_button(dc, h, x, L"×", true);
            e.list_hits.push_back({x, HitWhat::Remove, wi, si});
            tx += px(kTile);
        }
        if (w.monsters.size() < kWaveSize) {
            int ix = tx + (px(kTile) - px(kImage)) / 2;
            RECT plus = {ix, y + px(7), ix + px(kImage), y + px(7) + px(kImage)};
            bool sel = e.pick_wave == wi && e.pick_slot == static_cast<int>(w.monsters.size());
            draw_button(dc, h, plus, L"+", true, sel);
            e.list_hits.push_back({plus, HitWhat::Add, wi, static_cast<int>(w.monsters.size())});
        }
        int bx = row.right - px(6) - bw;
        RECT boss = {bx, y + px(6), bx + bw, y + px(6) + bh}, up = {bx, y + px(32), bx + bw, y + px(32) + bh},
             del = {bx, y + px(58), bx + bw, y + px(58) + bh};
        draw_button(dc, h, boss, w.boss ? L"Not boss" : L"Boss", true);
        draw_button(dc, h, up, L"Up", wi > 0);
        draw_button(dc, h, del, L"Delete", true);
        e.list_hits.push_back({boss, HitWhat::Boss, wi, 0});
        if (wi > 0)
            e.list_hits.push_back({up, HitWhat::Up, wi, 0});
        e.list_hits.push_back({del, HitWhat::Delete, wi, 0});
        y += px(kWaveRow) + gap;
    }
    if (static_cast<int>(e.waves.size()) < kWaves) {
        RECT add = {gap, y, gap + px(100), y + px(28)};
        draw_button(dc, h, add, L"Add wave", true, e.pick_wave == static_cast<int>(e.waves.size()));
        e.list_hits.push_back({add, HitWhat::AddWave, static_cast<int>(e.waves.size()), 0});
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(buf);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

void paint_grid(HWND h) {
    Editor& e = *g_ed;
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP buf = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ old_bmp = SelectObject(dc, buf);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    HGDIOBJ old_font = SelectObject(dc, ui.font);
    SetBkMode(dc, TRANSPARENT);
    e.grid_hits.clear();
    const int cw = rc.right / 3;
    for (size_t i = 0; i < e.shown.size(); ++i) {
        int x = static_cast<int>(i % 3) * cw, y = px(6) + static_cast<int>(i / 3) * px(kCell) - e.grid_top;
        if (y + px(kCell) < 0 || y > rc.bottom)
            continue;
        draw_monster(dc, e.shown[i], x, y + px(4), cw, false);
        e.grid_hits.push_back({{x, y, x + cw, y + px(kCell)}, HitWhat::Pick, 0, e.shown[i]});
    }
    if (e.shown.empty()) {
        RECT note = {0, px(20), rc.right, px(40)};
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, L"No monster by that name.", -1, &note, DT_CENTER | DT_SINGLELINE);
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(buf);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

void combo_fill(HWND c, const std::vector<std::wstring>& items, int sel) {
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (const std::wstring& s : items)
        SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
    SendMessageW(c, CB_SETCURSEL, sel, 0);
}

// The dropdowns, preview, Restore default and the enemy list, after a change.
void ed_refresh() {
    Editor& e = *g_ed;
    e.populating = true;
    const GameStage* g = ed_find(e.from);
    std::vector<std::wstring> series = stage_series();
    auto index = [](const auto& v, const auto& x) {
        auto it = std::find(v.begin(), v.end(), x);
        return it == v.end() ? 0 : static_cast<int>(it - v.begin());
    };
    std::wstring own = g ? g->series : L"";
    SendMessageW(e.series, CB_SETCURSEL, index(series, own), 0);
    std::vector<std::wstring> titles;
    e.song_numbers.clear();
    for (const GameStage& s : ed_list())
        if (s.series == own) {
            titles.push_back(s.title);
            e.song_numbers.push_back(s.number);
        }
    combo_fill(e.song, titles, index(e.song_numbers, e.from));
    // The background's series: the one shown if it has the background, else the stage's own, else the first.
    auto has = [&](const std::wstring& s) {
        auto b = series_backgrounds(s);
        return std::any_of(b.begin(), b.end(), [&](auto& x) { return x.first == e.background; });
    };
    if (!has(e.bg_series_name))
        e.bg_series_name = has(own) ? own : [&] {
            for (const std::wstring& s : series)
                if (has(s))
                    return s;
            return own;
        }();
    SendMessageW(e.bg_series, CB_SETCURSEL, index(series, e.bg_series_name), 0);
    std::vector<std::wstring> names;
    e.bg_ids.clear();
    for (auto& [id, title] : series_backgrounds(e.bg_series_name)) {
        e.bg_ids.push_back(id);
        names.push_back(title);
    }
    combo_fill(e.bg, names, index(e.bg_ids, e.background));
    std::wstring preview = background_name(e.background) + L"\n\nNo picture of this background yet.";
    SetWindowTextW(e.preview, preview.c_str());
    EnableWindow(e.restore, !ed_same());
    set_scroll(e.list, e.list_top, list_height());
    InvalidateRect(e.list, nullptr, FALSE);
    e.populating = false;
}

void ed_layout() {
    Editor& e = *g_ed;
    RECT rc;
    GetClientRect(e.wnd, &rc);
    const int A = px(kEdWidth), H = rc.bottom, m = px(12), rh = px(24), bh = px(28), gap = px(6), lw = px(86),
              cw = px(250), rb = px(90);
    int y1 = m, y2 = y1 + rh + px(10), x2 = m + lw + cw + gap, w2 = A - m - rb - gap - x2;
    for (HWND h : {e.from_label, e.series, e.song, e.list})
        ShowWindow(h, e.field ? SW_HIDE : SW_SHOW);
    if (e.field) // the background row first, Random on it, the preview under it
        y2 = y1;
    place(e.from_label, m, y1 + px(4), lw, px(18));
    place(e.series, m + lw, y1, cw, px(300));
    place(e.song, x2, y1, w2, px(300));
    place(e.random, A - m - rb, (e.field ? y2 : y1) - px(2), rb, bh);
    place(e.bg_label, m, y2 + px(4), lw, px(18));
    place(e.bg_series, m + lw, y2, cw, px(300));
    place(e.bg, x2, y2, w2, px(300));
    int cy = y2 + rh + px(14), fy = H - m - bh, pw = px(220);
    place(e.preview, m, cy, e.field ? A - 2 * m : pw, e.field ? fy - px(10) - cy : px(150));
    place(e.list, m + pw + px(12), cy, A - m - (m + pw + px(12)), fy - px(10) - cy);
    place(e.restore, m, fy, px(120), bh);
    place(e.done, A - m - rb, fy, rb, bh);
    place(e.cancel, A - m - 2 * rb - gap, fy, rb, bh);
    bool side = e.pick_wave >= 0;
    for (HWND h : {e.divider, e.search, e.close, e.grid})
        ShowWindow(h, side ? SW_SHOW : SW_HIDE);
    if (side) {
        int S = px(kSideWidth), cb = px(80);
        place(e.divider, A, 0, 2, H);
        place(e.search, A + m, m, S - 2 * m - cb - gap, rh);
        place(e.close, A + S - m - cb, m - px(2), cb, bh);
        place(e.grid, A + m, m + rh + px(10), S - 2 * m, H - m - (m + rh + px(10)));
    }
    set_scroll(e.list, e.list_top, list_height());
    set_scroll(e.grid, e.grid_top, grid_height());
    InvalidateRect(e.wnd, nullptr, TRUE);
}

// The window's width: A alone, or A and the picker. A stays where it is.
void ed_resize(bool side) {
    Editor& e = *g_ed;
    RECT wr, cr, work;
    GetWindowRect(e.wnd, &wr);
    GetClientRect(e.wnd, &cr);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT r = {0, 0, px(kEdWidth + (side ? kSideWidth : 0)), cr.bottom};
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(e.wnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(e.wnd, GWL_EXSTYLE)), ui.dpi);
    int w = r.right - r.left, x = side ? wr.left : e.home_x;
    if (side) {
        e.home_x = wr.left;
        if (x + w > work.right) // no room on the right: the screen's edge wins
            x = std::max<int>(work.left, work.right - w);
    }
    SetWindowPos(e.wnd, nullptr, x, wr.top, w, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ed_filter() {
    Editor& e = *g_ed;
    std::wstring q = lower(strip(text_of(e.search)));
    e.shown.clear();
    for (auto& [id, m] : ui.stages.monsters)
        if (lower(m.name).find(q) != std::wstring::npos)
            e.shown.push_back(id);
    std::sort(e.shown.begin(), e.shown.end(), [](int a, int b) {
        const std::wstring &x = ui.stages.monsters[a].name, &y = ui.stages.monsters[b].name;
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, x.c_str(),
                               static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    e.grid_top = 0;
    set_scroll(e.grid, e.grid_top, grid_height());
    InvalidateRect(e.grid, nullptr, FALSE);
}

void ed_pick(int wave, int slot) {
    Editor& e = *g_ed;
    bool open = e.pick_wave >= 0;
    e.pick_wave = wave;
    e.pick_slot = slot;
    if (!open) {
        SetWindowTextW(e.search, L"");
        ed_filter();
        ed_resize(true);
        ed_layout();
    }
    SetFocus(e.search);
    InvalidateRect(e.list, nullptr, FALSE);
}

void ed_pick_close() {
    Editor& e = *g_ed;
    if (e.pick_wave < 0)
        return;
    e.pick_wave = e.pick_slot = -1;
    ed_resize(false);
    ed_layout();
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
    e.list_top = 0;
    ed_pick_close();
    ed_refresh();
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

void ed_list_click(int x, int y) {
    Editor& e = *g_ed;
    SetFocus(e.list);
    for (auto it = e.list_hits.rbegin(); it != e.list_hits.rend(); ++it) { // drawn last, on top
        if (!PtInRect(&it->r, {x, y}))
            continue;
        auto& waves = e.waves;
        size_t w = static_cast<size_t>(it->wave);
        switch (it->what) {
        case HitWhat::Monster: ed_pick(it->wave, it->slot); return;
        case HitWhat::Add: ed_pick(it->wave, it->slot); return;
        case HitWhat::AddWave: ed_pick(it->wave, 0); return;
        case HitWhat::Remove:
            waves[w].monsters.erase(waves[w].monsters.begin() + it->slot);
            if (waves[w].monsters.empty())
                waves.erase(waves.begin() + it->wave);
            break;
        case HitWhat::Boss: waves[w].boss = !waves[w].boss; break;
        case HitWhat::Up: std::swap(waves[w], waves[w - 1]); break;
        case HitWhat::Delete: waves.erase(waves.begin() + it->wave); break;
        case HitWhat::Pick: return;
        }
        ed_pick_close(); // the waves moved: what the picker was for may be gone
        ed_refresh();
        return;
    }
}

void ed_grid_click(int x, int y) {
    Editor& e = *g_ed;
    for (const Hit& h : e.grid_hits) {
        if (!PtInRect(&h.r, {x, y}))
            continue;
        if (e.pick_wave >= static_cast<int>(e.waves.size()))
            e.waves.push_back({false, {h.slot}});
        else if (Wave& w = e.waves[static_cast<size_t>(e.pick_wave)]; e.pick_slot >= static_cast<int>(w.monsters.size()))
            w.monsters.push_back(h.slot);
        else
            w.monsters[static_cast<size_t>(e.pick_slot)] = h.slot;
        ed_pick_close();
        ed_refresh();
        return;
    }
}

LRESULT CALLBACK scroll_panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_ed)
        return DefWindowProcW(h, msg, wp, lp);
    Editor& e = *g_ed;
    bool list = h == e.list;
    int& top = list ? e.list_top : e.grid_top;
    switch (msg) {
    case WM_PAINT:
        list ? paint_list(h) : paint_grid(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        list ? ed_list_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) : ed_grid_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_VSCROLL:
        on_vscroll(h, top, list ? list_height() : grid_height(), wp);
        return 0;
    case WM_MOUSEWHEEL:
        top -= GET_WHEEL_DELTA_WPARAM(wp) * px(kCell) / WHEEL_DELTA;
        set_scroll(h, top, list ? list_height() : grid_height());
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SIZE:
        set_scroll(h, top, list ? list_height() : grid_height());
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK editor_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_ed)
        return DefWindowProcW(wnd, msg, wp, lp);
    Editor& e = *g_ed;
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (e.populating)
            return 0;
        int sel = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lp), CB_GETCURSEL, 0, 0));
        if (id == kEdSeries && code == CBN_SELCHANGE) {
            std::vector<std::wstring> series = stage_series();
            for (const GameStage& s : ed_list())
                if (sel >= 0 && sel < static_cast<int>(series.size()) && s.series == series[static_cast<size_t>(sel)]) {
                    ed_start(s.number);
                    break;
                }
        } else if (id == kEdSong && code == CBN_SELCHANGE && sel >= 0 && sel < static_cast<int>(e.song_numbers.size())) {
            ed_start(e.song_numbers[static_cast<size_t>(sel)]);
        } else if (id == kEdRandom && code == BN_CLICKED) {
            static std::mt19937 rng{std::random_device{}()};
            ed_start(ed_list()[std::uniform_int_distribution<size_t>(0, ed_list().size() - 1)(rng)].number);
        } else if (id == kEdBgSeries && code == CBN_SELCHANGE) {
            std::vector<std::wstring> series = stage_series();
            if (sel >= 0 && sel < static_cast<int>(series.size())) {
                e.bg_series_name = series[static_cast<size_t>(sel)];
                auto b = series_backgrounds(e.bg_series_name);
                if (!b.empty())
                    e.background = b.front().first;
                ed_field_from();
                ed_refresh();
            }
        } else if (id == kEdBg && code == CBN_SELCHANGE && sel >= 0 && sel < static_cast<int>(e.bg_ids.size())) {
            e.background = e.bg_ids[static_cast<size_t>(sel)];
            ed_field_from();
            ed_refresh();
        } else if (id == kEdRestore && code == BN_CLICKED) {
            ed_start(e.field ? kDefaultField : e.from);
        } else if (id == kEdSearch && code == EN_CHANGE) {
            ed_filter();
        } else if (id == kEdClose && code == BN_CLICKED) {
            ed_pick_close();
        } else if (id == IDOK && code == BN_CLICKED) {
            e.ok = e.finished = true;
        } else if (id == IDCANCEL) { // Cancel, Esc (closes the picker first)
            if (e.pick_wave >= 0 && reinterpret_cast<HWND>(lp) != e.cancel)
                ed_pick_close();
            else
                e.finished = true;
        }
        return 0;
    }
    case WM_CLOSE:
        e.finished = true;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// Edits the selected battle song's stage; Done saves it to its song.json.
void open_stage_editor() {
    const Record* sel = selected_record();
    if (!sel || ui.sel_song < 0 || g_ed)
        return;
    std::wstring game = strip(text_of(ui.game));
    if ((ui.stages.list.empty() && ui.stages.fields.empty()) || ui.stages.game != game) {
        if (game.empty()) {
            message(L"Choose the game folder first: the stage editor shows the game's own stages and monsters.");
            return;
        }
        ui.open_editor = true; // opens when they are read
        load_stages();
        return;
    }
    if (!settle_edits(L"first"))
        return;
    Record song = *sel;
    Editor e;
    g_ed = &e;
    e.field = song[L"stage"] == L"field";
    if (ed_list().empty()) {
        g_ed = nullptr;
        message(L"No stages of this kind were found in the game folder.");
        return;
    }
    SongStage s = song_stage(song);
    e.from = ed_find(s.from) ? s.from : ed_list().front().number;
    e.background = s.background.empty() ? ed_find(e.from)->background : s.background;
    e.waves = s.waves;
    e.bg_series_name = ed_find(e.from)->series;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT work, owner;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    GetWindowRect(ui.wnd, &owner);
    RECT r = {0, 0, px(kEdWidth), std::min<int>(px(e.field ? kEdFieldHeight : kEdHeight), work.bottom - work.top - px(60))};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, ui.dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = std::clamp<int>(owner.left + ((owner.right - owner.left) - w) / 2, work.left, std::max<int>(work.left, work.right - w));
    int y = std::clamp<int>(owner.top + ((owner.bottom - owner.top) - h) / 2, work.top, std::max<int>(work.top, work.bottom - h));
    std::wstring title = L"Stage editor — " + (song[L"title"].empty() ? fs::path(song[L"folder"]).filename().wstring() : song[L"title"]);
    e.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"theatrhythm_dlcmaker_stage", title.c_str(), style, x, y, w, h, ui.wnd,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | st, 0, 0, 10, 10, e.wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
        return c;
    };
    e.from_label = add(WC_STATICW, L"Start from:", SS_LEFT, 0);
    e.series = add(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kEdSeries);
    e.song = add(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kEdSong);
    e.random = add(WC_BUTTONW, L"Random", WS_TABSTOP, kEdRandom);
    e.bg_label = add(WC_STATICW, L"Background:", SS_LEFT, 0);
    e.bg_series = add(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kEdBgSeries);
    e.bg = add(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kEdBg);
    e.preview = add(WC_STATICW, L"", SS_CENTER, 0, WS_EX_STATICEDGE);
    e.list = add(L"theatrhythm_dlcmaker_panel", L"", WS_VSCROLL | WS_TABSTOP, 0);
    e.restore = add(WC_BUTTONW, L"Restore default", WS_TABSTOP, kEdRestore);
    e.cancel = add(WC_BUTTONW, L"Cancel", WS_TABSTOP, IDCANCEL);
    e.done = add(WC_BUTTONW, L"Done", WS_TABSTOP, IDOK);
    e.divider = add(WC_STATICW, L"", SS_ETCHEDVERT, 0);
    e.search = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kEdSearch, WS_EX_CLIENTEDGE);
    SendMessageW(e.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search monsters"));
    e.close = add(WC_BUTTONW, L"Close", WS_TABSTOP, kEdClose);
    e.grid = add(L"theatrhythm_dlcmaker_panel", L"", WS_VSCROLL | WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
    std::vector<std::wstring> series = stage_series();
    combo_fill(e.series, series, 0);
    combo_fill(e.bg_series, series, 0);
    ed_layout();
    ed_refresh();

    EnableWindow(ui.wnd, FALSE);
    ShowWindow(e.wnd, SW_SHOW);
    SetFocus(e.song);
    MSG msg;
    while (!e.finished && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_MOUSEWHEEL) { // the wheel scrolls the panel under the pointer, not the focused list
            HWND under = WindowFromPoint({GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)});
            if (under == e.list || under == e.grid) {
                SendMessageW(under, WM_MOUSEWHEEL, msg.wParam, msg.lParam);
                continue;
            }
        }
        if (IsDialogMessageW(e.wnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(ui.wnd, TRUE);
    DestroyWindow(e.wnd);
    SetForegroundWindow(ui.wnd);
    g_ed = nullptr;
    if (!e.ok)
        return;
    // song.json keeps the game stage it starts from, and the background and enemies only where they differ.
    const GameStage* g = ui.stages.find(e.from, e.field); // the editor (g_ed) is closed
    bool own_bg = !e.field && (!g || e.background != g->background), own_waves = !e.field && (!g || e.waves != g->waves);
    queue(JobKind::Edit,
          {L"set", song[L"folder"], L"stage_from=" + std::to_wstring(e.from), L"background=" + (own_bg ? e.background : L""),
           L"enemy_waves=" + (own_waves ? dlcmaker::waves_text<wchar_t>(e.waves) : L""), L"enemies="},
          L"save the stage of " + fs::path(song[L"folder"]).filename().wstring());
    reload();
}

// --- layout --------------------------------------------------------------------------------------

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, ui.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
    if (id)
        ui.by_id[id] = c;
    return c;
}

HWND label(const wchar_t* text) {
    HWND h = add(WC_STATICW, text, SS_LEFT, 0);
    ui.labels.push_back(h);
    return h;
}

// Layout constants in 96-dpi units.
constexpr int kMargin = 12, kButton = 96, kRow = 24, kGap = 6, kLabel = 118, kTreeWidth = 260, kBottom = 150;
HWND g_ws_label, g_game_label, g_save_label, g_problems_label, g_log_label;

void layout() {
    RECT rc;
    GetClientRect(ui.wnd, &rc);
    const int W = rc.right, H = rc.bottom, m = px(kMargin), bw = px(kButton), bh = px(28), rh = px(kRow), gap = px(kGap);
    // Top: the project folder.
    int y = m;
    place(g_ws_label, m, y + px(4), px(110), px(18));
    int bx = W - m - bw;
    place(ui.by_id[kOpenFolder], bx, y - px(1), bw, bh);
    place(ui.by_id[kReload], bx -= bw + gap, y - px(1), bw, bh);
    place(ui.by_id[kWorkspaceBrowse], bx -= bw + gap, y - px(1), bw, bh);
    place(ui.workspace, m + px(112), y, bx - gap - m - px(112), rh);
    // Bottom: buttons, the game and save folders, then the problems and the log.
    int by = H - m - bh;
    place(ui.by_id[kExit], W - m - bw, by, bw, bh);
    place(ui.by_id[kInstall], W - m - 2 * bw - gap, by, bw, bh);
    place(ui.by_id[kBuild], W - m - 3 * bw - 2 * gap, by, bw, bh);
    place(ui.by_id[kCheck], W - m - 4 * bw - 3 * gap, by, bw, bh);
    int sy = by - px(10) - rh, gy = sy - gap - rh;
    for (auto [lbl, edit, btn, row] : {std::tuple{g_game_label, ui.game, ui.by_id[kGameBrowse], gy},
                                       std::tuple{g_save_label, ui.save, ui.by_id[kSaveBrowse], sy}}) {
        place(lbl, m, row + px(4), px(110), px(18));
        place(edit, m + px(112), row, W - 2 * m - px(112) - bw - gap, rh);
        place(btn, W - m - bw, row - px(1), bw, bh);
    }
    // Middle: the tree, and the selection's heading with Save beside it, its fields and a song's charts.
    const int top = y + bh + px(10), tw = px(kTreeWidth), rx = m + tw + m, rw = W - rx - m;
    place(ui.heading, rx, top, rw - bw - gap, px(20));
    place(ui.save_button, rx + rw - bw, top - px(4), bw, bh);
    int fy = top + px(28), col = (rw - px(16)) / 2, lw = px(kLabel), cb = px(80);
    int i = 0;
    for (Kind pass : {Kind::Text, Kind::File}) { // values in two columns, then files in two columns
        i = 0;
        for (Field& f : ui.fields) {
            bool file = f.spec->kind == Kind::File || f.spec->kind == Kind::Check || f.spec->kind == Kind::Editor;
            if (!field_visible(f) || file != (pass == Kind::File))
                continue;
            int x = rx + (i % 2) * (col + px(16)), row = fy + (i / 2) * (rh + gap);
            place(f.label, x, row + px(4), lw, px(18));
            if (f.spec->kind == Kind::Editor) { // the button alone
                place(f.button, x + lw, row - px(1), px(110), rh + px(2));
            } else if (f.spec->kind == Kind::Check) {
                place(f.ctrl, x + lw, row, col - lw, rh);
            } else if (file) {
                place(f.ctrl, x + lw, row, col - lw - cb - gap, rh);
                place(f.button, x + col - cb, row - px(1), cb, rh + px(2));
            } else {
                place(f.ctrl, x + lw, row, col - lw, f.spec->kind == Kind::Stage ? px(200) : rh);
            }
            ++i;
        }
        fy += ((i + 1) / 2) * (rh + gap) + (pass == Kind::Text ? px(8) : 0);
    }
    fy += px(4);
    // The problems and the log are kBottom high, less what a song's charts need to show all four rows (down
    // to half of that).
    bool song = selected_record() && ui.sel_song >= 0, dlc = selected_record() && ui.sel_song < 0;
    int bottom = px(kBottom);
    if (song) {
        int need = HIWORD(SendMessageW(ui.charts, LVM_APPROXIMATEVIEWRECT, 4, MAKELPARAM(-1, -1))) + px(4);
        int room = gy - px(12) - px(24) - px(kBottom) - fy;
        bottom = std::clamp(px(kBottom) - (need - room), px(kBottom) / 2, px(kBottom));
    }
    int ly = gy - px(12) - bottom, half = (W - 3 * m) * 3 / 5, mid_bottom = ly - px(24);
    place(g_problems_label, m, ly - px(18), half, px(18));
    place(ui.problems, m, ly, half, bottom);
    place(g_log_label, 2 * m + half, ly - px(18), W - 3 * m - half, px(18));
    place(ui.log, 2 * m + half, ly, W - 3 * m - half, bottom);
    place(ui.tree, m, top, tw, mid_bottom - top - bh - gap);
    place(ui.by_id[kNewDlc], m, mid_bottom - bh, (tw - gap) / 2, bh);
    place(ui.by_id[kNewSong], m + (tw + gap) / 2, mid_bottom - bh, tw - (tw + gap) / 2, bh);
    for (int k = 0; k < 2; ++k)
        ShowWindow(ui.previews[k].wnd, dlc ? SW_SHOW : SW_HIDE);
    if (dlc) { // each logo under its field
        int ph = std::clamp(mid_bottom - fy, px(60), px(260));
        for (int k = 0; k < 2; ++k) {
            int x = rx + k * (col + px(16)) + lw, pw = col - lw;
            place(ui.previews[k].wnd, x, fy - px(6), pw, ph);
            update_preview(ui.previews[k], pw, ph);
        }
    }
    place(ui.charts, rx, fy, rw, mid_bottom - fy);
    SendMessageW(ui.charts, LVM_SETCOLUMNWIDTH, 5, LVSCW_AUTOSIZE_USEHEADER); // Check fills the rest: no sideways scroll bar
    // place() moves without repainting: redraw the window and every control, or moved ones stay blank or
    // leave their old image behind.
    RedrawWindow(ui.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// The release is this exe alone (tools/package.py --dlcmaker): ffmpeg.exe is a zip appended to it, then
// a trailer of the zip's size (8 bytes, little-endian), the release's name (120 bytes, NUL-padded) and
// kPayloadMagic. The first start of a release unpacks it with Windows' own tar.exe into
// %LOCALAPPDATA%\theatrhythm-dlcmaker\<release> and removes other releases' folders there. Returns that
// folder, or nothing for an exe without it (a build, or one in an unpacked folder). The command line
// (`window` false) unpacks it too, telling stderr.
constexpr char kPayloadMagic[8] = {'T', 'F', 'B', 'L', 'D', 'L', 'C', '1'};

fs::path unpack_payload(bool window = true) {
    const fs::path exe = exe_file();
    std::ifstream in(exe, std::ios::binary | std::ios::ate);
    const std::streamoff end = in ? static_cast<std::streamoff>(in.tellg()) : 0;
    char trailer[136];
    if (end < static_cast<std::streamoff>(sizeof trailer) || !in.seekg(end - sizeof trailer).read(trailer, sizeof trailer) ||
        std::memcmp(trailer + 128, kPayloadMagic, sizeof kPayloadMagic) != 0)
        return {};
    uint64_t size = 0;
    std::memcpy(&size, trailer, sizeof size);
    const std::wstring name = widen(std::string(trailer + 8, strnlen(trailer + 8, 120)));
    if (name.empty() || name.find_first_of(L"\\/:") != std::wstring::npos || size > static_cast<uint64_t>(end) - sizeof trailer)
        return {};
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const fs::path base = (n && n < MAX_PATH ? fs::path(local) : fs::temp_directory_path()) / L"theatrhythm-dlcmaker";
    const fs::path dir = base / name;
    std::error_code ec;
    if (fs::is_regular_file(dir / L"ffmpeg.exe", ec))
        return dir;

    // First start of this release: show the window while it unpacks (a few seconds).
    auto note = [window](const std::wstring& line) {
        if (window)
            append_log(line);
        else
            dlcmaker::print_err(narrow(line));
    };
    if (window) {
        layout();
        ShowWindow(ui.wnd, SW_SHOW);
    }
    note(L"Unpacking the DLC maker's ffmpeg into " + dir.wstring() + L" (once per release)...");
    if (window)
        UpdateWindow(ui.wnd);
    const fs::path tmp = base / (name + L".unpacking");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    {
        std::ofstream out(tmp / L"payload.zip", std::ios::binary);
        in.seekg(end - static_cast<std::streamoff>(sizeof trailer + size));
        std::vector<char> buf(1 << 20);
        for (uint64_t left = size; left && out;) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
            if (!in.read(buf.data(), static_cast<std::streamsize>(chunk)))
                break;
            out.write(buf.data(), static_cast<std::streamsize>(chunk));
            left -= chunk;
        }
        if (!in || !out) {
            note(L"Could not unpack the DLC maker's files into " + tmp.wstring() + L".");
            return {};
        }
    }
    wchar_t system[MAX_PATH];
    UINT m = GetSystemDirectoryW(system, MAX_PATH);
    const fs::path tar = fs::path(std::wstring(system, m < MAX_PATH ? m : 0)) / L"tar.exe";
    std::wstring output;
    long code = run({tar.wstring(), L"-xf", L"payload.zip"}, tmp, [&](const std::wstring& l) { output += l + L"\n"; });
    fs::remove(tmp / L"payload.zip", ec);
    if (code != 0 || !fs::is_regular_file(tmp / L"ffmpeg.exe", ec)) {
        note(L"Could not unpack the DLC maker's files with " + tar.wstring() + L" (exit code " + std::to_wstring(code) + L"):");
        note(output);
        return {};
    }
    fs::remove_all(dir, ec);
    fs::rename(tmp, dir, ec);
    if (ec) {
        note(L"Could not move the unpacked files to " + dir.wstring() + L": " + widen(ec.message()));
        return {};
    }
    for (const auto& old : fs::directory_iterator(base, ec)) // other releases (skipped while in use)
        if (old.path() != dir) {
            std::error_code ignore;
            fs::remove_all(old.path(), ignore);
        }
    note(L"Done.");
    return dir;
}

void create_controls() {
    g_ws_label = label(L"Project folder:");
    ui.workspace = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kWorkspaceEdit, WS_EX_CLIENTEDGE);
    HWND ws_browse = add(WC_BUTTONW, L"Browse...", WS_TABSTOP, kWorkspaceBrowse);
    HWND reload_button = add(WC_BUTTONW, L"Reload", WS_TABSTOP, kReload);
    HWND open = add(WC_BUTTONW, L"Open folder", WS_TABSTOP, kOpenFolder);

    ui.tree = add(WC_TREEVIEWW, L"", WS_TABSTOP | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS |
                  TVS_SHOWSELALWAYS, kTree);
    HWND nd = add(WC_BUTTONW, L"New DLC...", WS_TABSTOP, kNewDlc);
    HWND ns = add(WC_BUTTONW, L"New song...", WS_TABSTOP, kNewSong);
    ui.heading = add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS, 0);
    SendMessageW(ui.heading, WM_SETFONT, reinterpret_cast<WPARAM>(ui.bold), FALSE);

    int index = 0;
    auto make = [&](const FieldSpec& spec) {
        Field f;
        f.spec = &spec;
        f.label = add(WC_STATICW, spec.label, SS_LEFT, 0);
        int id = kFieldBase + index;
        if (spec.kind == Kind::Check) {
            f.ctrl = add(WC_BUTTONW, spec.cue, WS_TABSTOP | BS_AUTOCHECKBOX, id);
        } else if (spec.kind == Kind::Stage) {
            f.ctrl = add(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id);
            for (const wchar_t* s : kStageNames)
                SendMessageW(f.ctrl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        } else if (spec.kind != Kind::Editor) { // the editor's row has its button only
            DWORD st = WS_TABSTOP | ES_AUTOHSCROLL | (spec.kind == Kind::Text ? 0 : ES_READONLY);
            f.ctrl = add(WC_EDITW, L"", st, id, WS_EX_CLIENTEDGE);
        }
        if (spec.kind == Kind::File || spec.kind == Kind::Editor)
            f.button = add(WC_BUTTONW, spec.kind == Kind::Editor ? L"Stage editor..." : L"Choose...", WS_TABSTOP, id + 500);
        ui.fields.push_back(f);
        ++index;
    };
    for (const FieldSpec& s : kDlcFields)
        make(s);
    for (const FieldSpec& s : kSongFields)
        make(s);
    ui.save_button = add(WC_BUTTONW, L"Save", WS_TABSTOP, kSave);
    for (Preview& p : ui.previews)
        p.wnd = add(WC_STATICW, L"", SS_BITMAP | SS_CENTERIMAGE | SS_SUNKEN, 0);

    ui.charts = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER, kCharts);
    SendMessageW(ui.charts, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    SetWindowSubclass(ui.charts, header_borders, 0, 0);
    list_columns(ui.charts, {{L"Difficulty", 90}, {L"Level", 50}, {L"Notes", 60}, {L"BPM", 60}, {L"File", 120}, {L"Check", 260}});

    g_problems_label = label(L"Problems:");
    ui.problems = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER, kProblems);
    SendMessageW(ui.problems, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP | LVS_EX_GRIDLINES);
    SetWindowSubclass(ui.problems, header_borders, 0, 0);
    list_columns(ui.problems, {{L"Type", 64}, {L"Location", 170}, {L"Description", 1000}});
    g_log_label = label(L"Log:");
    ui.log = add(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, kLog, WS_EX_CLIENTEDGE);
    SendMessageW(ui.log, EM_SETLIMITTEXT, 8 * 1024 * 1024, 0);

    g_game_label = label(L"Game folder:");
    ui.game = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kGameEdit, WS_EX_CLIENTEDGE);
    HWND gb = add(WC_BUTTONW, L"Browse...", WS_TABSTOP, kGameBrowse);
    g_save_label = label(L"Save folder:");
    ui.save = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kSaveEdit, WS_EX_CLIENTEDGE);
    HWND sb = add(WC_BUTTONW, L"Browse...", WS_TABSTOP, kSaveBrowse);
    HWND check = add(WC_BUTTONW, L"Check", WS_TABSTOP, kCheck);
    HWND build = add(WC_BUTTONW, L"Export packs...", WS_TABSTOP, kBuild);
    HWND install = add(WC_BUTTONW, L"Install", WS_TABSTOP, kInstall);
    HWND exit = add(WC_BUTTONW, L"Exit", WS_TABSTOP, kExit);
    SendMessageW(ui.workspace, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"a folder of DLC folders, or one DLC folder"));
    SendMessageW(ui.game, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"your game dump (needed to build)"));
    SendMessageW(ui.save, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"the game's save folder (Install puts packs in its addcont folder)"));
    ui.inputs = {ui.workspace, ws_browse, reload_button, open, ui.tree, nd, ns, ui.game, gb, ui.save, sb, check, build, install, exit};

    // Settings: dlcmaker.ini, else the release's paths.ini (the folders theatrhythm-setup.exe chose).
    auto ini = read_ini(ini_path());
    std::map<std::wstring, std::wstring> paths;
    for (fs::path p = ui.root; !p.empty(); p = p.parent_path()) {
        std::error_code ec;
        if (fs::is_regular_file(p / L"paths.ini", ec)) {
            paths = read_ini(p / L"paths.ini");
            for (auto& [k, v] : paths)
                if (!v.empty() && fs::path(v).is_relative())
                    v = (p / v).lexically_normal().wstring();
            break;
        }
        if (p == p.parent_path())
            break;
    }
    auto value = [&](const wchar_t* key) { return !ini[key].empty() ? ini[key] : paths[key]; };
    SetWindowTextW(ui.workspace, ini[L"workspace"].c_str());
    SetWindowTextW(ui.game, value(L"game").c_str());
    SetWindowTextW(ui.save, value(L"save").c_str());
    ui.dlc = paths[L"dlc"];
    set_busy(false);
    show_selection();

    // A release ships ffmpeg.exe: unpacked from this exe, or next to it in an unpacked folder (the
    // commands look there and on PATH themselves).
    ui.tool = unpack_payload();
    std::error_code ec;
    if (!ui.tool.empty() && fs::is_regular_file(ui.tool / L"ffmpeg.exe", ec) && !GetEnvironmentVariableW(L"THEATRHYTHM_FFMPEG", nullptr, 0))
        SetEnvironmentVariableW(L"THEATRHYTHM_FFMPEG", (ui.tool / L"ffmpeg.exe").c_str());
    if (ini[L"workspace"].empty())
        append_log(L"Choose a project folder (Browse...), or make an empty one and press New DLC...");
    else
        reload();
    load_stages();
}

void on_command(int id, int code) {
    if (ui.populating)
        return;
    if (id >= kFieldBase && id < kFieldBase + 500) {
        bool box = ui.fields[static_cast<size_t>(id - kFieldBase)].spec->kind == Kind::Check;
        if (code == EN_CHANGE || code == CBN_SELCHANGE || (box && code == BN_CLICKED)) {
            ui.dirty = true;
            EnableWindow(ui.save_button, !ui.busy);
            if (box)
                apply_audio_state();
        }
        return;
    }
    if (id >= kFieldBase + 500 && id < kFieldBase + 1000 && code == BN_CLICKED) {
        const Field& f = ui.fields[static_cast<size_t>(id - kFieldBase - 500)];
        if (f.spec->kind == Kind::Editor)
            open_stage_editor();
        else
            choose_file(f);
        return;
    }
    switch (id) {
    case IDOK: // Enter: in the project folder reloads, elsewhere saves
        if (GetFocus() == ui.workspace) {
            if (settle_edits(L"first")) {
                store_settings();
                reload();
            }
        } else {
            save_edits();
        }
        break;
    case kWorkspaceBrowse: {
        if (!settle_edits(L"first"))
            break;
        std::wstring folder = pick(true, L"Select the project folder (DLC folders, or one DLC)", strip(text_of(ui.workspace)));
        if (!folder.empty()) {
            SetWindowTextW(ui.workspace, folder.c_str());
            ui.sel_folder.clear();
            store_settings();
            reload();
        }
        break;
    }
    case kReload:
        if (settle_edits(L"first")) {
            store_settings();
            reload();
        }
        break;
    case kOpenFolder:
        open_folder();
        break;
    case kNewDlc:
        new_dlc();
        break;
    case kNewSong:
        new_song();
        break;
    case kSave:
        save_edits();
        break;
    case kGameBrowse:
    case kSaveBrowse: {
        HWND edit = id == kGameBrowse ? ui.game : ui.save;
        std::wstring folder = pick(true, id == kGameBrowse ? L"Select the game folder (the one with eboot.bin)" : L"Select the save folder",
                                   strip(text_of(edit)));
        if (!folder.empty()) {
            SetWindowTextW(edit, folder.c_str());
            store_settings();
            if (id == kGameBrowse)
                load_stages();
        }
        break;
    }
    case kCheck:
        run_command(L"check");
        break;
    case kBuild:
        run_command(L"build");
        break;
    case kInstall:
        run_command(L"install");
        break;
    case kExit:
        PostMessageW(ui.wnd, WM_CLOSE, 0, 0);
        break;
    }
}

void on_job_done(long code, std::vector<std::wstring>& lines) {
    JobKind kind = lines.back() == L"I" ? JobKind::Info : lines.back() == L"E" ? JobKind::Edit
                 : lines.back() == L"S" ? JobKind::Stages : JobKind::Run;
    lines.pop_back();
    if (kind == JobKind::Info) {
        if (code != 0) {
            append_log(L"Could not read the project folder:");
            for (const std::wstring& l : lines)
                append_log(L"  " + l);
        } else {
            if (const Record* r = selected_record(); r && ui.sel_folder.empty())
                ui.sel_folder = (*r)[L"folder"];
            ui.model = parse_model(lines);
            show_model();
        }
    } else if (kind == JobKind::Edit && code != 0) {
        // The edit's output is in the log; drop what was queued after it, but still reload.
        ui.jobs.clear();
        ui.jobs.push_back({JobKind::Info, {L"info", strip(text_of(ui.workspace))}, L""});
        message(lines.empty() ? L"That did not work." : lines.back());
    } else if (kind == JobKind::Run) {
        append_log(code == 0 ? L"Done." : L"Stopped with problems (see above and the Problems list).");
    } else if (kind == JobKind::Stages) {
        ui.stages.loading = false;
        if (code != 0) {
            ui.stages.game.clear();
            ui.open_editor = false;
            append_log(L"Could not read the game's stages:");
            for (const std::wstring& l : lines)
                if (!is_stage_record(l))
                    append_log(L"  " + l);
        } else {
            parse_stages(ui.stages, lines);
            if (ui.open_editor)
                PostMessageW(ui.wnd, kMsgOpenEditor, 0, 0);
            ui.open_editor = false;
        }
    }
    start_next_job();
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->hwndFrom == ui.tree && hdr->code == TVN_SELCHANGINGW && !ui.populating) {
            if (!settle_edits(L"before switching"))
                return TRUE;
        }
        if (hdr->hwndFrom == ui.tree && hdr->code == TVN_SELCHANGEDW && !ui.populating) {
            auto* tv = reinterpret_cast<NMTREEVIEWW*>(lp);
            if (!tv->itemNew.hItem)
                break;
            LPARAM p = tv->itemNew.lParam;
            ui.sel_dlc = static_cast<int>(p / 100000);
            ui.sel_song = static_cast<int>(p % 100000) - 1;
            if (const Record* r = selected_record())
                ui.sel_folder = (*r)[L"folder"];
            show_selection();
        }
        break;
    }
    case WM_SIZE:
        if (ui.heading)
            layout();
        return 0;
    case WM_CTLCOLORSTATIC: {
        HWND h = reinterpret_cast<HWND>(lp);
        bool box = h == ui.log;
        for (const Field& f : ui.fields)
            box |= h == f.ctrl && (f.spec->kind == Kind::File || f.spec->kind == Kind::Text) &&
                   IsWindowEnabled(h); // read-only values and disabled ones look grey
        if (box) { // read-only edits read as text boxes, not labels
            SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    }
    case kMsgLine: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        append_log(L"  " + *line);
        return 0;
    }
    case kMsgDone: {
        std::unique_ptr<std::vector<std::wstring>> lines(reinterpret_cast<std::vector<std::wstring>*>(lp));
        on_job_done(static_cast<long>(wp), *lines);
        return 0;
    }
    case kMsgOpenEditor:
        open_stage_editor();
        return 0;
    case WM_SETCURSOR:
        if (ui.busy && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        if (ui.dirty) {
            int answer = MessageBoxW(wnd, L"Save your changes before closing?", kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDCANCEL)
                return 0;
            if (answer == IDYES) {
                save_edits();
                ui.close_after = true;
                return 0;
            }
        }
        if (ui.busy) {
            if (MessageBoxW(wnd, L"A job is still running. Close anyway? (It keeps running in the background.)", kTitle,
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
        }
        store_settings();
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int run_window(HINSTANCE inst) {
    ui.root = exe_folder();
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    ui.dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui.dpi);
    ui.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_BOLD;
    ui.bold = CreateFontIndirectW(&bold);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"theatrhythm_dlcmaker";
    RegisterClassExW(&wc);
    WNDCLASSEXW pc = wc;
    pc.lpfnWndProc = prompt_proc;
    pc.lpszClassName = L"theatrhythm_dlcmaker_prompt";
    RegisterClassExW(&pc);
    WNDCLASSEXW sc = wc;
    sc.lpfnWndProc = editor_proc;
    sc.lpszClassName = L"theatrhythm_dlcmaker_stage";
    RegisterClassExW(&sc);
    WNDCLASSEXW lc = wc;
    lc.lpfnWndProc = scroll_panel_proc;
    lc.hIcon = nullptr;
    lc.hbrBackground = nullptr;
    lc.lpszClassName = L"theatrhythm_dlcmaker_panel";
    RegisterClassExW(&lc);

    // 1060 x 760 at 96 dpi, within the work area: a fixed size, or maximized (no resizing border).
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME;
    RECT r = {0, 0, px(1060), px(760)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, ui.dpi);
    int w = std::min<int>(r.right - r.left, work.right - work.left), h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
    ui.wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                             nullptr, nullptr, inst, nullptr);
    create_controls();
    layout();
    ShowWindow(ui.wnd, SW_SHOW);
    SetFocus(ui.tree);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(ui.wnd, &msg)) // Tab between controls, Enter = save (or reload the folder)
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(ui.font);
    DeleteObject(ui.bold);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // With a command it is the command line (the window runs itself so), not the window.
    std::vector<std::string> args = dlcmaker::utf8_args(0, nullptr);
    if (!args.empty() && dlcmaker::is_command(args[0])) {
        dlcmaker::setup_console();
        if (!GetEnvironmentVariableW(L"THEATRHYTHM_FFMPEG", nullptr, 0)) // the window passes its own
            if (fs::path dir = unpack_payload(false); !dir.empty())
                SetEnvironmentVariableW(L"THEATRHYTHM_FFMPEG", (dir / L"ffmpeg.exe").c_str());
        return dlcmaker::cli_main(args);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int rc = run_window(inst);
    CoUninitialize();
    return rc;
}
