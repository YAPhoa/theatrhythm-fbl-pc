// SPDX-License-Identifier: GPL-2.0-or-later
// Project folders: a workspace holds DLC folders (dlc.json), a DLC holds song folders (song.json),
// a song folder holds its audio, video, art and charts. Files are found by name unless song.json
// names them. IDs are kept in each DLC's dlc.lock.json once assigned, so saves stay valid across builds.
#pragma once

#include "chart.h"
#include "util.h"
#include "value.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dlcmaker::project {

extern const std::vector<std::string> STAGES;     // battle, event, field
extern const std::vector<std::string> IMAGE_EXT;
constexpr int64_t NONE = 4294967295;
int64_t template_of(const std::string& stage);    // the base song a stage's songs are cloned from

// A battle stage's enemies: up to 10 waves of up to 3 monsters (MonsterTable numbers less 250000000),
// a boss wave waiting until it is beaten. song.json keeps them as "enemy_waves":
// [{"boss": false, "monsters": [83, 84]}, ...]; the window passes them as text, "0:83,84|1:5".
constexpr int WAVES = 10, WAVE_SIZE = 3;
Value waves_of(const Value& pack);   // a MonsterPackTable row as enemy_waves
Value pack_of(const Value& waves);   // enemy_waves as a MonsterPackTable row
std::string waves_text(const Value& waves); // ValueError when they are not valid
Value parse_waves(const std::string& text);

struct Issues {
    struct It {
        std::string level, where, msg;
    };
    std::vector<It> items;
    void error(const std::string& where, const std::string& msg) { items.push_back({"error", where, msg}); }
    void warn(const std::string& where, const std::string& msg) { items.push_back({"warning", where, msg}); }
    size_t errors() const;
    size_t warnings() const;
};

struct Dlc;

struct Song {
    Dlc* dlc = nullptr;
    fs::path folder;
    std::string where;
    Value config, title;
    std::string sort_name, stage;
    std::optional<int64_t> clone;
    fs::path audio, movie, jacket, thumbnail; // empty: none
    bool audio_from_video = false;
    double start = 0, fade_out = 0;
    Value end;
    int64_t tmpl = 0;
    chart::Loaded charts;
    Value chart_stage, preview;
    std::optional<double> length_s;
    std::string frames;  // decoded 48 kHz stereo 16-bit audio
    fs::path movie_out;

    Song(Dlc* d, const fs::path& folder, Issues& issues);
    std::string folder_name() const { return name_of(folder); }
    void check_charts(int64_t length_ms, Issues& issues) const;
};

struct Ids {
    Value package, series;               // series: Null with a base series
    std::vector<std::pair<std::string, Value>> songs; // folder name -> id, in song order
    Value song(const std::string& folder) const;
    Value json() const;
};

struct Dlc {
    fs::path folder;
    Value config, name, series_base, series_name, lock;
    std::string label;
    fs::path logo, logo_wide, lock_path;
    int64_t logo_from = 13;
    std::vector<std::unique_ptr<Song>> songs;

    Dlc(const fs::path& folder, Issues& issues);
    std::map<std::string, std::vector<Value>> fixed() const;
    Ids ids(std::map<std::string, std::set<int64_t>>& used) const;
    void save_lock(const Ids& ids) const;
};

struct Discovered {
    std::vector<std::unique_ptr<Dlc>> dlcs;
    Issues issues;
};
Discovered discover(const fs::path& path);
// {label: ids}; `installed` is (kind, id) used by other packs. Exit when the fixed IDs clash.
std::map<std::string, Ids> assign_ids(const std::vector<std::unique_ptr<Dlc>>& dlcs, const std::vector<std::pair<std::string, int64_t>>& installed);

// sort_name's default: lower case, only a-z, 0-9 and spaces.
std::string sort_key(const std::string& text);

} // namespace dlcmaker::project
