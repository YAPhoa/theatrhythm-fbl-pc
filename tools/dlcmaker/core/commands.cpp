// SPDX-License-Identifier: GPL-2.0-or-later
#include "commands.h"

#include "build.h"
#include "chart.h"
#include "fsb.h"
#include "game.h"
#include "image.h"
#include "media.h"
#include "project.h"
#include "unity.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace dlcmaker {

namespace {

const char* const kUsage =
    "usage: theatrhythm-dlcmaker [COMMAND ARGS...]   (no command: the window)\n"
    "\n"
    "Theatrhythm FBL DLC maker: custom song packs from a folder of DLCs, songs and charts.\n"
    "\n"
    "    theatrhythm-dlcmaker new FOLDER            start a DLC folder with an example song\n"
    "    theatrhythm-dlcmaker check PATH            parse a DLC folder (or a workspace of them) and validate it\n"
    "    theatrhythm-dlcmaker build PATH --out DIR  build each DLC into DIR/<label>/\n"
    "    theatrhythm-dlcmaker install PATH --user USER   build into USER/addcont/ (the runner's save folder)\n"
    "\n"
    "    theatrhythm-dlcmaker extract GAMESONG FOLDER --game DIR   a game song as a song folder (private tests)\n"
    "\n"
    "The window also uses: info, set, add-file, new-dlc, new-song, stage-data.\n"
    "\n"
    "Options: --game DIR (the decrypted dump; default THEATRHYTHM_GAME_DIR), --ffmpeg PATH. See README.md\n"
    "for the layout.";

const char* const kAppUsage =
    "Commands behind the DLC maker window.\n"
    "\n"
    "    theatrhythm-dlcmaker info PATH                    the workspace as records (below)\n"
    "    theatrhythm-dlcmaker set FOLDER KEY=VALUE...      edit the dlc.json or song.json in FOLDER\n"
    "    theatrhythm-dlcmaker add-file FOLDER ROLE FILE    copy FILE into FOLDER as its audio, movie, jacket, ...\n"
    "    theatrhythm-dlcmaker new-dlc WORKSPACE NAME       a DLC folder with a dlc.json\n"
    "    theatrhythm-dlcmaker new-song DLC NAME [STAGE]    a song folder with a song.json\n"
    "    theatrhythm-dlcmaker stage-data GAME              the game's battle stages and monsters, for the stage editor\n"
    "\n"
    "`info` prints one record per line, UTF-8: a kind (workspace, dlc, song, chart, issue; stage and monster for\n"
    "stage-data), then tab-separated key=value fields; in values a backslash escapes itself, tab (\\t) and newline (\\n).";

void log_line(const std::string& s) { print(s); }

bool starts(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }
bool contains(std::string_view s, std::string_view p) { return s.find(p) != std::string_view::npos; }

std::string pad(int64_t v, int width) {
    std::string s = std::to_string(v);
    return s.size() < static_cast<size_t>(width) ? std::string(width - s.size(), '0') + s : s;
}

std::string strip(std::string_view s) {
    const char* ws = " \t\n\r\v\f";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string_view::npos)
        return "";
    return std::string(s.substr(a, s.find_last_not_of(ws) - a + 1));
}

std::string basename(const std::string& key) { return key.substr(key.rfind('/') + 1); }

// str(v) as a record or message shows it: None is empty.
std::string sv(const Value& v) { return v.is_null() ? "" : py_str(v); }
std::string sv(const fs::path& p) { return p.empty() ? "" : u8(p); }

// _num: numbers as f'{v:g}', None empty, anything else as text.
std::string num(const Value& v) {
    if (v.is_null())
        return "";
    return v.is_number() ? format_g(py_float(v)) : py_str(v);
}

using Fields = std::vector<std::pair<std::string, std::string>>;

void rec(const std::string& kind, const Fields& fields) {
    std::string line = kind;
    for (const auto& [k, v] : fields) {
        line += '\t' + k + '=';
        for (char c : v) {
            if (c == '\\')
                line += "\\\\";
            else if (c == '\t')
                line += "\\t";
            else if (c == '\n')
                line += "\\n";
            else if (c != '\r')
                line += c;
        }
    }
    print(line);
}

std::string json_file(const Value& v, bool ascii) {
    JsonStyle st;
    st.indent = 1;
    st.ensure_ascii = ascii;
    return json_dump(v, st) + "\n";
}

// Python's int(text) and float(text) (text already stripped).
std::optional<int64_t> parse_int(std::string text) {
    std::string clean;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '_' && i > 0 && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i - 1])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 1])))
            continue;
        clean += text[i];
    }
    if (!clean.empty() && clean[0] == '+')
        clean.erase(0, 1);
    int64_t v = 0;
    auto [p, ec] = std::from_chars(clean.data(), clean.data() + clean.size(), v);
    if (clean.empty() || ec != std::errc() || p != clean.data() + clean.size())
        return std::nullopt;
    return v;
}

std::optional<double> parse_float(std::string text) {
    std::string clean;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == 'x' || c == 'X' || c == '(')
            return std::nullopt;
        if (c == '_' && i > 0 && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i - 1])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 1])))
            continue;
        clean += c;
    }
    if (clean.empty())
        return std::nullopt;
    char* end = nullptr;
    double v = std::strtod(clean.c_str(), &end);
    if (end != clean.c_str() + clean.size())
        return std::nullopt;
    return v;
}

// --- check, build, install --------------------------------------------------------------------------

