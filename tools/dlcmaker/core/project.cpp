// SPDX-License-Identifier: GPL-2.0-or-later
#include "project.h"

#include "media.h"

#include <algorithm>

namespace dlcmaker::project {

const std::vector<std::string> STAGES = {"battle", "event", "field"};
const std::vector<std::string> IMAGE_EXT = {".png", ".jpg", ".jpeg", ".webp"};

namespace {

const std::map<std::string, std::pair<int64_t, int64_t>> kIdRanges = {
    {"package", {100, 999}}, {"series", {200, 999}}, {"song", {600, 9999}}};

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

Value read_json(const fs::path& path, Issues& issues) {
    try {
        Value v = json_parse(read_text(path));
        if (v.is_map())
            return v;
        issues.error(u8(path), "cannot read: not a JSON object");
    } catch (const std::exception& e) {
        issues.error(u8(path), std::string("cannot read: ") + e.what());
    }
    return Value::of_map();
}

// `given` (relative to folder) or the first file named <name>.<ext>, else (audio, movie) any file with ext.
fs::path pick(const fs::path& folder, const std::string& name, const Value& given, const std::vector<std::string>& exts, Issues& issues,
              const std::string& where) {
    if (truthy(given)) {
        fs::path p = folder / to_path(py_str(given));
        if (!is_file(p)) {
            issues.error(where, py_str(given) + " not found");
            return {};
        }
        return p;
    }
    for (const std::string& e : exts)
        if (is_file(folder / to_path(name + e)))
            return folder / to_path(name + e);
    if (name == "audio" || name == "movie")
        for (const fs::path& p : sorted_dir(folder))
            if (has(exts, ascii_lower(suffix_of(p))))
                return p;
    return {};
}

bool starts_alnum(const std::string& s) {
    return !s.empty() && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'));
}

} // namespace

int64_t template_of(const std::string& stage) {
    return stage == "event" ? 508 : stage == "battle" ? 481 : stage == "field" ? 482 : 481;
}

std::string sort_key(const std::string& text) {
    std::string out;
    for (char c : ascii_lower(text))
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ')
            out += c;
    size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    return a == std::string::npos ? "" : out.substr(a, b - a + 1);
}

Value waves_of(const Value& pack) {
    Value out = Value::of_list();
    const Value& ids = pack.at("MonsterID");
    for (int w = 0; w < WAVES; ++w) {
        Value ms = Value::of_list();
        for (int k = w * WAVE_SIZE; k < (w + 1) * WAVE_SIZE && k < static_cast<int>(ids.list.size()); ++k)
            if (py_int(ids.list[static_cast<size_t>(k)]) != NONE)
                ms.list.push_back(Value(py_int(ids.list[static_cast<size_t>(k)]) - 250000000));
        if (!ms.list.empty()) {
            Value wave = Value::of_map();
            wave["boss"] = truthy(pack.at("BossFlag").list.at(static_cast<size_t>(w)));
            wave["monsters"] = ms;
            out.list.push_back(wave);
        }
    }
    return out;
}

Value pack_of(const Value& waves) {
    Value ids = Value::of_list(std::vector<Value>(WAVES * WAVE_SIZE, Value(NONE)));
    Value boss = Value::of_list(std::vector<Value>(WAVES, Value(false)));
    for (size_t w = 0; w < waves.list.size(); ++w) {
        boss.list[w] = truthy(waves.list[w].at("boss"));
        const Value& ms = waves.list[w].at("monsters");
        for (size_t i = 0; i < ms.list.size(); ++i)
            ids.list[w * WAVE_SIZE + i] = Value(250000000 + py_int(ms.list[i]));
    }
    Value row = Value::of_map();
    row["BossFlag"] = boss;
    row["MonsterID"] = ids;
    return row;
}

std::string waves_text(const Value& waves) {
    if (!waves.is_list() || waves.list.empty() || waves.list.size() > static_cast<size_t>(WAVES))
        throw ValueError("1 to " + std::to_string(WAVES) + " waves");
    for (const Value& w : waves.list) {
        Value ms = w.is_map() ? w.get("monsters") : Value();
        bool ok = ms.is_list() && !ms.list.empty() && ms.list.size() <= static_cast<size_t>(WAVE_SIZE);
        for (const Value& m : ms.list)
            ok = ok && m.t == Value::T::Int && m.i >= 0 && m.i < 10000;
        if (!ok)
            throw ValueError("each wave has 1 to " + std::to_string(WAVE_SIZE) + " monster numbers");
    }
    std::string out;
    for (size_t k = 0; k < waves.list.size(); ++k) {
        const Value& w = waves.list[k];
        out += (k ? "|" : "") + std::to_string(truthy(w.get("boss")) ? 1 : 0) + ":";
        for (size_t j = 0; j < w.at("monsters").list.size(); ++j)
            out += (j ? "," : "") + std::to_string(w.at("monsters").list[j].i);
    }
    return out;
}

Value parse_waves(const std::string& text) {
    Value waves = Value::of_list();
    for (size_t a = 0; a <= text.size();) {
        size_t b = text.find('|', a);
        if (b == std::string::npos)
            b = text.size();
        std::string part = text.substr(a, b - a);
        a = b + 1;
        size_t colon = part.find(':');
        std::string boss = part.substr(0, colon), ms = colon == std::string::npos ? "" : part.substr(colon + 1);
        Value wave = Value::of_map();
        wave["boss"] = boss == "1";
        Value list = Value::of_list();
        for (size_t c = 0; c <= ms.size();) {
            size_t d = ms.find(',', c);
            if (d == std::string::npos)
                d = ms.size();
            std::string m = ms.substr(c, d - c);
            c = d + 1;
            if (!m.empty())
                list.list.push_back(Value(py_int(Value(m))));
        }
        wave["monsters"] = list;
        waves.list.push_back(wave);
    }
    waves_text(waves);
    return waves;
}

size_t Issues::errors() const {
    return static_cast<size_t>(std::count_if(items.begin(), items.end(), [](const It& i) { return i.level == "error"; }));
}

size_t Issues::warnings() const {
    return static_cast<size_t>(std::count_if(items.begin(), items.end(), [](const It& i) { return i.level == "warning"; }));
}

Song::Song(Dlc* d, const fs::path& f, Issues& issues) : dlc(d), folder(f) {
    where = name_of(d->folder) + "/" + name_of(folder);
    const Value& c = config = read_json(folder / "song.json", issues);
    title = truthy(c.get("title")) ? c.get("title") : Value(name_of(folder));
    std::string base = title.is_map() ? py_str(title.get("default", Value(name_of(folder)))) : py_str(title);
    sort_name = truthy(c.get("sort_name")) ? py_str(c.get("sort_name")) : sort_key(base);
    if (!starts_alnum(sort_name))
        issues.error(where, "sort_name " + py_repr(Value(sort_name)) + " must start with a-z or 0-9 (music select index)");
    // "clone": a game song whose audio, charts (and event video) this song reuses from the player's
    // dump at build time; the project then holds no game files.
    Value cl = c.get("clone");
    if (!cl.is_null() && cl != Value(""))
        clone = py_int(cl);
    stage = py_str(c.get("stage", Value("battle")));
    if (!has(STAGES, stage))
        issues.error(where, "stage must be one of battle, event, field");
    audio = pick(folder, "audio", c.get("audio"), media::AUDIO_EXT, issues, where);
    movie = pick(folder, "movie", c.get("movie"), media::VIDEO_EXT, issues, where);
    // The music comes from the audio file, or from the video's sound track ("audio_from_video"; by
    // default when there is a video and no audio file).
    Value afv = c.get("audio_from_video");
    audio_from_video = !afv.is_null() ? truthy(afv) : (audio.empty() && !movie.empty());
    if (!clone && audio_from_video && movie.empty())
        issues.error(where, "audio from the video is set, but there is no video");
    if (!clone && !audio_from_video && audio.empty())
        issues.error(where, std::string("no audio file (mp3/ogg/flac/wav/m4a)") + (!movie.empty() ? ", or use the audio from the video" : ""));
    if (!clone && stage == "event" && movie.empty())
        issues.error(where, "an event stage needs a video (mp4/mkv/webm/mov)");
    jacket = pick(folder, "jacket", c.get("jacket"), IMAGE_EXT, issues, where);
    thumbnail = pick(folder, "thumbnail", c.get("thumbnail"), IMAGE_EXT, issues, where);
    if (jacket.empty() && !clone)
        issues.warn(where, "no jacket image: the template song's jacket is used");
    if (stage == "event" && thumbnail.empty() && !clone)
        issues.warn(where, "no thumbnail image: the template song's is used");
    start = py_float(c.get("start", Value(0.0)));
    end = c.get("end");
    fade_out = py_float(c.get("fade_out", Value(0.0)));
    tmpl = clone ? *clone : py_int(c.get("template", Value(template_of(stage))));
    if ((stage == "battle" || stage == "field") && !clone && !c.get("stage_from", Value(0)).is_int())
        issues.error(where, "stage_from must be a game song number");
    if (stage == "battle" && !clone && c.has("enemy_waves")) {
        try {
            waves_text(c.at("enemy_waves"));
        } catch (const ValueError& e) {
            issues.error(where, std::string("enemy_waves: ") + e.what());
        }
    }
    try {
        charts = chart::load(folder, c.get("chart_offset_ms", Value(0)));
    } catch (const ValueError& e) {
        charts = chart::Loaded();
        issues.error(where, std::string("charts: ") + e.what());
    }
    chart_stage = charts.stage;
    if (!clone && truthy(chart_stage) && chart_stage != Value(stage))
        issues.error(where, "the charts are " + py_str(chart_stage) + " charts (\"stage\" in charts.json): set the song's stage to " +
                                py_str(chart_stage));
    if (truthy(c.get("preview")))
        preview = c.get("preview");
    else {
        Value p = truthy(charts.preview) ? charts.preview : Value::of_map();
        preview = Value::of_list({p.get("start_s"), p.get("end_s")});
    }
    if (clone) {
        if (!charts.order.empty() || !audio.empty() || !movie.empty())
            issues.warn(where, "audio, video and charts come from game song " + std::to_string(*clone) + ": the files here are not used");
        charts = chart::Loaded();
    } else
        for (size_t k = 0; k < 3; ++k)
            if (!charts.has(chart::DIFFICULTIES[k]))
                issues.error(where, "no " + chart::DIFFICULTIES[k] + " chart (" + chart::DIFFICULTIES[k] + ".json or charts.json)");
}

void Song::check_charts(int64_t length_ms, Issues& issues) const {
    if (truthy(chart_stage) && chart_stage != Value(stage))
        return; // one error above, not one per note
    for (const std::string& d : charts.order) {
        const chart::Chart& ch = charts.by_difficulty.at(d);
        if (!has(chart::DIFFICULTIES, d)) {
            issues.error(where, "unknown difficulty " + py_repr(Value(d)));
            continue;
        }
        auto [errors, warnings] = chart::validate(ch, length_ms, stage);
        std::string at = where + " [" + d + "]";
        for (size_t k = 0; k < errors.size() && k < 12; ++k)
            issues.error(at, errors[k]);
        if (errors.size() > 12)
            issues.error(at, "... " + std::to_string(errors.size() - 12) + " more");
        for (const std::string& w : warnings)
            issues.warn(at, w);
    }
}

Value Ids::song(const std::string& folder) const {
    for (auto& [f, id] : songs)
        if (f == folder)
            return id;
    throw ValueError("'" + folder + "'");
}

Value Ids::json() const {
    Value v = Value::of_map();
    v["package"] = package;
    v["series"] = series;
    Value s = Value::of_map();
    for (auto& [f, id] : songs)
        s[f] = id;
    v["songs"] = s;
    return v;
}

Dlc::Dlc(const fs::path& f, Issues& issues) : folder(f) {
    std::string where = name_of(folder);
    const Value& c = config = read_json(folder / "dlc.json", issues);
    name = truthy(c.get("name")) ? c.get("name") : Value(name_of(folder));
    if (truthy(c.get("label")))
        label = py_str(c.get("label"));
    else {
        std::string n = py_str(name);
        for (char ch : n)
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == ' ')
                label += ch == ' ' ? '_' : ch;
        label = label.substr(0, 16);
        if (label.empty())
            label = "CustomDLC";
    }
    bool ok = !label.empty() && label.size() <= 16;
    for (char ch : label)
        ok = ok && ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_');
    if (!ok)
        issues.error(where, "label must be 1-16 letters, digits or _");
    Value s = c.get("series", Value::of_map());
    series_base = s.get("base");
    series_name = s.has("name") ? s.get("name") : name;
    logo = pick(folder, "logo", s.get("logo"), IMAGE_EXT, issues, where);
    logo_wide = pick(folder, "logo_wide", s.get("logo_wide"), IMAGE_EXT, issues, where);
    logo_from = py_int(s.get("logo_from", Value(13)));
    if (!truthy(series_base) && logo.empty())
        issues.warn(where, "no logo.png: the series shows base series " + std::to_string(logo_from) + "'s logo");
    lock_path = folder / "dlc.lock.json";
    lock = is_file(lock_path) ? json_parse(read_text(lock_path)) : Value::of_map();
    std::vector<fs::path> dirs;
    for (const fs::path& p : sorted_dir(folder))
        if (is_dir(p) && is_file(p / "song.json"))
            dirs.push_back(p);
    Value order = c.get("songs");
    if (truthy(order) && order.is_list()) {
        std::vector<fs::path> first;
        std::vector<std::string> names;
        for (const Value& n : order.list) {
            names.push_back(py_str(n));
            if (is_file(folder / to_path(py_str(n)) / "song.json"))
                first.push_back(folder / to_path(py_str(n)));
        }
        for (const fs::path& p : dirs)
            if (!has(names, name_of(p)))
                first.push_back(p);
        dirs = first;
    }
    for (const fs::path& p : dirs)
        songs.push_back(std::make_unique<Song>(this, p, issues));
    if (songs.empty())
        issues.error(where, "no song folders (a subfolder with song.json)");
}

