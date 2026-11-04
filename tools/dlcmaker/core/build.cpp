// SPDX-License-Identifier: GPL-2.0-or-later
#include "build.h"

#include "chart.h"
#include "image.h"
#include "media.h"
#include "unity.h"

#include <algorithm>
#include <map>
#include <set>

namespace dlcmaker::build {

namespace {

using unity::Bundle;

const std::vector<std::string> kTables = {"MusicTable",       "BMSBackGroundTable", "MonsterPackTable", "SoundTable", "SeriesTable",
                                          "QuestSquareTable", "QuestTable",         "TheaterTable",     "DLCMusicTable", "DLCPackTable"};
// pack text source -> base prefab it is cloned from
const std::vector<std::pair<std::string, std::string>> kTexts = {
    {"Table/Table_Series", "Table/Table_Series"}, {"Table/Table_PersonalName", "Table/Table_PersonalName"},
    {"Table/Table_MusicDLC", "Table/Table_DLCPack"}, {"Table/MusicTable", "Table/MusicTable"},
    {"Table/Table_DLCPack", "Table/Table_DLCPack"}};
constexpr int64_t NONE = 4294967295;
const std::vector<std::string> kFieldKeys = {"BackGroundIDValue", "FMSStartPosition", "FMSBGCameraEffectID", "FMSBGSkyBoxID", "ShadowAlpha"};
constexpr int64_t kEnemyPackBase = 260010000; // + song number: a song's own MonsterPackTable row (the game's end below 260001000)

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty())
        return s;
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size())
        s.replace(at, from.size(), to);
    return s;
}

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

bool starts(const std::string& s, const std::string& p) {
    return s.rfind(p, 0) == 0;
}