bool report(const std::vector<std::unique_ptr<project::Dlc>>& dlcs, const std::map<std::string, project::Ids>* ids,
            const project::Issues& issues) {
    for (const auto& d : dlcs) {
        const project::Ids* i = nullptr;
        if (ids && ids->count(d->label))
            i = &ids->at(d->label);
        std::string pack = i ? "DLCPACK" + pad(py_int(i->package), 7) : "pack id on build";
        std::string ser = truthy(d->series_base)
                              ? "series " + py_str(d->series_base) + " (base)"
                              : "series " + (i ? py_str(i->series) : std::string("new")) + " \"" + py_str(d->series_name) +
                                    "\" logo: " + (d->logo.empty() ? std::string("placeholder") : name_of(d->logo));
        print(d->label + "  \"" + py_str(d->name) + "\"  " + pack + "  " + ser);
        for (const auto& s : d->songs) {
            Value mid = i ? i->song(s->folder_name()) : Value();
            const fs::path& src = s->audio_from_video ? s->movie : s->audio;
            std::vector<std::string> files;
            if (s->clone)
                files.push_back("clone of game song " + std::to_string(*s->clone));
            else
                files.push_back("audio: " + (src.empty() ? std::string("-") : name_of(src)));
            if (s->stage == "event" && !s->clone)
                files.push_back("video: " + (s->movie.empty() ? std::string("-") : name_of(s->movie)));
            if (!s->clone || !s->jacket.empty())
                files.push_back("jacket: " + (s->jacket.empty() ? std::string("-") : name_of(s->jacket)));
            if (s->stage == "event" && (!s->clone || !s->thumbnail.empty()))
                files.push_back("thumbnail: " + (s->thumbnail.empty() ? std::string("-") : name_of(s->thumbnail)));
            std::string length = s->length_s ? format_fixed(*s->length_s, 1) + " s" : "";
            std::string title = s->title.is_str() ? s->title.s : py_str(s->title.get("default"));
            print("  " + s->folder_name() + "  " + (truthy(mid) ? "MUSIC" + pad(py_int(mid), 7) : "") + "  " + s->stage + "  \"" + title +
                  "\"  " + length);
            std::string joined;
            for (size_t n = 0; n < files.size(); ++n)
                joined += (n ? "  " : "") + files[n];
            print("      " + joined);
            std::string charts;
            for (const std::string& diff : s->charts.order) {
                const chart::Chart& c = s->charts.by_difficulty.at(diff);
                charts += (charts.empty() ? "" : " | ") + diff + " L" + py_str(c.level) + " " + std::to_string(c.notes.size()) + " notes";
            }
            print("      " + charts);
        }
    }
    for (const auto& it : issues.items) {
        std::string level;
        for (char c : it.level)
            level += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (level.size() < 8)
            level.append(8 - level.size(), ' ');
        print(level + it.where + ": " + it.msg);
    }
    print(std::to_string(issues.errors()) + " error(s), " + std::to_string(issues.warnings()) + " warning(s)");
    return issues.errors() == 0;
}

// (kind, id) used by packs already in addcont/ (not the ones being built).
std::vector<std::pair<std::string, int64_t>> installed(const fs::path& addcont, const std::set<std::string>& labels) {
    std::vector<std::pair<std::string, int64_t>> used;
    if (addcont.empty() || !is_dir(addcont))
        return used;
    for (const fs::path& p : sorted_dir(addcont)) {
        if (labels.count(name_of(p)) || !is_file(p / "dlcparam.json"))
            continue;
        std::string name = py_str(json_parse(read_text(p / "dlcparam.json")).at("PackageName"));
        if (starts(name, "DLCPACK"))
            used.emplace_back("package", std::stoll(name.substr(7)));
        const std::pair<const char*, std::pair<const char*, int64_t>> tables[] = {{"MusicTable", {"song", 160000000}},
                                                                                {"SeriesTable", {"series", 180000000}}};
        for (const auto& [table, kb] : tables) {
            fs::path f = p / to_path(std::string("table_") + table + ".bundle");
            if (!is_file(f))
                continue;
            unity::Bundle b = unity::Bundle::parse(read_file(f));
            for (auto& o : b.objects()) {
                if (o.class_id() != unity::kTextAsset)
                    continue;
                Value rows = msgpack_unpack(o.read().at("m_Script").s).at("LoadTableDataDictionary");
                for (const Item& it : rows.map)
                    used.emplace_back(kb.first, py_int(it.k) - kb.second);
            }
        }
    }
    return used;
}

bool game_running() { return process_running("theatrhythm-fbl"); } // the game, not this tool's window

int usage_error(const std::string& msg) {
    print_err("usage: theatrhythm-dlcmaker [-h] [--out OUT] [--user USER] [--game GAME] [--ffmpeg FFMPEG] {new,check,build,install} path");
    print_err("theatrhythm-dlcmaker: error: " + msg);
    return 2;
}

// Options as argparse takes them: "--name value" or "--name=value", anywhere.
struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    std::string error;
    bool help = false;
};

Args parse_args(const std::vector<std::string>& argv, const std::set<std::string>& names) {
    Args a;
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& s = argv[i];
        if (s == "-h" || s == "--help") {
            a.help = true;
            continue;
        }
        if (starts(s, "--")) {
            std::string name = s.substr(2), value;
            size_t eq = name.find('=');
            bool has = eq != std::string::npos;
            if (has)
                value = name.substr(eq + 1), name = name.substr(0, eq);
            if (!names.count(name) && name != "runner") {
                a.error = "unrecognized arguments: " + s;
                return a;
            }
            if (!has) {
                if (i + 1 >= argv.size()) {
                    a.error = "argument --" + name + ": expected one argument";
                    return a;
                }
                value = argv[++i];
            }
            a.opt[name] = value;
            continue;
        }
        a.pos.push_back(s);
    }
    return a;
}

std::string opt(const Args& a, const std::string& name) {
    auto it = a.opt.find(name);
    return it == a.opt.end() ? "" : it->second;
}

int new_folder_cmd(const fs::path& folder) {
    if (dlcmaker::exists(folder) && !sorted_dir(folder).empty())
        throw Exit(u8(folder) + " is not empty");
    fs::path song = folder / "My Song";
    make_dirs(song);
    Value dlc = Value::of_map(), series = Value::of_map();
    dlc["name"] = name_of(folder);
    series["name"] = name_of(folder);
    dlc["series"] = series;
    JsonStyle st;
    st.indent = 1;
    write_file(folder / "dlc.json", json_dump(dlc, st));
    Value cfg = Value::of_map();
    cfg["title"] = "My Song";
    cfg["sort_name"] = "my song";
    cfg["stage"] = "battle";
    cfg["start"] = 0.0;
    cfg["end"] = Value();
    cfg["fade_out"] = 1.0;
    cfg["preview"] = Value::of_list({30.0, 45.0});
    write_file(song / "song.json", json_dump(cfg, st));
    auto note = [](std::initializer_list<std::pair<const char*, Value>> kv) {
        Value n = Value::of_map();
        for (const auto& [k, v] : kv)
            n[k] = v;
        return n;
    };
    const std::tuple<const char*, int, Value> charts[] = {
        {"basic", 2,
         Value::of_list({note({{"beat", 4}, {"type", "tap"}, {"lane", 1}}), note({{"beat", 6}, {"type", "hold"}, {"lane", 1}, {"end_beat", 8}})})},
        {"expert", 5,
         Value::of_list({note({{"beat", 4}, {"type", "tap"}, {"lane", 1}}), note({{"beat", 5}, {"type", "slide"}, {"lane", 2}, {"dir", 90}})})},
        {"ultimate", 9,
         Value::of_list({note({{"beat", 4}, {"type", "twin"}, {"lane", 1}, {"dir", 270}, {"dir2", 90}, {"critical", true}})})}};
    for (const auto& [d, lv, notes] : charts) {
        Value c = Value::of_map();
        c["level"] = lv;
        c["bpm"] = 120;
        c["offset_ms"] = 0;
        c["notes"] = notes;
        write_file(song / to_path(std::string(d) + ".json"), json_dump(c, st));
    }
    print("created " + u8(folder) + ": add logo.png next to dlc.json; in \"My Song\" add the audio (mp3...), jacket.png and charts");
    return 0;
}

