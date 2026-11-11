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
#include <vssym32.h>
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
constexpr UINT kMsgRowAction = WM_APP + 3; // to the panel: wp what (1 open, 2 up, 3 down, 4 remove), lp the row
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
        {"UserInfo", L"Player"}, {"MainMenuInfo", L"Menu unlocks and notices"}, {"MusicStageInfo", L"Play totals"},
        {"RhythpoInfo", L"Rhythpo"}, {"MusicSelectInfo", L"Song select"}, {"SeriesQuestInfo", L"Series Quests"},
        {"MultiBattleInfo", L"Multi Battle stats"}, {"PartyInfo", L"Parties"}, {"CharacterInfo", L"Characters"},
        {"SummonStoneInfo", L"Summon stones"}, {"MyMoogleInfo", L"Moogles"}, {"AirShipInfo", L"Airships"},
        {"ItemInfo", L"Items and illustrations"}, {"CollecaInfo", L"CollectaCards"}, {"TheaterInfo", L"Theater"},
        {"ConfigInfo", L"Options"}, {"HelpInfo", L"Help"}, {"MusicPlayerInfo", L"Music player"},
        {"AchievementInfo", L"Feat notices seen"}, {"TipsInfo", L"Tips"}, {"MusicInfo", L"Songs"}, {"ProficaInfo", L"Profile card"},
        {"WorldDictionary", L"Worlds"}, {"StageDictionary", L"Stages"}, {"EndlessWorldUniqueInfo", L"Endless World"},
        {"PickedupFlags", L"Picked-up quests"}, {"PickupStageList", L"Picked-up stages"}, {"PartySets", L"Party sets"},
        {"AbilityData", L"Abilities"}, {"Items", L"Items"}, {"ProficaIllusts", L"Profile card illustrations"},
        {"Collecas", L"CollectaCards"}, {"MovieDatas", L"Movies"}, {"HelpRecordDictionary", L"Help pages"},
        {"AchievementRecordDictionary", L"Feat notices seen"}, {"TipsDictionary", L"Tips"}, {"MyMoogles", L"Moogles"},
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
        {"TotalFMSMoveDistance", L"Total field distance"}, {"LastSelectedMSDifficulty", L"Last selected difficulty"}, {"LastSelectedMusicID", L"Last selected song"},
        {"LastSelectedMSPlayStyle", L"Last selected play style"}, {"Version", L"Save version"},
        {"_highscore", L"High score"}, {"_maxChainCount", L"Max chain"}, {"CharacterID", L"Character"}, {"AirShipID", L"Airship"},
        {"MyMoogleID", L"Moogle"}, {"PossessionNumber", L"Owned"}, {"TotalUseCount", L"Times used in total"},
        {"UsedCount", L"Times used"}, {"HPGrownAmount", L"HP bonus"}, {"PowerGrownAmount", L"Power bonus"},
        {"MagicGrownAmount", L"Magic bonus"}, {"MindGrownAmount", L"Mind bonus"}, {"SpeedGrownAmount", L"Speed bonus"},
        {"LuckGrownAmount", L"Luck bonus"}, {"ViewHint", L"Hint viewed"}, {"LookedAchievementWindow", L"Seen"},
        {"SummonStoneIndex", L"Summon stone"}, {"Looked", L"Read"}, {"TotalGetCount", L"Stones obtained (total)"},
        {"GetCountFromRivalProfica", L"Stones obtained from rival ProfiCards"},
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
    bool hidden = false;   // not in the tree, nor in a page's path: its entries show in its place (Party sets)
};

struct Field {
    enum class Kind { Value, AllBool, AllInt, List, Table } kind = Kind::Value; // List: the Parties or Stones list; Table: a grid (Theater)
    Node* node = nullptr;          // Value: the value itself
    std::string key;               // AllBool / AllInt: the field each entry of the list has
    std::wstring label;
    HWND name = nullptr, ctrl = nullptr, hint = nullptr, on = nullptr, off = nullptr, del = nullptr, pic = nullptr; // pic: a party's stone's summon
    int64_t lo = 0, hi = 0;        // Int range
    bool read_only = false;
    int64_t pick = 0;              // a character, moogle or airship ID (its prefix): a portrait (ctrl) and
                                   // Change... (on) instead of the number
    bool browse = false;           // pick: which one the Moogles or Airships page shows (node is nullptr)
    int combo = 0;                 // a drop-down list (ctrl): 1 a summon stone effect, 2 its rarity, 3 a party's stone, 4 an ability
                                   // (a member's, or a character's last set), 5 a song (under its series' heading), 6 one of a game
                                   // enum's names (key: which)
    Node* slot = nullptr;          // combo 1 / 2: the effect slot, [effect, rarity]; 4: the list of abilities it is one of
    int64_t who = 0;               // combo 4: whose abilities
    bool heading = false;          // its name is a heading over the rows of its group (a party's member)
    HWND above = nullptr;          // a heading on its own line above it (a character's "Last set abilities", "Abilities")
};

// A party's members, [{CharacterID, Abilities: [id, ...]}, ...]: shown on the party's own page.
bool party_members(const Node& n) {
    if (!n.is_array())
        return false;
    for (const Node& m : n.a) {
        if (!m.is_map() || m.m.size() != 2)
            return false;
        const Node* c = m.find("CharacterID");
        const Node* a = m.find("Abilities");
        if (!c || c->k != Node::K::Int || c->u64 || !a || !a->is_array())
            return false;
        for (const Node& x : a->a)
            if (x.k != Node::K::Int || x.u64)
                return false;
    }
    return true;
}

// A summon stone's effects, [[effect, rarity], ...]: shown on the stone's own page.
bool effect_slots(const Node& n) {
    if (!n.is_array())
        return false;
    for (const Node& s : n.a)
        if (!s.is_array() || s.a.size() != 2 || s.a[0].k != Node::K::Int || s.a[1].k != Node::K::Int || s.a[0].u64 || s.a[1].u64)
            return false;
    return true;
}

struct Ui {
    HWND wnd = nullptr, file = nullptr, game = nullptr, tree = nullptr, panel = nullptr, find = nullptr, status = nullptr;
    HWND title = nullptr, subtitle = nullptr, empty = nullptr;
    HFONT font = nullptr, bold = nullptr, big = nullptr;
    UINT dpi = 96;
    fs::path root, path;               // exe folder; the open file
    Node doc;
    std::string original;              // the MessagePack as read (or last saved)
    bool dirty = false, populating = false, backed_up = false;
    // The page's controls in the order panel_add made them, and the last page's, kept while the next is
    // built so a control of the same kind in the same place is reused (Party 1 to Party 2: no new windows).
    struct Made {
        HWND h;
        std::wstring cls; // what panel_add was asked for (a control may change its own style, a combo box does)
        DWORD style, ex;
    };
    std::vector<Made> made, pool;
    size_t pool_next = 0;
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
    std::map<std::wstring, int> flag_w;    // a kind of table's tick box column, as dragged (by its heading; 96 dpi pixels)
};
Ui ui;

enum : int {
    kOpen = 100, kReload, kGameBrowse, kFind, kFindNext, kSave, kClose, kTree, kFileEdit, kGameEdit, kAction,
    kUndo, kBackups, kRestore, kOpenBackups,
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
        if (ctx == "Effects" && e.node->a.size() >= 2 && e.node->a[0].k == Node::K::Int && e.node->a[1].k == Node::K::Int) {
            if (e.node->a[0].i == kNone)
                return n + L". None";
            std::string t = ui.names.effect(e.node->a[0].i, e.node->a[1].i); // "EXP received +10%"
            return n + L". " + (t.empty() ? id_label(e.node->a[0].i) : widen(t));
        }
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
    if (e.dict == "MyProfica")
        return positional(e.dict);
    if (e.dict == "Summon stone") // the one on a profile card
        return positional("SummonStones");
    if (e.key_is_str)
        return nullptr;
    return e.parent >= 0 ? positional(ui.entries[static_cast<size_t>(e.parent)].dict) : nullptr;
}

// The label of a record's value, or of a container in one (Critical bar zones).
std::wstring element_label(const Entry& record, size_t index) {
    const auto* names = record_names(record);
    if (names && index < names->size())
        return (*names)[index];
    return std::to_wstring(index + 1);
}

// Lists shown as one table of names and tick boxes on their page, not as entries of their own: Theater's
// movies, the profile card illustrations, the help pages, the feats' notices. Each entry is {flag,
// CursorSelectedFlag}, or two flags in the save's order, as the "All" rows above the table have them (a
// help page: Unlocked, Looked; a feat: its hint viewed, its window seen).
struct FlagTable {
    const char *dict, *flag;
    const wchar_t *heading, *item, *column; // "Movies"; the table's columns: "Movie", "Owned"
    const char* flag2 = nullptr;            // a second box column
    const wchar_t* column2 = nullptr;
    int flags() const { return flag2 ? 2 : 1; }
    const char* flag_of(int col) const { return col == 2 ? flag2 : flag; } // by the column it shows in
};
bool flag_shaped(const Node& n, const char* flag) {
    if (!n.is_map() || n.m.empty())
        return false;
    for (const auto& [k, v] : n.m) {
        if (k.k != Node::K::Int || !v.is_map())
            return false;
        const Node* f = v.find(flag);
        if (!f || f->k != Node::K::Bool)
            return false;
        for (const auto& [kk, vv] : v.m)
            if (!vv.is_scalar())
                return false;
    }
    return true;
}

const FlagTable* flag_table(const Entry& e) {
    static const FlagTable tables[] = {
        {"MovieDatas", "ObtainedFlag", L"Movies", L"Movie", L"Owned"},
        {"ProficaIllusts", "UnlockedFlag", L"Illustrations", L"Illustration", L"Unlocked"},
        {"HelpRecordDictionary", "Unlocked", L"Help pages", L"Help page", L"Unlocked", "Looked", L"Read"},
        {"AchievementRecordDictionary", "ViewHint", L"Feats", L"Feat", L"Hint viewed", "LookedAchievementWindow", L"Seen"},
    };
    for (const FlagTable& t : tables)
        if (e.dict == t.dict)
            return flag_shaped(*e.node, t.flag) && (!t.flag2 || flag_shaped(*e.node, t.flag2)) ? &t : nullptr;
    return nullptr;
}

// A character's abilities, on the character's page.
const FlagTable kAbilities = {"AbilityData", "UnlockedFlag", L"Abilities", L"Ability", L"Unlocked"};

// One of a character's own lists, shown on their page and not as an entry of its own: their abilities (a
// table) and the abilities they last had set (three lists).
bool character_list(const Entry& owner, const Node& k, const Node& v) {
    if (!owner.key_is_int || owner.parent < 0 || k.k != Node::K::Str)
        return false;
    const Entry& list = ui.entries[static_cast<size_t>(owner.parent)];
    if (list.dict != "Characters" || !list.node->is_map())
        return false;
    if (k.s == "AbilityData")
        return flag_shaped(v, kAbilities.flag);
    if (k.s == "LastSetAbilities")
        return v.is_array() && std::all_of(v.a.begin(), v.a.end(), [](const Node& x) { return x.k == Node::K::Int && !x.u64; });
    return false;
}

// One of Parties' parties: its members show on its page, not as entries of their own.
bool is_party(const Entry& e) {
    return e.parent >= 0 && ui.entries[static_cast<size_t>(e.parent)].dict == "PartySets" && e.node->is_map();
}

