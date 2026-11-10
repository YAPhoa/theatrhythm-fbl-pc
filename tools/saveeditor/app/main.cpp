// SPDX-License-Identifier: GPL-2.0-or-later
// theatrhythm-saveeditor.exe: edits the game's save files in the style of theatrhythm-dlcmaker.exe.
// Open backup.inf (quests, characters, items, cards, options...), musicBackup.inf (songs and scores) or
// proficaBackup.inf (the profile card): the tree on the left shows what that file holds, and picking an
// entry shows its values on the right to change. A list (characters, items, cards...) also offers to
// change one value on all of its entries at once (unlock all, set every count).
//
// Saving writes the file in the game's own format and first copies the file it replaces into
// save-backups/ next to the exe. Names come from the player's decrypted dump (the game folder); without
// one, entries show their IDs. Settings (last file, game folder) are kept in saveeditor.ini next to the
// exe; unpacked inside the game's release folder, both default to what the game's paths.ini names.
//
// Command line: theatrhythm-saveeditor.exe dump FILE (the MessagePack inside, as JSON) and
// theatrhythm-saveeditor.exe check FILE... (decrypts, re-encodes and decrypts again, byte for byte).

#include "../core/names.h"
#include "../core/savefile.h"

#include "util.h"

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <tuple>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using saveeditor::Node;

namespace {

constexpr wchar_t kTitle[] = L"Theatrhythm Final Bar Line save editor";
constexpr UINT kMsgNames = WM_APP + 1, kMsgRefresh = WM_APP + 2;
constexpr int64_t kNone = 4294967295;
constexpr size_t kMaxFields = 300; // values shown for one entry (a few lists hold thousands of flags)

// --- text and files -------------------------------------------------------------------------------

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

std::wstring strip(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\""), b = s.find_last_not_of(L" \t\r\"");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

std::wstring lower(std::wstring s) {
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::string read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw saveeditor::SaveError("cannot read " + narrow(p.wstring()));
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path& p, const std::string& data) {
    fs::path tmp = p;
    tmp += L".saving";
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out)
            throw saveeditor::SaveError("cannot write " + narrow(tmp.wstring()));
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw saveeditor::SaveError("cannot replace " + narrow(p.wstring()) + " (is it open elsewhere?)");
}

fs::path exe_folder() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return fs::path(buf.substr(0, n)).parent_path();
}

// key=value lines (paths.ini, saveeditor.ini): UTF-8, # or ; comments.
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

// "PlayTimeDouble" -> "Play time double", "UnlockedFlag" -> "Unlocked", "_totalGetCount" -> "Total get count".
std::wstring words(std::string key) {
    while (!key.empty() && key[0] == '_')
        key.erase(0, 1);
    if (key.size() > 4 && key.ends_with("Flag"))
        key.resize(key.size() - 4);
    std::string out;
    for (size_t j = 0; j < key.size(); ++j) {
        char c = key[j];
        bool upper = c >= 'A' && c <= 'Z';
        bool next_lower = j + 1 < key.size() && key[j + 1] >= 'a' && key[j + 1] <= 'z';
        bool prev_lower = j > 0 && ((key[j - 1] >= 'a' && key[j - 1] <= 'z') || (key[j - 1] >= '0' && key[j - 1] <= '9'));
        if (j && upper && (prev_lower || next_lower))
            out += ' ';
        out += (j && upper && next_lower) ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z')
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return widen(out);
}

// What the save's own names mean to a player.
std::wstring friendly(const std::string& key) {
    static const std::map<std::string, const wchar_t*> names = {
        {"UserInfo", L"Player"}, {"MainMenuInfo", L"Main menu"}, {"MusicStageInfo", L"Music Stages"},
        {"RhythpoInfo", L"Rhythpo"}, {"MusicSelectInfo", L"Music select"}, {"SeriesQuestInfo", L"Series Quests"},
        {"MultiBattleInfo", L"Multi Battle"}, {"PartyInfo", L"Parties"}, {"CharacterInfo", L"Characters"},
        {"SummonStoneInfo", L"Summon stones"}, {"MyMoogleInfo", L"Moogles"}, {"AirShipInfo", L"Airships"},
        {"ItemInfo", L"Items and illustrations"}, {"CollecaInfo", L"CollectaCards"}, {"TheaterInfo", L"Theater"},
        {"ConfigInfo", L"Options"}, {"HelpInfo", L"Help"}, {"MusicPlayerInfo", L"Music player"},
        {"AchievementInfo", L"Feats"}, {"TipsInfo", L"Tips"}, {"MusicInfo", L"Songs"}, {"ProficaInfo", L"Profile card"},
        {"WorldDictionary", L"Worlds"}, {"StageDictionary", L"Stages"}, {"EndlessWorldUniqueInfo", L"Endless World"},
        {"PickedupFlags", L"Picked-up quests"}, {"PickupStageList", L"Picked-up stages"}, {"PartySets", L"Party sets"},
        {"AbilityData", L"Abilities"}, {"Items", L"Items"}, {"ProficaIllusts", L"Profile card illustrations"},
        {"Collecas", L"CollectaCards"}, {"MovieDatas", L"Movies"}, {"HelpRecordDictionary", L"Help pages"},
        {"AchievementRecordDictionary", L"Feats"}, {"TipsDictionary", L"Tips"}, {"MyMoogles", L"Moogles"},
        {"AirShips", L"Airships"}, {"SummonStones", L"Stones"}, {"MusicRecordDictionary", L"Songs"},
        {"DifficultyRecordDictionary", L"Difficulties"}, {"PlayStyleRecordDictionary", L"Play styles"},
        {"Playlist", L"Playlists"}, {"StandardPlayInfo", L"Standard play"}, {"SimplePlayInfo", L"Simple play"},
        {"PairPlayInfo", L"Pair play"}, {"EverydayPlayBonus", L"Daily play bonus"},
        {"DisconnectionPoint", L"Disconnection points"}, {"MyProfica", L"My profile card"},
        {"RivalProficaList", L"Rival profile cards"}, {"LastSetAbilities", L"Last set abilities"},
        {"ShownMoogleNewsMap", L"Moogle news shown"}, {"ShownMoogleNewsDiscontMap", L"Moogle discount news shown"},
        {"ShownDLCWindowMap", L"DLC windows shown"}, {"Characters", L"Characters"}, {"Abilities", L"Abilities"},
        {"Effects", L"Effects"}, {"LatestPlayedMusics", L"Recently played songs"},
        {"TotalScore", L"Total score"}, {"PlayTimeDouble", L"Play time (seconds)"}, {"TotalPlayTime", L"Total play time (seconds)"},
        {"TotalFMSMoveDistance", L"Total field distance"}, {"LastSelectedMSDifficulty", L"Last selected difficulty"},
        {"LastSelectedMSPlayStyle", L"Last selected play style"}, {"Version", L"Save version"},
        {"_highscore", L"High score"}, {"_maxChainCount", L"Max chain"}, {"CharacterID", L"Character"}, {"AirShipID", L"Airship"}, {"MyMoogleID", L"Moogle"},
    };
    auto it = names.find(key);
    return it != names.end() ? it->second : words(key);
}

// The positional records (MessagePack arrays the game reads by index), by the list that holds them.
const std::vector<const wchar_t*>* positional(const std::string& list) {
    static const std::vector<const wchar_t*> play = {L"High score", L"Last updated", L"Best rank", L"Max chain",
        L"Best clear mark", L"Critical bar zones", L"Play count", L"Clear count", L"Full chain clears", L"Max damage"};
    static const std::vector<const wchar_t*> stone = {L"Summon", L"Effects", L"Discoverer name",
        L"Discoverer account ID", L"Giver name", L"Giver account ID", L"Cursor selected", L"Locked"};
    static const std::vector<const wchar_t*> card = {L"Card ID", L"Illustration", L"Base design", L"Base colour",
        L"Summon stone", L"Recently played songs", L"Edited"};
    static const std::vector<const wchar_t*> effect = {L"Effect", L"Rarity"};
    if (list == "PlayStyleRecordDictionary") return &play;
    if (list == "SummonStones") return &stone;
    if (list == "MyProfica" || list == "RivalProficaList") return &card;
    if (list == "Effects") return &effect;
    return nullptr;
}

// --- the model: one entry per map or array in the file --------------------------------------------

struct Entry {
    Node* node = nullptr;
    std::string key;       // its key in its container (a string, an int ID in decimal, or an array index)
    bool key_is_str = false, key_is_int = false;
    size_t index = 0;      // in its container
    std::string dict;      // the name of what it is ("Characters", ...): its own string key, innermost when folded
    std::string outer;     // its own string key before folding (CharacterInfo for Characters)
    int parent = -1;
    std::vector<int> kids;
    std::wstring label;
    HTREEITEM item = nullptr;
    bool items_built = false;
};

struct Field {
    enum class Kind { Value, AllBool, AllInt } kind = Kind::Value;
    Node* node = nullptr;          // Value: the value itself
    std::string key;               // AllBool / AllInt: the field each entry of the list has
    std::wstring label;
    HWND name = nullptr, ctrl = nullptr, hint = nullptr, on = nullptr, off = nullptr;
    int64_t lo = 0, hi = 0;        // Int range
    bool read_only = false;
    int64_t pick = 0;              // a character, moogle or airship ID (its prefix): a portrait (ctrl) and
                                   // Change... (on) instead of the number
    bool browse = false;           // pick: which one the Moogles or Airships page shows (node is nullptr)
};

struct Ui {
    HWND wnd = nullptr, file = nullptr, game = nullptr, tree = nullptr, panel = nullptr, find = nullptr, status = nullptr;
    HWND title = nullptr, subtitle = nullptr, empty = nullptr;
    HFONT font = nullptr, bold = nullptr, big = nullptr;
    UINT dpi = 96;
    fs::path root, path;               // exe folder; the open file
    Node doc;
    std::string original;              // the MessagePack as read (or last saved)
    bool dirty = false, populating = false, backed_up = false;
    std::vector<Entry> entries;
    std::vector<int> roots;
    int sel = -1;
    std::vector<Field> fields;
    std::set<HWND> hints, read_only;
    int ptop = 0, pheight = 0;
    saveeditor::Names names;
    std::wstring names_note;
    std::wstring save_folder;          // paths.ini save=
    std::map<std::string, int64_t> browse; // the moogle or airship the Moogles / Airships page shows
};
Ui ui;

enum : int {
    kOpen = 100, kReload, kGameBrowse, kFind, kFindNext, kSave, kClose, kTree, kFileEdit, kGameEdit,
    kFieldBase = 1000, // + 8 * field + role (0 ctrl, 1 on, 2 off)
};

int px(int v) {
    return MulDiv(v, static_cast<int>(ui.dpi), 96);
}

std::wstring text_of(HWND h) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1);
    return s;
}