int project_main(const std::vector<std::string>& argv) {
    Args a = parse_args(argv, {"out", "user", "game", "ffmpeg"});
    if (a.help) {
        print(kUsage);
        return 0;
    }
    if (!a.error.empty())
        return usage_error(a.error);
    if (a.pos.size() < 2)
        return usage_error("the following arguments are required: " + std::string(a.pos.empty() ? "command, path" : "path"));
    if (a.pos.size() > 2)
        return usage_error("unrecognized arguments: " + a.pos[2]);
    const std::string& command = a.pos[0];
    if (command != "new" && command != "check" && command != "build" && command != "install")
        return usage_error("argument command: invalid choice: '" + command + "' (choose from 'new', 'check', 'build', 'install')");
    fs::path path = py_path(a.pos[1]);
    if (command == "new")
        return new_folder_cmd(path);
    project::Discovered found = project::discover(path);
    auto& dlcs = found.dlcs;
    project::Issues& issues = found.issues;
    std::string ffmpeg = media::find_ffmpeg(opt(a, "ffmpeg"));
    for (auto& d : dlcs) // decode audio (cached) so charts can be checked against the song's length
        for (auto& s : d->songs) {
            if ((s->audio.empty() && s->movie.empty()) || s->clone)
                continue;
            try {
                print(d->label + "/" + s->folder_name());
                build::prepare_song(*s, ffmpeg, d->folder / ".dlcmaker-cache" / s->folder.filename(), log_line, command != "check");
                s->check_charts(round_half_even(*s->length_s * 1000), issues);
            } catch (const media::ProcessError& e) {
                issues.error(s->where, std::string("ffmpeg failed: ") + e.what());
            }
        }
    fs::path addcont = a.opt.count("user") ? py_path(a.opt["user"]) / "addcont" : fs::path();
    std::optional<std::map<std::string, project::Ids>> ids;
    try {
        std::set<std::string> labels;
        for (auto& d : dlcs)
            labels.insert(d->label);
        ids = project::assign_ids(dlcs, installed(addcont, labels));
    } catch (const Exit& e) {
        issues.error("ids", e.what());
    }
    bool ok = report(dlcs, ids ? &*ids : nullptr, issues);
    if (command == "check" || !ok)
        return ok ? 0 : 1;
    fs::path out_root;
    if (command == "install") {
        if (!a.opt.count("user"))
            return usage_error("install needs --user (the runner save folder holding addcont/)");
        if (game_running())
            throw Exit("the game is running: close it first (its add-on files are in use)");
        out_root = addcont;
        make_dirs(out_root);
    } else {
        if (!a.opt.count("out"))
            return usage_error("build needs --out DIR");
        out_root = py_path(a.opt["out"]);
    }
    Game game(opt(a, "game"));
    for (auto& d : dlcs) {
        print("building " + d->label);
        fs::path out = build::build(game, *d, ids->at(d->label), out_root, log_line);
        d->save_lock(ids->at(d->label));
        print("  -> " + u8(out));
    }
    return 0;
}

// --- the window's commands ----------------------------------------------------------------------------

struct Role {
    const char* name;
    const char* stem;
    const std::vector<std::string>* exts;
};
const Role kRoles[] = {{"audio", "audio", &media::AUDIO_EXT},        {"movie", "movie", &media::VIDEO_EXT},
                       {"jacket", "jacket", &project::IMAGE_EXT},    {"thumbnail", "thumbnail", &project::IMAGE_EXT},
                       {"logo", "logo", &project::IMAGE_EXT},        {"logo_wide", "logo_wide", &project::IMAGE_EXT}};
const std::set<std::string> kNumbers = {"start", "end", "fade_out", "chart_offset_ms", "preview_start", "preview_end"};
const std::set<std::string> kBools = {"audio_from_video"};
const std::set<std::string> kInts = {"enemies", "stage_from"}; // a monster pack ID, a game song; "background" stays text

std::string waves_or_empty(const Value& waves) {
    try {
        return truthy(waves) ? project::waves_text(waves) : "";
    } catch (const ValueError&) {
        return "";
    }
}

// The song's length from the audio an earlier check or build decoded, if any (s).
std::optional<double> cached_length(const project::Song& song) {
    fs::path wav = song.dlc->folder / ".dlcmaker-cache" / song.folder.filename() / "song.wav";
    if (!is_file(wav))
        return std::nullopt;
    return media::wav_seconds(wav);
}

// The song's length from its audio source's header and the start/end cut (s), before any decode.
std::optional<double> probed_length(const project::Song& song, const std::optional<std::string>& ffmpeg) {
    const fs::path& src = song.audio_from_video ? song.movie : song.audio;
    std::optional<double> total;
    if (!src.empty() && ffmpeg)
        total = media::duration(*ffmpeg, src);
    if (!total)
        return std::nullopt;
    double end = song.end.is_null() ? *total : std::min(py_float(song.end), *total);
    return std::max(0.0, end - song.start);
}

