// SPDX-License-Identifier: GPL-2.0-or-later
// What the DLC maker's windows (main.cpp on Windows, main_linux.cpp) read from the commands (../core/commands.cpp):
// its records, the project folder they describe (`info`) and the game's stages and monsters
// (`stage-data`). Templated on the windows' text: UTF-16 (wchar_t) on Windows, UTF-8 (char) on Linux.
// Item is what a window keeps per tree row, Picture what it keeps per monster picture.
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dlcmaker {

template <class Char>
using String = std::basic_string<Char>;

// An ASCII text in the window's characters.
template <class Char>
String<Char> lit(std::string_view ascii) {
    return String<Char>(ascii.begin(), ascii.end());
}

template <class Char>
String<Char> num(long long v) {
    return lit<Char>(std::to_string(v));
}

// The leading decimal number of `s` (0 without one), as _wtoi/atoi read it.
template <class Char>
int to_int(const String<Char>& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    bool minus = i < s.size() && s[i] == '-';
    i += minus || (i < s.size() && s[i] == '+');
    long long v = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9' && v < 1000000000000LL; ++i)
        v = v * 10 + (s[i] - '0');
    return static_cast<int>(minus ? -v : v);
}

template <class Char>
std::vector<String<Char>> split(const String<Char>& s, Char sep) {
    std::vector<String<Char>> parts(1);
    for (Char c : s) {
        if (c == sep)
            parts.emplace_back();
        else
            parts.back() += c;
    }
    return parts;
}

// --- the project, as `dlcmaker info` prints it ---------------------------------------------------

template <class Char>
struct Record {
    String<Char> kind;
    std::map<String<Char>, String<Char>> f;
    String<Char> operator[](const String<Char>& k) const {
        auto it = f.find(k);
        return it == f.end() ? String<Char>() : it->second;
    }
};

// kind \t key=value \t ...; in values \\ \t \n are escaped.
template <class Char>
Record<Char> parse_record(const String<Char>& line) {
    Record<Char> r;
    std::vector<String<Char>> parts = split(line, Char('\t'));
    r.kind = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        size_t eq = parts[i].find(Char('='));
        if (eq == String<Char>::npos)
            continue;
        String<Char> v, raw = parts[i].substr(eq + 1);
        for (size_t j = 0; j < raw.size(); ++j) {
            if (raw[j] == Char('\\') && j + 1 < raw.size()) {
                Char e = raw[++j];
                v += e == Char('t') ? Char('\t') : e == Char('n') ? Char('\n') : e;
            } else {
                v += raw[j];
            }
        }
        r.f[parts[i].substr(0, eq)] = v;
    }
    return r;
}

template <class Char, class Item>
struct Song {
    Record<Char> r;
    std::vector<Record<Char>> charts, issues;
    Item item{};
};
template <class Char, class Item>
struct Dlc {
    Record<Char> r;
    std::vector<Song<Char, Item>> songs;
    std::vector<Record<Char>> issues; // the DLC's own (not its songs')
    Item item{};
};
template <class Char, class Item>
struct Model {
    String<Char> workspace;
    bool single = false; // the project folder is itself one DLC
    std::vector<Dlc<Char, Item>> dlcs;
    std::vector<Record<Char>> issues; // all of them
};

template <class Char, class Item>
Model<Char, Item> parse_model(const std::vector<String<Char>>& lines) {
    const auto L = lit<Char>;
    Model<Char, Item> m;
    for (const String<Char>& line : lines) {
        Record<Char> r = parse_record(line);
        if (r.kind == L("workspace")) {
            m.workspace = r[L("path")];
            m.single = r[L("single")] == L("1");
        } else if (r.kind == L("dlc")) {
            m.dlcs.push_back({r, {}, {}, {}});
        } else if (r.kind == L("song") && !m.dlcs.empty()) {
            m.dlcs.back().songs.push_back({r, {}, {}, {}});
        } else if (r.kind == L("chart") && !m.dlcs.empty() && !m.dlcs.back().songs.empty()) {
            m.dlcs.back().songs.back().charts.push_back(r);
        } else if (r.kind == L("issue")) {
            m.issues.push_back(r);
            for (auto& d : m.dlcs) {
                if (r[L("dlc")] != d.r[L("folder")])
                    continue;
                if (r[L("song")].empty())
                    d.issues.push_back(r);
                for (auto& s : d.songs)
                    if (r[L("song")] == s.r[L("folder")])
                        s.issues.push_back(r);
            }
        }
    }
    return m;
}