bool ends(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string pad7(int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%07lld", static_cast<long long>(v));
    return buf;
}

// (h, asset id) of a seed: the bundle's new names.
std::pair<std::string, std::string> names(const std::string& seed) {
    std::string h = md5_hex(seed);
    return {h, "dlc" + h.substr(3)};
}

void rename_bundle(Value& t, const std::string& h, const std::string& asset_id) {
    t["m_Name"] = h + ".bundle";
    t["m_AssetBundleName"] = h + ".bundle";
    Value container = Value::of_list();
    for (const Value& pair : t.at("m_Container").list)
        container.list.push_back(Value::of_list({Value(asset_id), pair.list.at(1)}));
    t["m_Container"] = container;
}

// The base bundle at `path` renamed; `old` -> `now` in object names.
std::pair<Bundle, std::string> clone(const Game& game, const fs::path& path, const std::string& seed, const std::string& old = "",
                                     const std::string& now = "") {
    Bundle b = Bundle::parse(game.read_bundle(path));
    auto [h, asset_id] = names(seed);
    std::string cab;
    int cabs = 0;
    for (const Bundle::Node& n : b.nodes)
        if (n.name.find('.') == std::string::npos)
            cab = n.name, ++cabs;
    if (cabs != 1)
        throw ValueError("expected one CAB in " + u8(path));
    for (auto& o : b.objects()) {
        Value t = o.read();
        if (o.class_id() == unity::kAssetBundle)
            rename_bundle(t, h, asset_id);
        else {
            if (Value* name = t.find("m_Name"); name && name->is_str())
                name->s = replace_all(name->s, old, now);
            for (const char* f : {"m_StreamData", "m_Resource"}) {
                Value* s = t.find(f);
                if (!s || !s->is_map())
                    continue;
                const char* k = s->has("path") ? "path" : "m_Source";
                if (Value* src = s->find(k); src && truthy(*src))
                    src->s = replace_all(src->s, cab, "CAB-" + h);
            }
        }
        o.write(t);
    }
    for (Bundle::Node& n : b.nodes)
        n.name = replace_all(n.name, cab, "CAB-" + h);
    return {std::move(b), asset_id};
}

void replace_stream(Bundle& b, const std::string& suffix, const std::string& data, size_t offset = 0, size_t size = std::string::npos) {
    Bundle::Node& n = b.node_ending(suffix);
    if (size == std::string::npos)
        n.data = data;
    else
        n.data = n.data.substr(0, offset) + data + (offset + size < n.data.size() ? n.data.substr(offset + size) : "");
}

// A TextAsset bundle (chart, table) cloned from `tmpl` holding `script` bytes.
std::pair<std::string, std::string> text_asset(const Game& game, const fs::path& tmpl, const std::string& seed, const std::string& script,
                                               const std::string& name = "") {
    auto [b, aid] = clone(game, tmpl, seed);
    for (auto& o : b.objects())
        if (o.class_id() == unity::kTextAsset) {
            Value t = o.read();
            t["m_Script"] = script;
            if (!name.empty())
                t["m_Name"] = name;
            o.write(t);
        }
    return {b.save(), aid};
}

// An AudioClip bundle cloned from `tmpl` holding FSB5 PCM16 `fsb` (m_CompressionFormat 0).
std::pair<std::string, std::string> audio_clip(const Game& game, const fs::path& tmpl, const std::string& seed, const std::string& fsb,
                                               double length_s) {
    auto [b, aid] = clone(game, tmpl, seed);
    for (auto& o : b.objects())
        if (o.class_id() == unity::kAudioClip) {
            Value t = o.read();
            t["m_CompressionFormat"] = 0;
            t["m_BitsPerSample"] = 16;
            t["m_Channels"] = 2;
            t["m_Frequency"] = 48000;
            t["m_Length"] = length_s;
            t["m_Resource"]["m_Offset"] = 0;
            t["m_Resource"]["m_Size"] = Value(static_cast<int64_t>(fsb.size()));
            o.write(t);
        }
    replace_stream(b, ".resource", fsb);
    return {b.save(), aid};
}

// `b` (a bundle with one streamed Texture2D) with `art` as its texture, PS4-tiled.
void texture(Bundle& b, const image::Image& art) {
    std::vector<Bundle::Ref> tex;
    for (auto& o : b.objects())
        if (o.class_id() == unity::kTexture2D)
            tex.push_back(o);
    if (tex.size() != 1)
        throw ValueError("expected one Texture2D");
    Value t = tex[0].read();
    const Value& sd = t.at("m_StreamData");
    std::string data = image::encode_texture(art, static_cast<int>(py_int(t.at("m_Width"))), static_cast<int>(py_int(t.at("m_Height"))),
                                             static_cast<int>(py_int(t.at("m_TextureFormat"))));
    int64_t size = py_int(sd.at("size"));
    if (static_cast<int64_t>(data.size()) != size)
        throw ValueError("texture size " + std::to_string(data.size()) + " != " + std::to_string(size));
    replace_stream(b, ".resS", data, static_cast<size_t>(py_int(sd.at("offset"))), static_cast<size_t>(size));
}

// An I2 LanguageSource bundle cloned from `tmpl` holding only `terms`: {term: str (every language) |
// {lang code: str, 'default': str}}.
std::pair<std::string, std::string> text_source(const Game& game, const fs::path& tmpl, const std::string& seed, const Value& terms) {
    Bundle b = Bundle::parse(game.read_bundle(tmpl));
    auto [h, asset_id] = names(seed);
    for (auto& o : b.objects()) {
        if (o.class_id() == unity::kMonoBehaviour) {
            Value t = o.read();
            Value& src = t.at("mSource");
            std::vector<Value> codes;
            for (const Value& l : src.at("mLanguages").list)
                codes.push_back(l.get("Code"));
            size_t n = codes.size();
            Value list = Value::of_list();
            for (const Item& it : terms.map) {
                Value langs = Value::of_list();
                const Value& v = it.v;
                for (size_t k = 0; k < n; ++k) {
                    if (v.is_map()) {
                        std::string c = py_str(codes[k]), base = c.substr(0, c.find('-'));
                        const Value* x = v.find(c);
                        if (!x)
                            x = v.find(base);
                        langs.list.push_back(x ? *x : v.get("default", Value("")));
                    } else
                        langs.list.push_back(v);
                }
                Value term = Value::of_map();
                term["Term"] = it.k;
                term["TermType"] = 0;
                term["Description"] = "";
                term["Languages"] = langs;
                term["Flags"] = Value::of_list(std::vector<Value>(n, Value(0)));
                term["Languages_Touch"] = Value::of_list();
                list.list.push_back(term);
            }
            src["mTerms"] = list;
            if (truthy(t.get("mTerms")))
                t["mTerms"] = list;
            o.write(t);
        } else if (o.class_id() == unity::kAssetBundle) {
            Value t = o.read();
            rename_bundle(t, h, asset_id);
            o.write(t);
        }
    }
    if (b.nodes.size() != 1)
        throw ValueError("expected one file in " + u8(tmpl));
    b.nodes[0].name = "CAB-" + h;
    return {b.save(), asset_id};
}

struct Pack {
    Game& game;
    fs::path out;
    Catalog cat;
    std::map<std::string, Value> rows;  // table -> {id: [row]}
    std::map<std::string, Value> texts; // text source -> {term: text}

    Pack(Game& g, fs::path o) : game(g), out(std::move(o)), cat(g.catalog()) {
        for (const std::string& t : kTables)
            rows[t] = Value::of_map();
        for (auto& [t, base] : kTexts)
            texts[t] = Value::of_map();
    }

    void write(const std::string& name, const std::string& data, const std::string& key, const std::string& asset_id,
               const std::vector<std::string>& types) {
        write_file(out / to_path(name + ".bundle"), data);
        for (const std::string& t : types)
            cat.add(Key(key), asset_id, name, t, data.size());
    }

    std::vector<std::string> base_types(const std::string& key) const {
        std::vector<std::string> out_types;
        for (int32_t e : game.base().at(Key(key)))
            out_types.push_back(game.base().types.at(static_cast<size_t>(game.base().entries.at(static_cast<size_t>(e))[6])));
        return out_types;
    }
};

Value row_of(const Table& t, int64_t id) {
    const Value* r = t.get(id);
    if (!r)
        throw Exit("game table row " + std::to_string(id) + " not found");
    return *r;
}

} // namespace