int info(const std::string& arg) {
    fs::path path = resolve(py_path(arg));
    std::optional<std::string> ffmpeg = media::try_ffmpeg();
    project::Discovered found = project::discover(path);
    auto& dlcs = found.dlcs;
    project::Issues& issues = found.issues;
    std::map<std::string, std::pair<fs::path, fs::path>> owners; // issue "where" -> (dlc folder, song folder)
    for (auto& d : dlcs) {
        owners[name_of(d->folder)] = {d->folder, fs::path()};
        for (auto& s : d->songs) {
            owners[s->where] = {d->folder, s->folder};
            s->length_s = cached_length(*s);
            if (s->length_s && *s->length_s)
                s->check_charts(round_half_even(*s->length_s * 1000), issues); // chart checks need the decoded audio
            else if (!s->clone)
                s->length_s = probed_length(*s, ffmpeg);
        }
    }
    rec("workspace", {{"path", u8(path)}, {"single", is_file(path / "dlc.json") ? "1" : "0"}});
    for (auto& d : dlcs) {
        const Value& c = d->config;
        Value s_ = c.get("series", Value::of_map());
        rec("dlc", {{"folder", u8(d->folder)},
                    {"name", sv(c.get("name", ""))},
                    {"label", sv(c.get("label", ""))},
                    {"auto_label", d->label},
                    {"series", sv(s_.get("name", ""))},
                    {"logo", sv(d->logo)},
                    {"logo_wide", sv(d->logo_wide)},
                    {"package", sv(c.get("package", d->lock.get("package", "")))},
                    {"series_id", sv(s_.get("id", d->lock.get("series", "")))}});
        for (auto& sp : d->songs) {
            const project::Song& s = *sp;
            const Value& sc = s.config;
            Value pv = truthy(sc.get("preview")) ? sc.get("preview") : Value::of_list({Value(), Value()});
            Value pv0 = pv.list.size() > 0 ? pv.list[0] : Value(), pv1 = pv.list.size() > 1 ? pv.list[1] : Value();
            std::string title = s.title.is_str() ? s.title.s : sv(s.title.get("default", ""));
            Value ct = sc.get("title", "");
            std::optional<double> cached = cached_length(s);
            rec("song", {{"dlc", u8(d->folder)},
                         {"folder", u8(s.folder)},
                         {"title", ct.is_str() ? ct.s : title},
                         {"sort_name", sv(sc.get("sort_name", ""))},
                         {"auto_sort_name", s.sort_name},
                         {"stage", s.stage},
                         {"start", num(sc.get("start"))},
                         {"end", num(sc.get("end"))},
                         {"fade_out", num(sc.get("fade_out"))},
                         {"preview_start", num(pv0)},
                         {"preview_end", num(pv1)},
                         {"copyright", sv(sc.get("copyright", ""))},
                         {"auto_preview_start", truthy(s.preview) ? num(s.preview.list.at(0)) : ""},
                         {"auto_preview_end", truthy(s.preview) ? num(s.preview.list.at(1)) : ""},
                         {"chart_offset_ms", num(sc.get("chart_offset_ms"))},
                         {"audio", sv(s.audio)},
                         {"movie", sv(s.movie)},
                         {"jacket", sv(s.jacket)},
                         {"thumbnail", sv(s.thumbnail)},
                         {"length", s.length_s && *s.length_s ? format_g(round_to(*s.length_s, 1)) : ""},
                         {"id", sv(sc.get("id", d->lock.get("songs", Value::of_map()).get(s.folder_name(), "")))},
                         {"clone", s.clone ? format_g(static_cast<double>(*s.clone)) : ""},
                         {"audio_from_video", s.audio_from_video ? "1" : "0"},
                         {"checked", cached ? "1" : "0"},
                         {"stage_from", num(sc.get("stage_from"))},
                         {"background", sv(sc.get("background", ""))},
                         {"enemies", num(sc.get("enemies"))},
                         {"enemy_waves", waves_or_empty(sc.get("enemy_waves"))}});
            if (s.clone)
                for (size_t n = 0; n < 3; ++n)
                    rec("chart", {{"song", u8(s.folder)},
                                  {"difficulty", chart::DIFFICULTIES[n]},
                                  {"level", ""},
                                  {"notes", ""},
                                  {"bpm", ""},
                                  {"file", "game song " + std::to_string(*s.clone)}});
            for (const std::string& diff : chart::DIFFICULTIES) {
                auto it = s.charts.by_difficulty.find(diff);
                if (it == s.charts.by_difficulty.end())
                    continue;
                const chart::Chart& ch = it->second;
                rec("chart", {{"song", u8(s.folder)},
                              {"difficulty", diff},
                              {"level", num(ch.level)},
                              {"notes", std::to_string(ch.notes.size())},
                              {"bpm", num(ch.bpm)},
                              {"file", ch.source.empty() ? "" : name_of(ch.source)}});
            }
        }
    }
    for (const auto& it : issues.items) {
        size_t cut = it.where.find(" [");
        std::string base = it.where.substr(0, cut), diff = cut == std::string::npos ? "" : it.where.substr(cut + 2);
        while (!diff.empty() && diff.back() == ']')
            diff.pop_back();
        auto o = owners.find(base);
        rec("issue", {{"level", it.level},
                      {"where", it.where},
                      {"message", it.msg},
                      {"dlc", o == owners.end() ? "" : sv(o->second.first)},
                      {"song", o == owners.end() ? "" : sv(o->second.second)},
                      {"difficulty", diff}});
    }
    return 0;
}

fs::path config_file(const fs::path& folder) {
    for (const char* name : {"song.json", "dlc.json"})
        if (is_file(folder / name))
            return folder / name;
    throw Exit(u8(folder) + ": no song.json or dlc.json");
}

void save(const fs::path& path, const Value& config) { write_file(path, json_file(config, false)); }

int set_fields(const std::string& folder_arg, const std::vector<std::string>& pairs) {
    fs::path f = config_file(py_path(folder_arg));
    Value c = json_parse(read_text(f));
    std::optional<std::string> stage;
    for (const std::string& pair : pairs) {
        size_t eq = pair.find('=');
        if (pair.substr(0, eq) == "stage")
            stage = eq == std::string::npos ? "" : pair.substr(eq + 1);
    }
    std::string old_stage = py_str(c.get("stage", "battle"));
    if (stage && !stage->empty() && *stage != old_stage) // a battle stage's settings mean nothing on a field, and back
        for (const char* key : {"stage_from", "background", "enemy_waves", "enemies"}) {
            const Value* v = c.find(key);
            bool was = v && !v->is_null();
            if (v)
                c.erase(key);
            if (was)
                print(std::string("note: ") + key + " cleared (it was for a " + py_str(c.get("stage", "battle")) + " stage)");
        }
    for (const std::string& pair : pairs) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos)
            throw Exit("expected KEY=VALUE: " + pair);
        std::string key = pair.substr(0, eq), text = strip(pair.substr(eq + 1));
        Value value(text);
        if (kBools.count(key) && !text.empty()) {
            std::string low = ascii_lower(text);
            if (low != "true" && low != "false" && low != "1" && low != "0")
                throw Exit(key + ": true or false, not " + py_repr(Value(text)));
            value = low == "true" || low == "1";
        } else if (key == "enemy_waves" && !text.empty()) {
            try {
                value = project::parse_waves(text);
            } catch (const ValueError& e) {
                throw Exit(std::string("enemy_waves: ") + e.what());
            }
        } else if (kInts.count(key) && !text.empty()) {
            auto v = parse_int(text);
            if (!v)
                throw Exit(key + ": not a whole number: " + py_repr(Value(text)));
            value = *v;
        } else if (kNumbers.count(key) && !text.empty()) {
            auto v = parse_float(text);
            if (!v)
                throw Exit(key + ": not a number: " + py_repr(Value(text)));
            if (key == "chart_offset_ms" && std::isfinite(*v) && *v == std::floor(*v))
                value = static_cast<int64_t>(*v);
            else
                value = *v;
        }
        bool empty = value.is_str() && value.s.empty();
        if (key == "preview_start" || key == "preview_end") {
            Value old = c.get("preview");
            Value pv = truthy(old) && old.is_list() ? old : Value::of_list({Value(), Value()});
            while (pv.list.size() < 2)
                pv.list.push_back(Value());
            pv.list[key == "preview_end"] = empty ? Value() : value;
            if (pv.list.size() == 2 && pv.list[0].is_null() && pv.list[1].is_null())
                c.erase("preview");
            else
                c["preview"] = pv;
            continue;
        }
        Value* target = &c;
        std::string name = key;
        if (starts(key, "series.")) {
            if (!c.has("series"))
                c["series"] = Value::of_map();
            target = &c.at("series");
            name = key.substr(7);
        }
        if (empty)
            target->erase(name);
        else
            (*target)[name] = value;
    }
    const Value* pv = c.find("preview");
    if (pv && pv->is_list() && std::any_of(pv->list.begin(), pv->list.end(), [](const Value& v) { return v.is_null(); }))
        print("note: the preview needs both a start and an end; until then the default one is used");
    save(f, c);
    print("saved " + u8(f));
    return 0;
}