std::map<std::string, std::vector<Value>> Dlc::fixed() const {
    const Value& c = config;
    Value lock_songs = lock.get("songs", Value::of_map());
    std::map<std::string, std::vector<Value>> out;
    out["package"] = {c.has("package") ? c.get("package") : lock.get("package")};
    Value series = c.get("series", Value::of_map());
    out["series"] = truthy(series_base) ? std::vector<Value>{} : std::vector<Value>{series.has("id") ? series.get("id") : lock.get("series")};
    std::vector<Value> s;
    for (const auto& song : songs)
        s.push_back(song->config.has("id") ? song->config.get("id") : lock_songs.get(song->folder_name()));
    out["song"] = s;
    return out;
}

Ids Dlc::ids(std::map<std::string, std::set<int64_t>>& used) const {
    auto take = [&](const std::string& kind, const Value& want) -> Value {
        if (!want.is_null())
            return want;
        int64_t n = kIdRanges.at(kind).first;
        while (used[kind].count(n))
            ++n;
        if (n > kIdRanges.at(kind).second)
            throw Exit("no free " + kind + " id");
        used[kind].insert(n);
        return Value(n);
    };
    auto f = fixed();
    Ids ids;
    ids.package = take("package", f["package"][0]);
    ids.series = truthy(series_base) ? Value() : take("series", f["series"][0]);
    for (size_t k = 0; k < songs.size(); ++k)
        ids.songs.emplace_back(songs[k]->folder_name(), take("song", f["song"][k]));
    return ids;
}