void message(const std::wstring& text, UINT icon = MB_ICONWARNING) {
    MessageBoxW(ui.wnd, text.c_str(), kTitle, MB_OK | icon);
}

std::wstring id_label(int64_t id) {
    std::string n = ui.names.get(id);
    if (!n.empty())
        return widen(n);
    const char* kind = saveeditor::kind_of(id);
    return *kind ? widen(kind) + L" " + std::to_wstring(id) : std::to_wstring(id);
}

std::wstring number(const Node& n) {
    if (n.k == Node::K::Int)
        return n.u64 ? std::to_wstring(static_cast<uint64_t>(n.i)) : std::to_wstring(n.i);
    wchar_t buf[64];
    swprintf(buf, 64, L"%.9g", n.f);
    return buf;
}

// A container hides itself when it only wraps one other (CharacterInfo = {Characters, Version}).
Node* fold(Node* n, std::string& dict) {
    for (;;) {
        if (!n->is_map())
            return n;
        Node* only = nullptr;
        bool other = false;
        std::string key;
        for (auto& [k, v] : n->m) {
            if (!v.is_scalar()) {
                if (only)
                    other = true;
                only = &v;
                key = k.key_text();
            } else if (!(k.k == Node::K::Str && k.s == "Version")) {
                other = true;
            }
        }
        if (!only || other)
            return n;
        n = only;
        dict = key;
    }
}

std::wstring label_of(const Entry& e) {
    const Entry* p = e.parent >= 0 ? &ui.entries[static_cast<size_t>(e.parent)] : nullptr;
    std::string ctx = p ? p->dict : "";
    if (e.key_is_str)
        return friendly(e.outer);
    if (p && p->node->is_array()) {
        std::wstring n = std::to_wstring(e.index + 1);
        if (ctx == "PartySets")
            return L"Party " + n;
        if (ctx == "Characters") {
            const Node* c = e.node->find("CharacterID");
            return L"Member " + n + (c && c->k == Node::K::Int ? L": " + id_label(c->i) : L"");
        }
        if (ctx == "SummonStones" && !e.node->a.empty() && e.node->a[0].k == Node::K::Int)
            return n + L". " + id_label(e.node->a[0].i);
        if (ctx == "Effects" && !e.node->a.empty() && e.node->a[0].k == Node::K::Int)
            return e.node->a[0].i == kNone ? n + L". (empty)" : n + L". " + id_label(e.node->a[0].i);
        if (ctx == "RivalProficaList")
            return L"Rival card " + n;
        return n;
    }
    if (e.key_is_int) {
        int64_t k = std::atoll(e.key.c_str());
        if (ctx == "DifficultyRecordDictionary") {
            static const wchar_t* d[] = {L"Basic", L"Expert", L"Ultimate", L"Supreme"};
            return k >= 0 && k < 4 ? d[k] : L"Difficulty " + widen(e.key);
        }
        if (ctx == "PlayStyleRecordDictionary") {
            static const wchar_t* s[] = {L"Standard", L"Simple", L"Pair"};
            return k >= 0 && k < 3 ? s[k] : L"Play style " + widen(e.key);
        }
        return id_label(k);
    }
    return widen(e.key);
}

// The names of a positional record's values: an element of a list of records (a play style's record,
// a summon stone, an effect), or My profile card itself.
const std::vector<const wchar_t*>* record_names(const Entry& e) {
    if (e.key_is_str)
        return e.dict == "MyProfica" ? positional(e.dict) : nullptr;
    return e.parent >= 0 ? positional(ui.entries[static_cast<size_t>(e.parent)].dict) : nullptr;
}

// The label of a record's value, or of a container in one (Critical bar zones).
std::wstring element_label(const Entry& record, size_t index) {
    const auto* names = record_names(record);
    if (names && index < names->size())
        return (*names)[index];
    return std::to_wstring(index + 1);
}

int add_entry(Node* node, const std::string& key, bool is_str, bool is_int, size_t index, int parent) {
    Entry e;
    e.outer = is_str ? key : "";
    std::string dict = e.outer;
    e.node = fold(node, dict);
    e.dict = dict;
    e.key = key;
    e.key_is_str = is_str;
    e.key_is_int = is_int;
    e.index = index;
    e.parent = parent;
    ui.entries.push_back(std::move(e));
    int me = static_cast<int>(ui.entries.size()) - 1;
    if (parent >= 0)
        ui.entries[static_cast<size_t>(parent)].kids.push_back(me);
    Node* n = ui.entries[static_cast<size_t>(me)].node;
    if (n->is_map()) {
        for (size_t j = 0; j < n->m.size(); ++j) {
            auto& [k, v] = n->m[j];
            if (!v.is_scalar())
                add_entry(&v, k.key_text(), k.k == Node::K::Str, k.k == Node::K::Int, j, me);
        }
    } else if (n->is_array()) {
        for (size_t j = 0; j < n->a.size(); ++j)
            if (!n->a[j].is_scalar())
                add_entry(&n->a[j], std::to_string(j), false, false, j, me);
    }
    return me;
}

void relabel() {
    for (Entry& e : ui.entries)
        e.label = label_of(e);
    for (Entry& e : ui.entries) // a container in a positional record takes the record's name for it
        if (e.parent >= 0 && !e.key_is_str && !e.key_is_int)
            if (const Entry& p = ui.entries[static_cast<size_t>(e.parent)]; record_names(p))
                e.label = element_label(p, e.index);
    // Entries of one list that share a name (a character's several illustrations) are numbered.
    auto number_twins = [](const std::vector<int>& kids) {
        std::map<std::wstring, int> seen, count;
        for (int k : kids)
            ++count[ui.entries[static_cast<size_t>(k)].label];
        for (int k : kids) {
            std::wstring& l = ui.entries[static_cast<size_t>(k)].label;
            if (count[l] > 1)
                l += L" (" + std::to_wstring(++seen[l]) + L")";
        }
    };
    number_twins(ui.roots);
    for (const Entry& e : ui.entries)
        number_twins(e.kids);
}

void build_model() {
    ui.entries.clear();
    ui.roots.clear();
    ui.entries.reserve(20000);
    if (ui.doc.is_map())
        for (size_t j = 0; j < ui.doc.m.size(); ++j) {
            auto& [k, v] = ui.doc.m[j];
            if (!v.is_scalar())
                ui.roots.push_back(add_entry(&v, k.key_text(), k.k == Node::K::Str, k.k == Node::K::Int, j, -1));
        }
    relabel();
}

// --- tree -----------------------------------------------------------------------------------------

struct PickKind;
const PickKind* browse_kind(const Entry& e);

void insert_items(int parent) {
    const std::vector<int>& kids = parent < 0 ? ui.roots : ui.entries[static_cast<size_t>(parent)].kids;
    HTREEITEM where = parent < 0 ? TVI_ROOT : ui.entries[static_cast<size_t>(parent)].item;
    for (int k : kids) {
        Entry& e = ui.entries[static_cast<size_t>(k)];
        if (e.item)
            continue;
        TVINSERTSTRUCTW ins = {};
        ins.hParent = where;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        ins.item.pszText = LPSTR_TEXTCALLBACKW;
        ins.item.lParam = k;
        ins.item.cChildren = e.kids.empty() || browse_kind(e) ? 0 : 1; // moogles: picked on the Moogles page
        e.item = reinterpret_cast<HTREEITEM>(SendMessageW(ui.tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&ins)));
    }
    if (parent >= 0)
        ui.entries[static_cast<size_t>(parent)].items_built = true;
}

void reveal(int k) {
    if (int p = ui.entries[static_cast<size_t>(k)].parent; p >= 0 && browse_kind(ui.entries[static_cast<size_t>(p)])) {
        ui.browse[ui.entries[static_cast<size_t>(p)].dict] = std::atoll(ui.entries[static_cast<size_t>(k)].key.c_str());
        if (ui.sel == p)
            PostMessageW(ui.wnd, kMsgRefresh, 0, 0); // already on that page: show the one found
        k = p;
    }
    std::vector<int> chain;
    for (int p = k; p >= 0; p = ui.entries[static_cast<size_t>(p)].parent)
        chain.push_back(p);
    std::reverse(chain.begin(), chain.end());
    for (size_t j = 0; j + 1 < chain.size(); ++j) {
        Entry& e = ui.entries[static_cast<size_t>(chain[j])];
        if (!e.items_built)
            insert_items(chain[j]);
        TreeView_Expand(ui.tree, e.item, TVE_EXPAND);
    }
    TreeView_SelectItem(ui.tree, ui.entries[static_cast<size_t>(k)].item);
    TreeView_EnsureVisible(ui.tree, ui.entries[static_cast<size_t>(k)].item);
}