template <class Char>
int count(const std::vector<Record<Char>>& issues, std::string_view level) {
    int n = 0;
    for (const Record<Char>& r : issues)
        n += r[lit<Char>("level")] == lit<Char>(level);
    return n;
}

// "1 error, 2 warnings", or empty.
template <class Char>
String<Char> issue_summary(const std::vector<Record<Char>>& issues) {
    int e = count(issues, "error"), w = count(issues, "warning");
    String<Char> s;
    if (e)
        s += num<Char>(e) + lit<Char>(e == 1 ? " error" : " errors");
    if (w)
        s += lit<Char>(s.empty() ? "" : ", ") + num<Char>(w) + lit<Char>(w == 1 ? " warning" : " warnings");
    return s;
}

template <class Char>
String<Char> issue_suffix(const std::vector<Record<Char>>& issues) {
    String<Char> s = issue_summary(issues);
    return s.empty() ? s : lit<Char>("   (") + s + lit<Char>(")");
}

// --- the game's battle stages, as `dlcmaker stage-data` prints them -------------------------------

// An enemy list: up to 10 waves of up to 3 monsters (MonsterTable numbers); "boss:m,m|..." as text.
struct Wave {
    bool boss = false;
    std::vector<int> monsters;
    bool operator==(const Wave&) const = default;
};
constexpr int kWaves = 10, kWaveSize = 3, kDefaultStage = 481, kDefaultField = 482; // dlcmaker's template battle and field songs

template <class Char>
std::vector<Wave> parse_waves(const String<Char>& text) {
    std::vector<Wave> out;
    if (text.empty())
        return out;
    for (const String<Char>& part : split(text, Char('|'))) {
        size_t colon = part.find(Char(':'));
        if (colon == String<Char>::npos)
            continue;
        Wave w;
        w.boss = part.substr(0, colon) == lit<Char>("1");
        for (const String<Char>& m : split(part.substr(colon + 1), Char(',')))
            if (!m.empty())
                w.monsters.push_back(to_int(m));
        if (!w.monsters.empty())
            out.push_back(w);
    }
    return out;
}

template <class Char>
String<Char> waves_text(const std::vector<Wave>& waves) {
    String<Char> s;
    for (const Wave& w : waves) {
        s += lit<Char>(s.empty() ? "" : "|") + lit<Char>(w.boss ? "1:" : "0:");
        for (size_t i = 0; i < w.monsters.size(); ++i)
            s += lit<Char>(i ? "," : "") + num<Char>(w.monsters[i]);
    }
    return s;
}

template <class Char>
struct GameStage {
    int number = 0;
    String<Char> title, series, background, enemies;
    std::vector<Wave> waves;
};
template <class Char, class Picture>
struct GameMonster {
    String<Char> name, image;
    Picture picture{}; // the window's rendering of `image`, made when first shown
};
template <class Char, class Picture>
struct Stages {
    String<Char> game;               // the game folder they were read from
    bool loading = false;
    std::vector<GameStage<Char>> list;   // battle stages, in the game's series order
    std::vector<GameStage<Char>> fields; // field stages (no enemies), likewise
    std::map<int, GameMonster<Char, Picture>> monsters;
    const GameStage<Char>* find(int number, bool field = false) const {
        for (const GameStage<Char>& s : field ? fields : list)
            if (s.number == number)
                return &s;
        return nullptr;
    }
};