int add_file(const std::string& folder_arg, const std::string& role, const std::string& src_arg) {
    fs::path folder = py_path(folder_arg), src = py_path(src_arg);
    if (!is_file(src))
        throw Exit(u8(src) + ": not a file");
    if (role == "chart") {
        if (ascii_lower(suffix_of(src)) != ".json")
            throw Exit("a chart is a .json file (charts.json, or basic/expert/ultimate/supreme.json)");
        Value c;
        try {
            c = json_parse(read_text(src));
        } catch (const ValueError& e) {
            throw Exit(name_of(src) + ": not valid JSON: " + e.what());
        }
        std::string stem = ascii_lower(stem_of(src)), name;
        if (c.is_map() && c.has("difficulties"))
            name = "charts.json";
        else if (std::find(chart::DIFFICULTIES.begin(), chart::DIFFICULTIES.end(), stem) != chart::DIFFICULTIES.end())
            name = stem + ".json";
        else
            throw Exit(name_of(src) + ": name it basic/expert/ultimate/supreme.json, or use one charts.json with \"difficulties\"");
        dlcmaker::copy_file(src, folder / to_path(name));
        print("copied " + name_of(src) + " -> " + u8(folder / to_path(name)));
        return 0;
    }
    const Role* r = nullptr;
    for (const Role& x : kRoles)
        if (role == x.name)
            r = &x;
    if (!r)
        throw Exit("unknown role " + py_repr(Value(role)) + " (one of audio, movie, jacket, thumbnail, logo, logo_wide, chart)");
    std::string ext = ascii_lower(suffix_of(src));
    if (std::find(r->exts->begin(), r->exts->end(), ext) == r->exts->end()) {
        std::string all;
        for (const std::string& e : *r->exts)
            all += (all.empty() ? "" : " ") + e;
        throw Exit(name_of(src) + ": a " + role + " must be one of " + all);
    }
    fs::path dest = folder / to_path(r->stem + ext);
    if (!same_file(src, dest)) {
        fs::path tmp = folder / to_path(name_of(dest) + ".part");
        print("copying " + name_of(src) + " (" + format_fixed(static_cast<double>(dlcmaker::file_size(src)) / 1e6, 1) + " MB)");
        dlcmaker::copy_file(src, tmp);
        for (const std::string& e : *r->exts) { // the folder keeps one file per role
            fs::path old = folder / to_path(r->stem + e);
            if (is_file(old) && old != dest)
                remove_file(old);
        }
        replace_file(tmp, dest);
    }
    fs::path f = config_file(folder);
    Value c = json_parse(read_text(f));
    Value* names = name_of(f) == "dlc.json" ? c.find("series") : &c;
    if (names && names->is_map() && names->has(role)) { // an explicit file name would win over the new file
        names->erase(role);
        save(f, c);
    }
    print(role + ": " + u8(dest));
    return 0;
}

fs::path new_folder(const std::string& parent, const std::string& raw) {
    std::string name = strip(raw);
    bool bad = name.empty() || name == "." || name == "..";
    for (unsigned char ch : name)
        if (ch < 0x20 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(ch)) != std::string_view::npos)
            bad = true;
    if (bad)
        throw Exit(py_repr(Value(name)) + " cannot be a folder name");
    fs::path folder = py_path(parent) / to_path(name);
    if (dlcmaker::exists(folder))
        throw Exit(u8(folder) + " already exists");
    make_dirs(folder);
    return folder;
}

int new_dlc(const std::string& workspace, const std::string& name) {
    if (is_file(py_path(workspace) / "dlc.json"))
        throw Exit(workspace + " is itself a DLC: open the folder above it to add another");
    fs::path folder = new_folder(workspace, name);
    Value c = Value::of_map(), series = Value::of_map();
    c["name"] = strip(name);
    series["name"] = strip(name);
    c["series"] = series;
    save(folder / "dlc.json", c);
    print("created " + u8(folder));
    return 0;
}

int new_song(const std::string& dlc, const std::string& name, const std::string& stage) {
    if (!is_file(py_path(dlc) / "dlc.json"))
        throw Exit(dlc + ": not a DLC folder (no dlc.json)");
    if (std::find(project::STAGES.begin(), project::STAGES.end(), stage) == project::STAGES.end())
        throw Exit("stage must be one of battle, event, field");
    fs::path folder = new_folder(dlc, name);
    std::string sort;
    for (char ch : ascii_lower(name))
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == ' ')
            sort += ch;
    sort = strip(sort);
    Value c = Value::of_map();
    c["title"] = strip(name);
    c["sort_name"] = sort;
    c["stage"] = stage;
    c["start"] = 0.0;
    c["end"] = Value();
    c["fade_out"] = 1.0;
    save(folder / "song.json", c);
    print("created " + u8(folder));
    return 0;
}

// --- the game's own assets --------------------------------------------------------------------------

// (typetree, stream bytes) of the Texture2D in the bundle holding `key`.
std::pair<Value, std::string> texture_of(const Game& game, const Key& key) {
    unity::Bundle b = unity::Bundle::parse(game.read_bundle(game.bundle_path(key)));
    for (auto& o : b.objects()) {
        if (o.class_id() != unity::kTexture2D)
            continue;
        Value t = o.read();
        const Value& sd = t.at("m_StreamData");
        std::string data = b.node_ending(".resS").data.substr(static_cast<size_t>(py_int(sd.at("offset"))), static_cast<size_t>(py_int(sd.at("size"))));
        return {t, data};
    }
    throw ValueError("no Texture2D in " + key.s);
}