void fill_tree() {
    ui.populating = true;
    SendMessageW(ui.tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(ui.tree);
    insert_items(-1);
    SendMessageW(ui.tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ui.tree, nullptr, TRUE);
    ui.populating = false;
}

// --- the panel: the picked entry's values ---------------------------------------------------------

void update_title() {
    std::wstring t = kTitle;
    if (!ui.path.empty())
        t = ui.path.filename().wstring() + (ui.dirty ? L" (changed)" : L"") + L" - " + t;
    SetWindowTextW(ui.wnd, t.c_str());
    EnableWindow(GetDlgItem(ui.wnd, kSave), ui.dirty);
}

void mark_changed() {
    ui.dirty = saveeditor::pack(ui.doc) != ui.original;
    update_title();
}

void set_status(const std::wstring& s) {
    SetWindowTextW(ui.status, s.c_str());
}

HWND panel_add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, ui.panel,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
    return c;
}

void clear_panel() {
    for (Field& f : ui.fields)
        for (HWND h : {f.name, f.ctrl, f.hint, f.on, f.off})
            if (h)
                DestroyWindow(h);
    ui.fields.clear();
    ui.hints.clear();
    ui.read_only.clear();
    ui.ptop = 0;
}

// What an int may be set to: what the game's field can hold, judged by the value it has now.
void int_range(Field& f, const std::string& key) {
    const Node& n = *f.node;
    if (n.u64 || n.i > 0xffffffffLL || n.i < INT32_MIN) {
        f.read_only = true; // account IDs and the like
        return;
    }
    if (n.i > INT32_MAX) {
        f.lo = 0;
        f.hi = 0xffffffffLL;
    } else if (n.i < 0 || key.find("Offset") != std::string::npos || key.find("Point") != std::string::npos) {
        f.lo = INT32_MIN;
        f.hi = INT32_MAX;
    } else {
        f.lo = 0;
        f.hi = INT32_MAX;
    }
}

std::wstring hint_of(const Node& n) {
    if (n.k == Node::K::Int && !n.u64) {
        if (n.i == kNone)
            return L"(none)";
        if (n.i >= 10000000 && *saveeditor::kind_of(n.i))
            return id_label(n.i);
    }
    return L"";
}

// --- what the portrait picker picks ----------------------------------------------------------------

// What a kind of ID is called in the picker, the save's list of them, and whether it has pictures.
struct PickKind {
    int64_t prefix;
    const wchar_t *one, *many;
    const char *info, *list; // ui.doc[info][list]: {id: {UnlockedFlag...}}
};
const PickKind* pick_kind(int64_t prefix) {
    static const PickKind kinds[] = {
        {230, L"character", L"Characters", "CharacterInfo", "Characters"},
        {360, L"moogle", L"Moogles", "MyMoogleInfo", "MyMoogles"},
        {370, L"airship", L"Airships", "AirShipInfo", "AirShips"},
        {280, L"summon", L"Summons", nullptr, nullptr}, // every summon: the save has no list of them
    };
    for (const PickKind& k : kinds)
        if (k.prefix == prefix)
            return &k;
    return nullptr;
}

// The pick kind of a value: a CharacterID, MyMoogleID or AirShipID field (0: none).
int64_t pick_prefix(const std::string& key, const Node& n) {
    if (n.k != Node::K::Int || n.u64)
        return 0;
    int64_t prefix = key.ends_with("CharacterID") ? 230 : key.ends_with("MoogleID") ? 360 : key.ends_with("AirShipID") ? 370
                   : key == "Summon" || key.ends_with("SummonMonsterID") ? 280 // a summon stone's summon
                                                                         : 0;
    return prefix && (n.i == kNone || n.i / 1000000 == prefix) ? prefix : 0;
}

// The list page a pick chooses for (the Moogles or Airships page), or nullptr.
const PickKind* browse_kind(const Entry& e) {
    for (int64_t p : {360, 370})
        if (e.dict == pick_kind(p)->list)
            return pick_kind(p);
    return nullptr;
}

// --- portraits ------------------------------------------------------------------------------------

std::map<std::tuple<int64_t, int, bool>, HBITMAP> g_face_bitmaps;

void forget_faces() { // the names (and portraits) were read again
    for (auto& [k, b] : g_face_bitmaps)
        DeleteObject(b);
    g_face_bitmaps.clear();
}

// A character's portrait at `size` px on white (greyed out when `dim`), made once.
HBITMAP face_bitmap(int64_t id, int size, bool dim) {
    auto& made = g_face_bitmaps;
    auto key = std::make_tuple(id, size, dim);
    if (auto it = made.find(key); it != made.end())
        return it->second;
    auto face = ui.names.faces.find(id);
    if (face == ui.names.faces.end())
        return nullptr;
    dlcmaker::image::Image im = dlcmaker::image::resize_lanczos(face->second, size, size);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto* out = static_cast<uint8_t*>(bits);
    for (size_t p = 0; p < im.rgba.size(); p += 4) {
        unsigned a = im.rgba[p + 3], c[3];
        for (size_t ch = 0; ch < 3; ++ch)
            c[ch] = (im.rgba[p + ch] * a + 255 * (255 - a) + 127) / 255; // on white
        if (dim) { // grey, and faded toward white
            unsigned g = (c[0] * 30 + c[1] * 59 + c[2] * 11) / 100;
            for (unsigned& v : c)
                v = (g + 2 * 255) / 3;
        }
        out[p] = static_cast<uint8_t>(c[2]);
        out[p + 1] = static_cast<uint8_t>(c[1]);
        out[p + 2] = static_cast<uint8_t>(c[0]);
        out[p + 3] = 255;
    }
    made[key] = bmp;
    return bmp;
}