void prepare_song(project::Song& song, const std::string& ffmpeg, const fs::path& cache, const Log& log, bool video) {
    const fs::path& src = song.audio_from_video ? song.movie : song.audio;
    log("  audio  " + name_of(src));
    fs::path wav = media::decode_audio(ffmpeg, src, cache, song.start, song.end, song.fade_out);
    song.frames = media::read_wav(wav);
    song.length_s = media::seconds(song.frames);
    if (song.stage == "event" && video) {
        log("  video  " + name_of(song.movie) + " (" + format_fixed(*song.length_s + media::MOVIE_TAIL_S, 1) + " s, cached after the first build)");
        song.movie_out = media::encode_movie(ffmpeg, song.movie, cache, song.start, *song.length_s);
    }
}

fs::path build(Game& game, const project::Dlc& dlc, const project::Ids& ids, const fs::path& out_root, const Log& log) {
    fs::path out = out_root / to_path(dlc.label);
    if (dlcmaker::exists(out))
        remove_tree(out);
    make_dirs(out);
    Pack pk(game, out);
    const CatalogBase& base = game.base();
    int64_t package_n = py_int(ids.package);
    std::string package = "DLCPACK" + pad7(package_n);
    int64_t pack_id = 500000000 + package_n;
    const Table &music = game.table("MusicTable"), &sound = game.table("SoundTable"), &series = game.table("SeriesTable");
    std::map<std::string, Value> sound_by_label;
    for (auto& [id, r] : sound.rows)
        sound_by_label[py_str(r.at("Label"))] = r;
    {
        Value r = Value::of_map();
        r["ReleaseDateString"] = "";
        r["Price"] = 0;
        r["BonusItemID"] = NONE;
        r["BonusItemCount"] = 0;
        pk.rows["DLCPackTable"].set(Value(pack_id), Value::of_list({r}));
    }
    pk.texts["Table/Table_DLCPack"]["Table/Table_DLCPack/PackName/" + package] = dlc.name;

    std::map<std::string, image::Image> images;
    auto art_of = [&](const fs::path& p) -> const image::Image& {
        std::string k = u8(p);
        auto it = images.find(k);
        if (it == images.end())
            it = images.emplace(k, image::load(p)).first;
        return it->second;
    };

    // The pack's series: its own sort group (a base group would pull it into that quest world, whose
    // Series Quests panel never preloads a pack series' logo), logos cloned under its name.
    int64_t sid;
    if (truthy(dlc.series_base))
        sid = 180000000 + py_int(dlc.series_base);
    else {
        int64_t series_n = py_int(ids.series);
        sid = 180000000 + series_n;
        std::string old = "SERIES" + pad7(dlc.logo_from), now = "SERIES" + pad7(series_n);
        Value r = row_of(series, 180000000 + dlc.logo_from);
        r["SortOrder"] = 11100 + series_n;
        r["CopyrightSortOrder"] = 11100 + series_n;
        r["JacketLottery"] = 0.025;
        r["Copyright"] = " ";
        r["DisplayCopyrightJP"] = false;
        r["DisplayCopyrightEN"] = false;
        r["SortSeriesID"] = sid;
        pk.rows["SeriesTable"].set(Value(sid), Value::of_list({r}));
        pk.texts["Table/Table_Series"]["Table/Table_Series/SeriesName/" + now] = dlc.series_name;
        for (const Key& k : base.keys) {
            if (k.is_int || !starts(k.s, std::string(P) + "Textures/SeriesLogo/") || !ends(k.s, "/" + old + ".png"))
                continue;
            std::string nk = replace_all(k.s, old, now);
            auto [b, aid] = clone(game, game.bundle_path(k), nk, old, now);
            const fs::path& art = contains(nk, "_1420/") && !dlc.logo_wide.empty() ? dlc.logo_wide : dlc.logo;
            if (!art.empty())
                texture(b, art_of(art));
            std::string dir = nk.substr(0, nk.rfind('/'));
            pk.write("logo_" + dir.substr(dir.rfind('/') + 1), b.save(), nk, aid, pk.base_types(k.s));
        }
        log("  series " + py_str(dlc.series_name) + " (" + now + ")" + (dlc.logo.empty() ? ", placeholder logo" : ""));
    }

    for (size_t i = 0; i < dlc.songs.size(); ++i) {
        const project::Song& song = *dlc.songs[i];
        int64_t mid = py_int(ids.song(song.folder_name()));
        std::string label = "MUSIC" + pad7(mid), src = "MUSIC" + pad7(song.tmpl);
        log("song " + label + " " + song.folder_name());
        Value row = row_of(music, 160000000 + song.tmpl);
        Value sort_series = series.get(sid) && truthy(*series.get(sid)) ? *series.get(sid) : pk.rows["SeriesTable"].at_v(Value(sid)).list.at(0);
        row["SoundLabel"] = label;
        row["EditionType"] = 0;
        row["IsShowMusicSelect"] = true;
        row["IsShowMusicPlayer"] = true;
        row["SortOrder"] = py_int(row.at("SortOrder")) + 1 + static_cast<int64_t>(i);
        row["SeriesID"] = sid;
        row["RecomendSeriesID"] = sid;
        row["SortSeriesID"] = sort_series.at("SortSeriesID");
        row["MovieFileName"] = song.stage == "event" ? label + ".mp4" : "";
        int64_t length_ms = 0;
        double pv0 = 0, pv1 = 0;
        if (!song.clone) { // own audio and charts (a clone keeps the game song's length, preview, levels, credits)
            length_ms = round_half_even(*song.length_s * 1000);
            const Value& p = song.preview;
            if (truthy(p) && !p.list.at(0).is_null())
                pv0 = py_float(p.list.at(0)), pv1 = py_float(p.list.at(1));
            else
                pv0 = std::min(30.0, *song.length_s / 3), pv1 = std::min(30.0, *song.length_s / 3) + 15;
            Value levels = Value::of_list();
            for (const std::string& d : chart::DIFFICULTIES)
                levels.list.push_back(song.charts.has(d) && truthy(song.charts.by_difficulty.at(d).level) ? song.charts.by_difficulty.at(d).level : Value(0));
            row["MusicLength"] = round_to(*song.length_s + 1.0, 1);
            row["StartPreview"] = pv0;
            row["EndPreview"] = pv1;
            row["DifficultyValue"] = levels;
            row["Difficulty04Flag"] = song.charts.has("supreme");
            row["Composer"] = NONE;
            row["Lyricist"] = NONE;
            row["Arranger"] = NONE;
            row["Vocal"] = NONE;
        } else
            log("  clone  game song " + std::to_string(*song.clone) + ": its audio, charts" + (song.stage == "event" ? " and video" : ""));
        const Value& c = song.config;
        if (c.has("color"))
            row["BGColorCode"] = c.at("color");
        if (song.stage == "field" && !song.clone && !c.get("stage_from").is_null()) {
            // The field (path, camera, sky) of a game field song; the template's by default.
            const Value* b = music.get(160000000 + py_int(c.at("stage_from")));
            if (!b || !truthy(*b) || b->at("MSType") != Value(0))
                throw Exit(song.where + ": stage_from " + py_str(c.at("stage_from")) + " is not a game field song");
            for (const std::string& k : kFieldKeys)
                row[k] = b->at(k);
            log("  stage  field of game song " + py_str(c.at("stage_from")) + ", background " + py_str(row.at("BackGroundIDValue")));
        }
        if (song.stage == "battle" && !song.clone) {
            // The stage starts from a game song's background and enemies (the template's by default);
            // "background" and an edited enemy list ("enemy_waves", a pack of its own) replace them.
            if (!c.get("stage_from").is_null()) {
                const Value* b = music.get(160000000 + py_int(c.at("stage_from")));
                if (!b || !truthy(*b) || b->at("MSType") != Value(1))
                    throw Exit(song.where + ": stage_from " + py_str(c.at("stage_from")) + " is not a game battle song");
                row["BackGroundIDValue"] = b->at("BackGroundIDValue");
                row["MonsterPackID"] = b->at("MonsterPackID");
            }
            if (c.has("background"))
                row["BackGroundIDValue"] = py_str(c.at("background"));
            if (c.has("enemies"))
                row["MonsterPackID"] = py_int(c.at("enemies"));
            if (truthy(c.get("enemy_waves"))) {
                const Table& monsters = game.table("MonsterTable");
                std::set<int64_t> unknown;
                for (const Value& w : c.at("enemy_waves").list)
                    for (const Value& m : w.at("monsters").list)
                        if (!monsters.get(250000000 + py_int(m)))
                            unknown.insert(py_int(m));
                if (!unknown.empty()) {
                    std::string list;
                    for (int64_t m : unknown)
                        list += (list.empty() ? "" : ", ") + std::to_string(m);
                    throw Exit(song.where + ": enemy_waves: no monster " + list + " in the game");
                }
                int64_t pid = kEnemyPackBase + mid;
                pk.rows["MonsterPackTable"].set(Value(pid), Value::of_list({project::pack_of(c.at("enemy_waves"))}));
                row["MonsterPackID"] = pid;
            }
            if (c.has("background") || truthy(c.get("enemy_waves")))
                log("  stage  " +
                    (truthy(c.get("enemy_waves")) ? std::string("own enemies")
                                                  : "enemies of game song " + (c.has("stage_from") ? py_str(c.at("stage_from")) : std::to_string(song.tmpl))) +
                    ", background " + py_str(row.at("BackGroundIDValue")));
        }
        pk.rows["MusicTable"].set(Value(160000000 + mid), Value::of_list({row}));
        auto snd = sound_by_label.find(src);
        if (snd == sound_by_label.end())
            throw Exit("no SoundTable row " + src);
        Value srow = snd->second;
        srow["Label"] = label;
        pk.rows["SoundTable"].set(Value(420000000 + 2000 + mid), Value::of_list({srow})); // without it: "Failed to load data"
        Value dm = Value::of_map();
        dm["MusicID"] = 160000000 + mid;
        dm["SortOrder"] = static_cast<int64_t>(i);
        dm["DLCPackID"] = pack_id;
        pk.rows["DLCMusicTable"].set(Value(170000000 + mid), Value::of_list({dm}));
        pk.texts["Table/MusicTable"]["Table/MusicTable/MusicName/" + label] = song.title;
        pk.texts["Table/MusicTable"]["Table/MusicTable/MusicSortName/" + label] = song.sort_name;
        std::string dlc_label = "MUSICDLC" + pad7(mid);
        pk.texts["Table/Table_MusicDLC"]["Table/Table_MusicDLC/MusicText/" + dlc_label] = song.title;
        pk.texts["Table/Table_MusicDLC"]["Table/Table_MusicDLC/CopyrightText/" + dlc_label] = c.get("copyright", Value(""));
        if (song.stage == "event") { // a plain file the runner overlays on Media/StreamingAssets/
            fs::path movie = !song.clone ? song.movie_out : game.dir / "Media" / "StreamingAssets" / "Movie" / "EMS" / to_path(src + ".mp4");
            fs::path dest = out / "StreamingAssets" / "Movie" / "EMS" / to_path(label + ".mp4");
            make_dirs(dest.parent_path());
            if (!hard_link(movie, dest))
                dlcmaker::copy_file(movie, dest);
        }
        std::set<std::string> written;
        const std::string music_prefix = std::string(P) + "Music/";
        for (const Key& k : base.keys) {
            if (k.is_int || !starts(k.s, P) || !(contains(k.s, "/" + src + "/") || contains(k.s, "/" + src + ".")))
                continue;
            std::string nk = replace_all(k.s, src, label), name = nk.substr(nk.rfind('/') + 1);
            fs::path bpath = game.bundle_path(k);
            if (contains(k.s, music_prefix) && song.clone) {
                pk.cat.copy(k, Key(nk)); // the game song's own chart and audio bundles, under the new song's keys
                continue;
            }
            if (contains(k.s, music_prefix)) {
                std::pair<std::string, std::string> made;
                if (starts(name, "trigger")) {
                    std::string d = chart::difficulty_of_code(name.substr(7, 2));
                    if (d.empty())
                        throw ValueError("'" + name.substr(7, 2) + "'");
                    if (!song.charts.has(d))
                        continue; // no chart for this difficulty: no key
                    const chart::Chart& ch = song.charts.by_difficulty.at(d);
                    made = text_asset(game, bpath, nk, chart::trigger_file(ch.notes, song.stage, length_ms));
                    log("  chart  " + d + ": " + std::to_string(ch.notes.size()) + " notes, level " + py_str(ch.level));
                    written.insert(d);
                } else if (name == "music.wav")
                    made = audio_clip(game, bpath, nk, media::fsb5_pcm16(song.frames), *song.length_s);
                else if (name == "music_preview.wav")
                    made = audio_clip(game, bpath, nk, media::fsb5_pcm16(media::cut(song.frames, pv0, pv1)), pv1 - pv0);
                else
                    continue;
                pk.write("song" + std::to_string(mid) + "_" + replace_all(name, ".", "_"), made.first, nk, made.second, pk.base_types(k.s));
                continue;
            }
            auto [b, aid] = clone(game, bpath, nk, src, label); // Jacket/, EMSThumbnail/: own names
            const fs::path& art = contains(nk, "/Jacket/") ? song.jacket : contains(nk, "/EMSThumbnail/") ? song.thumbnail : fs::path();
            if (!art.empty())
                texture(b, art_of(art));
            std::string dir = nk.substr(0, nk.rfind('/'));
            pk.write("song" + std::to_string(mid) + "_" + dir.substr(dir.rfind('/') + 1) + "_" + replace_all(name, ".", "_"), b.save(), nk, aid,
                     pk.base_types(k.s));
        }
        // A difficulty the template song has no chart for (most have no Supreme): cloned from one it has.
        std::string tmpl_key = music_prefix + src + "/trigger0301.bytes";
        if (!song.clone)
            for (const std::string& d : song.charts.order) {
                if (written.count(d))
                    continue;
                std::string name = "trigger" + chart::code_of(d) + "01", nk = music_prefix + label + "/" + name + ".bytes";
                const chart::Chart& ch = song.charts.by_difficulty.at(d);
                auto [data, aid] = text_asset(game, game.bundle_path(Key(tmpl_key)), nk, chart::trigger_file(ch.notes, song.stage, length_ms), name);
                log("  chart  " + d + ": " + std::to_string(ch.notes.size()) + " notes, level " + py_str(ch.level));
                pk.write("song" + std::to_string(mid) + "_" + name + "_bytes", data, nk, aid, pk.base_types(tmpl_key));
            }
    }

    fs::path table_tmpl = game.bundle_path(Key(std::string(P) + "Data/DLCPackTable.bytes"));
    for (const std::string& t : kTables) {
        std::string key = std::string(P) + "Data/" + package + "/" + t + ".bytes";
        Value doc = Value::of_map();
        doc["LoadTableDataDictionary"] = pk.rows[t];
        auto [data, aid] = text_asset(game, table_tmpl, key, msgpack_pack(doc), t);
        pk.write("table_" + t, data, key, aid, {"UnityEngine.TextAsset"});
    }
    for (auto& [t, tbase] : kTexts) {
        std::string key = std::string(TEXT) + package + "/" + t + ".prefab";
        auto [data, aid] = text_source(game, game.bundle_path(Key(std::string(TEXT) + tbase + ".prefab")), key, pk.texts[t]);
        pk.write("text_" + t.substr(t.rfind('/') + 1), data, key, aid, {"UnityEngine.GameObject"});
    }
    pk.cat.write(out / "catalog.json", package);
    Value param = Value::of_map();
    param["Version"] = 1;
    param["PackageName"] = package;
    param["HasContents"] = true;
    write_file(out / "dlcparam.json", json_dump(param));
    return out;
}

} // namespace dlcmaker::build
