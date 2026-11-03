// SPDX-License-Identifier: GPL-2.0-or-later
// Charts: loading, validation and the game's TriggerFile format.
//
// A song's charts are either one file per difficulty (basic/expert/ultimate/supreme .json):
//     {"level": 2, "bpm": 190, "offset_ms": 581.8, "notes": [note, ...]}
// or one combined charts.json:
//     {"bpm": 190, "beat0_ms": 581.8, "preview": {...}, "levels": {"basic": 2, ...},
//      "difficulties": {"basic": [note, ...], ...}}
// A note: {"t": ms | "beat": beats from offset, "type": "tap"|"slide"|"twin"|"hold", "lane": 0-3,
//          "dir": deg, "dir2": deg (twin), "end": ms | "end_beat": beats (hold), "end_dir": deg|null,
//          "critical": bool, "end_critical": bool (hold: a critical release)}. Directions: 0 = up,
// clockwise, multiples of 45. On a hold, "critical" is its start; the game marks starts, releases or both.
//
// Field stages (song.json "stage": "field") put notes at 9 heights, "lane" 0-8 (0 = bottom, 4 = middle,
// 8 = top), and add a curved hold whose height moves along a path:
//     {"t", "type": "curve", "lane", "path": [{"t" | "beat", "lane", "critical"?}, ...], "end" | "end_beat",
//      "end_lane", "end_dir": deg|null, "critical"?, "end_critical"?}
// A "hold" keeps its height; on a field it may list the game's "ticks" (LinearHold_Pass times, ms or
// {"t", "critical"}), which change nothing we know of. The mapping to TriggerFile records is in records().
#pragma once

#include "util.h"
#include "value.h"

#include <map>
#include <string>
#include <vector>

namespace dlcmaker::chart {

extern const std::vector<std::string> DIFFICULTIES; // basic, expert, ultimate, supreme
std::string code_of(const std::string& difficulty);  // "01".."04"
std::string difficulty_of_code(const std::string& code); // "" when unknown

struct Chart {
    std::string difficulty;
    Value level, bpm, beat0;
    std::vector<Value> notes;
    fs::path source;
};

struct Loaded {
    std::map<std::string, Chart> by_difficulty;
    std::vector<std::string> order; // difficulties in the order they were found
    Value preview, stage;           // from charts.json
    bool has(const std::string& d) const { return by_difficulty.count(d) != 0; }
};

Loaded load(const fs::path& song_dir, const Value& shift_ms); // ValueError on a bad chart
std::pair<std::vector<std::string>, std::vector<std::string>> validate(const Chart& c, int64_t length_ms, const std::string& stage);

struct Record {
    int64_t t, type, lane, a1, a2, crit;
};
std::vector<Record> records(const std::vector<Value>& notes);
std::string trigger_file(const std::vector<Value>& notes, const std::string& stage, int64_t length_ms);
std::pair<std::string, std::vector<Value>> read_trigger(std::string_view data);

} // namespace dlcmaker::chart