void draw_frame(HDC dc, RECT r, COLORREF line, int width) {
    HPEN pen = CreatePen(PS_INSIDEFRAME, width, line);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(WHITE_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

// A portrait in a frame, filling `r`. Without one (airships have no pictures) the name is written in
// the frame instead, and it returns false.
bool draw_face(HDC dc, int64_t id, RECT r, bool selected, bool dim) {
    draw_frame(dc, r, selected ? RGB(47, 111, 208) : RGB(204, 202, 196), selected ? px(3) : 1);
    int inset = selected ? px(3) : 1, size = r.right - r.left - 2 * inset;
    if (HBITMAP bmp = id == kNone ? nullptr : face_bitmap(id, size, dim)) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(dc, r.left + inset, r.top + inset, size, size, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        return true;
    }
    SetBkMode(dc, TRANSPARENT);
    if (id == kNone) {
        SetTextColor(dc, RGB(136, 136, 136));
        DrawTextW(dc, L"empty", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return true;
    }
    // The name, word-wrapped and centred in the frame.
    std::wstring n = id_label(id);
    RECT box = {r.left + px(6), r.top, r.right - px(6), r.bottom};
    RECT fit = box;
    DrawTextW(dc, n.c_str(), -1, &fit, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    int hgt = static_cast<int>(std::min(fit.bottom - fit.top, box.bottom - box.top));
    box.top += (box.bottom - box.top - hgt) / 2;
    box.bottom = box.top + hgt;
    SetTextColor(dc, dim ? GetSysColor(COLOR_GRAYTEXT) : RGB(60, 60, 60));
    DrawTextW(dc, n.c_str(), -1, &box, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    return false;
}

// The portrait in a character field: clicking it is pressing Change... (the control after it).
LRESULT CALLBACK face_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW));
        draw_face(dc, static_cast<int64_t>(GetWindowLongPtrW(h, GWLP_USERDATA)), r, false, false);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    case WM_LBUTTONUP:
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h) + 1, BN_CLICKED), 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// A portrait (clicking it is Change...), the name, and Change...
void add_pick_controls(Field& f, int id, int64_t current) {
    f.ctrl = panel_add(L"theatrhythm_saveeditor_face", L"", 0, id);
    SetWindowLongPtrW(f.ctrl, GWLP_USERDATA, static_cast<LONG_PTR>(current));
    f.on = panel_add(WC_BUTTONW, L"Change...", WS_TABSTOP | BS_PUSHBUTTON, id + 1);
    f.hint = panel_add(WC_STATICW, (current == kNone ? L"(none)" : id_label(current)).c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
}

// The Moogles or Airships page's "Selected:" row: which one's values show below it.
void add_browse(int64_t prefix, int64_t current) {
    Field f;
    f.pick = prefix;
    f.browse = true;
    f.label = L"Selected:";
    f.name = panel_add(WC_STATICW, f.label.c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    add_pick_controls(f, kFieldBase + 8 * static_cast<int>(ui.fields.size()), current);
    ui.fields.push_back(std::move(f));
}

void add_value(Node& n, const std::wstring& label, const std::string& key) {
    Field f;
    f.node = &n;
    f.label = label;
    int id = kFieldBase + 8 * static_cast<int>(ui.fields.size());
    f.name = panel_add(WC_STATICW, label.c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    if (int64_t p = pick_prefix(key, n)) {
        f.pick = p;
        add_pick_controls(f, id, n.i);
        ui.fields.push_back(std::move(f));
        return;
    }
    switch (n.k) {
    case Node::K::Bool:
        f.ctrl = panel_add(WC_BUTTONW, L"", WS_TABSTOP | BS_AUTOCHECKBOX, id);
        Button_SetCheck(f.ctrl, n.b ? BST_CHECKED : BST_UNCHECKED);
        break;
    case Node::K::Int:
        int_range(f, key);
        if (key.ends_with("account ID") || key.ends_with("AccountID")) // a PSN account's, 0 when none
            f.read_only = true;
        [[fallthrough]];
    case Node::K::Float:
    case Node::K::Str: {
        bool ro = f.read_only || key == "Version" || (n.k == Node::K::Str && key.empty());
        f.read_only = ro;
        std::wstring text = n.k == Node::K::Str ? widen(n.s) : number(n);
        f.ctrl = panel_add(WC_EDITW, text.c_str(), ES_AUTOHSCROLL | (ro ? ES_READONLY | WS_DISABLED : WS_TABSTOP), id, WS_EX_CLIENTEDGE);
        break;
    }
    case Node::K::Ext: {
        std::string ts = saveeditor::timestamp_text(n);
        f.read_only = true;
        f.ctrl = panel_add(WC_EDITW, widen(ts.empty() ? "(binary)" : ts).c_str(), ES_AUTOHSCROLL | ES_READONLY | WS_DISABLED, id, WS_EX_CLIENTEDGE);
        break;
    }
    default:
        f.read_only = true;
        f.ctrl = panel_add(WC_EDITW, n.k == Node::K::Nil ? L"(none)" : L"(binary)", ES_READONLY | WS_DISABLED, id, WS_EX_CLIENTEDGE);
        break;
    }
    f.hint = panel_add(WC_STATICW, hint_of(n).c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    ui.hints.insert(f.hint);
    if (f.read_only)
        ui.read_only.insert(f.ctrl);
    ui.fields.push_back(std::move(f));
}

// "Change all": one row per on/off or number field every entry of a list has. A list is an array, or
// a map keyed by IDs (a record such as a character has named keys).
void add_all_rows(const Entry& e) {
    std::vector<Node*> items;
    if (e.node->is_map()) {
        for (auto& [k, v] : e.node->m) {
            if (k.k != Node::K::Int)
                return;
            items.push_back(&v);
        }
    } else {
        if (record_names(e))
            return;
        for (Node& v : e.node->a)
            items.push_back(&v);
    }
    if (items.size() < 2)
        return;
    bool scalars = std::all_of(items.begin(), items.end(), [](Node* n) { return n->k == Node::K::Bool; });
    if (scalars) { // a list of flags (Picked-up quests)
        Field f;
        f.kind = Field::Kind::AllBool;
        f.label = L"Every entry";
        ui.fields.push_back(std::move(f));
        return;
    }
    if (!items[0]->is_map())
        return;
    for (auto& [k, v] : items[0]->m) {
        if (k.k != Node::K::Str || (v.k != Node::K::Bool && v.k != Node::K::Int) || k.s == "CursorSelectedFlag")
            continue;
        // Not IDs: every party member set to one character, or every slot to one ability, makes no sense.
        if (v.k == Node::K::Int && (v.i > INT32_MAX || k.s.ends_with("ID") || (v.i >= 10000000 && *saveeditor::kind_of(v.i))))
            continue;
        bool everywhere = std::all_of(items.begin(), items.end(), [&](Node* n) {
            const Node* x = n->is_map() ? n->find(k.s) : nullptr;
            return x && x->k == v.k;
        });
        if (!everywhere)
            continue;
        Field f;
        f.kind = v.k == Node::K::Bool ? Field::Kind::AllBool : Field::Kind::AllInt;
        f.key = k.s;
        f.label = friendly(k.s);
        ui.fields.push_back(std::move(f));
    }
}

std::vector<Node*> list_items(const Entry& e) {
    std::vector<Node*> items;
    if (e.node->is_map())
        for (auto& [k, v] : e.node->m)
            items.push_back(&v);
    else
        for (Node& v : e.node->a)
            items.push_back(&v);
    return items;
}

std::wstring all_hint(const Field& f) {
    const Entry& e = ui.entries[static_cast<size_t>(ui.sel)];
    size_t on = 0, total = 0;
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (Node* n : list_items(e)) {
        const Node* x = f.key.empty() ? n : n->find(f.key);
        if (!x)
            continue;
        ++total;
        if (x->k == Node::K::Bool)
            on += x->b;
        else if (x->k == Node::K::Int) {
            lo = std::min(lo, x->i);
            hi = std::max(hi, x->i);
        }
    }
    if (f.kind == Field::Kind::AllBool)
        return std::to_wstring(on) + L" of " + std::to_wstring(total) + L" on";
    if (total == 0)
        return L"";
    return lo == hi ? L"all " + std::to_wstring(lo) : L"from " + std::to_wstring(lo) + L" to " + std::to_wstring(hi);
}

void create_all_controls() {
    for (size_t j = 0; j < ui.fields.size(); ++j) {
        Field& f = ui.fields[j];
        if (f.kind == Field::Kind::Value)
            continue;
        int id = kFieldBase + 8 * static_cast<int>(j);
        f.name = panel_add(WC_STATICW, f.label.c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
        if (f.kind == Field::Kind::AllBool) {
            f.on = panel_add(WC_BUTTONW, L"All on", WS_TABSTOP | BS_PUSHBUTTON, id + 1);
            f.off = panel_add(WC_BUTTONW, L"All off", WS_TABSTOP | BS_PUSHBUTTON, id + 2);
        } else {
            f.ctrl = panel_add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, id, WS_EX_CLIENTEDGE);
            SendMessageW(f.ctrl, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"value"));
            f.on = panel_add(WC_BUTTONW, L"Set on all", WS_TABSTOP | BS_PUSHBUTTON, id + 1);
        }
        f.hint = panel_add(WC_STATICW, all_hint(f).c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
        ui.hints.insert(f.hint);
    }
}

constexpr int kRowH = 28, kLabelW = 230, kCtrlW = 170, kFace = 72;
HWND g_all_heading = nullptr, g_values_heading = nullptr;

HDWP g_defer = nullptr; // while the panel is laid out: its controls move together

void place(HWND h, int x, int y, int w, int hgt) {
    if (!h)
        return;
    if (g_defer)
        g_defer = DeferWindowPos(g_defer, h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    else
        SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

void layout_panel() {
    RECT rc;
    GetClientRect(ui.panel, &rc);
    const int W = rc.right, m = px(14), rh = px(kRowH), lw = px(kLabelW), cw = px(kCtrlW);
    g_defer = BeginDeferWindowPos(static_cast<int>(ui.fields.size()) * 5 + 8);
    int y = m - ui.ptop;
    place(ui.title, m, y, W - 2 * m, px(26));
    y += px(28);
    place(ui.subtitle, m, y, W - 2 * m, px(18));
    y += px(30);
    if (IsWindowVisible(ui.empty)) {
        place(ui.empty, m, y, W - 2 * m, px(36));
        y += px(42);
    }
    bool any_value = false, any_all = false;
    for (const Field& f : ui.fields)
        (f.kind == Field::Kind::Value ? any_value : any_all) = true;
    ShowWindow(g_all_heading, any_all ? SW_SHOW : SW_HIDE);
    ShowWindow(g_values_heading, any_value && any_all ? SW_SHOW : SW_HIDE);
    auto rows = [&](bool all) {
        for (const Field& f : ui.fields) {
            if ((f.kind != Field::Kind::Value) != all)
                continue;
            place(f.name, m, y + px(4), lw - px(8), px(18));
            int x = m + lw;
            if (f.pick) {
                const int face = px(kFace);
                place(f.ctrl, x, y, face, face);
                place(f.hint, x + face + px(12), y + px(10), std::max(px(40), W - m - x - face - px(12)), px(18));
                place(f.on, x + face + px(12), y + px(34), px(96), px(26));
                y += face + px(10);
                continue;
            }
            if (f.kind == Field::Kind::Value) {
                bool check = f.node->k == Node::K::Bool;
                place(f.ctrl, x, y + (check ? px(2) : 0), check ? px(20) : cw, check ? px(20) : px(23));
                x += (check ? px(28) : cw + px(10));
            } else if (f.kind == Field::Kind::AllBool) {
                place(f.on, x, y - px(1), px(80), px(26));
                place(f.off, x + px(86), y - px(1), px(80), px(26));
                x += px(176);
            } else {
                place(f.ctrl, x, y, px(80), px(23));
                place(f.on, x + px(86), y - px(1), px(80), px(26));
                x += px(176);
            }
            place(f.hint, x, y + px(4), std::max(px(40), W - m - x), px(18));
            y += rh;
        }
    };
    if (any_all) {
        place(g_all_heading, m, y, W - 2 * m, px(20));
        y += px(26);
        rows(true);
        y += px(10);
    }
    if (any_value && any_all) {
        place(g_values_heading, m, y, W - 2 * m, px(20));
        y += px(26);
    }
    rows(false);
    if (g_defer)
        EndDeferWindowPos(g_defer);
    g_defer = nullptr;
    ui.pheight = y + ui.ptop + m;
    if (int most = std::max(0, ui.pheight - static_cast<int>(rc.bottom)); ui.ptop > most) { // taller window: less to scroll
        ui.ptop = most;
        layout_panel();
        return;
    }
    SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, ui.pheight, static_cast<UINT>(rc.bottom), ui.ptop, 0};
    SetScrollInfo(ui.panel, SB_VERT, &si, TRUE);
    InvalidateRect(ui.panel, nullptr, TRUE);
}

std::wstring entry_path(int k) {
    std::wstring s;
    for (int p = ui.entries[static_cast<size_t>(k)].parent; p >= 0; p = ui.entries[static_cast<size_t>(p)].parent)
        s = ui.entries[static_cast<size_t>(p)].label + (s.empty() ? L"" : L"  >  " + s);
    return s;
}

void show_entry() {
    SendMessageW(ui.panel, WM_SETREDRAW, FALSE, 0);
    clear_panel();
    if (ui.sel < 0) {
        SetWindowTextW(ui.title, ui.path.empty() ? L"No save open" : L"");
        SetWindowTextW(ui.subtitle, L"");
        SetWindowTextW(ui.empty, ui.path.empty() ? L"Open backup.inf (quests, characters, items, cards, options), musicBackup.inf "
                                                   L"(songs and scores) or proficaBackup.inf (your profile card)."
                                                 : L"Pick an entry on the left.");
        ShowWindow(ui.empty, SW_SHOW);
    } else {
        Entry& e = ui.entries[static_cast<size_t>(ui.sel)];
        SetWindowTextW(ui.title, e.label.c_str());
        std::wstring sub = entry_path(ui.sel);
        if (int64_t id = e.key_is_int ? std::atoll(e.key.c_str()) : 0; id >= 10000000) { // an ID, not a difficulty or style
            const char* kind = saveeditor::kind_of(id);
            sub += (sub.empty() ? L"" : L"    ") + std::wstring(*kind ? widen(kind) + L" " : L"ID ") + widen(e.key);
        }
        SetWindowTextW(ui.subtitle, sub.c_str());
        add_all_rows(e);
        create_all_controls();
        size_t shown = 0, scalars = 0;
        const PickKind* bk = browse_kind(e);
        SetWindowTextW(g_values_heading, bk ? (std::wstring(L"One ") + bk->one).c_str() : L"This entry");
        if (bk && !e.kids.empty()) { // Moogles, Airships: pick one, and its values show
            const Entry* one = &ui.entries[static_cast<size_t>(e.kids[0])];
            for (int k : e.kids)
                if (ui.entries[static_cast<size_t>(k)].key == std::to_string(ui.browse[e.dict]))
                    one = &ui.entries[static_cast<size_t>(k)];
            ui.browse[e.dict] = std::atoll(one->key.c_str());
            add_browse(bk->prefix, ui.browse[e.dict]);
            for (auto& [k, v] : one->node->m)
                if (v.is_scalar() && k.k == Node::K::Str && k.s != "CursorSelectedFlag") {
                    add_value(v, friendly(k.s), k.s);
                    ++shown;
                }
        } else if (e.node->is_map()) {
            for (auto& [k, v] : e.node->m)
                if (v.is_scalar() && !(k.k == Node::K::Str && k.s == "CursorSelectedFlag") && ++scalars <= kMaxFields) {
                    std::wstring label = k.k == Node::K::Str ? friendly(k.s) : id_label(k.k == Node::K::Int ? k.i : 0);
                    add_value(v, label, k.k == Node::K::Str ? k.s : "id");
                    ++shown;
                }
        } else {
            for (size_t j = 0; j < e.node->a.size(); ++j)
                if (e.node->a[j].is_scalar() && element_label(e, j) != L"Cursor selected" && ++scalars <= kMaxFields) {
                    add_value(e.node->a[j], element_label(e, j), record_names(e) ? narrow(element_label(e, j)) : "");
                    ++shown;
                }
        }
        std::wstring note;
        if (scalars > shown)
            note = L"Showing the first " + std::to_wstring(shown) + L" of " + std::to_wstring(scalars) + L" values; change them all above.";
        else if (ui.fields.empty())
            note = e.kids.empty() ? L"Nothing to change here." : L"Pick one of its " + std::to_wstring(e.kids.size()) + L" entries on the left.";
        SetWindowTextW(ui.empty, note.c_str());
        ShowWindow(ui.empty, note.empty() ? SW_HIDE : SW_SHOW);
    }
    layout_panel();
    SendMessageW(ui.panel, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(ui.panel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void refresh_hints() {
    for (Field& f : ui.fields)
        if (f.hint && !f.pick)
            SetWindowTextW(f.hint, f.kind == Field::Kind::Value ? hint_of(*f.node).c_str() : all_hint(f).c_str());
}

void commit_text(Field& f, bool final) {
    if (f.read_only || f.kind != Field::Kind::Value)
        return;
    std::wstring t = strip(text_of(f.ctrl));
    Node& n = *f.node;
    bool ok = true;
    if (n.k == Node::K::Str) {
        n.s = narrow(t);
    } else if (n.k == Node::K::Int) {
        wchar_t* end = nullptr;
        long long v = t.empty() ? 0 : std::wcstoll(t.c_str(), &end, 10);
        ok = !t.empty() && end && *end == 0 && v >= f.lo && v <= f.hi;
        if (ok)
            n.i = v;
    } else if (n.k == Node::K::Float) {
        wchar_t* end = nullptr;
        double v = t.empty() ? 0 : std::wcstod(t.c_str(), &end);
        ok = !t.empty() && end && *end == 0 && std::isfinite(v);
        if (ok)
            n.f = v;
    }
    if (!ok) {
        if (final) {
            SetWindowTextW(f.ctrl, number(n).c_str());
            SetWindowTextW(f.hint, hint_of(n).c_str());
        } else {
            std::wstring why = n.k == Node::K::Int ? L"a whole number from " + std::to_wstring(f.lo) + L" to " + std::to_wstring(f.hi)
                                                   : L"a number";
            SetWindowTextW(f.hint, (L"Not saved: needs " + why).c_str());
        }
        return;
    }
    SetWindowTextW(f.hint, hint_of(n).c_str());
    mark_changed();
}

void set_all(Field& f, bool on, int64_t value) {
    const Entry& e = ui.entries[static_cast<size_t>(ui.sel)];
    for (Node* n : list_items(e)) {
        Node* x = f.key.empty() ? n : n->find(f.key);
        if (!x)
            continue;
        if (f.kind == Field::Kind::AllBool && x->k == Node::K::Bool)
            x->b = on;
        else if (f.kind == Field::Kind::AllInt && x->k == Node::K::Int)
            x->i = value;
    }
    mark_changed();
    // Values shown below may be among those changed.
    for (Field& v : ui.fields)
        if (v.kind == Field::Kind::Value && !v.pick && v.node->k == Node::K::Bool)
            Button_SetCheck(v.ctrl, v.node->b ? BST_CHECKED : BST_UNCHECKED);
    refresh_hints();
}

// --- the picker: a grid of portraits, as the DLC maker's monster picker -----------------------------
// For characters, moogles and airships: a field holding one of their IDs (a party's member, moogle or
// airship), or the Moogles and Airships pages, where it chooses which one to show.

struct Picker {
    HWND wnd = nullptr, search = nullptr, grid = nullptr, note = nullptr;
    const PickKind* kind = nullptr;
    int64_t current = kNone, result = kNone;
    bool browse = false;                   // choosing which to show: locked ones can be picked too
    std::vector<int64_t> all, shown;
    std::set<int64_t> locked, taken;       // not unlocked in this save; already in this party
    std::vector<std::pair<RECT, int64_t>> hits;
    int top = 0;
};
Picker g_pick;
constexpr int kCellW = 104, kCellH = 112, kPickFace = 72;

int picker_columns() {
    RECT rc;
    GetClientRect(g_pick.grid, &rc);
    return std::max(1, static_cast<int>(rc.right / px(kCellW)));
}

int picker_height() {
    int cols = picker_columns(), rows = (static_cast<int>(g_pick.shown.size()) + cols - 1) / cols;
    return rows * px(kCellH) + px(8);
}

void picker_scroll() {
    RECT rc;
    GetClientRect(g_pick.grid, &rc);
    g_pick.top = std::clamp(g_pick.top, 0, std::max(0, picker_height() - static_cast<int>(rc.bottom)));
    SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, picker_height(), static_cast<UINT>(rc.bottom), g_pick.top, 0};
    SetScrollInfo(g_pick.grid, SB_VERT, &si, TRUE);
    InvalidateRect(g_pick.grid, nullptr, FALSE);
}

void picker_filter() {
    std::wstring q = lower(strip(text_of(g_pick.search)));
    g_pick.shown.clear();
    for (int64_t id : g_pick.all)
        if (lower(id_label(id)).find(q) != std::wstring::npos)
            g_pick.shown.push_back(id);
    g_pick.top = 0;
    picker_scroll();
}

void paint_picker(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP buf = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ old_bmp = SelectObject(dc, buf), old_font = SelectObject(dc, ui.font);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    g_pick.hits.clear();
    const int cols = picker_columns(), cw = rc.right / cols, face = px(kPickFace);
    for (size_t i = 0; i < g_pick.shown.size(); ++i) {
        int64_t id = g_pick.shown[i];
        int x = static_cast<int>(i % static_cast<size_t>(cols)) * cw, y = px(8) + static_cast<int>(i / static_cast<size_t>(cols)) * px(kCellH) - g_pick.top;
        if (y + px(kCellH) < 0 || y > rc.bottom)
            continue;
        bool off = g_pick.locked.count(id) || g_pick.taken.count(id);
        RECT img = {x + (cw - face) / 2, y, x + (cw - face) / 2 + face, y + face};
        bool pictured = draw_face(dc, id, img, id == g_pick.current, off);
        SetTextColor(dc, GetSysColor(off ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        if (pictured) {
            RECT name = {x + 2, y + face + px(4), x + cw - 2, y + face + px(20)};
            std::wstring n = id_label(id);
            DrawTextW(dc, n.c_str(), -1, &name, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if (off) {
            RECT why = {x + 2, y + face + px(pictured ? 20 : 4), x + cw - 2, y + face + px(pictured ? 36 : 20)};
            DrawTextW(dc, g_pick.taken.count(id) ? L"in this party" : L"not unlocked", -1, &why, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        g_pick.hits.push_back({{x, y, x + cw, y + px(kCellH)}, id});
    }
    if (g_pick.shown.empty()) {
        RECT note = {0, px(20), rc.right, px(40)};
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        std::wstring t = std::wstring(L"No ") + g_pick.kind->one + L" by that name.";
        DrawTextW(dc, t.c_str(), -1, &note, DT_CENTER | DT_SINGLELINE);
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(buf);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

void picker_click(int x, int y) {
    for (const auto& [r, id] : g_pick.hits) {
        if (!PtInRect(&r, {x, y}))
            continue;
        std::wstring n = id_label(id);
        if (g_pick.taken.count(id)) {
            SetWindowTextW(g_pick.note, (n + L" is already in this party.").c_str());
        } else if (!g_pick.browse && g_pick.locked.count(id)) {
            SetWindowTextW(g_pick.note, (n + L" isn't unlocked in this save yet. Unlock it under " + g_pick.kind->many + L" first.").c_str());
        } else {
            g_pick.result = id;
            DestroyWindow(g_pick.wnd);
        }
        return;
    }
}

LRESULT CALLBACK picker_grid_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        paint_picker(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_pick.wnd)
            picker_scroll();
        return 0;
    case WM_LBUTTONUP:
        picker_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_VSCROLL: {
        SCROLLINFO si = {sizeof(si), SIF_ALL, 0, 0, 0, 0, 0};
        GetScrollInfo(h, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP: g_pick.top -= px(40); break;
        case SB_LINEDOWN: g_pick.top += px(40); break;
        case SB_PAGEUP: g_pick.top -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: g_pick.top += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: g_pick.top = si.nTrackPos; break;
        }
        picker_scroll();
        return 0;
    }
    case WM_MOUSEWHEEL:
        g_pick.top -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * px(kCellH) / 2;
        picker_scroll();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void picker_layout() {
    RECT rc;
    GetClientRect(g_pick.wnd, &rc);
    const int m = px(12);
    place(g_pick.search, m, m, std::min(px(260), static_cast<int>(rc.right) - 2 * m), px(24));
    place(g_pick.note, m, rc.bottom - m - px(18), rc.right - 2 * m, px(18));
    place(g_pick.grid, m, m + px(32), rc.right - 2 * m, rc.bottom - 2 * m - px(32) - px(26));
}

LRESULT CALLBACK picker_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == g_pick.search && HIWORD(wp) == EN_CHANGE)
            picker_filter();
        else if (LOWORD(wp) == IDCANCEL)
            DestroyWindow(h);
        return 0;
    case WM_SIZE:
        if (g_pick.grid)
            picker_layout();
        return 0;
    case WM_MOUSEWHEEL: // the wheel over the search box scrolls the grid too
        return SendMessageW(g_pick.grid, msg, wp, lp);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_pick.wnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Picks one of a kind (`prefix`), starting at `current`. For a field (`browse` false): unlocked ones
// only and, for a party member, not one another slot of the party has (`member`: the field's node).
// Returns the ID, or kNone when the picker was closed.
int64_t pick_id(int64_t prefix, int64_t current, bool browse, const Node* member, const std::wstring& what) {
    g_pick = Picker();
    g_pick.kind = pick_kind(prefix);
    g_pick.current = current;
    g_pick.browse = browse;
    const Node* list = nullptr;
    if (const Node* info = g_pick.kind->info ? ui.doc.find(g_pick.kind->info) : nullptr)
        list = info->find(g_pick.kind->list);
    std::vector<int64_t> ids;
    if (list) {
        for (const auto& [k, v] : list->m)
            if (k.k == Node::K::Int) {
                ids.push_back(k.i);
                const Node* u = v.find("UnlockedFlag");
                if (u && u->k == Node::K::Bool && !u->b)
                    g_pick.locked.insert(k.i);
            }
    } else {
        for (const auto& [id, n] : ui.names.by_id)
            if (id / 1000000 == prefix)
                ids.push_back(id);
    }
    for (int64_t id : ids) // ones the game has a name or picture for (not its unused slots)
        if (ui.names.faces.count(id) || !ui.names.get(id).empty() || !ui.names.loaded)
            g_pick.all.push_back(id);
    std::sort(g_pick.all.begin(), g_pick.all.end(), [](int64_t a, int64_t b) {
        bool la = g_pick.locked.count(a), lb = g_pick.locked.count(b);
        if (la != lb && !g_pick.browse)
            return lb; // unlocked first
        auto sa = ui.names.sort.find(a), sb = ui.names.sort.find(b);
        int64_t oa = sa == ui.names.sort.end() ? a : sa->second, ob = sb == ui.names.sort.end() ? b : sb->second;
        return oa != ob ? oa < ob : a < b;
    });
    // The other members of this party (the list the picked member is in).
    if (member && prefix == 230 && ui.sel >= 0)
        if (int p = ui.entries[static_cast<size_t>(ui.sel)].parent; p >= 0 && ui.entries[static_cast<size_t>(p)].node->is_array())
            for (const Node& m : ui.entries[static_cast<size_t>(p)].node->a)
                if (const Node* c = m.find("CharacterID"); c && c != member && c->k == Node::K::Int && c->i != kNone)
                    g_pick.taken.insert(c->i);

    RECT owner;
    GetWindowRect(ui.wnd, &owner);
    int w = px(720), h = px(600);
    std::wstring title = std::wstring(L"Choose ") + (prefix == 370 ? L"an " : L"a ") + g_pick.kind->one + (what.empty() ? L"" : L" - " + what);
    g_pick.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"theatrhythm_saveeditor_picker", title.c_str(),
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
                                 owner.left + (owner.right - owner.left - w) / 2, owner.top + (owner.bottom - owner.top - h) / 2, w, h,
                                 ui.wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto child = [&](const wchar_t* cls, DWORD style, DWORD ex) {
        HWND c = CreateWindowExW(ex, cls, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_pick.wnd, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
        return c;
    };
    g_pick.search = child(WC_EDITW, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
    std::wstring cue = std::wstring(L"Search ") + lower(g_pick.kind->many);
    SendMessageW(g_pick.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    g_pick.grid = child(L"theatrhythm_saveeditor_picker_grid", WS_VSCROLL, WS_EX_CLIENTEDGE);
    g_pick.note = child(WC_STATICW, SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    std::wstring note = std::wstring(L"Click ") + (prefix == 370 ? L"an " : L"a ") + g_pick.kind->one + L" to choose it.";
    if (prefix != 370 && ui.names.faces.empty())
        note = L"No pictures: set the game folder to see them.";
    SetWindowTextW(g_pick.note, note.c_str());
    picker_layout();
    picker_filter();
    EnableWindow(ui.wnd, FALSE);
    ShowWindow(g_pick.wnd, SW_SHOW);
    SetFocus(g_pick.search);
    MSG msg;
    while (g_pick.wnd && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_pick.wnd && IsDialogMessageW(g_pick.wnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(ui.wnd, TRUE);
    SetActiveWindow(ui.wnd);
    return g_pick.result;
}

void on_field_command(int id, int code) {
    size_t j = static_cast<size_t>((id - kFieldBase) / 8);
    int role = (id - kFieldBase) % 8;
    if (j >= ui.fields.size())
        return;
    Field& f = ui.fields[j];
    if (f.pick) {
        if (role != 1 || code != BN_CLICKED)
            return;
        const Entry& e = ui.entries[static_cast<size_t>(ui.sel)];
        if (f.browse) { // the Moogles or Airships page: show the one picked
            int64_t picked = pick_id(f.pick, ui.browse[e.dict], true, nullptr, L"");
            if (picked != kNone) {
                ui.browse[e.dict] = picked;
                PostMessageW(ui.wnd, kMsgRefresh, 0, 0);
            }
            return;
        }
        int64_t picked = pick_id(f.pick, f.node->i, false, f.node, e.label);
        if (picked != kNone && picked != f.node->i) {
            f.node->i = picked;
            mark_changed();
            relabel(); // "Member 1: <name>"
            InvalidateRect(ui.tree, nullptr, TRUE);
            PostMessageW(ui.wnd, kMsgRefresh, 0, 0); // the panel shows the new one (not from inside its own button)
        }
        return;
    }
    if (f.kind == Field::Kind::Value) {
        if (f.node->k == Node::K::Bool && code == BN_CLICKED) {
            f.node->b = Button_GetCheck(f.ctrl) == BST_CHECKED;
            mark_changed();
            refresh_hints(); // "12 of 35 on" above
        } else if (code == EN_CHANGE && !ui.populating) {
            commit_text(f, false);
        } else if (code == EN_KILLFOCUS) {
            commit_text(f, true);
        }
        return;
    }
    if (code != BN_CLICKED)
        return;
    if (f.kind == Field::Kind::AllBool) {
        set_all(f, role == 1, 0);
        return;
    }
    std::wstring t = strip(text_of(f.ctrl));
    wchar_t* end = nullptr;
    long long v = t.empty() ? -1 : std::wcstoll(t.c_str(), &end, 10);
    if (t.empty() || !end || *end || v < 0 || v > INT32_MAX) {
        message(L"Type a whole number from 0 to 2147483647 first.");
        return;
    }
    set_all(f, true, v);
}

// --- files ----------------------------------------------------------------------------------------

fs::path ini_path() {
    return ui.root / L"saveeditor.ini";
}

void store_settings() {
    std::ofstream out(ini_path(), std::ios::binary);
    out << "; theatrhythm-saveeditor.exe settings\n";
    out << "file=" << narrow(ui.path.wstring()) << "\n";
    out << "game=" << narrow(strip(text_of(ui.game))) << "\n";
}

bool save_file();

bool settle_changes(const wchar_t* before) {
    if (!ui.dirty)
        return true;
    std::wstring q = std::wstring(L"Save your changes to ") + ui.path.filename().wstring() + L" " + before + L"?";
    int a = MessageBoxW(ui.wnd, q.c_str(), kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (a == IDCANCEL)
        return false;
    return a == IDNO || save_file();
}

bool game_running() {
    return dlcmaker::process_running("theatrhythm-fbl");
}

void open_file(const fs::path& p) {
    try {
        std::string plain = saveeditor::decrypt_file(read_bytes(p));
        Node doc = saveeditor::unpack(plain);
        if (saveeditor::pack(doc) != plain)
            throw saveeditor::SaveError("this save would not write back unchanged, so the editor will not touch it");
        ui.doc = std::move(doc);
        ui.original = std::move(plain);
        ui.path = p;
        ui.dirty = false;
        ui.backed_up = false;
        ui.sel = -1;
        ui.browse.clear();
        SetWindowTextW(ui.file, p.wstring().c_str());
        build_model();
        fill_tree();
        show_entry();
        update_title();
        store_settings();
        set_status(L"Opened " + p.filename().wstring() + L": " + std::to_wstring(ui.entries.size()) + L" entries. " + ui.names_note);
    } catch (const std::exception& e) {
        message(L"Cannot open " + p.wstring() + L":\n" + widen(e.what()));
    }
}

bool save_file() {
    if (ui.path.empty())
        return false;
    if (game_running()) {
        message(L"Close the game first. It keeps the save in memory and writes it back when it saves, which would undo "
                L"your changes (or yours would undo its progress).");
        return false;
    }
    try {
        std::string plain = saveeditor::pack(ui.doc);
        std::string file = saveeditor::encrypt_file(plain);
        if (saveeditor::decrypt_file(file) != plain) // what the game will read back
            throw saveeditor::SaveError("the written file does not read back the same");
        if (!ui.backed_up) {
            fs::path dir = ui.root / L"save-backups";
            fs::create_directories(dir);
            std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &now);
            wchar_t stamp[32];
            wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &tm);
            fs::path to = dir / (ui.path.stem().wstring() + L"-" + stamp + ui.path.extension().wstring());
            fs::copy_file(ui.path, to, fs::copy_options::overwrite_existing);
            ui.backed_up = true;
            set_status(L"The file as it was is in " + to.wstring());
        }
        write_bytes(ui.path, file);
        ui.original = plain;
        mark_changed();
        set_status(L"Saved " + ui.path.wstring() + L". The file as it was is in " + (ui.root / L"save-backups").wstring() + L".");
        return true;
    } catch (const std::exception& e) {
        message(L"Cannot save:\n" + widen(e.what()));
        return false;
    }
}

std::wstring pick(bool folder, const wchar_t* title, const std::wstring& start) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return L"";
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetTitle(title);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        COMDLG_FILTERSPEC specs[] = {{L"Theatrhythm saves (*.inf)", L"*.inf"}, {L"All files", L"*.*"}};
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

fs::path save_data_folder() {
    if (!ui.path.empty())
        return ui.path.parent_path();
    if (!ui.save_folder.empty())
        return fs::path(ui.save_folder) / L"savedata" / L"1" / L"CUSA32853" / L"SaveData";
    return {};
}

// Names load in the background (reading the dump's tables takes a few seconds).
void load_names() {
    std::string dir = narrow(strip(text_of(ui.game)));
    if (dir.empty()) {
        ui.names_note = L"Set the game folder to see names instead of IDs.";
        set_status(ui.names_note);
        return;
    }
    set_status(L"Reading names from the game folder...");
    HWND wnd = ui.wnd;
    std::thread([wnd, dir] {
        auto* names = new saveeditor::Names;
        std::string err = names->load(dir);
        auto* note = new std::wstring(err.empty() ? L"" : L"No names (" + widen(err) + L").");
        PostMessageW(wnd, kMsgNames, reinterpret_cast<WPARAM>(note), reinterpret_cast<LPARAM>(names));
    }).detach();
}

void find_next() {
    std::wstring q = lower(strip(text_of(ui.find)));
    if (q.empty() || ui.entries.empty())
        return;
    // The next entry named exactly that (or with a number: "Cloud (2)"), else the next that has it in its name.
    size_t n = ui.entries.size();
    // After a moogle found on the Moogles page, on from that moogle (not from the page).
    static size_t last = SIZE_MAX;
    bool on_page = last < n && ui.sel >= 0 && ui.entries[last].parent == ui.sel;
    size_t start = on_page ? last + 1 : ui.sel < 0 ? 0 : static_cast<size_t>(ui.sel) + 1;
    for (bool exact : {true, false})
        for (size_t j = 0; j < n; ++j) {
            size_t k = (start + j) % n;
            std::wstring l = lower(ui.entries[k].label);
            bool hit = exact ? (l == q || (l.rfind(q + L" (", 0) == 0 && l.back() == L')')) : l.find(q) != std::wstring::npos;
            if (hit) {
                last = k;
                reveal(static_cast<int>(k));
                return;
            }
        }
    set_status(L"Nothing named \"" + strip(text_of(ui.find)) + L"\".");
}

// --- the window -----------------------------------------------------------------------------------

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, ui.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
    return c;
}

HWND g_file_label, g_game_label;
constexpr int kMargin = 12, kButton = 96, kTreeW = 340;

void layout() {
    RECT rc;
    GetClientRect(ui.wnd, &rc);
    const int W = rc.right, H = rc.bottom, m = px(kMargin), bw = px(kButton), bh = px(28), rh = px(24), gap = px(6);
    int y = m;
    place(g_file_label, m, y + px(4), px(90), px(18));
    int bx = W - m - bw;
    place(GetDlgItem(ui.wnd, kReload), bx, y - px(2), bw, bh);
    place(GetDlgItem(ui.wnd, kOpen), bx -= bw + gap, y - px(2), bw, bh);
    place(ui.file, m + px(94), y, bx - gap - m - px(94), rh);
    y += px(34);
    place(g_game_label, m, y + px(4), px(90), px(18));
    place(GetDlgItem(ui.wnd, kGameBrowse), W - m - bw, y - px(2), bw, bh);
    place(ui.game, m + px(94), y, W - m - bw - gap - m - px(94), rh);
    y += px(40);
    int by = H - m - bh;
    place(GetDlgItem(ui.wnd, kClose), W - m - bw, by, bw, bh);
    place(GetDlgItem(ui.wnd, kSave), W - m - 2 * bw - gap, by, bw, bh);
    place(ui.status, m, by + px(6), W - 2 * m - 2 * bw - 3 * gap, px(18));
    int tw = px(kTreeW);
    place(ui.find, m, y, tw - px(70) - gap, rh);
    place(GetDlgItem(ui.wnd, kFindNext), m + tw - px(70), y - px(2), px(70), bh);
    int top = y + px(32), bottom = by - px(10);
    place(ui.tree, m, top, tw, bottom - top);
    place(ui.panel, m + tw + gap, y, W - 2 * m - tw - gap, bottom - y);
    layout_panel();
}

// Scrolls the panel to `pos`: its controls move together and only the strip uncovered is painted
// (placing each of the Options' hundreds of controls again on every wheel notch was slow).
void scroll_panel(int pos) {
    RECT rc;
    GetClientRect(ui.panel, &rc);
    pos = std::clamp(pos, 0, std::max(0, ui.pheight - static_cast<int>(rc.bottom)));
    if (pos == ui.ptop)
        return;
    int dy = ui.ptop - pos;
    ui.ptop = pos;
    SetScrollPos(ui.panel, SB_VERT, pos, TRUE);
    ScrollWindowEx(ui.panel, 0, dy, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    UpdateWindow(ui.panel);
}

LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        on_field_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        HWND c = reinterpret_cast<HWND>(lp);
        if (ui.hints.count(c))
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        int bg = ui.read_only.count(c) ? COLOR_BTNFACE : COLOR_WINDOW; // values the editor leaves alone look grey
        SetBkColor(dc, GetSysColor(bg));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(bg));
    }
    case WM_VSCROLL: {
        SCROLLINFO si = {sizeof(si), SIF_ALL, 0, 0, 0, 0, 0};
        GetScrollInfo(h, SB_VERT, &si);
        int pos = si.nPos, line = px(kRowH);
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        }
        scroll_panel(pos);
        return 0;
    }
    case WM_MOUSEWHEEL: // three rows a notch; a touchpad's smaller steps add up
        scroll_panel(ui.ptop - GET_WHEEL_DELTA_WPARAM(wp) * 3 * px(kRowH) / WHEEL_DELTA);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void on_command(int id, int code) {
    switch (id) {
    case kOpen: {
        if (!settle_changes(L"before opening another file"))
            return;
        fs::path start = save_data_folder();
        std::wstring p = pick(false, L"Open a save file (backup.inf, musicBackup.inf, proficaBackup.inf)", start.wstring());
        if (!p.empty())
            open_file(p);
        break;
    }
    case kReload:
        if (ui.path.empty() || !settle_changes(L"before reloading"))
            return;
        open_file(ui.path);
        break;
    case kGameBrowse: {
        std::wstring p = pick(true, L"Game folder (your decrypted dump), for names", strip(text_of(ui.game)));
        if (!p.empty()) {
            SetWindowTextW(ui.game, p.c_str());
            store_settings();
            load_names();
        }
        break;
    }
    case kFindNext:
        find_next();
        break;
    case IDOK: // Enter: find, from the find box
        if (GetFocus() == ui.find)
            find_next();
        else if (HWND f = GetFocus(); f && GetParent(f) == ui.panel)
            SetFocus(ui.tree); // ends the edit (commits it)
        break;
    case kSave:
        save_file();
        break;
    case kClose:
        PostMessageW(ui.wnd, WM_CLOSE, 0, 0);
        break;
    case kGameEdit:
        if (code == EN_KILLFOCUS)
            store_settings();
        break;
    }
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->hwndFrom != ui.tree)
            break;
        if (hdr->code == TVN_GETDISPINFOW) {
            auto* di = reinterpret_cast<NMTVDISPINFOW*>(lp);
            if (di->item.mask & TVIF_TEXT) {
                const std::wstring& l = ui.entries[static_cast<size_t>(di->item.lParam)].label;
                lstrcpynW(di->item.pszText, l.c_str(), di->item.cchTextMax);
            }
            return 0;
        }
        if (hdr->code == TVN_ITEMEXPANDINGW) {
            auto* tv = reinterpret_cast<NMTREEVIEWW*>(lp);
            int k = static_cast<int>(tv->itemNew.lParam);
            if (tv->action == TVE_EXPAND && !ui.entries[static_cast<size_t>(k)].items_built) {
                SendMessageW(ui.tree, WM_SETREDRAW, FALSE, 0);
                insert_items(k);
                SendMessageW(ui.tree, WM_SETREDRAW, TRUE, 0);
            }
            return FALSE;
        }
        if (hdr->code == TVN_SELCHANGEDW && !ui.populating) {
            auto* tv = reinterpret_cast<NMTREEVIEWW*>(lp);
            if (HWND f = GetFocus(); f && GetParent(f) == ui.panel)
                SetFocus(ui.tree); // commit a value being typed before the panel goes
            ui.sel = tv->itemNew.hItem ? static_cast<int>(tv->itemNew.lParam) : -1;
            show_entry();
        }
        break;
    }
    case kMsgRefresh:
        show_entry();
        return 0;
    case kMsgNames: {
        std::unique_ptr<std::wstring> note(reinterpret_cast<std::wstring*>(wp));
        std::unique_ptr<saveeditor::Names> names(reinterpret_cast<saveeditor::Names*>(lp));
        forget_faces();
        ui.names = std::move(*names);
        ui.names_note = *note;
        set_status(ui.names.loaded ? L"Names read from the game folder." : ui.names_note);
        if (!ui.entries.empty()) {
            relabel();
            InvalidateRect(ui.tree, nullptr, TRUE);
            if (ui.sel >= 0) {
                SetWindowTextW(ui.title, ui.entries[static_cast<size_t>(ui.sel)].label.c_str());
                show_entry();
            }
        }
        return 0;
    }
    case WM_SIZE:
        if (ui.panel)
            layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {px(820), px(520)};
        return 0;
    }
    case WM_CLOSE:
        if (!settle_changes(L"before closing"))
            return 0;
        store_settings();
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void create_controls() {
    g_file_label = add(WC_STATICW, L"Save file", SS_LEFT, 0);
    ui.file = add(WC_EDITW, L"", ES_AUTOHSCROLL | ES_READONLY, kFileEdit, WS_EX_CLIENTEDGE);
    add(WC_BUTTONW, L"Open...", WS_TABSTOP | BS_PUSHBUTTON, kOpen);
    add(WC_BUTTONW, L"Reload", WS_TABSTOP | BS_PUSHBUTTON, kReload);
    g_game_label = add(WC_STATICW, L"Game folder", SS_LEFT, 0);
    ui.game = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kGameEdit, WS_EX_CLIENTEDGE);
    SendMessageW(ui.game, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"your decrypted dump, for names (optional)"));
    add(WC_BUTTONW, L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, kGameBrowse);
    ui.find = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kFind, WS_EX_CLIENTEDGE);
    SendMessageW(ui.find, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Find a character, item, song..."));
    add(WC_BUTTONW, L"Find", WS_TABSTOP | BS_PUSHBUTTON, kFindNext);
    ui.tree = add(WC_TREEVIEWW, L"", WS_TABSTOP | WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS, kTree);
    SetWindowTheme(ui.tree, L"Explorer", nullptr);
    ui.panel = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_CONTROLPARENT, L"theatrhythm_saveeditor_panel", L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 10, 10, ui.wnd, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    ui.title = panel_add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    SendMessageW(ui.title, WM_SETFONT, reinterpret_cast<WPARAM>(ui.big), FALSE);
    ui.subtitle = panel_add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    ui.hints.clear();
    ui.empty = panel_add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0);
    g_all_heading = panel_add(WC_STATICW, L"Change all entries", SS_LEFT, 0);
    SendMessageW(g_all_heading, WM_SETFONT, reinterpret_cast<WPARAM>(ui.bold), FALSE);
    g_values_heading = panel_add(WC_STATICW, L"This entry", SS_LEFT, 0);
    SendMessageW(g_values_heading, WM_SETFONT, reinterpret_cast<WPARAM>(ui.bold), FALSE);
    ui.status = add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    add(WC_BUTTONW, L"Save", WS_TABSTOP | BS_PUSHBUTTON, kSave);
    add(WC_BUTTONW, L"Close", WS_TABSTOP | BS_PUSHBUTTON, kClose);
}

int run_window(HINSTANCE inst) {
    ui.root = exe_folder();
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_TREEVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    ui.dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui.dpi);
    ui.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_BOLD;
    ui.bold = CreateFontIndirectW(&bold);
    LOGFONTW big = bold;
    big.lfHeight = big.lfHeight * 3 / 2;
    ui.big = CreateFontIndirectW(&big);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"theatrhythm_saveeditor";
    RegisterClassExW(&wc);
    WNDCLASSEXW pc = wc;
    pc.lpfnWndProc = panel_proc;
    pc.hIcon = nullptr;
    pc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    pc.lpszClassName = L"theatrhythm_saveeditor_panel";
    RegisterClassExW(&pc);
    WNDCLASSEXW fc = pc;
    fc.lpfnWndProc = face_proc;
    fc.lpszClassName = L"theatrhythm_saveeditor_face";
    RegisterClassExW(&fc);
    WNDCLASSEXW kc = wc;
    kc.lpfnWndProc = picker_proc;
    kc.lpszClassName = L"theatrhythm_saveeditor_picker";
    RegisterClassExW(&kc);
    WNDCLASSEXW gc = pc;
    gc.lpfnWndProc = picker_grid_proc;
    gc.lpszClassName = L"theatrhythm_saveeditor_picker_grid";
    RegisterClassExW(&gc);

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, px(1060), px(760)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, ui.dpi);
    int w = std::min<int>(r.right - r.left, work.right - work.left), h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
    ui.wnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, kTitle, style | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             w, h, nullptr, nullptr, inst, nullptr);
    create_controls();

    // Settings: saveeditor.ini, else the release's paths.ini (the folders theatrhythm-setup.exe chose).
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
    ui.save_folder = paths[L"save"];
    std::wstring game = !ini[L"game"].empty() ? ini[L"game"] : paths[L"game"];
    if (game.empty()) {
        wchar_t env[32768];
        if (DWORD n = GetEnvironmentVariableW(L"THEATRHYTHM_GAME_DIR", env, 32768); n && n < 32768)
            game = env;
    }
    SetWindowTextW(ui.game, game.c_str());
    layout();
    update_title();
    show_entry();
    ShowWindow(ui.wnd, SW_SHOW);
    load_names();

    // The file: the command line's, the last one, else the save folder's backup.inf.
    std::vector<std::string> args = dlcmaker::utf8_args(0, nullptr);
    std::error_code ec;
    fs::path file = !args.empty() ? fs::path(widen(args[0])) : fs::path(ini[L"file"]);
    if (file.empty() || !fs::is_regular_file(file, ec))
        if (fs::path d = save_data_folder(); !d.empty() && fs::is_regular_file(d / L"backup.inf", ec))
            file = d / L"backup.inf";
    if (!file.empty() && fs::is_regular_file(file, ec))
        open_file(file);
    SetFocus(ui.tree);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(ui.wnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(ui.font);
    DeleteObject(ui.bold);
    DeleteObject(ui.big);
    return 0;
}

// --- command line ---------------------------------------------------------------------------------

int cli(const std::vector<std::string>& args) {
    dlcmaker::setup_console();
    try {
        if (args[0] == "dump" && args.size() == 2) {
            Node n = saveeditor::unpack(saveeditor::decrypt_file(read_bytes(fs::path(widen(args[1])))));
            std::string j = saveeditor::to_json(n) + "\n";
            std::fwrite(j.data(), 1, j.size(), stdout);
            return 0;
        }
        if (args[0] == "check" && args.size() >= 2) {
            int bad = 0;
            for (size_t j = 1; j < args.size(); ++j) {
                fs::path p = widen(args[j]);
                std::string plain = saveeditor::decrypt_file(read_bytes(p));
                std::string again = saveeditor::pack(saveeditor::unpack(plain));
                std::string file = saveeditor::encrypt_file(again);
                bool ok = again == plain && saveeditor::decrypt_file(file) == plain;
                std::printf("%s %s: %zu bytes of MessagePack\n", ok ? "ok" : "FAILED", narrow(p.wstring()).c_str(), plain.size());
                bad += !ok;
            }
            return bad ? 1 : 0;
        }
        if ((args[0] == "rewrite" && args.size() == 3) || (args[0] == "set" && args.size() >= 4)) {
            // rewrite: what Save writes with nothing changed. set: PATH=VALUE edits, PATH being keys
            // and array indexes split by / with * for every entry (CharacterInfo/Characters/*/UnlockedFlag=true).
            Node doc = saveeditor::unpack(saveeditor::decrypt_file(read_bytes(fs::path(widen(args[1])))));
            for (size_t j = 3; j < args.size(); ++j) {
                size_t eq = args[j].find('=');
                if (eq == std::string::npos)
                    throw saveeditor::SaveError("expected PATH=VALUE: " + args[j]);
                std::vector<std::string> keys;
                std::stringstream ss(args[j].substr(0, eq));
                for (std::string k; std::getline(ss, k, '/');)
                    keys.push_back(k);
                std::string value = args[j].substr(eq + 1);
                std::vector<Node*> at = {&doc};
                for (const std::string& k : keys) {
                    std::vector<Node*> next;
                    for (Node* n : at) {
                        if (n->is_map()) {
                            for (auto& [mk, mv] : n->m)
                                if (k == "*" || mk.key_text() == k)
                                    next.push_back(&mv);
                        } else if (n->is_array()) {
                            for (size_t i = 0; i < n->a.size(); ++i)
                                if (k == "*" || std::to_string(i) == k)
                                    next.push_back(&n->a[i]);
                        }
                    }
                    at = std::move(next);
                }
                if (at.empty())
                    throw saveeditor::SaveError("nothing at " + args[j].substr(0, eq));
                for (Node* n : at) {
                    if (n->k == Node::K::Bool && (value == "true" || value == "false")) n->b = value == "true";
                    else if (n->k == Node::K::Int) n->i = std::stoll(value);
                    else if (n->k == Node::K::Float) n->f = std::stod(value);
                    else if (n->k == Node::K::Str) n->s = value;
                    else throw saveeditor::SaveError("cannot set " + args[j]);
                }
                std::printf("set %zu value(s) at %s\n", at.size(), args[j].substr(0, eq).c_str());
            }
            write_bytes(fs::path(widen(args[2])), saveeditor::encrypt_file(saveeditor::pack(doc)));
            return 0;
        }
        std::fprintf(stderr, "usage: theatrhythm-saveeditor dump FILE | check FILE... | rewrite FILE OUT | set FILE OUT PATH=VALUE...\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    std::vector<std::string> args = dlcmaker::utf8_args(0, nullptr);
    if (!args.empty() && (args[0] == "dump" || args[0] == "check" || args[0] == "rewrite" || args[0] == "set"))
        return cli(args);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int rc = run_window(inst);
    CoUninitialize();
    return rc;
}