void Dlc::save_lock(const Ids& ids) const {
    write_file(lock_path, json_dump(ids.json(), {1, true, false}));
}

Discovered discover(const fs::path& path) {
    Discovered d;
    std::vector<fs::path> folders;
    if (is_file(path / "dlc.json"))
        folders = {path};
    else
        for (const fs::path& p : sorted_dir(path))
            if (is_dir(p) && is_file(p / "dlc.json"))
                folders.push_back(p);
    if (folders.empty())
        d.issues.error(u8(path), "no dlc.json here or in any subfolder");
    for (const fs::path& f : folders)
        d.dlcs.push_back(std::make_unique<Dlc>(f, d.issues));
    std::map<std::string, int> labels;
    for (const auto& dlc : d.dlcs)
        ++labels[dlc->label];
    for (auto& [l, n] : labels)
        if (n > 1)
            d.issues.error(l, "two DLCs share this label");
    return d;
}

std::map<std::string, Ids> assign_ids(const std::vector<std::unique_ptr<Dlc>>& dlcs,
                                      const std::vector<std::pair<std::string, int64_t>>& installed) {
    std::map<std::string, std::set<int64_t>> used = {{"package", {}}, {"series", {}}, {"song", {}}};
    for (auto& [kind, v] : installed)
        used[kind].insert(v);
    for (const auto& d : dlcs)
        for (auto& [kind, vals] : d->fixed())
            for (const Value& v : vals) {
                if (v.is_null())
                    continue;
                auto [lo, hi] = kIdRanges.at(kind);
                int64_t n = py_int(v);
                if (!(lo <= n && n <= hi))
                    throw Exit(name_of(d->folder) + ": " + kind + " id " + py_str(v) + " is outside " + std::to_string(lo) + "-" + std::to_string(hi));
                if (used[kind].count(n))
                    throw Exit(name_of(d->folder) + ": " + kind + " id " + py_str(v) + " is used twice");
                used[kind].insert(n);
            }
    std::map<std::string, Ids> out;
    for (const auto& d : dlcs)
        out[d->label] = d->ids(used);
    return out;
}

} // namespace dlcmaker::project