// Replaces the stages and monsters with the records in `lines` (the window frees its pictures first).
template <class Char, class Picture>
void parse_stages(Stages<Char, Picture>& out, const std::vector<String<Char>>& lines) {
    const auto L = lit<Char>;
    out.list.clear();
    out.fields.clear();
    out.monsters.clear();
    for (const String<Char>& line : lines) {
        Record<Char> r = parse_record(line);
        if (r.kind == L("monster"))
            out.monsters[to_int(r[L("id")])] = {r[L("name")], r[L("image")], {}};
        else if (r.kind == L("stage"))
            out.list.push_back({to_int(r[L("number")]), r[L("title")], r[L("series")], r[L("background")], r[L("enemies")],
                                parse_waves(r[L("waves")])});
        else if (r.kind == L("field"))
            out.fields.push_back({to_int(r[L("number")]), r[L("title")], r[L("series")], r[L("background")], {}, {}});
    }
}

// A line of `stage-data` that is one of its records, not a message.
template <class Char>
bool is_stage_record(const String<Char>& line) {
    for (std::string_view kind : {"stage\t", "field\t", "monster\t"})
        if (line.size() >= kind.size() && std::equal(kind.begin(), kind.end(), line.begin()))
            return true;
    return false;
}

// A battle song's stage: the game stage it starts from (the template's by default), and its
// background and enemies (that stage's unless the song sets its own). A field song's is the game
// field song whose field it walks (background, camera, sky).
template <class Char>
struct SongStage {
    int from = kDefaultStage;
    String<Char> background;
    std::vector<Wave> waves;
};

template <class Char, class Picture>
SongStage<Char> song_stage(const Record<Char>& r, const Stages<Char, Picture>& stages) {
    const auto L = lit<Char>;
    SongStage<Char> s;
    if (r[L("stage")] == L("field")) {
        s.from = r[L("stage_from")].empty() ? kDefaultField : to_int(r[L("stage_from")]);
        if (const GameStage<Char>* g = stages.find(s.from, true))
            s.background = g->background;
        return s;
    }
    const GameStage<Char>* by_pack = nullptr; // an older song.json names a game monster pack ("enemies")
    for (const GameStage<Char>& g : stages.list)
        if (!r[L("enemies")].empty() && g.enemies == r[L("enemies")] && !by_pack)
            by_pack = &g;
    if (!r[L("stage_from")].empty())
        s.from = to_int(r[L("stage_from")]);
    else if (by_pack)
        s.from = by_pack->number;
    const GameStage<Char>* g = stages.find(s.from);
    s.background = !r[L("background")].empty() ? r[L("background")] : g ? g->background : String<Char>();
    s.waves = parse_waves(r[L("enemy_waves")]);
    if (s.waves.empty())
        s.waves = by_pack ? by_pack->waves : g ? g->waves : std::vector<Wave>();
    return s;
}

// The series of `list`, in its order.
template <class Char>
std::vector<String<Char>> stage_series(const std::vector<GameStage<Char>>& list) {
    std::vector<String<Char>> out;
    for (const GameStage<Char>& s : list)
        if (std::find(out.begin(), out.end(), s.series) == out.end())
            out.push_back(s.series);
    return out;
}

// A series' backgrounds: (background, the first song of that series on it).
template <class Char>
std::vector<std::pair<String<Char>, String<Char>>> series_backgrounds(const std::vector<GameStage<Char>>& list,
                                                                      const String<Char>& series) {
    std::vector<std::pair<String<Char>, String<Char>>> out;
    for (const GameStage<Char>& s : list)
        if (s.series == series && std::none_of(out.begin(), out.end(), [&](auto& b) { return b.first == s.background; }))
            out.emplace_back(s.background, s.title);
    return out;
}

// Named as the background list names it: after the first song of the shown series on it.
template <class Char>
String<Char> background_name(const std::vector<GameStage<Char>>& list, const String<Char>& shown_series, const String<Char>& bg) {
    const auto L = lit<Char>;
    for (auto& [id, title] : series_backgrounds(list, shown_series))
        if (id == bg)
            return title + L(" (") + shown_series + L(")");
    for (const GameStage<Char>& s : list)
        if (s.background == bg)
            return s.title + L(" (") + s.series + L(")");
    return L("background ") + bg;
}

} // namespace dlcmaker