image::Image game_image(const Game& game, const Key& key) {
    auto [t, data] = texture_of(game, key);
    return image::decode_texture(data, static_cast<int>(py_int(t.at("m_Width"))), static_cast<int>(py_int(t.at("m_Height"))),
                                 static_cast<int>(py_int(t.at("m_TextureFormat"))));
}

// The number in `k` after `pre`: 7 digits followed by `tail` ("#" in it: the digits again), ending `k`
// when `anchored` (the regexes earlier (Python) releases matched keys with).
std::optional<int64_t> match7(const std::string& k, std::string_view pre, std::string_view tail, bool anchored) {
    for (size_t at = k.find(pre); at != std::string::npos; at = k.find(pre, at + 1)) {
        size_t d = at + pre.size();
        if (d + 7 > k.size() || !std::all_of(k.begin() + d, k.begin() + d + 7, [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        std::string digits = k.substr(d, 7), want;
        for (char c : tail)
            want += c == '#' ? digits : std::string(1, c);
        if (anchored ? k.size() == d + 7 + want.size() && k.compare(d + 7, want.size(), want) == 0 : k.compare(d + 7, want.size(), want) == 0)
            return std::stoll(digits);
    }
    return std::nullopt;
}

bool all_digits(const std::string& s) {
    return !s.empty() && s.size() < 19 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

struct StageData {
    Value stages, fields, monsters;
};

// The base game's battle stages a song can start from, its field stages, and the monsters an enemy list
// can hold. A stage counts only when its monsters, background, sky box and camera effect are all in the
// base game (not a DLC's); stages come in the game's series order. Monster pictures (their collection
// cards) are written to `images`.
StageData read_stage_data(Game& game, const fs::path& images) {
    const CatalogBase& cat = game.base();
    std::set<int64_t> bgs, fbgs, skies, effects, have;
    for (const Key& key : cat.keys) {
        if (key.is_int || !starts(key.s, P))
            continue;
        const std::string& k = key.s;
        if (auto v = match7(k, "/BMSBG/BMSBG", "/BMSBG#.prefab", true))
            bgs.insert(*v);
        if (auto v = match7(k, "/FMSBG/FMSBG", "/FMSBG#.prefab", true))
            fbgs.insert(*v);
        if (auto v = match7(k, "/FMSBGSkyBox/FMSBGSB", ".mat", true))
            skies.insert(*v);
        if (auto v = match7(k, "/FMSBGCameraEffect/FMSBGCE", "/", false))
            effects.insert(*v);
        if (auto v = match7(k, "/Monster/MONSTER", "/", false))
            have.insert(*v);
    }
    const Table& music = game.table("MusicTable");
    const Table& packs = game.table("MonsterPackTable");
    const Table& series = game.table("SeriesTable");
    Value titles = game.texts("Table/MusicTable"), series_names = game.texts("Table/Table_Series");
    using Ordered = std::pair<std::pair<double, double>, Value>;
    std::vector<Ordered> stages, fields;
    for (const auto& [mid, r] : music.rows) {
        int64_t n = mid - 160000000;
        Value title = titles.get("Table/MusicTable/MusicName/MUSIC" + pad(n, 7));
        int64_t sid = py_int(r.at("SeriesID")) - 180000000;
        const Value* srow = series.get(py_int(r.at("SeriesID")));
        std::pair<double, double> order = {srow ? py_float(srow->get("SortOrder", 9999)) : 9999.0, py_float(r.at("SortOrder"))};
        Value info = Value::of_map();
        info["number"] = n;
        info["title"] = title;
        info["series"] = series_names.get("Table/Table_Series/SeriesName/SERIES" + pad(sid, 7), "");
        info["background"] = r.at("BackGroundIDValue");
        std::string bg = py_str(r.at("BackGroundIDValue"));
        int64_t ms_type = py_int(r.at("MSType"));
        if (ms_type == 0 && truthy(title) && all_digits(bg) && fbgs.count(std::stoll(bg) - 200000000)) {
            int64_t sky = py_int(r.at("FMSBGSkyBoxID")), fx = py_int(r.at("FMSBGCameraEffectID"));
            if ((sky == project::NONE || skies.count(sky - 610000000)) && (fx == project::NONE || effects.count(fx - 600000000)))
                fields.emplace_back(order, info);
        }
        const Value* pack = packs.get(py_int(r.at("MonsterPackID")));
        if (ms_type != 1 || !truthy(title) || !pack || !truthy(*pack) || !all_digits(bg))
            continue;
        if (!bgs.count(std::stoll(bg) - 190000000))
            continue;
        Value waves = project::waves_of(*pack);
        bool missing = false;
        for (const Value& w : waves.list)
            for (const Value& m : w.at("monsters").list)
                missing = missing || !have.count(py_int(m));
        if (!truthy(waves) || missing)
            continue;
        Value st = info;
        st["enemies"] = r.at("MonsterPackID");
        st["waves"] = waves;
        stages.emplace_back(order, st);
    }
    auto by_order = [](const Ordered& a, const Ordered& b) { return a.first < b.first; };
    std::stable_sort(stages.begin(), stages.end(), by_order);
    std::stable_sort(fields.begin(), fields.end(), by_order);
    StageData out{Value::of_list(), Value::of_list(), Value::of_map()};
    for (auto& s : stages)
        out.stages.list.push_back(std::move(s.second));
    for (auto& s : fields)
        out.fields.list.push_back(std::move(s.second));
    // Monsters: every one with its model in the base game, named as the game names them; colour variants
    // that share a name are numbered in the order of their numbers.
    Value names = game.texts("Table/Table_Monster");
    std::map<int64_t, int64_t> cards;
    for (const auto& [k, v] : game.table("CollecaTable").rows)
        for (const Value& m : v.at("MonsterID").list)
            if (py_int(m) != project::NONE)
                cards.emplace(py_int(m) - 250000000, k - 300000000);
    std::map<int64_t, std::pair<std::string, std::string>> monsters; // number -> (name, image)
    std::map<std::string, std::vector<int64_t>> same;
    for (int64_t m : have) {
        Value name = names.get("Table/Table_Monster/MonsterName/MONSTER" + pad(m, 7));
        if (!truthy(name))
            continue;
        monsters[m] = {py_str(name), ""};
        same[py_str(name)].push_back(m);
    }
    for (const auto& [name, ms] : same)
        if (ms.size() > 1)
            for (size_t i = 0; i < ms.size(); ++i)
                monsters[ms[i]].first = name + " " + std::to_string(i + 1);
    make_dirs(images);
    for (auto& [m, info] : monsters) {
        auto card = cards.find(m);
        if (card == cards.end())
            continue;
        std::string key;
        for (const char* d : {"CollecaS", "CollecaL"}) {
            std::string k = std::string(P) + "Textures/" + d + "/COLLECA" + pad(card->second, 7) + ".png";
            if (key.empty() && cat.has(Key(k)))
                key = k;
        }
        if (key.empty())
            continue;
        image::Image im = game_image(game, Key(key));
        int box[4];
        if (image::bbox(im, box))
            im = image::crop(im, box[0], box[1], box[2], box[3]);
        im = image::thumbnail(im, 128);
        for (size_t p = 0; p < im.rgba.size(); p += 4) { // on white, as Pillow pastes with the alpha as mask
            unsigned a = im.rgba[p + 3];
            for (size_t ch = 0; ch < 3; ++ch) {
                unsigned tmp = 255 * (255 - a) + im.rgba[p + ch] * a + 128;
                im.rgba[p + ch] = static_cast<uint8_t>(((tmp >> 8) + tmp) >> 8);
            }
            im.rgba[p + 3] = 255;
        }
        fs::path file = images / to_path("monster" + pad(m, 4) + ".png");
        write_file(file, image::png(im, false));
        info.second = u8(file);
    }
    for (const auto& [m, info] : monsters) {
        Value v = Value::of_map();
        v["name"] = info.first;
        v["image"] = info.second;
        out.monsters[std::to_string(m)] = v;
    }
    return out;
}

// Records for the stage editor: one `stage` per game battle stage (waves as "boss:monsters|..."), one
// `field` per game field stage, one `monster` per monster. Read once per game dump and kept in the temp
// folder.
int stage_data(const std::string& game_dir) {
    Game game(game_dir);
    std::string stamp = u8(resolve(game.dir)) + "|" + std::to_string(mtime(game.aa / "catalog.json").ns()) + "|2";
    fs::path cache = temp_dir() / "dlcmaker" / to_path("stages-" + sha1_hex(stamp).substr(0, 12));
    Value data;
    try {
        if (is_file(cache / "stages.json"))
            data = json_parse(read_text(cache / "stages.json"));
    } catch (const ValueError&) {
        data = Value();
    }
    bool fresh = truthy(data) && data.is_map() && data.has("monsters");
    if (fresh)
        for (const Item& it : data.at("monsters").map) {
            std::string image = sv(it.v.get("image", ""));
            if (!image.empty() && !is_file(to_path(image)))
                fresh = false;
        }
    if (!fresh) {
        print("reading the game's stages and monsters (the first time only)");
        StageData sd = read_stage_data(game, cache);
        data = Value::of_map();
        data["stages"] = sd.stages;
        data["fields"] = sd.fields;
        data["monsters"] = sd.monsters;
        JsonStyle st;
        st.ensure_ascii = false;
        write_file(cache / "stages.json", json_dump(data, st));
    }
    for (const Item& it : data.at("monsters").map)
        rec("monster", {{"id", py_str(it.k)}, {"name", sv(it.v.get("name"))}, {"image", sv(it.v.get("image"))}});
    for (const Value& s : data.at("stages").list)
        rec("stage", {{"number", sv(s.get("number"))},
                      {"title", sv(s.get("title"))},
                      {"series", sv(s.get("series"))},
                      {"enemies", sv(s.get("enemies"))},
                      {"background", sv(s.get("background"))},
                      {"waves", project::waves_text(s.at("waves"))}});
    for (const Value& s : data.at("fields").list)
        rec("field", {{"number", sv(s.get("number"))}, {"title", sv(s.get("title"))}, {"series", sv(s.get("series"))}, {"background", sv(s.get("background"))}});
    return 0;
}

// A game song as an ordinary song folder: its audio (audio.ogg, from FMOD's FSB5 Vorbis), charts
// (charts.json, from the TriggerFiles), event video (movie.mp4), jacket and thumbnail (png), and a
// song.json with its preview and stage. For private tests and as a starting point: the folder then holds
// the game's own files, so it is never something to share.
int extract(Game& game, int64_t number, const fs::path& folder, const std::string& title_arg) {
    const CatalogBase& cat = game.base();
    std::string src = "MUSIC" + pad(number, 7);
    const Value* found = game.table("MusicTable").get(160000000 + number);
    if (!found || !truthy(*found))
        throw Exit("game song " + std::to_string(number) + " not found");
    Value row = found->is_list() ? found->list.at(0) : *found;
    std::vector<std::string> keys;
    for (const Key& k : cat.keys)
        if (!k.is_int && starts(k.s, P) && (contains(k.s, "/" + src + "/") || contains(k.s, "/" + src + ".") || contains(k.s, "/" + src + "_")))
            keys.push_back(k.s);
    std::vector<std::string> triggers;
    for (const std::string& k : keys)
        if (contains(k, "/Music/") && starts(basename(k), "trigger"))
            triggers.push_back(k);
    std::sort(triggers.begin(), triggers.end());
    auto script = [&](const std::string& key) {
        unity::Bundle b = unity::Bundle::parse(game.read_bundle(game.bundle_path(Key(key))));
        for (auto& o : b.objects())
            if (o.class_id() == unity::kTextAsset)
                return o.read().at("m_Script").s;
        throw ValueError("no TextAsset in " + key);
    };
    std::vector<std::pair<std::string, std::pair<Value, std::vector<Value>>>> charts; // in first-seen order
    std::string stage;
    for (const std::string& k : triggers) {
        std::string base = basename(k);
        std::string diff = base.size() >= 9 ? chart::difficulty_of_code(base.substr(7, 2)) : "";
        if (diff.empty())
            continue;
        size_t index = static_cast<size_t>(std::find(chart::DIFFICULTIES.begin(), chart::DIFFICULTIES.end(), diff) - chart::DIFFICULTIES.begin());
        Value level = row.at("DifficultyValue").list.at(index);
        if (!truthy(level))
            continue;
        auto [st, notes] = chart::read_trigger(script(k));
        stage = st;
        auto it = std::find_if(charts.begin(), charts.end(), [&](const auto& c) { return c.first == diff; });
        if (it == charts.end())
            charts.push_back({diff, {level, std::move(notes)}});
        else
            it->second = {level, std::move(notes)};
    }
    make_dirs(folder);
    auto music = std::find_if(keys.begin(), keys.end(), [&](const std::string& k) { return ends(k, "/" + src + "/music.wav"); });
    if (music == keys.end())
        throw ValueError("no music.wav for " + src);
    std::string fsb;
    {
        unity::Bundle b = unity::Bundle::parse(game.read_bundle(game.bundle_path(Key(*music))));
        for (auto& o : b.objects())
            if (o.class_id() == unity::kAudioClip) {
                Value r = o.read().at("m_Resource");
                fsb = b.node_ending(".resource").data.substr(static_cast<size_t>(py_int(r.at("m_Offset"))), static_cast<size_t>(py_int(r.at("m_Size"))));
                break;
            }
    }
    double length = fsb::to_ogg(fsb, game.dir / "eboot.bin", folder / "audio.ogg");
    print("audio.ogg  " + format_fixed(length, 1) + " s");
    Value out = Value::of_map(), levels = Value::of_map(), diffs = Value::of_map();
    out["bpm"] = Value();
    if (stage == "field")
        out["stage"] = "field";
    std::string summary;
    for (const auto& [d, c] : charts) {
        levels[d] = c.first;
        diffs[d] = Value::of_list(c.second);
        summary += (summary.empty() ? "" : ", ") + d + " L" + py_str(c.first) + " " + std::to_string(c.second.size()) + " notes";
    }
    out["levels"] = levels;
    out["difficulties"] = diffs;
    write_file(folder / "charts.json", json_file(out, true));
    print("charts.json  " + summary);
    // The largest jacket: its size is the number after the last "_" in its name.
    auto size_of = [](const std::string& k) -> int64_t {
        for (size_t u = k.find('_'); u != std::string::npos; u = k.find('_', u + 1)) {
            size_t e = u + 1;
            while (e < k.size() && k[e] >= '0' && k[e] <= '9')
                ++e;
            if (e > u + 1 && e < k.size() && k[e] == '.')
                return -std::stoll(k.substr(u + 1, e - u - 1));
        }
        return 0;
    };
    std::vector<std::string> jackets;
    for (const std::string& k : keys)
        if (contains(k, "/Jacket/"))
            jackets.push_back(k);
    std::stable_sort(jackets.begin(), jackets.end(), [&](const std::string& a, const std::string& b) { return size_of(a) < size_of(b); });
    if (!jackets.empty()) {
        write_file(folder / "jacket.png", image::png(game_image(game, Key(jackets[0]))));
        print("jacket.png  from " + basename(jackets[0]));
    }
    std::string title = title_arg.empty() ? "Game song " + std::to_string(number) : title_arg;
    Value config = Value::of_map();
    config["title"] = title;
    config["stage"] = stage.empty() ? "battle" : stage;
    config["start"] = 0.0;
    config["end"] = Value();
    config["fade_out"] = 0.0;
    config["preview"] = Value::of_list({row.get("StartPreview"), row.get("EndPreview")});
    std::string sort;
    for (char ch : ascii_lower(title)) {
        char c = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == ' ' ? ch : ' ';
        if (c != ' ' || sort.empty() || sort.back() != ' ')
            sort += c;
    }
    sort = strip(sort);
    config["sort_name"] = sort.empty() ? "song " + std::to_string(number) : sort;
    if (stage == "event") {
        Value file = row.get("MovieFileName");
        fs::path movie = game.dir / "Media" / "StreamingAssets" / "Movie" / "EMS" / to_path(truthy(file) ? py_str(file) : src + ".mp4");
        dlcmaker::copy_file(movie, folder / "movie.mp4");
        print("movie.mp4  from " + name_of(movie));
        config["audio_from_video"] = false; // the game's video has no music track: the audio is audio.ogg
        auto thumb = std::find_if(keys.begin(), keys.end(), [](const std::string& k) { return contains(k, "/EMSThumbnail/"); });
        if (thumb != keys.end()) {
            write_file(folder / "thumbnail.png", image::png(game_image(game, Key(*thumb))));
            print("thumbnail.png");
        }
    } else {
        config["stage_from"] = number; // its background and enemies
    }
    config["color"] = row.get("BGColorCode");
    Value kept = Value::of_map();
    for (const Item& it : config.map)
        if (!it.v.is_null())
            kept.set(it.k, it.v);
    write_file(folder / "song.json", json_file(kept, false));
    print("song.json  " + py_str(config.at("stage")) + " stage, preview " + py_str(config.at("preview").list[0]) + "-" +
          py_str(config.at("preview").list[1]) + " s");
    return 0;
}

int extract_main(const std::vector<std::string>& argv) {
    Args a = parse_args(argv, {"game", "title"});
    if (a.help) {
        print("usage: theatrhythm-dlcmaker extract GAMESONG FOLDER --game DIR [--title TEXT]");
        return 0;
    }
    if (!a.error.empty() || a.pos.size() != 2)
        return usage_error(a.error.empty() ? "extract needs GAMESONG FOLDER" : a.error);
    auto number = parse_int(strip(a.pos[0]));
    if (!number)
        return usage_error("argument number: invalid int value: '" + a.pos[0] + "'");
    Game game(opt(a, "game"));
    return extract(game, *number, py_path(a.pos[1]), opt(a, "title"));
}

int app_main(const std::vector<std::string>& argv) {
    const std::string& cmd = argv[0];
    std::vector<std::string> args(argv.begin() + 1, argv.end());
    struct Arity {
        size_t lo, hi;
    };
    static const std::map<std::string, Arity> kArity = {{"info", {1, 1}},    {"set", {1, SIZE_MAX}},   {"add-file", {3, 3}},
                                                        {"new-dlc", {2, 2}}, {"new-song", {2, 3}},     {"stage-data", {1, 2}}};
    Arity ar = kArity.at(cmd);
    if (args.size() < ar.lo || args.size() > ar.hi)
        throw Exit(kAppUsage);
    if (cmd == "info")
        return info(args[0]);
    if (cmd == "set")
        return set_fields(args[0], std::vector<std::string>(args.begin() + 1, args.end()));
    if (cmd == "add-file")
        return add_file(args[0], args[1], args[2]);
    if (cmd == "new-dlc")
        return new_dlc(args[0], args[1]);
    if (cmd == "new-song")
        return new_song(args[0], args[1], args.size() > 2 ? args[2] : "battle");
    return stage_data(args[0]); // a second argument (the runner folder earlier (Python) releases took) is ignored
}

} // namespace

bool is_command(const std::string& arg) {
    static const std::set<std::string> kCommands = {"new",  "check", "build",    "install", "extract",  "info",
                                                    "set",  "add-file", "new-dlc", "new-song", "stage-data", "-h", "--help"};
    return kCommands.count(arg) != 0;
}

int cli_main(const std::vector<std::string>& args) {
    try {
        if (!args.empty() && (args[0] == "info" || args[0] == "set" || args[0] == "add-file" || args[0] == "new-dlc" || args[0] == "new-song" ||
                              args[0] == "stage-data"))
            return app_main(args);
        if (!args.empty() && args[0] == "extract")
            return extract_main(std::vector<std::string>(args.begin() + 1, args.end()));
        return project_main(args);
    } catch (const Exit& e) {
        print_err(e.what());
        return 1;
    } catch (const std::exception& e) {
        print_err(std::string("error: ") + e.what());
        return 1;
    }
}

} // namespace dlcmaker