int add_entry(Node* node, const std::string& key, bool is_str, bool is_int, size_t index, int parent) {
    Entry e;
    e.outer = is_str ? key : "";
    std::string dict = e.outer;
    // A container in a positional record is named by its place: a stone's "Effects", a card's "Summon stone".
    if (!is_str && !is_int && parent >= 0)
        if (const auto* names = record_names(ui.entries[static_cast<size_t>(parent)]); names && index < names->size())
            dict = narrow((*names)[index]);
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
    if (n->is_map() && !flag_table(ui.entries[static_cast<size_t>(me)])) {
        for (size_t j = 0; j < n->m.size(); ++j) {
            auto& [k, v] = n->m[j];
            if (!v.is_scalar() && !(is_party(ui.entries[static_cast<size_t>(me)]) && k.k == Node::K::Str && k.s == "Characters" && party_members(v)) &&
                !character_list(ui.entries[static_cast<size_t>(me)], k, v))
                add_entry(&v, k.key_text(), k.k == Node::K::Str, k.k == Node::K::Int, j, me);
        }
    } else if (n->is_array()) {
        for (size_t j = 0; j < n->a.size(); ++j)
            if (!n->a[j].is_scalar() && !(record_names(ui.entries[static_cast<size_t>(me)]) == positional("SummonStones") &&
                                          element_label(ui.entries[static_cast<size_t>(me)], j) == L"Effects" && effect_slots(n->a[j])))
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
    // A list with nothing to change on its own page is left out: Parties > Party 1, not Parties > Party
    // sets > Party 1.
    for (Entry& e : ui.entries)
        e.hidden = e.dict == "PartySets" && e.node->is_array() && !e.kids.empty() && e.parent >= 0 &&
                   std::all_of(e.kids.begin(), e.kids.end(), [](int k) { return is_party(ui.entries[static_cast<size_t>(k)]); });
}

// --- tree -----------------------------------------------------------------------------------------

struct PickKind;
const PickKind* browse_kind(const Entry& e);

void insert_items(int parent, HTREEITEM where = nullptr) {
    const std::vector<int>& kids = parent < 0 ? ui.roots : ui.entries[static_cast<size_t>(parent)].kids;
    if (!where)
        where = parent < 0 ? TVI_ROOT : ui.entries[static_cast<size_t>(parent)].item;
    for (int k : kids) {
        Entry& e = ui.entries[static_cast<size_t>(k)];
        if (e.hidden) { // its entries, here
            insert_items(k, where);
            continue;
        }
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
        if (e.hidden) // built with the level above it
            continue;
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

bool can_undo();

void update_title() {
    std::wstring t = kTitle;
    if (!ui.path.empty())
        t = ui.path.filename().wstring() + (ui.dirty ? L" (changed)" : L"") + L" - " + t;
    SetWindowTextW(ui.wnd, t.c_str());
    EnableWindow(GetDlgItem(ui.wnd, kSave), ui.dirty);
    EnableWindow(GetDlgItem(ui.wnd, kUndo), can_undo());
}

void mark_changed() {
    ui.dirty = saveeditor::pack(ui.doc) != ui.original;
    update_title();
}

void set_status(const std::wstring& s) {
    SetWindowTextW(ui.status, s.c_str());
}

// The last page's next control, when it is of this kind: the same class, style and edge (a grid is never
// reused: its columns and rows are its own).
HWND reusable(const wchar_t* cls, DWORD style, DWORD ex) {
    if (ui.pool_next >= ui.pool.size() || lstrcmpiW(cls, WC_LISTVIEWW) == 0 || lstrcmpiW(cls, WC_LISTBOXW) == 0)
        return nullptr;
    Ui::Made& m = ui.pool[ui.pool_next];
    if (lstrcmpiW(m.cls.c_str(), cls) != 0 || (m.style & ~WS_DISABLED) != (style & ~WS_DISABLED) || m.ex != ex)
        return nullptr;
    ++ui.pool_next;
    HWND c = m.h;
    m.h = nullptr;
    return c;
}

// Controls start at a real size, not 10x10: an edit box sets its text margins from the font when it
// gets one (WM_SETFONT) and leaves none if it is too narrow then, so a new box would show its text
// 3 px further left than a reused one.
constexpr int kStartW = 200, kStartH = 24;

HWND panel_add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND c = reusable(cls, style, ex);
    if (c) { // as if new: its ID, text or emptied list, enabled state, no picture, last in the tab order
        SetWindowLongPtrW(c, GWLP_ID, id);
        SetWindowLongPtrW(c, GWLP_USERDATA, 0);
        if (lstrcmpiW(cls, WC_COMBOBOXW) == 0)
            SendMessageW(c, CB_RESETCONTENT, 0, 0);
        else
            SetWindowTextW(c, text);
        EnableWindow(c, !(style & WS_DISABLED));
        ShowWindow(c, SW_SHOWNA);
        SetWindowPos(c, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(c, nullptr, TRUE);
    } else {
        c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, px(kStartW), px(kStartH), ui.panel,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    }
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
    ui.made.push_back({c, cls, style, ex});
    return c;
}

void clear_panel() {
    for (const Ui::Made& m : ui.made)
        DestroyWindow(m.h);
    ui.made.clear();
    ui.fields.clear();
    ui.hints.clear();
    ui.read_only.clear();
    ui.ptop = 0;
}

// As clear_panel, but the controls go to the pool for the next page to reuse.
void clear_panel_keep() {
    for (const Ui::Made& m : ui.pool)
        if (m.h)
            DestroyWindow(m.h);
    ui.pool = std::move(ui.made);
    ui.pool_next = 0;
    ui.made.clear();
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
    case WM_SETCURSOR: // a picture to click (not the one beside a page's title)
        SetCursor(LoadCursorW(nullptr, GetDlgCtrlID(h) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_LBUTTONUP:
        if (GetDlgCtrlID(h))
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
    f.label = L"Showing:";
    f.name = panel_add(WC_STATICW, f.label.c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    add_pick_controls(f, kFieldBase + 8 * static_cast<int>(ui.fields.size()), current);
    ui.fields.push_back(std::move(f));
}

constexpr int64_t kSeriesRow = -2; // a song list's series heading (not a song: choosing it picks its first song)

// The names of a game enum's values, by value, for a save field that holds one (the game's own words,
// from its Enum texts; the values from its code).
const std::vector<const wchar_t*>* enum_names(const std::string& key) {
    static const std::map<std::string, std::vector<const wchar_t*>> names = {
        {"LastSelectedSortType", {L"Category", L"All", L"Favorites"}},
        {"LastCharacterSortType", {L"Series", L"Level", L"Type"}},       // Party Edit's (CharacterSortType)
        {"LastSummonStoneSortType", {L"Acquired", L"Summon", L"Rarity"}}, // (SummonStoneSortType)
        {"LastSelectedSortCategory", {L"Series", L"MS Type", L"Song Name", L"Difficulty", L"Cleared Rank", L"DLC", L"Popularity"}},
    };
    auto it = names.find(key);
    return it == names.end() ? nullptr : &it->second;
}

// Values chosen from drop-down lists of their names: a summon stone effect slot's effect (or empty) and
// rarity, a party's stone, a member's ability, a song, an enum's value.
void fill_combo(Field& f) {
    SendMessageW(f.ctrl, WM_SETREDRAW, FALSE, 0); // filled without drawing each item
    SendMessageW(f.ctrl, CB_RESETCONTENT, 0, 0);
    const int64_t effect = f.combo == 2 ? f.slot->a[0].i : kNone;
    auto item = [&](const std::wstring& text, int64_t v) {
        LRESULT k = SendMessageW(f.ctrl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(f.ctrl, CB_SETITEMDATA, static_cast<WPARAM>(k), static_cast<LPARAM>(v));
        if (v == f.node->i)
            SendMessageW(f.ctrl, CB_SETCURSEL, static_cast<WPARAM>(k), 0);
    };
    if (f.combo == 5) { // every song, under its series' heading, in the game's order
        auto order = [](int64_t id) {
            auto o = ui.names.sort.find(id);
            return o == ui.names.sort.end() ? id : o->second;
        };
        std::map<std::pair<int64_t, int64_t>, std::vector<std::pair<int64_t, int64_t>>> series; // (order, ID) -> (order, song)
        for (const auto& [song, s] : ui.names.music_series)
            if (!ui.names.get(song).empty())
                series[{order(s), s}].push_back({order(song), song});
        static const wchar_t* type[] = {L"FMS", L"BMS", L"EMS"}; // the game's names for its stage types
        for (auto& [s, songs] : series) {
            item(id_label(s.second), kSeriesRow);
            std::sort(songs.begin(), songs.end());
            std::map<std::wstring, int> count;
            for (const auto& [o, song] : songs)
                ++count[id_label(song)];
            for (const auto& [o, song] : songs) { // a name the series has twice: which stage type
                std::wstring name = id_label(song);
                auto t = ui.names.music_type.find(song);
                if (count[name] > 1 && t != ui.names.music_type.end() && t->second >= 0 && t->second < 3)
                    name += L" (" + std::wstring(type[t->second]) + L")";
                item(name, song);
            }
        }
        if (SendMessageW(f.ctrl, CB_GETCURSEL, 0, 0) == CB_ERR) // not a song of the game's list (or no game folder)
            item(id_label(f.node->i), f.node->i);
    } else if (f.combo == 6) {
        const auto& names = *enum_names(f.key);
        for (size_t i = 0; i < names.size(); ++i)
            item(names[i], static_cast<int64_t>(i));
        if (SendMessageW(f.ctrl, CB_GETCURSEL, 0, 0) == CB_ERR)
            item(L"Value " + std::to_wstring(f.node->i) + L" (unknown)", f.node->i);
    } else if (f.combo == 3) { // a party's summon stone: its place in the list of stones
        item(L"None", -1);
        const Node* info = ui.doc.find("SummonStoneInfo");
        const Node* stones = info ? info->find("SummonStones") : nullptr;
        if (stones && stones->is_array())
            for (size_t i = 0; i < stones->a.size(); ++i) {
                const Node& st = stones->a[i];
                std::wstring t = std::to_wstring(i + 1) + L". ";
                if (st.is_array() && !st.a.empty() && st.a[0].k == Node::K::Int)
                    t += id_label(st.a[0].i);
                if (st.is_array() && st.a.size() > 1 && st.a[1].is_array() && !st.a[1].a.empty() && st.a[1].a[0].is_array() &&
                    st.a[1].a[0].a.size() >= 2 && st.a[1].a[0].a[0].k == Node::K::Int && st.a[1].a[0].a[0].i != kNone)
                    t += L"  (" + widen(ui.names.effect(st.a[1].a[0].a[0].i, st.a[1].a[0].a[1].i)) + L"...)";
                item(t, static_cast<int64_t>(i));
            }
        if (SendMessageW(f.ctrl, CB_GETCURSEL, 0, 0) == CB_ERR) // a place past the list: shown as it is
            item(L"Stone " + std::to_wstring(f.node->i + 1) + L" (not in the list)", f.node->i);
        int64_t summon = 0; // the stone's summon, pictured beside the list (only a picture: the list picks)
        if (stones && stones->is_array() && f.node->i >= 0 && f.node->i < static_cast<int64_t>(stones->a.size()))
            if (const Node& st = stones->a[static_cast<size_t>(f.node->i)]; st.is_array() && !st.a.empty() && st.a[0].k == Node::K::Int)
                summon = st.a[0].i;
        f.pic = panel_add(L"theatrhythm_saveeditor_face", L"", 0, 0);
        SetWindowLongPtrW(f.pic, GWLP_USERDATA, static_cast<LONG_PTR>(summon));
    } else if (f.combo == 4) { // an ability: one of the character's, learned ones first
        item(L"None", kNone);
        const Node* info = ui.doc.find("CharacterInfo");
        const Node* chars = info ? info->find("Characters") : nullptr;
        const Node* data = nullptr;
        if (chars && chars->is_map())
            for (const auto& [k, v] : chars->m)
                if (k.k == Node::K::Int && k.i == f.who && v.is_map())
                    data = v.find("AbilityData");
        std::set<int64_t> others; // the list's other slots: an ability is set once
        for (const Node& x : f.slot->a)
            if (&x != f.node && x.k == Node::K::Int && x.i != kNone)
                others.insert(x.i);
        std::vector<std::pair<std::wstring, int64_t>> learned, not_yet;
        if (data && data->is_map())
            for (const auto& [k, v] : data->m)
                if (k.k == Node::K::Int && !others.count(k.i)) {
                    const Node* u = v.is_map() ? v.find("UnlockedFlag") : nullptr;
                    bool on = u && u->k == Node::K::Bool && u->b;
                    (on ? learned : not_yet).push_back({id_label(k.i), k.i});
                }
        std::sort(learned.begin(), learned.end());
        std::sort(not_yet.begin(), not_yet.end());
        for (const auto& [t, v] : learned)
            item(t, v);
        for (const auto& [t, v] : not_yet)
            item(t + L"  (not learned)", v);
        if (SendMessageW(f.ctrl, CB_GETCURSEL, 0, 0) == CB_ERR) // not one of this character's
            item(id_label(f.node->i) + L"  (not this character's)", f.node->i);
    } else if (f.combo == 1) {
        item(L"None", kNone);
        std::vector<int64_t> ids;
        for (const auto& [i, t] : ui.names.effect_text)
            ids.push_back(i);
        if (ids.empty())
            for (int64_t i = 290000000; i < 290000010; ++i)
                ids.push_back(i);
        if (f.node->i != kNone && std::find(ids.begin(), ids.end(), f.node->i) == ids.end())
            ids.push_back(f.node->i);
        std::sort(ids.begin(), ids.end());
        for (int64_t i : ids)
            item(id_label(i), i);
    } else if (effect == kNone) { // an empty slot has no rarity
        item(L"", f.node->i);
        EnableWindow(f.ctrl, FALSE);
    } else {
        int64_t levels = 3;
        if (auto v = ui.names.effect_values.find(effect); v != ui.names.effect_values.end())
            levels = static_cast<int64_t>(v->second.size());
        for (int64_t r = 0; r < std::max(levels, f.node->i + 1); ++r) {
            std::wstring t = std::to_wstring(r + 1);
            if (std::string e = ui.names.effect(effect, r); !e.empty())
                t += L"    " + widen(e);
            item(t, r);
        }
    }
    SendMessageW(f.ctrl, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(f.ctrl, nullptr, TRUE);
}

// The wheel over a closed list scrolls the page (it would change the list's value as the page went by).
LRESULT CALLBACK combo_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEWHEEL && !SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0))
        return SendMessageW(ui.panel, msg, wp, lp);
    return DefSubclassProc(h, msg, wp, lp);
}

void add_combo(Field& f, int id) {
    f.ctrl = panel_add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | (f.combo == 5 ? CBS_OWNERDRAWFIXED | CBS_HASSTRINGS : 0), id);
    SetWindowSubclass(f.ctrl, combo_proc, 1, 0);
    fill_combo(f);
    f.hint = panel_add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
}

// A stone's effect slots on its page: each one's effect and rarity lists.
void add_effect_rows(Node& effects) {
    for (size_t i = 0; i < effects.a.size(); ++i)
        for (int c = 1; c <= 2; ++c) {
            Field f;
            f.slot = &effects.a[i];
            f.node = &f.slot->a[static_cast<size_t>(c - 1)];
            f.combo = c;
            f.label = L"Effect " + std::to_wstring(i + 1) + (c == 2 ? L" rarity" : L"");
            f.name = panel_add(WC_STATICW, f.label.c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
            add_combo(f, kFieldBase + 8 * static_cast<int>(ui.fields.size()));
            ui.fields.push_back(std::move(f));
        }
}

void add_value(Node& n, const std::wstring& label, const std::string& key);

// A party's members on its page: each one's character (a picture to change) and abilities.
void add_member_rows(Node& members) {
    for (size_t i = 0; i < members.a.size(); ++i) {
        Node& m = members.a[i];
        add_value(*m.find("CharacterID"), L"Member " + std::to_wstring(i + 1), "CharacterID");
        ui.fields.back().heading = true;
        SendMessageW(ui.fields.back().name, WM_SETFONT, reinterpret_cast<WPARAM>(ui.bold), FALSE);
        Node& abilities = *m.find("Abilities");
        for (size_t j = 0; j < abilities.a.size(); ++j) {
            Field f;
            f.slot = &abilities;
            f.who = m.find("CharacterID")->i;
            f.node = &abilities.a[j];
            f.combo = 4;
            f.label = L"Member " + std::to_wstring(i + 1) + L" ability " + std::to_wstring(j + 1);
            f.name = panel_add(WC_STATICW, (L"Ability " + std::to_wstring(j + 1)).c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
            add_combo(f, kFieldBase + 8 * static_cast<int>(ui.fields.size()));
            ui.fields.push_back(std::move(f));
        }
    }
}

HWND add_heading(const wchar_t* text) {
    HWND h = panel_add(WC_STATICW, text, SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui.bold), FALSE);
    return h;
}

// A character's last set abilities, each chosen from a list of the character's abilities.
void add_last_set_rows(Node& list, int64_t who) {
    for (size_t j = 0; j < list.a.size(); ++j) {
        Field f;
        f.slot = &list;
        f.who = who;
        f.node = &list.a[j];
        f.combo = 4;
        f.label = L"Last set ability " + std::to_wstring(j + 1);
        if (j == 0)
            f.above = add_heading(L"Last set abilities");
        f.name = panel_add(WC_STATICW, (L"Ability " + std::to_wstring(j + 1)).c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
        add_combo(f, kFieldBase + 8 * static_cast<int>(ui.fields.size()));
        ui.fields.push_back(std::move(f));
    }
}

void add_value(Node& n, const std::wstring& label, const std::string& key) {
    Field f;
    f.node = &n;
    f.label = label;
    int id = kFieldBase + 8 * static_cast<int>(ui.fields.size());
    f.name = panel_add(WC_STATICW, label.c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    if (n.k == Node::K::Int && !n.u64 && key == "SummonStoneIndex") {
        f.combo = 3;
        add_combo(f, id);
        ui.fields.push_back(std::move(f));
        return;
    }
    if (n.k == Node::K::Int && !n.u64 && (key == "LastSelectedMusicID" || enum_names(key))) { // song select's song and sort
        f.combo = key == "LastSelectedMusicID" ? 5 : 6;
        f.key = key;
        add_combo(f, id);
        ui.fields.push_back(std::move(f));
        return;
    }
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
        if (v.k == Node::K::Int && (v.i > INT32_MAX || k.s.ends_with("ID") || k.s == "SummonStoneIndex" || (v.i >= 10000000 && *saveeditor::kind_of(v.i))))
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
        if (f.kind == Field::Kind::Value || f.kind == Field::Kind::List || f.kind == Field::Kind::Table)
            continue;
        int id = kFieldBase + 8 * static_cast<int>(j);
        f.name = panel_add(WC_STATICW, f.label.c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
        if (f.kind == Field::Kind::AllBool) {
            bool unlock = f.key == "UnlockedFlag";
            f.on = panel_add(WC_BUTTONW, unlock ? L"Unlock all" : L"All on", WS_TABSTOP | BS_PUSHBUTTON, id + 1);
            f.off = panel_add(WC_BUTTONW, unlock ? L"Lock all" : L"All off", WS_TABSTOP | BS_PUSHBUTTON, id + 2);
        } else {
            f.ctrl = panel_add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, id, WS_EX_CLIENTEDGE);
            SendMessageW(f.ctrl, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"value"));
            f.on = panel_add(WC_BUTTONW, L"Set on all", WS_TABSTOP | BS_PUSHBUTTON, id + 1);
        }
        f.hint = panel_add(WC_STATICW, all_hint(f).c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
        ui.hints.insert(f.hint);
    }
}

// The Parties and Stones pages' list: one owner-drawn list box, a row a party or stone (row_list_proc,
// draw_list_row). One set of Up, Down and Remove buttons, children of the list, sits on the row under
// the mouse, or on the selected row while the list has the keyboard.
struct RowList {
    struct Row {
        std::vector<int64_t> faces; // a party's members in slot order (kNone: an empty slot); a stone's summon
        std::wstring title, badge, lines[2];
        bool grey[2] = {false, false};
        bool current = false, beyond = false, divider = false; // divider: the first party after the game's 5
    };
    HWND list = nullptr, buttons[3] = {}; // Up, Down, Remove
    Node* node = nullptr;                 // the parties or the stones
    bool parties = false, fits = true;    // fits: the list is as tall as its rows (the page scrolls it)
    std::vector<Row> rows;
    int lh = 0;          // a line of text
    int hot = -1;        // the row under the mouse
    int at = -1;         // the row the buttons are on
    int down = -1;       // the row a click started on
    bool tracking = false;
    bool pin = false;    // the buttons stay on the selected row (after a key, or a move or removal) until
    int pin_item = -2;   // the mouse goes from the row it was on (-2: not known yet) to another
    struct {             // after a move or a removal the page is built again: the row to select, the scroll
        bool set = false;
        int sel = 0, top = 0;
    } restore;
};
RowList g_rows;
void place_row_buttons();

constexpr int kFlagW = 80; // a flag table's tick box column
constexpr int kRowH = 28, kLabelW = 230, kCtrlW = 170, kFace = 72;
HWND g_all_heading = nullptr, g_values_heading = nullptr;
HWND g_action = nullptr; // under the title: Add a stone... / Remove this stone
HWND g_face = nullptr;   // beside the title: the character's portrait
enum class Action { None, AddStone, RemoveStone, AddParty } g_action_kind = Action::None;

HDWP g_defer = nullptr; // while the panel is laid out: its controls move together

void place(HWND h, int x, int y, int w, int hgt) {
    if (!h)
        return;
    if (g_defer)
        g_defer = DeferWindowPos(g_defer, h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    else
        SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

int fit_columns(HWND lv, int inside, int w);

void layout_panel() {
    RECT rc;
    GetClientRect(ui.panel, &rc);
    // A party's page has short labels (Ability 1, Airship): its values sit closer to them.
    const bool grouped = std::any_of(ui.fields.begin(), ui.fields.end(), [](const Field& f) { return f.heading; });
    const int W = rc.right, m = px(14), rh = px(kRowH), lw = px(grouped ? 120 : kLabelW), cw = px(kCtrlW);
    g_defer = BeginDeferWindowPos(static_cast<int>(ui.fields.size()) * 5 + 8);
    int y = m - ui.ptop;
    bool face = IsWindowVisible(g_face) || (GetWindowLongW(g_face, GWL_STYLE) & WS_VISIBLE);
    place(g_face, W - m - px(kFace), y, px(kFace), px(kFace));
    place(ui.title, m, y, W - 2 * m - (face ? px(kFace) + px(10) : 0), px(26));
    y += px(28);
    place(ui.subtitle, m, y, W - 2 * m, px(18));
    y += px(30);
    if (g_action_kind == Action::RemoveStone) {
        place(g_action, m, y - px(4), px(150), px(28));
        y += px(36);
    }
    if (GetWindowLongW(ui.empty, GWL_STYLE) & WS_VISIBLE) { // its own flag: the panel may be hidden while redrawing is off
        place(ui.empty, m, y, W - 2 * m, px(36));
        y += px(42);
    }
    bool any_value = false, any_all = false;
    for (const Field& f : ui.fields)
        if (f.kind != Field::Kind::List)
            (f.kind == Field::Kind::Value || f.kind == Field::Kind::Table ? any_value : any_all) = true;
    ShowWindow(g_all_heading, any_all ? SW_SHOW : SW_HIDE);
    ShowWindow(g_values_heading, any_value && any_all ? SW_SHOW : SW_HIDE);
    // The Parties or Stones list: as tall as its rows, and the page scrolls it. A window can't be much
    // taller than 32767 pixels (its mouse positions are 16 bits), so hundreds of stones are shown in a
    // list as tall as the window, which scrolls on its own. Add a party / Add a stone... under it.
    for (const Field& f : ui.fields) {
        if (f.kind != Field::Kind::List)
            continue;
        const int limit = f.hi <= 30000 ? 30000 : std::max(px(260), static_cast<int>(rc.bottom) - px(170));
        g_rows.fits = f.hi <= limit;
        const int hgt = static_cast<int>(std::min<int64_t>(f.hi, limit));
        place(f.ctrl, m, y, W - 2 * m, hgt);
        y += hgt + px(10);
        place(g_action, m, y, px(150), px(28));
        place(f.hint, m + px(162), y + px(6), std::max(px(40), W - 2 * m - px(162)), px(18));
        y += px(44);
    }
    auto rows = [&](bool all) {
        bool first = true, in_member = false;
        for (const Field& f : ui.fields) {
            if (f.kind == Field::Kind::List || (f.kind != Field::Kind::Value && f.kind != Field::Kind::Table) != all)
                continue;
            if (f.above) { // "Abilities" on its own line, set apart from the rows above
                if (!first)
                    y += px(18);
                first = false;
                place(f.above, m, y, W - 2 * m, px(20));
                y += px(28);
            }
            if (f.kind == Field::Kind::Table) { // the page's width, every row's height: the panel scrolls it
                // Names take the rest of the width; sized before the grid is, so neither needs a scroll bar.
                const int inside = W - 2 * m - 2 * GetSystemMetrics(SM_CXBORDER);
                auto dragged = ui.flag_w.find(f.label); // the tick boxes' width, as last dragged
                const int boxes = std::max(1, Header_GetItemCount(ListView_GetHeader(f.ctrl)) - 1);
                fit_columns(f.ctrl, inside, inside - boxes * px(dragged == ui.flag_w.end() ? kFlagW : dragged->second));
                place(f.ctrl, m, y, W - 2 * m, static_cast<int>(f.hi));
                y += static_cast<int>(f.hi) + px(8);
                continue;
            }
            const bool member = f.heading || f.combo == 4;
            if (in_member && !member) // the party's own values, apart from its last member
                y += px(56);
            in_member = member;
            int x = m + (all ? std::min(lw, px(kLabelW)) : lw); // "All ..." rows: the usual width, their counts beside them
            if (f.pick) { // a picture: set apart from the rows above (a party's members), its name beside its middle
                const int face = px(kFace);
                if (!first)
                    y += f.heading ? px(34) : px(18);
                first = false;
                if (f.heading) { // "Member 1" on its own line, its rows under it
                    place(f.name, m, y, W - 2 * m, px(20));
                    y += px(28);
                } else {
                    place(f.name, m, y + (face - px(18)) / 2, lw - px(8), px(18));
                }
                place(f.ctrl, x, y, face, face);
                place(f.hint, x + face + px(12), y + px(10), std::max(px(40), W - m - x - face - px(12)), px(18));
                place(f.on, x + face + px(12), y + px(34), px(96), px(26));
                y += face + px(10);
                continue;
            }
            first = false;
            if (f.pic) { // a party's summon stone: its summon's picture, the list beside it
                const int face = px(kFace);
                place(f.name, m, y + (face - px(18)) / 2, lw - px(8), px(18));
                place(f.pic, x, y, face, face);
                place(f.ctrl, x + face + px(12), y + (face - px(23)) / 2, px(330), px(300));
                place(f.hint, x + face + px(352), y + px(4), std::max(px(40), W - m - x - face - px(352)), px(18));
                y += face + px(10);
                continue;
            }
            place(f.name, m, y + px(4), lw - px(8), px(18));
            if (f.combo) {
                place(f.ctrl, x, y, px(330), px(f.combo == 5 ? 480 : 300)); // the height is the dropped list's
                place(f.hint, x + px(340), y + px(4), std::max(px(40), W - m - x - px(340)), px(18));
                y += rh;
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
    if (g_rows.list) { // its rows are drawn to its width, and its buttons sit on a row
        InvalidateRect(g_rows.list, nullptr, TRUE);
        place_row_buttons();
    }
    ui.pheight = y + ui.ptop + m;
    if (int most = std::max(0, ui.pheight - static_cast<int>(rc.bottom)); ui.ptop > most) { // taller window: less to scroll
        ui.ptop = most;
        layout_panel();
        return;
    }
    SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, ui.pheight, static_cast<UINT>(rc.bottom), ui.ptop, 0};
    SetScrollInfo(ui.panel, SB_VERT, &si, TRUE);
    RECT now;
    GetClientRect(ui.panel, &now);
    if (now.right != rc.right) { // the scroll bar came or went: lay out to the new width
        layout_panel();
        return;
    }
    InvalidateRect(ui.panel, nullptr, TRUE);
}

std::wstring entry_path(int k) {
    std::wstring s;
    for (int p = ui.entries[static_cast<size_t>(k)].parent; p >= 0; p = ui.entries[static_cast<size_t>(p)].parent)
        if (!ui.entries[static_cast<size_t>(p)].hidden)
            s = ui.entries[static_cast<size_t>(p)].label + (s.empty() ? L"" : L"  >  " + s);
    return s;
}

constexpr size_t kGameParties = 5; // the game's Game.Define.PartySetMax: it shows and switches between 5

// The parties of the Parties page (its hidden Party sets list), or nullptr.
Node* party_list(const Entry& e) {
    for (int k : e.kids)
        if (const Entry& x = ui.entries[static_cast<size_t>(k)]; x.hidden && x.dict == "PartySets")
            return x.node;
    return nullptr;
}

// The party the game has picked: its place in the list.
Node* active_party() {
    Node* stage = ui.doc.find("MusicStageInfo");
    Node* n = stage ? stage->find("PartyNumber") : nullptr;
    return n && n->k == Node::K::Int ? n : nullptr;
}

constexpr int kDividerH = 30, kRowButtonW = 64, kRowButtonGap = 6, kRowPad = 10;

int row_face(bool parties) {
    return px(parties ? 40 : 48);
}

// A row's height: its pictures or its lines (a party: name, members, stone; a stone: name, effects),
// the taller, and the "Not selectable in the game" line above the first party after the game's 5.
int row_height(const RowList::Row& r) {
    const int lines = g_rows.parties ? 3 : 2;
    return std::max(row_face(g_rows.parties), lines * g_rows.lh) + 2 * px(kRowPad) + (r.divider ? px(kDividerH) : 0);
}

// The room a row keeps on its right for the buttons.
int row_buttons_w() {
    return 3 * px(kRowButtonW) + 2 * px(kRowButtonGap) + px(20);
}

int row_at(HWND list, int x, int y) {
    LRESULT r = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
    return HIWORD(r) ? -1 : static_cast<int>(LOWORD(r));
}

void invalidate_row(int k) {
    RECT r;
    if (g_rows.list && k >= 0 && SendMessageW(g_rows.list, LB_GETITEMRECT, static_cast<WPARAM>(k), reinterpret_cast<LPARAM>(&r)) != LB_ERR)
        InvalidateRect(g_rows.list, &r, TRUE);
}

void set_hot(int k) {
    if (k == g_rows.hot)
        return;
    invalidate_row(g_rows.hot);
    g_rows.hot = k;
    invalidate_row(k);
    place_row_buttons();
}

bool list_has_keyboard() {
    HWND f = GetFocus();
    return f && g_rows.list && (f == g_rows.list || GetParent(f) == g_rows.list);
}

// The buttons on the row under the mouse, or the selected one while the list has the keyboard (or after
// a key, a move or a removal: pinned); Up and Down greyed at the ends, Remove at the game's 5 parties.
void place_row_buttons() {
    if (!g_rows.list)
        return;
    const int sel = static_cast<int>(SendMessageW(g_rows.list, LB_GETCURSEL, 0, 0));
    int k = g_rows.pin && sel >= 0 ? sel : g_rows.hot >= 0 ? g_rows.hot : list_has_keyboard() ? sel : -1;
    RECT r, client;
    GetClientRect(g_rows.list, &client);
    if (k < 0 || k >= static_cast<int>(g_rows.rows.size()) ||
        SendMessageW(g_rows.list, LB_GETITEMRECT, static_cast<WPARAM>(k), reinterpret_cast<LPARAM>(&r)) == LB_ERR || r.bottom <= client.top ||
        r.top >= client.bottom) {
        for (HWND b : g_rows.buttons)
            ShowWindow(b, SW_HIDE);
        invalidate_row(std::exchange(g_rows.at, -1)); // its text has the row's width again
        return;
    }
    if (g_rows.rows[static_cast<size_t>(k)].divider)
        r.top += px(kDividerH);
    const int bw = px(kRowButtonW), bh = px(26), gap = px(kRowButtonGap), n = static_cast<int>(g_rows.rows.size());
    const int x = r.right - px(12) - 3 * bw - 2 * gap, y = (r.top + r.bottom - bh) / 2;
    const bool enable[3] = {k > 0, k + 1 < n, !g_rows.parties || n > static_cast<int>(kGameParties)};
    for (int b = 0; b < 3; ++b) {
        EnableWindow(g_rows.buttons[b], enable[b]);
        SetWindowPos(g_rows.buttons[b], HWND_TOP, x + b * (bw + gap), y, bw, bh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (k != g_rows.at) { // the text of both rows is cut at a new place
        invalidate_row(g_rows.at);
        invalidate_row(k);
    }
    g_rows.at = k;
}

void post_row_action(int what, int k) {
    if (k >= 0)
        PostMessageW(ui.panel, kMsgRowAction, static_cast<WPARAM>(what), static_cast<LPARAM>(k));
}

// The list's mouse and keys: a click on a row (or Enter) opens it; Alt+Up, Alt+Down and Delete move
// and remove the selected row; the wheel scrolls the page unless the list scrolls on its own.
LRESULT CALLBACK row_list_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    constexpr UINT_PTR kLeaveTimer = 0x7a51;
    auto selected = [&] { return static_cast<int>(DefSubclassProc(h, LB_GETCURSEL, 0, 0)); }; // (a message to h would come back here)
    switch (msg) {
    case WM_GETDLGCODE: { // Enter opens the row (not the window's default button)
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (auto* m = reinterpret_cast<MSG*>(lp); m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN)
            r |= DLGC_WANTALLKEYS;
        return r;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_DELETE) {
            post_row_action(wp == VK_RETURN ? 1 : 4, selected());
            return 0;
        }
        g_rows.pin = true; // the arrows move the selection: the buttons go with it, not with the mouse
        g_rows.pin_item = -2;
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_UP || wp == VK_DOWN) {
            post_row_action(wp == VK_UP ? 2 : 3, selected());
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!g_rows.tracking) {
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
            g_rows.tracking = TrackMouseEvent(&t);
        }
        const int k = row_at(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (g_rows.pin && g_rows.pin_item == -2)
            g_rows.pin_item = k;
        else if (g_rows.pin && k != g_rows.pin_item) {
            g_rows.pin = false;
            invalidate_row(selected());
        }
        set_hot(k);
        break;
    }
    case WM_MOUSELEAVE: {
        g_rows.tracking = false;
        POINT pt;
        RECT wr;
        GetCursorPos(&pt);
        GetWindowRect(h, &wr);
        if (PtInRect(&wr, pt)) // onto a row's button: still that row, until the mouse leaves the list
            SetTimer(h, kLeaveTimer, 150, nullptr);
        else
            set_hot(-1);
        return 0;
    }
    case WM_TIMER:
        if (wp == kLeaveTimer) {
            POINT pt;
            RECT wr;
            GetCursorPos(&pt);
            GetWindowRect(h, &wr);
            if (!PtInRect(&wr, pt)) {
                KillTimer(h, kLeaveTimer);
                set_hot(-1);
            }
            return 0;
        }
        break;
    case WM_SETCURSOR: // a row is a link to its page
        if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == h) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (row_at(h, pt.x, pt.y) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        g_rows.down = row_at(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_LBUTTONUP: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        const int k = row_at(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (k >= 0 && k == g_rows.down)
            post_row_action(1, k);
        g_rows.down = -1;
        return r;
    }
    case WM_LBUTTONDBLCLK: // the first click opened it
        return 0;
    case WM_MOUSEWHEEL:
        if (g_rows.fits)
            return SendMessageW(ui.panel, msg, wp, lp);
        [[fallthrough]];
    case WM_VSCROLL: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        place_row_buttons();
        return r;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (msg == WM_KILLFOCUS && !(wp && GetParent(reinterpret_cast<HWND>(wp)) == h))
            g_rows.pin = false; // gone elsewhere than its buttons
        invalidate_row(selected());
        place_row_buttons();
        return r;
    }
    case WM_COMMAND: // its buttons
        for (int b = 0; b < 3; ++b)
            if (HIWORD(wp) == BN_CLICKED && reinterpret_cast<HWND>(lp) == g_rows.buttons[b]) {
                post_row_action(2 + b, g_rows.at);
                return 0;
            }
        break;
    case WM_NCDESTROY:
        KillTimer(h, kLeaveTimer);
        RemoveWindowSubclass(h, row_list_proc, 1);
        if (g_rows.list == h)
            g_rows.list = nullptr;
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// One row: its pictures, its name and badge, its lines; the theme's hover and selection, and the focus
// rectangle. High contrast: the system's highlight colours instead of the theme.
void draw_list_row(const DRAWITEMSTRUCT& di) {
    if (di.itemID >= g_rows.rows.size())
        return;
    const RowList::Row& row = g_rows.rows[di.itemID];
    HDC dc = di.hDC;
    RECT r = di.rcItem;
    FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ was = SelectObject(dc, ui.font);
    const int line = std::max(1, px(1));
    if (row.divider) { // "Not selectable in the game" and a rule, above the first party after the 5th
        const wchar_t* t = L"Not selectable in the game";
        SIZE sz;
        GetTextExtentPoint32W(dc, t, lstrlenW(t), &sz);
        const int mid = r.top + px(kDividerH) / 2, x = r.left + px(14);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        TextOutW(dc, x, mid - sz.cy / 2, t, lstrlenW(t));
        RECT rule = {x + sz.cx + px(10), mid, r.right - px(12), mid + line};
        FillRect(dc, &rule, GetSysColorBrush(COLOR_GRAYTEXT));
        r.top += px(kDividerH);
    }
    HIGHCONTRAST hc = {sizeof(hc), 0, nullptr};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
    const bool contrast = hc.dwFlags & HCF_HIGHCONTRASTON;
    const bool focus = GetFocus() == di.hwndItem;
    const bool selected = (di.itemState & ODS_SELECTED) && (focus || g_rows.pin);
    const bool hot = static_cast<int>(di.itemID) == g_rows.hot;
    RECT box = {r.left + px(2), r.top + px(1), r.right - px(2), r.bottom - px(1)};
    COLORREF text = GetSysColor(COLOR_WINDOWTEXT), grey = GetSysColor(COLOR_GRAYTEXT);
    bool inverse = false; // high contrast, selected: the highlight's colours
    if (selected || hot) {
        HTHEME theme = contrast ? nullptr : OpenThemeData(di.hwndItem, L"ListView");
        if (theme) {
            DrawThemeBackground(theme, dc, LVP_LISTITEM, selected && hot ? LISS_HOTSELECTED : selected ? LISS_SELECTED : LISS_HOT, &box, nullptr);
            CloseThemeData(theme);
        } else if (selected) {
            FillRect(dc, &box, GetSysColorBrush(COLOR_HIGHLIGHT));
            text = grey = GetSysColor(COLOR_HIGHLIGHTTEXT);
            inverse = true;
        } else {
            FrameRect(dc, &box, GetSysColorBrush(COLOR_HOTLIGHT));
        }
    }
    if (row.current) { // the game's current party: a bar down its left edge
        RECT bar = {r.left + px(3), r.top + px(6), r.left + px(7), r.bottom - px(6)};
        FillRect(dc, &bar, GetSysColorBrush(inverse ? COLOR_HIGHLIGHTTEXT : COLOR_HIGHLIGHT));
    }
    // The pictures, in slot order; an empty slot is a dotted outline.
    const int face = row_face(g_rows.parties), cy = (r.top + r.bottom) / 2;
    int x = r.left + px(16);
    for (int64_t id : row.faces) {
        RECT f = {x, cy - face / 2, x + face, cy - face / 2 + face};
        if (id == kNone) {
            HPEN pen = CreatePen(PS_DOT, 1, grey);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, f.left, f.top, f.right, f.bottom);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
        } else {
            FillRect(dc, &f, GetSysColorBrush(COLOR_WINDOW));
            draw_face(dc, id, f, false, false);
        }
        x += face + px(4);
    }
    x += px(12);
    // The name (and its badge), then its lines, centred on the row (left of the buttons, on their row).
    const int right = std::max(x + px(40), static_cast<int>(r.right) - (static_cast<int>(di.itemID) == g_rows.at ? row_buttons_w() : px(12)));
    const int count = 1 + (row.lines[0].empty() ? 0 : 1) + (row.lines[1].empty() ? 0 : 1);
    int y = cy - count * g_rows.lh / 2;
    SelectObject(dc, ui.bold);
    SetTextColor(dc, row.beyond ? grey : text);
    RECT t = {x, y, right, y + g_rows.lh};
    DrawTextW(dc, row.title.c_str(), -1, &t, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT tw = t;
    DrawTextW(dc, row.title.c_str(), -1, &tw, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    if (!row.badge.empty()) { // Current (the highlight's colours) or Locked (an outline)
        SelectObject(dc, ui.font);
        SIZE sz;
        GetTextExtentPoint32W(dc, row.badge.c_str(), static_cast<int>(row.badge.size()), &sz);
        RECT b = {std::min(static_cast<int>(tw.right), right) + px(8), y + (g_rows.lh - sz.cy - px(4)) / 2, 0, 0};
        b.right = b.left + sz.cx + px(14);
        b.bottom = b.top + sz.cy + px(4);
        const bool strong = row.current;
        COLORREF fill = strong ? GetSysColor(inverse ? COLOR_HIGHLIGHTTEXT : COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
        COLORREF edge = strong ? fill : grey;
        HBRUSH br = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, line, edge);
        HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
        RoundRect(dc, b.left, b.top, b.right, b.bottom, px(8), px(8));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pen);
        SetTextColor(dc, strong ? GetSysColor(inverse ? COLOR_HIGHLIGHT : COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT));
        DrawTextW(dc, row.badge.c_str(), -1, &b, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(dc, ui.font);
    for (int j = 0; j < 2; ++j)
        if (!row.lines[j].empty()) {
            y += g_rows.lh;
            SetTextColor(dc, row.grey[j] ? grey : text);
            RECT l = {x, y, right, y + g_rows.lh};
            DrawTextW(dc, row.lines[j].c_str(), -1, &l, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    if (focus && (di.itemState & ODS_FOCUS) && !(di.itemState & ODS_NOFOCUSRECT))
        DrawFocusRect(dc, &box);
    SelectObject(dc, was);
}

// The Parties or Stones list on its page. Each row's text (what a screen reader says) is the whole row.
void add_row_list(bool parties, Node& list) {
    g_rows.parties = parties;
    g_rows.node = &list;
    g_rows.rows.clear();
    g_rows.hot = g_rows.at = g_rows.down = -1;
    g_rows.pin = g_rows.tracking = false;
    std::vector<std::wstring> said;
    const Node* info = ui.doc.find("SummonStoneInfo");
    const Node* stones = info ? info->find("SummonStones") : nullptr;
    const Node* active = parties ? active_party() : nullptr;
    for (size_t i = 0; i < list.a.size(); ++i) {
        const Node& n = list.a[i];
        RowList::Row row;
        std::wstring speech;
        if (parties) {
            std::wstring members;
            if (const Node* chars = n.find("Characters"); chars && chars->is_array())
                for (const Node& m : chars->a) {
                    const Node* c = m.find("CharacterID");
                    const int64_t id = c && c->k == Node::K::Int ? c->i : kNone;
                    row.faces.push_back(id);
                    if (id != kNone)
                        members += (members.empty() ? L"" : L", ") + id_label(id);
                }
            row.title = L"Party " + std::to_wstring(i + 1);
            row.current = active && active->i == static_cast<int64_t>(i);
            row.badge = row.current ? L"Current" : L"";
            row.beyond = i >= kGameParties;
            row.divider = i == kGameParties;
            std::wstring stone;
            if (const Node* st = n.find("SummonStoneIndex"); st && st->k == Node::K::Int && stones && stones->is_array() && st->i >= 0 &&
                st->i < static_cast<int64_t>(stones->a.size()) && stones->a[static_cast<size_t>(st->i)].is_array() &&
                !stones->a[static_cast<size_t>(st->i)].a.empty())
                stone = L"Summon stone " + std::to_wstring(st->i + 1) + L". " + id_label(stones->a[static_cast<size_t>(st->i)].a[0].i);
            if (members.empty() && stone.empty()) {
                row.lines[0] = L"Empty: open it to choose members";
                row.grey[0] = true;
            } else {
                row.lines[0] = members.empty() ? L"No members" : members;
                row.grey[0] = members.empty();
                row.lines[1] = stone.empty() ? L"No summon stone" : stone;
                row.grey[1] = stone.empty();
            }
            speech = row.title + (row.current ? L", current party" : L"") + (row.beyond ? L", not selectable in the game" : L"") + L". " +
                     row.lines[0] + (row.lines[1].empty() ? L"" : L". " + row.lines[1]);
        } else {
            const int64_t summon = n.is_array() && !n.a.empty() && n.a[0].k == Node::K::Int ? n.a[0].i : kNone;
            row.faces.push_back(summon);
            row.title = std::to_wstring(i + 1) + L". " + (summon == kNone ? L"?" : id_label(summon));
            std::wstring fx;
            if (n.is_array() && n.a.size() > 1 && n.a[1].is_array())
                for (const Node& slot : n.a[1].a)
                    if (slot.is_array() && slot.a.size() >= 2 && slot.a[0].k == Node::K::Int && slot.a[0].i != kNone)
                        fx += (fx.empty() ? L"" : L", ") + widen(ui.names.effect(slot.a[0].i, slot.a[1].k == Node::K::Int ? slot.a[1].i : 0));
            row.lines[0] = fx.empty() ? L"No effects" : fx;
            row.grey[0] = fx.empty();
            const bool locked = n.is_array() && n.a.size() > 7 && n.a[7].k == Node::K::Bool && n.a[7].b;
            row.badge = locked ? L"Locked" : L"";
            speech = row.title + (locked ? L", locked" : L"") + L". " + row.lines[0];
        }
        g_rows.rows.push_back(std::move(row));
        said.push_back(std::move(speech));
    }
    HDC dc = GetDC(ui.panel);
    HGDIOBJ was = SelectObject(dc, ui.font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, was);
    ReleaseDC(ui.panel, dc);
    g_rows.lh = tm.tmHeight + px(3);

    Field f;
    f.kind = Field::Kind::List;
    f.slot = &list;
    const int id = kFieldBase + 8 * static_cast<int>(ui.fields.size());
    f.ctrl = panel_add(WC_LISTBOXW, L"",
                       WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_CLIPCHILDREN | LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                       id);
    g_rows.list = f.ctrl;
    SetWindowTheme(f.ctrl, L"Explorer", nullptr);
    SetWindowSubclass(f.ctrl, row_list_proc, 1, 0);
    static const wchar_t* names[] = {L"Up", L"Down", L"Remove"};
    for (int b = 0; b < 3; ++b) {
        g_rows.buttons[b] = CreateWindowExW(0, WC_BUTTONW, names[b], WS_CHILD | BS_PUSHBUTTON, 0, 0, 10, 10, f.ctrl,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id + 2 + b)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_rows.buttons[b], WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
    }
    SendMessageW(f.ctrl, WM_SETREDRAW, FALSE, 0);
    int total = 0;
    for (size_t i = 0; i < said.size(); ++i) { // each row is measured as it is added (WM_MEASUREITEM)
        SendMessageW(f.ctrl, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(said[i].c_str()));
        total += static_cast<int>(SendMessageW(f.ctrl, LB_GETITEMHEIGHT, i, 0));
    }
    SendMessageW(f.ctrl, WM_SETREDRAW, TRUE, 0);
    f.hi = total + 2 * GetSystemMetrics(SM_CYBORDER);
    const wchar_t* why = parties && list.a.size() <= kGameParties ? L"Remove is available when there are more than 5 parties." : L"";
    f.hint = panel_add(WC_STATICW, why, SS_LEFT | SS_NOPREFIX, 0);
    ui.hints.insert(f.hint);
    if (g_rows.restore.set && !list.a.empty()) { // after a move or removal: the same row, selected
        SendMessageW(f.ctrl, LB_SETTOPINDEX, static_cast<WPARAM>(g_rows.restore.top), 0);
        SendMessageW(f.ctrl, LB_SETCURSEL, static_cast<WPARAM>(std::clamp(g_rows.restore.sel, 0, static_cast<int>(list.a.size()) - 1)), 0);
        g_rows.pin = true;
        g_rows.pin_item = -2;
        SetFocus(f.ctrl);
    }
    ui.fields.push_back(std::move(f));
}

// The grid's header row, drawn here so it reads as a header and not a first row: the face colour,
// the section headings' font, one thin rule under it (the outline's colour) and between its names.
// Drawn in full, it shows no hover or pressed look (its columns are not buttons).
LRESULT draw_table_header(NMCUSTOMDRAW& cd) {
    if (cd.dwDrawStage == CDDS_PREPAINT) {
        RECT all;
        GetClientRect(cd.hdr.hwndFrom, &all);
        FillRect(cd.hdc, &all, GetSysColorBrush(COLOR_BTNFACE));
        return CDRF_NOTIFYITEMDRAW;
    }
    if (cd.dwDrawStage != CDDS_ITEMPREPAINT)
        return CDRF_DODEFAULT;
    RECT r = cd.rc;
    FillRect(cd.hdc, &r, GetSysColorBrush(COLOR_BTNFACE));
    RECT under = {r.left, r.bottom - 1, r.right, r.bottom};
    FillRect(cd.hdc, &under, GetSysColorBrush(COLOR_WINDOWFRAME));
    if (static_cast<int>(cd.dwItemSpec) + 1 < Header_GetItemCount(cd.hdr.hwndFrom)) { // the last one's edge is the outline
        RECT between = {r.right - 1, r.top, r.right, r.bottom};
        FillRect(cd.hdc, &between, GetSysColorBrush(COLOR_WINDOWFRAME));
    }
    wchar_t text[64] = {};
    HDITEMW item = {};
    item.mask = HDI_TEXT | HDI_FORMAT;
    item.pszText = text;
    item.cchTextMax = 64;
    Header_GetItem(cd.hdr.hwndFrom, static_cast<int>(cd.dwItemSpec), &item);
    InflateRect(&r, -px(6), 0);
    r.bottom -= 1;
    HGDIOBJ was = SelectObject(cd.hdc, ui.bold);
    SetBkMode(cd.hdc, TRANSPARENT);
    SetTextColor(cd.hdc, GetSysColor(COLOR_WINDOWTEXT));
    DrawTextW(cd.hdc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | ((item.fmt & HDF_CENTER) ? DT_CENTER : DT_LEFT));
    SelectObject(cd.hdc, was);
    return CDRF_SKIPDEFAULT;
}

// The grid's own messages: its wheel scrolls the page (the grid is as tall as its rows: it has no
// scrolling of its own), and its header is drawn by draw_table_header.
bool g_fitting = false; // a table's columns are being sized by the page, not dragged

// A table's columns fill its width: the names' column as wide as w (within reason), the tick box
// columns share the rest. Returns a box column's width.
int fit_columns(HWND lv, int inside, int w) {
    const int boxes = std::max(1, Header_GetItemCount(ListView_GetHeader(lv)) - 1);
    const int names = std::clamp(w, std::min(px(100), inside), std::max(px(100), inside - boxes * px(48)));
    const int each = std::max(0, inside - names) / boxes;
    g_fitting = true;
    ListView_SetColumnWidth(lv, 0, names);
    for (int c = 1; c <= boxes; ++c) // the last takes what dividing left over
        ListView_SetColumnWidth(lv, c, c < boxes ? each : std::max(0, inside - names - (boxes - 1) * each));
    g_fitting = false;
    return each;
}

// ref: the table's FlagTable. Dragging the line between its columns sizes both (the last column's right
// edge is the table's own, not dragged); the page remembers the width for every table of its kind.
LRESULT CALLBACK table_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (msg == WM_MOUSEWHEEL)
        return SendMessageW(ui.panel, msg, wp, lp);
    if (auto* hd = reinterpret_cast<NMHEADERW*>(lp); msg == WM_NOTIFY && hd->hdr.hwndFrom == ListView_GetHeader(h)) {
        const UINT code = hd->hdr.code;
        if (code == NM_CUSTOMDRAW)
            return draw_table_header(*reinterpret_cast<NMCUSTOMDRAW*>(lp));
        if ((code == HDN_BEGINTRACKW || code == HDN_BEGINTRACKA || code == HDN_DIVIDERDBLCLICKW || code == HDN_DIVIDERDBLCLICKA) && hd->iItem != 0)
            return TRUE;
        if ((code == HDN_ITEMCHANGEDW || code == HDN_ITEMCHANGEDA) && hd->iItem == 0 && !g_fitting) {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            RECT rc; // its outer width, as the page sizes it (a scroll bar it shows while dragged is gone after)
            GetWindowRect(h, &rc);
            int flags = fit_columns(h, rc.right - rc.left - 2 * GetSystemMetrics(SM_CXBORDER), ListView_GetColumnWidth(h, 0));
            if (const auto* t = reinterpret_cast<const FlagTable*>(ref))
                ui.flag_w[t->heading] = MulDiv(flags, 96, static_cast<int>(ui.dpi));
            InvalidateRect(h, nullptr, FALSE);
            return r;
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// A flag table as a grid: an entry's name and its tick box, one row an entry (names shared by several
// are numbered, as in the tree). It looks like the tree beside it (a thin themed border, Explorer's
// rows); its header is draw_table_header's, and the box is drawn here, centred in its column (a list
// view's own tick boxes sit at a cell's left edge).
void add_flag_table(Node& list, const FlagTable& t) {
    Field f;
    f.kind = Field::Kind::Table;
    f.label = t.heading;
    f.ctrl = panel_add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                       kFieldBase + 8 * static_cast<int>(ui.fields.size()));
    SetWindowTheme(f.ctrl, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(f.ctrl, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(f.ctrl, table_proc, 1, reinterpret_cast<DWORD_PTR>(&t));
    SetWindowLongPtrW(f.ctrl, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&t)); // which flags its columns are
    const wchar_t* heads[] = {t.item, t.column, t.column2};
    for (int c = 0; c <= t.flags(); ++c) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = c >= 1 ? LVCFMT_CENTER : LVCFMT_LEFT;
        col.cx = c >= 1 ? px(kFlagW) : px(300);
        col.pszText = const_cast<wchar_t*>(heads[c]);
        ListView_InsertColumn(f.ctrl, c, &col);
    }
    std::map<std::wstring, int> count, seen;
    for (auto& [k, v] : list.m)
        ++count[id_label(k.i)];
    int row = 0;
    for (auto& [k, v] : list.m) {
        std::wstring name = id_label(k.i);
        if (count[name] > 1)
            name += L" (" + std::to_wstring(++seen[name]) + L")";
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row++;
        it.pszText = name.data();
        it.lParam = reinterpret_cast<LPARAM>(&v); // the entry: its flags by column
        ListView_InsertItem(f.ctrl, &it);
    }
    SetWindowPos(f.ctrl, nullptr, 0, 0, px(600), px(400), SWP_NOZORDER | SWP_NOACTIVATE); // a real size: its header lays itself out
    RECT r = {}, hr = {};
    ListView_GetItemRect(f.ctrl, 0, &r, LVIR_BOUNDS);
    GetWindowRect(ListView_GetHeader(f.ctrl), &hr);
    f.hi = (hr.bottom - hr.top) + row * (r.bottom - r.top) + 2 * GetSystemMetrics(SM_CYBORDER); // its height: the header and every row
    ui.fields.push_back(std::move(f));
}

void refresh_hints();

// The flag a table's row shows in box column `col` (1 or 2).
Node* table_value(HWND lv, int item, int col) {
    LVITEMW it = {};
    it.mask = LVIF_PARAM;
    it.iItem = item;
    const auto* t = reinterpret_cast<const FlagTable*>(GetWindowLongPtrW(lv, GWLP_USERDATA));
    if (item < 0 || !t || col < 1 || col > t->flags() || !ListView_GetItem(lv, &it))
        return nullptr;
    Node* flag = reinterpret_cast<Node*>(it.lParam)->find(t->flag_of(col));
    return flag && flag->k == Node::K::Bool ? flag : nullptr;
}

// Ticks or unticks a flag table's entry.
void table_toggle(HWND lv, int item, int col = 1) {
    Node* owned = table_value(lv, item, col);
    if (!owned || owned->k != Node::K::Bool)
        return;
    owned->b = !owned->b;
    ListView_RedrawItems(lv, item, item);
    for (Field& f : ui.fields) // a character's last set abilities: learned ones first
        if (f.combo == 4)
            fill_combo(f);
    mark_changed();
    refresh_hints(); // "4 of 28 on" above
}

// The grid's notifications: its boxes drawn, a click on one or Space ticks it.
LRESULT on_table_notify(NMHDR* hdr) {
    HWND lv = hdr->hwndFrom;
    switch (hdr->code) {
    case NM_CUSTOMDRAW: {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
            return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
            return CDRF_NOTIFYPOSTPAINT;
        for (int c = 1; cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT && c <= 2; ++c) {
            const Node* owned = table_value(lv, static_cast<int>(cd->nmcd.dwItemSpec), c);
            RECT cell;
            if (!owned || !ListView_GetSubItemRect(lv, static_cast<int>(cd->nmcd.dwItemSpec), c, LVIR_BOUNDS, &cell))
                continue;
            HDC dc = cd->nmcd.hdc;
            SIZE box = {px(13), px(13)};
            HTHEME theme = OpenThemeData(lv, L"BUTTON");
            int state = owned->b ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;
            if (theme)
                GetThemePartSize(theme, dc, BP_CHECKBOX, state, nullptr, TS_DRAW, &box);
            RECT r = {(cell.left + cell.right - box.cx) / 2, (cell.top + cell.bottom - box.cy) / 2, 0, 0};
            r.right = r.left + box.cx;
            r.bottom = r.top + box.cy;
            if (theme) {
                DrawThemeBackground(theme, dc, BP_CHECKBOX, state, &r, nullptr);
                CloseThemeData(theme);
            } else {
                DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONCHECK | (owned->b ? DFCS_CHECKED : 0));
            }
        }
        return CDRF_DODEFAULT;
    }
    case NM_CLICK: { // on the tick box column
        auto* ia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
        LVHITTESTINFO hit = {};
        hit.pt = ia->ptAction;
        if (ListView_SubItemHitTest(lv, &hit) >= 0 && hit.iSubItem >= 1)
            table_toggle(lv, hit.iItem, hit.iSubItem);
        return 0;
    }
    case LVN_KEYDOWN:
        if (reinterpret_cast<NMLVKEYDOWN*>(hdr)->wVKey == VK_SPACE)
            table_toggle(lv, ListView_GetNextItem(lv, -1, LVNI_SELECTED));
        return 0;
    }
    return 0;
}

void show_entry() {
    SendMessageW(ui.panel, WM_SETREDRAW, FALSE, 0);
    // The last page's controls are kept for panel_add to reuse; its own values are not committed while
    // they are rewritten (EN_CHANGE).
    clear_panel_keep();
    const bool populating = ui.populating;
    ui.populating = true;
    g_action_kind = Action::None;
    ShowWindow(g_face, SW_HIDE);
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
        SetWindowTextW(ui.subtitle, sub.c_str());
        // A character's page shows their portrait.
        int64_t own = e.key_is_int ? std::atoll(e.key.c_str()) : 0;
        SetWindowLongPtrW(g_face, GWLP_USERDATA, static_cast<LONG_PTR>(own));
        ShowWindow(g_face, own >= 10000000 && ui.names.faces.count(own) ? SW_SHOW : SW_HIDE);
        InvalidateRect(g_face, nullptr, FALSE);
        SetWindowTextW(g_all_heading, (L"All " + e.label).c_str());
        // Summon stones are the ones you have: they can be added and removed.
        bool stone = e.parent >= 0 && ui.entries[static_cast<size_t>(e.parent)].dict == "SummonStones" && e.node->is_array();
        Node* parties = party_list(e);
        g_action_kind = e.dict == "SummonStones" && e.node->is_array() ? Action::AddStone
                        : stone                                        ? Action::RemoveStone
                        : parties                                      ? Action::AddParty
                                                                       : Action::None;
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
        } else if (const FlagTable* t = flag_table(e)) { // Theater, illustrations: every entry, a name and its box
            SetWindowTextW(g_values_heading, t->heading);
            add_flag_table(*e.node, *t);
        } else if (e.node->is_map()) {
            for (auto& [k, v] : e.node->m)
                if (is_party(e) && k.k == Node::K::Str && k.s == "Characters" && party_members(v))
                    add_member_rows(v);
                else if (v.is_scalar() && !(k.k == Node::K::Str && (k.s == "CursorSelectedFlag" || k.s.ends_with("AccountID"))) &&
                    ++scalars <= kMaxFields) {
                    std::wstring label = k.k == Node::K::Str ? friendly(k.s) : id_label(k.k == Node::K::Int ? k.i : 0);
                    add_value(v, label, k.k == Node::K::Str ? k.s : "id");
                    ++shown;
                }
            for (auto& [k, v] : e.node->m) // a character's: under their values
                if (k.k == Node::K::Str && k.s == "LastSetAbilities" && character_list(e, k, v))
                    add_last_set_rows(v, own);
            for (auto& [k, v] : e.node->m)
                if (k.k == Node::K::Str && k.s == "AbilityData" && character_list(e, k, v)) {
                    add_flag_table(v, kAbilities);
                    ui.fields.back().above = add_heading(kAbilities.heading);
                }
        } else {
            for (size_t j = 0; j < e.node->a.size(); ++j)
                if (record_names(e) == positional("SummonStones") && element_label(e, j) == L"Effects" && effect_slots(e.node->a[j]))
                    add_effect_rows(e.node->a[j]);
                else if (e.node->a[j].is_scalar() && element_label(e, j) != L"Cursor selected" && !element_label(e, j).ends_with(L"account ID") &&
                    ++scalars <= kMaxFields) {
                    add_value(e.node->a[j], element_label(e, j), record_names(e) ? narrow(element_label(e, j)) : "");
                    ++shown;
                }
        }
        if (g_action_kind == Action::AddStone)
            add_row_list(false, *e.node);
        if (parties) {
            for (Field& f : ui.fields) // the page's own values, under the parties
                if (f.kind == Field::Kind::Value) {
                    f.above = add_heading(L"Party Edit");
                    break;
                }
            add_row_list(true, *parties);
        }
        std::wstring note;
        if (g_action_kind == Action::AddStone)
            note = std::to_wstring(e.node->a.size()) + L" stones (the game allows 999). Click a stone to open it.";
        else if (parties)
            note = L"Click a party to open it.";
        else if (scalars > shown)
            note = L"Showing the first " + std::to_wstring(shown) + L" of " + std::to_wstring(scalars) + L" values; change them all above.";
        else if (ui.fields.empty())
            note = e.kids.empty() ? L"Nothing to change here." : L"Pick one of its " + std::to_wstring(e.kids.size()) + L" entries on the left.";
        SetWindowTextW(ui.empty, note.c_str());
        ShowWindow(ui.empty, note.empty() ? SW_HIDE : SW_SHOW);
    }
    SetWindowTextW(g_action, g_action_kind == Action::AddStone    ? L"Add a stone..."
                             : g_action_kind == Action::AddParty ? L"Add a party"
                                                                 : L"Remove this stone");
    ShowWindow(g_action, g_action_kind == Action::None ? SW_HIDE : SW_SHOW);
    for (const Ui::Made& m : ui.pool) // the last page's controls this one has no use for
        if (m.h)
            DestroyWindow(m.h);
    ui.pool.clear();
    ui.populating = populating;
    layout_panel();
    SendMessageW(ui.panel, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(ui.panel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void refresh_hints() {
    for (Field& f : ui.fields)
        if (f.hint && !f.pick && !f.combo && f.kind != Field::Kind::List)
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

// --- undo: one step back, for the changes that touch many values or a list --------------------------

struct Undo {
    bool has = false;
    Node doc;
    std::wstring what;
    std::vector<std::wstring> chain; // the labels down to the page it was made on
} g_undo;

bool can_undo() {
    return g_undo.has;
}

std::vector<std::wstring> chain_of(int k) {
    std::vector<std::wstring> c;
    for (; k >= 0; k = ui.entries[static_cast<size_t>(k)].parent)
        c.insert(c.begin(), ui.entries[static_cast<size_t>(k)].label);
    return c;
}

void update_title();

// Keeps the file as it is now, so `what` can be undone.
void snapshot(const std::wstring& what) {
    g_undo.doc = ui.doc;
    g_undo.what = what;
    g_undo.chain = ui.sel >= 0 ? chain_of(ui.sel) : std::vector<std::wstring>();
    g_undo.has = true;
    update_title();
}

void clear_panel();
void build_model();
void fill_tree();
void reveal(int k);
void mark_changed();
void set_status(const std::wstring& s);

void undo() {
    if (!g_undo.has)
        return;
    clear_panel();
    ui.doc = std::move(g_undo.doc);
    g_undo.has = false;
    update_title();
    ui.sel = -1;
    build_model();
    fill_tree();
    mark_changed();
    // The page it was made on, or the nearest one still there (a removed stone's list).
    for (size_t depth = g_undo.chain.size(); depth > 0; --depth) {
        std::vector<std::wstring> want(g_undo.chain.begin(), g_undo.chain.begin() + static_cast<std::ptrdiff_t>(depth));
        for (size_t k = 0; k < ui.entries.size(); ++k)
            if (ui.entries[k].label == want.back() && chain_of(static_cast<int>(k)) == want) {
                reveal(static_cast<int>(k));
                set_status(L"Undid: " + g_undo.what + L".");
                return;
            }
    }
    show_entry();
    set_status(L"Undid: " + g_undo.what + L".");
}

// How many of a list's entries `f` would change.
size_t would_change(const Field& f, bool on, int64_t value, size_t* total) {
    const Entry& e = ui.entries[static_cast<size_t>(ui.sel)];
    size_t n = 0, all = 0;
    for (Node* it : list_items(e)) {
        const Node* x = f.key.empty() ? it : it->find(f.key);
        if (!x)
            continue;
        ++all;
        if (f.kind == Field::Kind::AllBool && x->k == Node::K::Bool && x->b != on)
            ++n;
        else if (f.kind == Field::Kind::AllInt && x->k == Node::K::Int && x->i != value)
            ++n;
    }
    *total = all;
    return n;
}

void set_all(Field& f, bool on, int64_t value) {
    const Entry& e = ui.entries[static_cast<size_t>(ui.sel)];
    size_t total = 0, n = would_change(f, on, value, &total);
    std::wstring what = f.kind == Field::Kind::AllInt ? L"Set " + f.label + L" to " + std::to_wstring(value) + L" on all " + e.label
                        : f.key == "UnlockedFlag"     ? std::wstring(on ? L"Unlock" : L"Lock") + L" all " + e.label
                                                      : L"Turn " + f.label + (on ? L" on" : L" off") + L" for all " + e.label;
    if (n == 0) {
        set_status(L"Nothing to change: " + e.label + L" already " + (f.kind == Field::Kind::AllInt ? L"all have that value." : on ? L"all on." : L"all off."));
        return;
    }
    // Turning things off, or setting a number everywhere, is easy to regret: ask first (and for big changes).
    if (!on || f.kind == Field::Kind::AllInt || n > 5) {
        std::wstring q = what + L"?\n\n" + std::to_wstring(n) + L" of " + std::to_wstring(total) + L" will change. Ctrl+Z (Undo) takes it back.";
        if (MessageBoxW(ui.wnd, q.c_str(), kTitle, MB_OKCANCEL | MB_ICONQUESTION | (on ? 0 : MB_DEFBUTTON2)) != IDOK)
            return;
    }
    snapshot(what);
    for (Node* it : list_items(e)) {
        Node* x = f.key.empty() ? it : it->find(f.key);
        if (!x)
            continue;
        if (f.kind == Field::Kind::AllBool && x->k == Node::K::Bool)
            x->b = on;
        else if (f.kind == Field::Kind::AllInt && x->k == Node::K::Int)
            x->i = value;
    }
    set_status(what + L": " + std::to_wstring(n) + L" changed. Undo takes it back.");
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
    HWND wnd = nullptr, search = nullptr, grid = nullptr, note = nullptr, ok = nullptr, cancel = nullptr;
    const PickKind* kind = nullptr;
    Node* list = nullptr;                  // the save's list of them (UnlockedFlag), if it has one
    int64_t current = kNone, chosen = kNone;
    bool browse = false;                   // choosing which to show: locked ones can be picked too
    bool done = false, unlock = false;     // OK'd; the chosen one is to be unlocked
    std::vector<int64_t> all, shown;
    std::set<int64_t> locked, taken;       // not unlocked in this save; already in this party
    std::map<int64_t, std::wstring> names; // twins numbered: "Enterprise (2)"; kNone "None"
    std::vector<std::pair<RECT, int64_t>> hits;
    int top = 0;
};
Picker g_pick;
constexpr int kCellW = 104, kCellH = 130, kPickFace = 72;

std::wstring tile_name(int64_t id) {
    auto it = g_pick.names.find(id);
    return it != g_pick.names.end() ? it->second : id_label(id);
}

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
        if (lower(tile_name(id)).find(q) != std::wstring::npos)
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
        bool pictured = draw_face(dc, id, img, id == g_pick.chosen, off);
        SetTextColor(dc, GetSysColor(off ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        int below = y + face + px(4);
        if (pictured) { // the name, on up to two lines
            RECT name = {x + 4, below, x + cw - 4, below + px(32)};
            std::wstring n = tile_name(id);
            DrawTextW(dc, n.c_str(), -1, &name, DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
            below += px(32);
        }
        if (off) {
            RECT why = {x + 2, below, x + cw - 2, below + px(16)};
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

// Closes the picker with the chosen one.
void picker_finish() {
    g_pick.done = true;
    DestroyWindow(g_pick.wnd);
}

// A click selects (OK or a double-click chooses it). One not unlocked yet can be unlocked and chosen.
void picker_click(int x, int y, bool twice) {
    for (const auto& [r, id] : g_pick.hits) {
        if (!PtInRect(&r, {x, y}))
            continue;
        std::wstring n = tile_name(id);
        if (g_pick.taken.count(id)) {
            SetWindowTextW(g_pick.note, (n + L" is already in this party.").c_str());
            return;
        }
        if (!g_pick.browse && g_pick.locked.count(id)) {
            std::wstring q = n + L" isn't unlocked in this save yet.\n\nUnlock it and choose it?";
            if (MessageBoxW(g_pick.wnd, q.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;
            g_pick.chosen = id;
            g_pick.unlock = true;
            picker_finish();
            return;
        }
        g_pick.chosen = id;
        InvalidateRect(g_pick.grid, nullptr, FALSE);
        if (twice) {
            picker_finish();
            return;
        }
        SetWindowTextW(g_pick.note, (L"Selected: " + n + L". OK (or a double-click) chooses it.").c_str());
        EnableWindow(g_pick.ok, TRUE);
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
    case WM_LBUTTONDOWN:
        picker_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false);
        return 0;
    case WM_LBUTTONDBLCLK:
        picker_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
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
    const int bw = px(84), bh = px(28), by = rc.bottom - m - bh;
    place(g_pick.cancel, rc.right - m - bw, by, bw, bh);
    place(g_pick.ok, rc.right - m - 2 * bw - px(6), by, bw, bh);
    place(g_pick.note, m, by + px(6), rc.right - 3 * m - 2 * bw, px(18));
    place(g_pick.grid, m, m + px(32), rc.right - 2 * m, by - px(10) - m - px(32));
}

LRESULT CALLBACK picker_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == g_pick.search && HIWORD(wp) == EN_CHANGE)
            picker_filter();
        else if (LOWORD(wp) == IDOK && IsWindowEnabled(g_pick.ok))
            picker_finish();
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

// Picks one of a kind (`prefix`), starting at `current`, into `out`; false when cancelled. For a field
// (`browse` false): unlocked ones (or one unlocked on the way, which this unlocks) and, for a party
// member, not one another slot of the party has (`member`: the field's node). An airship can be none.
bool pick_id(int64_t prefix, int64_t current, bool browse, const Node* member, const std::wstring& what, int64_t& out) {
    g_pick = Picker();
    g_pick.kind = pick_kind(prefix);
    g_pick.current = g_pick.chosen = current;
    g_pick.browse = browse;
    Node* list = nullptr;
    if (Node* info = g_pick.kind->info ? ui.doc.find(g_pick.kind->info) : nullptr)
        list = info->find(g_pick.kind->list);
    g_pick.list = list;
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
    if (prefix == 370 && !browse) // a party may have no airship
        g_pick.all.insert(g_pick.all.begin(), kNone);
    std::map<std::wstring, int> count, seen;
    for (int64_t id : g_pick.all)
        ++count[id == kNone ? L"None" : id_label(id)];
    for (int64_t id : g_pick.all) {
        std::wstring n = id == kNone ? L"None" : id_label(id);
        g_pick.names[id] = count[n] > 1 ? n + L" (" + std::to_wstring(++seen[n]) + L")" : n;
    }
    // The other members of this party (the list the picked member is in).
    if (member && prefix == 230 && ui.sel >= 0) {
        const Entry& se = ui.entries[static_cast<size_t>(ui.sel)];
        const Node* list = se.node->is_map() ? se.node->find("Characters") : nullptr; // a party's page
        if (!list && se.parent >= 0)
            list = ui.entries[static_cast<size_t>(se.parent)].node; // a member's own page
        if (list && list->is_array())
            for (const Node& m : list->a)
                if (const Node* c = m.find("CharacterID"); c && c != member && c->k == Node::K::Int && c->i != kNone)
                    g_pick.taken.insert(c->i);
    }

    RECT owner;
    GetWindowRect(ui.wnd, &owner);
    int w = px(720), h = px(600);
    std::wstring title = std::wstring(L"Choose ") + (prefix == 370 ? L"an " : L"a ") + g_pick.kind->one + (what.empty() ? L"" : L" - " + what);
    g_pick.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"theatrhythm_saveeditor_picker", title.c_str(),
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
                                 owner.left + (owner.right - owner.left - w) / 2, owner.top + (owner.bottom - owner.top - h) / 2, w, h,
                                 ui.wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto child = [&](const wchar_t* cls, DWORD style, DWORD ex) {
        HWND c = CreateWindowExW(ex, cls, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, px(kStartW), px(kStartH), g_pick.wnd, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
        return c;
    };
    g_pick.search = child(WC_EDITW, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
    std::wstring cue = std::wstring(L"Search ") + lower(g_pick.kind->many);
    SendMessageW(g_pick.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    g_pick.grid = child(L"theatrhythm_saveeditor_picker_grid", WS_VSCROLL, WS_EX_CLIENTEDGE);
    g_pick.note = child(WC_STATICW, SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    g_pick.ok = child(WC_BUTTONW, WS_TABSTOP | BS_DEFPUSHBUTTON, 0);
    SetWindowLongPtrW(g_pick.ok, GWLP_ID, IDOK);
    SetWindowTextW(g_pick.ok, L"OK");
    g_pick.cancel = child(WC_BUTTONW, WS_TABSTOP | BS_PUSHBUTTON, 0);
    SetWindowLongPtrW(g_pick.cancel, GWLP_ID, IDCANCEL);
    SetWindowTextW(g_pick.cancel, L"Cancel");
    EnableWindow(g_pick.ok, FALSE); // until one is selected
    std::wstring note = std::wstring(L"Click ") + (prefix == 370 ? L"an " : L"a ") + g_pick.kind->one + L" to select it, then OK (or double-click).";
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
    if (!g_pick.done)
        return false;
    out = g_pick.chosen;
    return true;
}

// After a pick: the one chosen was not unlocked and the player agreed to unlock it.
void unlock_picked() {
    if (!g_pick.unlock || !g_pick.list)
        return;
    for (auto& [k, v] : g_pick.list->m)
        if (k.k == Node::K::Int && k.i == g_pick.chosen)
            if (Node* u = v.find("UnlockedFlag"); u && u->k == Node::K::Bool)
                u->b = true;
}

void remove_stone(Node& stones, size_t index);
void move_stone(Node& stones, size_t index, int by);
void remove_party(Node& sets, size_t index);
void move_party(Node& sets, size_t index, int by);

void on_field_command(int id, int code) {
    size_t j = static_cast<size_t>((id - kFieldBase) / 8);
    int role = (id - kFieldBase) % 8;
    if (j >= ui.fields.size())
        return;
    Field& f = ui.fields[j];
    if (f.kind == Field::Kind::List) { // the Parties or Stones list (its rows' buttons and keys: row_list_proc)
        if (code == LBN_SELCHANGE)
            place_row_buttons();
        return;
    }
    if (f.pick) {
        if (role != 1 || code != BN_CLICKED)
            return;
        const Entry& e = ui.entries[static_cast<size_t>(ui.sel)];
        if (f.browse) { // the Moogles or Airships page: show the one picked
            int64_t picked = kNone;
            if (pick_id(f.pick, ui.browse[e.dict], true, nullptr, L"", picked) && picked != kNone) {
                ui.browse[e.dict] = picked;
                PostMessageW(ui.wnd, kMsgRefresh, 0, 0);
            }
            return;
        }
        int64_t picked = kNone;
        std::wstring field = f.label;
        if (pick_id(f.pick, f.node->i, false, f.node, is_party(e) ? e.label + L", " + f.label : e.label, picked) && picked != f.node->i) {
            snapshot(L"Change " + field + L" to " + (picked == kNone ? std::wstring(L"none") : id_label(picked)));
            unlock_picked();
            f.node->i = picked;
            mark_changed();
            relabel(); // "Member 1: <name>"
            InvalidateRect(ui.tree, nullptr, TRUE);
            PostMessageW(ui.wnd, kMsgRefresh, 1, 0); // the panel shows the new one (not from inside its own button)
        }
        return;
    }
    if (f.combo) {
        LRESULT k = code == CBN_SELCHANGE ? SendMessageW(f.ctrl, CB_GETCURSEL, 0, 0) : CB_ERR;
        if (k == CB_ERR)
            return;
        if (SendMessageW(f.ctrl, CB_GETITEMDATA, static_cast<WPARAM>(k), 0) == kSeriesRow) { // a heading: its first song
            k = k + 1 < SendMessageW(f.ctrl, CB_GETCOUNT, 0, 0) ? k + 1 : k - 1;
            SendMessageW(f.ctrl, CB_SETCURSEL, static_cast<WPARAM>(k), 0);
        }
        snapshot(L"Change " + f.label);
        f.node->i = static_cast<int64_t>(SendMessageW(f.ctrl, CB_GETITEMDATA, static_cast<WPARAM>(k), 0));
        if (Node* slot = f.slot; f.combo == 1 && slot->a[1].k == Node::K::Int) { // an empty slot has rarity 0; another effect may have fewer
            auto v = ui.names.effect_values.find(f.node->i);
            int64_t levels = v == ui.names.effect_values.end() ? 3 : static_cast<int64_t>(v->second.size());
            if (f.node->i == kNone || slot->a[1].i >= levels)
                slot->a[1].i = 0;
        }
        mark_changed();
        if (f.combo >= 5) // nothing else on the page names it
            return;
        relabel(); // "1. EXP received +10%"
        InvalidateRect(ui.tree, nullptr, TRUE);
        PostMessageW(ui.wnd, kMsgRefresh, 1, 0); // the rarity list names the new effect
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
    if (code != BN_CLICKED) // (STN_CLICKED is the same 0)
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
        g_undo = Undo();
        update_title();
        SetWindowTextW(ui.file, p.wstring().c_str());
        build_model();
        fill_tree();
        show_entry();
        update_title();
        store_settings();
        set_status(L"Opened " + p.filename().wstring() + L": " + std::to_wstring(ui.entries.size()) + L" entries. " + ui.names_note);
        if (game_running())
            message(L"The game is running. It keeps your save in memory and writes it back when it saves, so close it "
                    L"before you save here (the editor won't save while it runs).",
                    MB_ICONINFORMATION);
    } catch (const std::exception& e) {
        message(L"Cannot open " + p.wstring() + L":\n" + widen(e.what()));
    }
}

fs::path backup_folder() {
    return ui.root / L"save-backups";
}

// Copies the open file, as it is on disk, into save-backups/ (named with the date and time). Every
// save and restore does, so each version the editor replaced can be put back.
fs::path back_up() {
    fs::path dir = backup_folder();
    fs::create_directories(dir);
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &tm);
    fs::path to = dir / (ui.path.stem().wstring() + L"-" + stamp + ui.path.extension().wstring());
    for (int n = 2; fs::exists(to); ++n) // two saves in one second
        to = dir / (ui.path.stem().wstring() + L"-" + stamp + L"-" + std::to_wstring(n) + ui.path.extension().wstring());
    fs::copy_file(ui.path, to);
    ui.backed_up = true;
    return to;
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
        fs::path to = back_up();
        write_bytes(ui.path, file);
        ui.original = plain;
        mark_changed();
        set_status(L"Saved " + ui.path.filename().wstring() + L". The file it replaced is save-backups\\" + to.filename().wstring() +
                   L" (Backups... restores it).");
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

std::wstring pick(bool folder, const wchar_t* title, const std::wstring& start);
void open_file(const fs::path& p);

// Puts a backup back in place of the open file (backing up the file it replaces first).
void restore_backup() {
    if (ui.path.empty()) {
        message(L"Open the save file to restore first.");
        return;
    }
    if (!settle_changes(L"before restoring a backup"))
        return;
    if (game_running()) {
        message(L"Close the game first: it would write its own copy of the save back over the one restored.");
        return;
    }
    std::error_code ec;
    fs::create_directories(backup_folder(), ec);
    std::wstring p = pick(false, L"Restore a backup", backup_folder().wstring());
    if (p.empty())
        return;
    fs::path from = p;
    std::wstring stem = ui.path.stem().wstring();
    if (from.filename().wstring().rfind(stem + L"-", 0) != 0) {
        message(L"That backup is not of " + ui.path.filename().wstring() + L" (backups are named after the file they came from).");
        return;
    }
    std::wstring q = L"Replace " + ui.path.filename().wstring() + L" with the backup " + from.filename().wstring() +
                     L"?\n\nThe file it replaces is backed up first.";
    if (MessageBoxW(ui.wnd, q.c_str(), kTitle, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    try {
        saveeditor::decrypt_file(read_bytes(from)); // a save the game can read
        fs::path kept = back_up();
        fs::copy_file(from, ui.path, fs::copy_options::overwrite_existing);
        open_file(ui.path);
        set_status(L"Restored " + from.filename().wstring() + L". The file it replaced is save-backups\\" + kept.filename().wstring() + L".");
    } catch (const std::exception& e) {
        message(L"Cannot restore:\n" + widen(e.what()));
    }
}

void backups_menu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (ui.path.empty() ? MF_GRAYED : 0), kRestore, L"Restore a backup...");
    AppendMenuW(m, MF_STRING, kOpenBackups, L"Open the backup folder");
    RECT r;
    GetWindowRect(GetDlgItem(ui.wnd, kBackups), &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, r.left, r.top, 0, ui.wnd, nullptr);
    DestroyMenu(m);
    if (cmd)
        PostMessageW(ui.wnd, WM_COMMAND, static_cast<WPARAM>(cmd), 0);
}

void open_backup_folder() {
    std::error_code ec;
    fs::create_directories(backup_folder(), ec);
    ShellExecuteW(ui.wnd, L"open", backup_folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
            if (ui.entries[k].hidden)
                continue;
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
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, px(kStartW), px(kStartH), ui.wnd,
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
    place(GetDlgItem(ui.wnd, kBackups), W - m - 3 * bw - 2 * gap, by, bw, bh);
    place(GetDlgItem(ui.wnd, kUndo), W - m - 4 * bw - 3 * gap, by, bw, bh);
    place(ui.status, m, by + px(6), W - 2 * m - 4 * bw - 5 * gap, px(18));
    int tw = px(kTreeW);
    place(ui.find, m, y, tw - px(70) - gap, rh);
    place(GetDlgItem(ui.wnd, kFindNext), m + tw - px(70), y - px(2), px(70), bh);
    int top = y + px(32), bottom = by - px(10);
    place(ui.tree, m, top, tw, bottom - top);
    place(ui.panel, m + tw + gap, y, W - 2 * m - tw - gap, bottom - y);
    layout_panel();
}

// --- adding and removing summon stones -----------------------------------------------------------

constexpr size_t kMaxStones = 999; // the game's HaveSummonStoneMax

// The model and tree again after stones were added or removed (they hold pointers into the list),
// showing `target`.
void rebuild(const Node* target) {
    clear_panel();
    ui.sel = -1;
    build_model();
    fill_tree();
    mark_changed();
    for (size_t k = 0; k < ui.entries.size(); ++k)
        if (ui.entries[k].node == target) {
            reveal(static_cast<int>(k));
            return;
        }
    show_entry();
}

// The account ID on the stones you found yourself (no discoverer name): yours.
int64_t own_account(const Node& stones) {
    std::map<int64_t, int> seen;
    for (const Node& s : stones.a)
        if (s.is_array() && s.a.size() >= 4 && s.a[2].k == Node::K::Str && s.a[2].s.empty() && s.a[3].k == Node::K::Int)
            ++seen[s.a[3].i];
    int64_t best = 0;
    int most = 0;
    for (const auto& [id, n] : seen)
        if (n > most) {
            best = id;
            most = n;
        }
    return best;
}

bool stone_shaped(const Node& s) {
    using K = Node::K;
    if (!s.is_array() || s.a.size() != 8 || s.a[1].k != K::Array || s.a[1].a.empty())
        return false;
    for (const Node& slot : s.a[1].a)
        if (!slot.is_array() || slot.a.size() != 2 || slot.a[0].k != K::Int || slot.a[1].k != K::Int)
            return false;
    return s.a[0].k == K::Int && s.a[2].k == K::Str && s.a[3].k == K::Int && s.a[4].k == K::Str && s.a[5].k == K::Int &&
           s.a[6].k == K::Bool && s.a[7].k == K::Bool;
}

// A new stone of `summon`, shaped as the game writes one (from another stone when there is one):
// found by you, no giver, one effect (EXP received, rarity 1) and two empty slots.
Node new_stone(const Node& stones, int64_t summon) {
    Node s;
    if (!stones.a.empty() && stone_shaped(stones.a.back())) {
        s = stones.a.back();
    } else {
        auto num = [](int64_t v, uint8_t tag, bool u64) {
            Node n;
            n.k = Node::K::Int;
            n.i = v;
            n.tag = tag;
            n.u64 = u64;
            return n;
        };
        Node str, flag, slot, effects;
        str.k = Node::K::Str;
        flag.k = Node::K::Bool;
        slot.k = effects.k = s.k = Node::K::Array;
        slot.a = {num(kNone, 0xce, false), num(0, 0x00, false)};
        effects.a = {slot, slot, slot};
        s.a = {num(0, 0xce, false), effects, str, num(0, 0xcf, true), str, num(0, 0x00, false), flag, flag};
    }
    s.a[0].i = summon;
    for (size_t j = 0; j < s.a[1].a.size(); ++j) {
        s.a[1].a[j].a[0].i = j == 0 ? 290000000 : kNone;
        s.a[1].a[j].a[1].i = 0;
    }
    s.a[2].s.clear();
    s.a[3].i = own_account(stones);
    s.a[4].s.clear();
    s.a[5].i = 0;
    s.a[6].b = false; // not seen yet: the game marks it new
    s.a[7].b = false;
    return s;
}

void add_stone(Node& stones) {
    if (stones.a.size() >= kMaxStones) {
        message(L"You have " + std::to_wstring(stones.a.size()) + L" stones, as many as the game allows.");
        return;
    }
    int64_t summon = kNone;
    if (!pick_id(280, kNone, false, nullptr, L"new stone", summon) || summon == kNone)
        return;
    snapshot(L"Add a " + id_label(summon) + L" stone");
    stones.a.push_back(new_stone(stones, summon));
    rebuild(&stones.a.back());
    set_status(L"Added stone " + std::to_wstring(stones.a.size()) + L" (" + id_label(summon) + L"). Set its effects below.");
}

// "Remove ...?" with Remove and Keep (Keep the default).
bool ask_remove(const std::wstring& head, const std::wstring& text) {
    TASKDIALOG_BUTTON buttons[] = {{IDOK, L"Remove"}, {IDCANCEL, L"Keep"}};
    TASKDIALOGCONFIG tc = {};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = ui.wnd;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = kTitle;
    tc.pszMainInstruction = head.c_str();
    tc.pszContent = text.c_str();
    tc.cButtons = 2;
    tc.pButtons = buttons;
    tc.nDefaultButton = IDCANCEL;
    int pressed = IDCANCEL;
    return SUCCEEDED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr)) && pressed == IDOK;
}

void remove_stone(Node& stones, size_t index) {
    const Node& s = stones.a[index];
    std::wstring name = std::to_wstring(index + 1) + L". " + (s.a.empty() || s.a[0].k != Node::K::Int ? L"" : id_label(s.a[0].i));
    // Parties point at their stone by its place in the list.
    Node* sets = nullptr;
    if (Node* party = ui.doc.find("PartyInfo"))
        sets = party->find("PartySets");
    std::wstring users;
    if (sets && sets->is_array())
        for (size_t p = 0; p < sets->a.size(); ++p)
            if (const Node* i = sets->a[p].find("SummonStoneIndex"); i && i->k == Node::K::Int && i->i == static_cast<int64_t>(index))
                users += (users.empty() ? L"Party " : L", Party ") + std::to_wstring(p + 1);
    std::wstring q = L"The stones after it move up one place (parties using them follow).";
    if (stone_shaped(s) && s.a[7].b)
        q = L"It is locked in the game (kept from being sold).\n\n" + q;
    if (!users.empty())
        q = users + L" will have no summon stone.\n\n" + q;
    if (!ask_remove(L"Remove stone " + name + L"?", q))
        return;
    snapshot(L"Remove stone " + name);
    stones.a.erase(stones.a.begin() + static_cast<std::ptrdiff_t>(index));
    if (sets && sets->is_array())
        for (Node& p : sets->a)
            if (Node* i = p.find("SummonStoneIndex"); i && i->k == Node::K::Int && i->i >= static_cast<int64_t>(index))
                i->i = i->i == static_cast<int64_t>(index) ? -1 : i->i - 1;
    rebuild(&stones);
    set_status(L"Removed stone " + name + L".");
}

// Swaps a stone with the one above or below it; parties keep the stone they had.
void move_stone(Node& stones, size_t index, int by) {
    size_t other = index + static_cast<size_t>(by);
    if ((by < 0 && index == 0) || other >= stones.a.size())
        return;
    std::wstring name = std::to_wstring(index + 1) + L". " +
                        (stones.a[index].is_array() && !stones.a[index].a.empty() ? id_label(stones.a[index].a[0].i) : L"");
    snapshot(L"Move stone " + name + (by < 0 ? L" up" : L" down"));
    std::swap(stones.a[index], stones.a[other]);
    if (Node* party = ui.doc.find("PartyInfo"))
        if (Node* sets = party->find("PartySets"); sets && sets->is_array())
            for (Node& p : sets->a)
                if (Node* i = p.find("SummonStoneIndex"); i && i->k == Node::K::Int) {
                    if (i->i == static_cast<int64_t>(index))
                        i->i = static_cast<int64_t>(other);
                    else if (i->i == static_cast<int64_t>(other))
                        i->i = static_cast<int64_t>(index);
                }
    int keep = ui.ptop; // stay where the row was
    rebuild(&stones);
    ui.ptop = keep;
    layout_panel();
    set_status(L"Moved stone " + name + L" to " + std::to_wstring(other + 1) + L".");
}

// A new party at the end: no members, abilities, stone or airship, the first moogle (as the game's
// empty parties are), shaped as the last party.
void add_party(Node& sets) {
    if (sets.a.empty() || !sets.a.back().is_map())
        return;
    snapshot(L"Add a party");
    Node p = sets.a.back();
    if (Node* chars = p.find("Characters"); chars && chars->is_array())
        for (Node& m : chars->a) {
            if (Node* c = m.find("CharacterID"))
                c->i = kNone;
            if (Node* a = m.find("Abilities"); a && a->is_array())
                for (Node& x : a->a)
                    x.i = kNone;
        }
    if (Node* st = p.find("SummonStoneIndex"))
        st->i = -1;
    if (Node* air = p.find("AirShipID"))
        air->i = kNone;
    if (Node* moogle = p.find("MyMoogleID"))
        moogle->i = 360000000;
    sets.a.push_back(std::move(p));
    rebuild(&sets.a.back());
    set_status(L"Added Party " + std::to_wstring(sets.a.size()) +
               (sets.a.size() > kGameParties ? L". The game only switches between Parties 1 to 5." : L"."));
}

// The game's current party stays the one it was (Party 1 when it is the one removed).
void remove_party(Node& sets, size_t index) {
    if (sets.a.size() <= kGameParties)
        return;
    std::wstring name = L"Party " + std::to_wstring(index + 1);
    Node* active = active_party();
    std::wstring q = L"The parties after it move up one place.";
    if (active && active->i == static_cast<int64_t>(index))
        q = L"It is the game's current party: Party 1 will be.\n\n" + q;
    if (!ask_remove(L"Remove " + name + L"?", q))
        return;
    snapshot(L"Remove " + name);
    sets.a.erase(sets.a.begin() + static_cast<std::ptrdiff_t>(index));
    if (active && active->i >= static_cast<int64_t>(index))
        active->i = active->i == static_cast<int64_t>(index) ? 0 : active->i - 1;
    rebuild(ui.entries[static_cast<size_t>(ui.sel)].node); // the Parties page
    set_status(L"Removed " + name + L".");
}

// Swaps a party with the one above or below it; the game's current party stays the same party.
void move_party(Node& sets, size_t index, int by) {
    size_t other = index + static_cast<size_t>(by);
    if ((by < 0 && index == 0) || other >= sets.a.size())
        return;
    std::wstring name = L"Party " + std::to_wstring(index + 1);
    snapshot(L"Move " + name + (by < 0 ? L" up" : L" down"));
    std::swap(sets.a[index], sets.a[other]);
    if (Node* active = active_party()) {
        if (active->i == static_cast<int64_t>(index))
            active->i = static_cast<int64_t>(other);
        else if (active->i == static_cast<int64_t>(other))
            active->i = static_cast<int64_t>(index);
    }
    int keep = ui.ptop;
    rebuild(ui.entries[static_cast<size_t>(ui.sel)].node);
    ui.ptop = keep;
    layout_panel();
    set_status(L"Moved " + name + L" to " + std::to_wstring(other + 1) + L".");
}

// A row of the Parties or Stones list: 1 open it, 2 up, 3 down, 4 remove. After a move or a removal the
// list is built again with the same row selected (row_list_proc posts these: the list is rebuilt).
void row_action(int what, int k) {
    if (!g_rows.list || !g_rows.node || k < 0 || k >= static_cast<int>(g_rows.node->a.size()))
        return;
    Node& list = *g_rows.node;
    if (what == 1) {
        for (size_t j = 0; j < ui.entries.size(); ++j)
            if (ui.entries[j].node == &list.a[static_cast<size_t>(k)]) {
                reveal(static_cast<int>(j));
                return;
            }
        return;
    }
    if (what == 4 && g_rows.parties && list.a.size() <= kGameParties) {
        set_status(L"Remove is available when there are more than 5 parties.");
        MessageBeep(MB_OK);
        return;
    }
    const int n = static_cast<int>(list.a.size());
    g_rows.restore.set = true;
    g_rows.restore.sel = what == 2 ? std::max(0, k - 1) : what == 3 ? std::min(n - 1, k + 1) : std::min(k, n - 2);
    g_rows.restore.top = static_cast<int>(SendMessageW(g_rows.list, LB_GETTOPINDEX, 0, 0));
    if (what == 2 || what == 3)
        (g_rows.parties ? move_party : move_stone)(list, static_cast<size_t>(k), what == 2 ? -1 : 1);
    else if (what == 4)
        (g_rows.parties ? remove_party : remove_stone)(list, static_cast<size_t>(k));
    g_rows.restore.set = false;
}

void on_action() {

    if (ui.sel < 0)
        return;
    Entry& e = ui.entries[static_cast<size_t>(ui.sel)];
    if (g_action_kind == Action::AddStone)
        add_stone(*e.node);
    else if (Node* sets = g_action_kind == Action::AddParty ? party_list(e) : nullptr)
        add_party(*sets);
    else if (g_action_kind == Action::RemoveStone && e.parent >= 0)
        remove_stone(*ui.entries[static_cast<size_t>(e.parent)].node, e.index);
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

// A row of the song list: a series heading (bold, on the face colour, not highlighted) or a song under
// it (indented); the closed list shows the song alone.
void draw_song_row(const DRAWITEMSTRUCT& di) {
    HDC dc = di.hDC;
    RECT r = di.rcItem;
    if (di.itemID == static_cast<UINT>(-1)) {
        FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW));
        return;
    }
    const bool heading = SendMessageW(di.hwndItem, CB_GETITEMDATA, di.itemID, 0) == kSeriesRow;
    const bool closed = di.itemState & ODS_COMBOBOXEDIT, lit = (di.itemState & ODS_SELECTED) && !heading;
    std::wstring text(static_cast<size_t>(std::max<LRESULT>(0, SendMessageW(di.hwndItem, CB_GETLBTEXTLEN, di.itemID, 0))), L' ');
    SendMessageW(di.hwndItem, CB_GETLBTEXT, di.itemID, reinterpret_cast<LPARAM>(text.data()));
    FillRect(dc, &r, GetSysColorBrush(lit ? COLOR_HIGHLIGHT : heading && !closed ? COLOR_BTNFACE : COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(lit ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
    HGDIOBJ was = SelectObject(dc, heading ? ui.bold : ui.font);
    RECT t = r;
    t.left += heading || closed ? px(4) : px(18);
    DrawTextW(dc, text.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, was);
    if ((di.itemState & ODS_FOCUS) && closed)
        DrawFocusRect(dc, &r);
}

LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == kAction && HIWORD(wp) == BN_CLICKED)
            on_action();
        else
            on_field_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_NOTIFY: {
        wchar_t cls[32] = {};
        if (GetClassNameW(reinterpret_cast<NMHDR*>(lp)->hwndFrom, cls, 32) && std::wstring(cls) == WC_LISTVIEWW)
            return on_table_notify(reinterpret_cast<NMHDR*>(lp)); // Theater's grid
        break;
    }
    case WM_MEASUREITEM: // the song list's rows; the Parties or Stones list's
        if (auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp); mi->CtlType == ODT_COMBOBOX) {
            mi->itemHeight = static_cast<UINT>(px(20));
            return TRUE;
        } else if (mi->CtlType == ODT_LISTBOX && mi->itemID < g_rows.rows.size()) {
            mi->itemHeight = static_cast<UINT>(row_height(g_rows.rows[mi->itemID]));
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp); di->CtlType == ODT_COMBOBOX) {
            draw_song_row(*di);
            return TRUE;
        } else if (di->CtlType == ODT_LISTBOX) {
            draw_list_row(*di);
            return TRUE;
        }
        break;
    case kMsgRowAction:
        row_action(static_cast<int>(wp), static_cast<int>(lp));
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
    case kUndo:
        undo();
        break;
    case kBackups:
        backups_menu();
        break;
    case kRestore:
        restore_backup();
        break;
    case kOpenBackups:
        open_backup_folder();
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
            set_status(L""); // a note about the last page ("Nothing named...") is done with
            show_entry();
        }
        break;
    }
    case kMsgRefresh: { // wp 1: a value on the page changed, so stay where it is
        int keep = ui.ptop;
        show_entry();
        if (wp == 1 && keep) {
            ui.ptop = keep;
            layout_panel();
        }
        return 0;
    }
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
    SendMessageW(ui.find, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Find by name (Ctrl+F)"));
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
    g_action = panel_add(WC_BUTTONW, L"", WS_TABSTOP | BS_PUSHBUTTON, kAction);
    ShowWindow(g_action, SW_HIDE);
    g_face = panel_add(L"theatrhythm_saveeditor_face", L"", 0, 0);
    ShowWindow(g_face, SW_HIDE);
    ui.made.clear(); // the panel's own: every page has them (only a page's controls are reused or destroyed)
    ui.status = add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    add(WC_BUTTONW, L"Undo", WS_TABSTOP | BS_PUSHBUTTON, kUndo);
    add(WC_BUTTONW, L"Backups...", WS_TABSTOP | BS_PUSHBUTTON, kBackups);
    add(WC_BUTTONW, L"Save", WS_TABSTOP | BS_PUSHBUTTON, kSave);
    add(WC_BUTTONW, L"Close", WS_TABSTOP | BS_PUSHBUTTON, kClose);
}

// Ctrl+S saves, Ctrl+F goes to Find, Ctrl+Z undoes (an edit box keeps its own Ctrl+Z).
bool shortcut(const MSG& msg) {
    if (msg.message != WM_KEYDOWN || GetKeyState(VK_CONTROL) >= 0 || !IsWindowEnabled(ui.wnd))
        return false;
    wchar_t cls[32] = {};
    GetClassNameW(GetFocus(), cls, 32);
    switch (msg.wParam) {
    case 'S':
        if (HWND f = GetFocus(); f && GetParent(f) == ui.panel)
            SetFocus(ui.tree); // commit a value being typed
        if (ui.dirty)
            save_file();
        return true;
    case 'F':
        SetFocus(ui.find);
        SendMessageW(ui.find, EM_SETSEL, 0, -1);
        return true;
    case 'Z':
        if (lstrcmpiW(cls, WC_EDITW) == 0)
            return false;
        undo();
        return true;
    }
    return false;
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
    gc.style |= CS_DBLCLKS;
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
        if (shortcut(msg))
            continue;
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
