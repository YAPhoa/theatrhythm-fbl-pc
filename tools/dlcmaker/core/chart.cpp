// SPDX-License-Identifier: GPL-2.0-or-later
#include "chart.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace dlcmaker::chart {

const std::vector<std::string> DIFFICULTIES = {"basic", "expert", "ultimate", "supreme"};

std::string code_of(const std::string& d) {
    for (size_t k = 0; k < DIFFICULTIES.size(); ++k)
        if (DIFFICULTIES[k] == d)
            return "0" + std::to_string(k + 1);
    throw ValueError("'" + d + "'");
}

std::string difficulty_of_code(const std::string& code) {
    for (size_t k = 0; k < DIFFICULTIES.size(); ++k)
        if ("0" + std::to_string(k + 1) == code)
            return DIFFICULTIES[k];
    return "";
}

namespace {

int ms_type(const std::string& stage) {
    return stage == "field" ? 0 : stage == "battle" ? 1 : stage == "event" ? 2 : throw ValueError("'" + stage + "'");
}

const std::set<std::string> kTypes = {"tap", "slide", "twin", "hold"};
const std::set<std::string> kFieldTypes = {"tap", "slide", "twin", "hold", "curve"};

// TwinSlide (dir1, dir2) -> the raw angle pair the game's own charts use most for it (208 charts).
const std::map<std::pair<int, int>, std::pair<int, int>> kTwinRaw = {
    {{0, 0}, {-360, 0}},       {{0, 90}, {-360, -270}},   {{0, 180}, {-360, -180}},  {{0, 270}, {-360, -90}},
    {{45, 45}, {45, -315}},    {{45, 135}, {-315, -225}}, {{45, 225}, {-315, -135}}, {{45, 315}, {-315, -45}},
    {{90, 90}, {90, -270}},    {{90, 180}, {-270, -180}}, {{90, 270}, {-270, -90}},  {{135, 45}, {-225, -315}},
    {{135, 135}, {-225, 135}}, {{135, 225}, {-225, -135}}, {{135, 315}, {-225, -45}}, {{180, 0}, {180, 0}},
    {{180, 90}, {-180, -270}}, {{180, 180}, {-180, 180}}, {{180, 270}, {-180, -90}}, {{225, 45}, {225, 45}},
    {{225, 135}, {-135, -225}}, {{225, 225}, {225, -135}}, {{225, 315}, {-135, -45}}, {{270, 0}, {-90, -360}},
    {{270, 90}, {-90, -270}},  {{270, 180}, {-90, -180}}, {{270, 270}, {270, -90}},  {{315, 45}, {-45, -315}},
    {{315, 135}, {-45, -225}}, {{315, 225}, {-45, -135}}, {{315, 315}, {315, -45}},
};

double num(const Value& v) {
    if (!v.is_number())
        throw ValueError(std::string("unsupported operand type for +: '") + py_type(v) + "'");
    return py_float(v);
}

// t + shift, rounded as int(round(x)): an int stays exact, a float rounds half to even.
Value shifted(const Value& t, const Value& shift) {
    if (t.is_int() && shift.is_int())
        return Value(py_int(t) + py_int(shift));
    return Value(round_half_even(num(t) + num(shift)));
}

Value times(const Value& note, const Value& bpm, const Value& beat0, const Value& shift) {
    Value n = note;
    if (!n.is_map())
        throw ValueError("a note must be an object, not " + py_repr(note));
    for (auto [key, beat] : {std::pair<const char*, const char*>{"t", "beat"}, {"end", "end_beat"}}) {
        if (const Value* b = n.find(beat)) {
            if (!truthy(bpm))
                throw ValueError(std::string("\"") + beat + "\" needs \"bpm\" in the chart file");
            double v = (truthy(beat0) ? num(beat0) : 0) + num(*b) * 60000.0 / num(bpm);
            n.erase(beat);
            n[key] = Value(v);
        }
        if (Value* k = n.find(key))
            *k = shifted(*k, shift);
    }
    if (Value* path = n.find("path"); path && path->is_list())
        for (Value& x : path->list)
            if (x.is_map())
                x = times(x, bpm, beat0, shift);
    if (Value* ticks = n.find("ticks"); ticks && ticks->is_list())
        for (Value& x : ticks->list)
            x = x.is_map() ? times(x, bpm, beat0, shift) : shifted(x, shift);
    return n;
}

void put(Loaded& l, Chart c) {
    if (!l.has(c.difficulty))
        l.order.push_back(c.difficulty);
    l.by_difficulty[c.difficulty] = std::move(c);
}

} // namespace

Loaded load(const fs::path& song_dir, const Value& shift) {
    Loaded l;
    fs::path combined = song_dir / "charts.json";
    if (is_file(combined)) {
        Value c = json_parse(read_text(combined));
        Value bpm = c.get("bpm"), beat0 = c.has("beat0_ms") ? c.get("beat0_ms") : c.get("offset_ms");
        l.preview = c.get("preview");
        l.stage = c.get("stage");
        Value diffs = c.get("difficulties", Value::of_map());
        Value levels = c.get("levels", Value::of_map());
        for (const Item& it : diffs.map) {
            Chart ch;
            ch.difficulty = py_str(it.k);
            ch.level = levels.get(ch.difficulty);
            for (const Value& n : it.v.list)
                ch.notes.push_back(times(n, bpm, beat0, shift));
            ch.bpm = bpm, ch.beat0 = beat0, ch.source = combined;
            put(l, std::move(ch));
        }
    }
    for (const std::string& d : DIFFICULTIES) {
        fs::path f = song_dir / (d + ".json");
        if (!is_file(f))
            continue;
        Value c = json_parse(read_text(f));
        Value bpm = c.get("bpm"), beat0 = c.has("offset_ms") ? c.get("offset_ms") : c.get("beat0_ms");
        Chart ch;
        ch.difficulty = d;
        ch.level = c.get("level");
        for (const Value& n : c.at("notes").list)
            ch.notes.push_back(times(n, bpm, beat0, shift));
        ch.bpm = bpm, ch.beat0 = beat0, ch.source = f;
        put(l, std::move(ch));
    }
    return l;
}

namespace {

using Strings = std::vector<std::string>;

std::string at_msg(const std::vector<int64_t>& times, const std::string& what) {
    return std::to_string(times.size()) + " " + what + " (first at " + std::to_string(times[0]) + " ms)";
}

std::string t_text(const Value& n) {
    return py_str(n.get("t"));
}

int64_t T(const Value& n, const char* key = "t") {
    return py_int(n.at(key));
}

bool in_lanes(const Value& lane, int count) {
    if (!lane.is_number())
        return false;
    double v = py_float(lane);
    return v == std::floor(v) && v >= 0 && v < count;
}

bool bad_dir(const Value& n, const char* k) {
    const Value* v = n.find(k);
    return v && !v->is_null() && (!v->is_int() || py_int(*v) % 45);
}

double lane_of(const Value& v) {
    return py_float(v);
}

Strings timing(const Chart& chart, const std::vector<Value>& notes, int64_t length_ms) {
    Strings warnings;
    std::set<int64_t> tset;
    for (const Value& n : notes)
        tset.insert(T(n));
    std::vector<int64_t> ts(tset.begin(), tset.end());
    if (chart.level.is_null())
        warnings.push_back("no level given");
    if (T(notes[0]) < 1200)
        warnings.push_back("first note at " + std::to_string(T(notes[0])) + " ms (official charts start 1.2 s in or later)");
    int64_t last = INT64_MIN;
    for (const Value& n : notes)
        last = std::max(last, n.has("end") ? T(n, "end") : T(n));
    if (length_ms - last < 2000)
        warnings.push_back("last note " + std::to_string(length_ms - last) + " ms before the song ends (under 2 s of outro)");
    std::vector<int64_t> short_holds;
    for (const Value& h : notes)
        if (h.get("type") == Value("hold") && T(h, "end") - T(h) < 100)
            short_holds.push_back(T(h));
    if (!short_holds.empty())
        warnings.push_back(at_msg(short_holds, "holds under 100 ms"));
    if (truthy(chart.bpm)) {
        double beat = 60000.0 / py_float(chart.bpm), b0 = truthy(chart.beat0) ? py_float(chart.beat0) : 0;
        auto off = [&](int64_t t, double step) {
            double x = (static_cast<double>(t) - b0) / step;
            return std::abs(x - std::nearbyint(x)) * step;
        };
        std::vector<int64_t> bad;
        for (int64_t t : ts)
            if (std::min(off(t, beat / 4), off(t, beat / 3)) >= 2)
                bad.push_back(t);
        if (!bad.empty())
            warnings.push_back(at_msg(bad, "notes off the " + format_g(py_float(chart.bpm)) + " BPM grid (16ths and triplets) by 2 ms or more"));
    }
    return warnings;
}

std::pair<Strings, Strings> field(const std::vector<Value>& notes, int64_t length_ms) {
    Strings errors, warnings;
    for (size_t i = 0; i < notes.size(); ++i) {
        const Value& n = notes[i];
        std::string where = "note " + std::to_string(i + 1) + " at " + t_text(n) + " ms";
        Value kind = n.get("type");
        if (!kind.is_str() || !kFieldTypes.count(kind.s)) {
            errors.push_back(where + ": unknown type " + py_repr(kind));
            continue;
        }
        if (!n.get("t").is_int()) {
            errors.push_back(where + ": no time");
            continue;
        }
        int64_t t = T(n);
        if (!(0 <= t && t < length_ms))
            errors.push_back(where + ": outside the song (0-" + std::to_string(length_ms) + " ms)");
        if (!in_lanes(n.get("lane"), 9)) {
            errors.push_back(where + ": lane (height) must be 0-8");
            continue;
        }
        for (const char* k : {"dir", "dir2", "end_dir"})
            if (bad_dir(n, k))
                errors.push_back(where + ": " + k + " must be a multiple of 45");
        if ((kind.s == "slide" || kind.s == "twin") && n.get("dir").is_null())
            errors.push_back(where + ": " + kind.s + " needs \"dir\"");
        if (kind.s == "twin" && n.get("dir2").is_null())
            errors.push_back(where + ": twin needs \"dir2\"");
        if (kind.s != "hold" && kind.s != "curve") {
            for (const char* k : {"end", "end_lane", "path", "end_dir", "end_critical", "ticks"}) {
                Value v = n.get(k);
                if (!(v.is_null() || v == Value(false)))
                    errors.push_back(where + ": \"" + k + "\" is only for holds");
            }
            continue;
        }
        if (!n.get("end").is_int() || T(n, "end") <= t) {
            errors.push_back(where + ": " + kind.s + " needs \"end\" after its start");
            continue;
        }
        int64_t end = T(n, "end");
        if (end >= length_ms)
            errors.push_back(where + ": " + kind.s + " ends after the song");
        if (end - t < 30)
            errors.push_back(where + ": hold of " + std::to_string(end - t) + " ms (under 30 ms: make it a tap)");
        std::vector<int64_t> pts;
        if (kind.s == "hold") {
            if (n.has("path") || n.has("end_lane"))
                errors.push_back(where + ": a hold keeps its height (a \"curve\" moves)");
            for (const Value& x : n.get("ticks", Value::of_list()).list)
                pts.push_back(x.is_map() ? T(x) : py_int(x));
        } else {
            Value path = n.get("path", Value::of_list());
            bool ok = path.is_list();
            for (const Value& p : path.list)
                ok = ok && p.is_map() && p.get("t").is_int();
            if (!ok) {
                errors.push_back(where + ": \"path\" must be a list of {\"t\", \"lane\"} points");
                continue;
            }
            bool heights_ok = in_lanes(n.get("end_lane"), 9);
            for (const Value& p : path.list)
                heights_ok = heights_ok && in_lanes(p.get("lane"), 9);
            if (!heights_ok) {
                errors.push_back(where + ": path and end_lane heights must be 0-8");
                continue;
            }
            std::vector<double> heights = {lane_of(n.at("lane"))};
            for (const Value& p : path.list) {
                pts.push_back(T(p));
                heights.push_back(lane_of(p.at("lane")));
            }
            heights.push_back(lane_of(n.at("end_lane")));
            if (std::set<double>(heights.begin(), heights.end()).size() == 1)
                warnings.push_back(where + ": the curve never changes height (a \"hold\" would do)");
            double d = heights[heights.size() - 1] - heights[heights.size() - 2]; // the game's up/down releases follow the last move (3009 of 3009)
            Value ed = n.get("end_dir");
            if ((ed == Value(0) && d < 0) || (ed == Value(180) && d > 0))
                warnings.push_back(where + ": the release slide points against the curve's last move");
        }
        bool rising = true;
        for (size_t k = 1; k < pts.size(); ++k)
            rising = rising && pts[k] > pts[k - 1];
        if (!pts.empty() && (!rising || pts[0] <= t || pts.back() >= end))
            errors.push_back(where + ": path/tick times must rise strictly between the start and the end");
    }
    if (!errors.empty())
        return {errors, warnings};
    // What happens at each moment: starts (and taps) at their height, hold ends at their end height.
    std::map<int64_t, std::vector<Value>> at;
    std::map<int64_t, int> starts;
    for (const Value& n : notes)
        ++starts[T(n)];
    for (const Value& n : notes) {
        at[T(n)].push_back(n.at("lane"));
        Value kind = n.get("type");
        if (kind == Value("hold") || kind == Value("curve"))
            at[T(n, "end")].push_back(n.has("end_lane") ? n.at("end_lane") : n.at("lane"));
    }
    for (auto& [t, hs] : at) {
        if (starts[t] > 2)
            errors.push_back(std::to_string(starts[t]) + " notes at " + std::to_string(t) + " ms (field charts have at most 2 at once)");
        std::vector<Value> unique;
        for (const Value& h : hs)
            if (std::find(unique.begin(), unique.end(), h) == unique.end())
                unique.push_back(h);
        if (unique.size() != hs.size()) {
            // max(hs, key=hs.count): the first height that occurs most often
            const Value* best = &hs[0];
            size_t best_n = 0;
            for (const Value& h : hs) {
                size_t c = static_cast<size_t>(std::count(hs.begin(), hs.end(), h));
                if (c > best_n)
                    best = &h, best_n = c;
            }
            errors.push_back("two notes at height " + py_repr(*best) + " at " + std::to_string(t) + " ms");
        } else if (hs.size() > 1) {
            std::vector<Value> sorted = hs;
            std::stable_sort(sorted.begin(), sorted.end(), [](const Value& a, const Value& b) { return py_float(a) < py_float(b); });
            if (py_float(sorted.back()) - py_float(sorted.front()) < 2)
                warnings.push_back("chord at " + std::to_string(t) + " ms with heights " + py_repr(Value::of_list(sorted)) +
                                   " (the game keeps them 2 or more apart)");
        }
    }
    std::vector<int64_t> ts, near;
    for (auto& kv : at)
        ts.push_back(kv.first);
    for (size_t k = 1; k < ts.size(); ++k)
        if (ts[k] - ts[k - 1] <= 15)
            near.push_back(ts[k]);
    if (!near.empty())
        errors.push_back(at_msg(near, "notes 1-15 ms after another one (make them one chord or space them)"));
    std::vector<const Value*> curves;
    for (const Value& n : notes)
        if (n.get("type") == Value("curve"))
            curves.push_back(&n);
    for (size_t k = 1; k < curves.size(); ++k) {
        const Value &a = *curves[k - 1], &b = *curves[k];
        if (T(b) < T(a, "end"))
            errors.push_back("curve at " + std::to_string(T(b)) + " ms starts before the curve at " + std::to_string(T(a)) + " ms ends (" +
                             std::to_string(T(a, "end")) + " ms)");
    }
    for (const Value& h : notes) {
        if (h.get("type") != Value("hold"))
            continue;
        for (const Value& n : notes) {
            if (&n == &h || n.at("lane") != h.at("lane") || !(T(h) <= T(n) && T(n) <= T(h, "end")))
                continue;
            if (!(n.get("type") == Value("hold") && T(n) == T(h)))
                errors.push_back(py_str(n.get("type")) + " at " + std::to_string(T(n)) + " ms is at height " + py_repr(h.at("lane")) +
                                 " while the hold at " + std::to_string(T(h)) + " ms holds it");
        }
    }
    std::vector<int64_t> inside;
    for (const Value* c : curves)
        for (const Value& n : notes)
            if (T(*c) < T(n) && T(n) < T(*c, "end"))
                inside.push_back(T(n));
    std::sort(inside.begin(), inside.end());
    if (!inside.empty())
        warnings.push_back(at_msg(inside, "notes during a curve (rare in the game's charts)"));
    return {errors, warnings};
}

} // namespace

std::pair<Strings, Strings> validate(const Chart& chart, int64_t length_ms, const std::string& stage) {
    Strings errors, warnings;
    std::vector<Value> notes = chart.notes;
    // sorted(notes, key=lambda n: n.get('t', 0))
    std::stable_sort(notes.begin(), notes.end(), [](const Value& a, const Value& b) {
        Value x = a.get("t", Value(0)), y = b.get("t", Value(0));
        return x.is_number() && y.is_number() && py_float(x) < py_float(y);
    });
    if (notes.empty())
        return {{"no notes"}, warnings};
    if (stage == "field") {
        auto [e, w] = field(notes, length_ms);
        if (e.empty()) {
            Strings more = timing(chart, notes, length_ms);
            w.insert(w.end(), more.begin(), more.end());
        }
        return {e, w};
    }
    std::map<double, int64_t> busy;
    std::map<int64_t, int> times;
    std::map<std::pair<int64_t, double>, int> lane_time;
    for (size_t i = 0; i < notes.size(); ++i) {
        const Value& n = notes[i];
        std::string where = "note " + std::to_string(i + 1) + " at " + t_text(n) + " ms";
        Value kind = n.get("type");
        if (!kind.is_str() || !kTypes.count(kind.s)) {
            errors.push_back(where + ": unknown type " + py_repr(kind));
            continue;
        }
        if (!n.get("t").is_int()) {
            errors.push_back(where + ": no time");
            continue;
        }
        int64_t t = T(n);
        if (!(0 <= t && t < length_ms))
            errors.push_back(where + ": outside the song (0-" + std::to_string(length_ms) + " ms)");
        if (!in_lanes(n.get("lane"), 4)) {
            errors.push_back(where + ": lane must be 0-3");
            continue;
        }
        for (const char* k : {"dir", "dir2", "end_dir"})
            if (bad_dir(n, k))
                errors.push_back(where + ": " + k + " must be a multiple of 45");
        if ((kind.s == "slide" || kind.s == "twin") && n.get("dir").is_null())
            errors.push_back(where + ": " + kind.s + " needs \"dir\"");
        if (kind.s == "twin" && n.get("dir2").is_null())
            errors.push_back(where + ": twin needs \"dir2\"");
        if (kind.s != "hold" && (!n.get("end_dir").is_null() || truthy(n.get("end_critical"))))
            errors.push_back(where + ": \"end_dir\" and \"end_critical\" are only for holds");
        if (kind.s == "hold") {
            if (!n.get("end").is_int() || T(n, "end") <= t) {
                errors.push_back(where + ": hold needs \"end\" after its start");
                continue;
            }
            int64_t end = T(n, "end");
            if (end >= length_ms)
                errors.push_back(where + ": hold ends after the song");
            if (end - t < 30)
                errors.push_back(where + ": hold of " + std::to_string(end - t) + " ms (under 30 ms: make it a tap)");
        }
        double lane = lane_of(n.at("lane"));
        int same = ++lane_time[{t, lane}];
        ++times[t];
        auto b = busy.find(lane);
        if (same > 1)
            errors.push_back(where + ": two notes in lane " + py_repr(n.at("lane")) + " at once");
        else if ((b == busy.end() ? -1 : b->second) >= t)
            errors.push_back(where + ": lane " + py_repr(n.at("lane")) + " is still held");
        else if (b != busy.end() && t - b->second < 40)
            errors.push_back(where + ": " + std::to_string(t - b->second) + " ms after the lane " + py_repr(n.at("lane")) +
                             " hold ends (under 40 ms)");
        if (kind.s == "hold" || b == busy.end() || b->second < t)
            busy[lane] = kind.s == "hold" ? T(n, "end") : -1000000000; // only hold ends need the 40 ms gap
    }
    for (auto& [t, c] : times)
        if (c > 4)
            errors.push_back(std::to_string(c) + " notes at " + std::to_string(t) + " ms (at most 4)");
    std::vector<int64_t> ts, near;
    for (auto& kv : times)
        ts.push_back(kv.first);
    for (size_t k = 1; k < ts.size(); ++k)
        if (ts[k] - ts[k - 1] <= 15)
            near.push_back(ts[k]);
    if (!near.empty())
        errors.push_back(at_msg(near, "notes 1-15 ms after another one (make them one chord or space them)"));
    if (!errors.empty())
        return {errors, warnings};
    Strings more = timing(chart, notes, length_ms);
    warnings.insert(warnings.end(), more.begin(), more.end());
    return {errors, warnings};
}

std::vector<Record> records(const std::vector<Value>& notes) {
    std::vector<Record> recs;
    auto mod360 = [](int64_t v) { return ((v % 360) + 360) % 360; };
    for (const Value& n : notes) {
        int64_t crit = truthy(n.get("critical")) ? 1 : 0, t = T(n), lane = py_int(n.at("lane"));
        std::string kind = py_str(n.at("type"));
        if (kind == "tap")
            recs.push_back({t, 0, lane, 0, 0, crit});
        else if (kind == "slide")
            recs.push_back({t, 1, lane, mod360(py_int(n.at("dir"))), 0, crit});
        else if (kind == "twin") {
            int d0 = static_cast<int>(mod360(py_int(n.at("dir")))), d1 = static_cast<int>(mod360(py_int(n.at("dir2"))));
            auto it = kTwinRaw.find({d0, d1});
            std::pair<int, int> raw = it != kTwinRaw.end() ? it->second : std::pair<int, int>{d0 - 360, d1 - 360};
            recs.push_back({t, 2, lane, raw.first, raw.second, crit});
        } else if (kind == "hold" || kind == "curve") {
            bool curve = kind == "curve";
            recs.push_back({t, curve ? 7 : 3, lane, 0, 0, crit});
            std::vector<Value> points;
            if (curve)
                points = n.get("path", Value::of_list()).list;
            else
                for (const Value& x : n.get("ticks", Value::of_list()).list) {
                    if (x.is_map())
                        points.push_back(x);
                    else {
                        Value p = Value::of_map();
                        p["t"] = x;
                        points.push_back(p);
                    }
                }
            for (const Value& p : points)
                recs.push_back({T(p), curve ? 8 : 4, p.has("lane") ? py_int(p.at("lane")) : lane, 0, 0, truthy(p.get("critical")) ? 1 : 0});
            Value e = n.get("end_dir");
            int64_t ecrit = truthy(n.get("end_critical")) ? 1 : 0;
            int64_t el = curve && n.has("end_lane") ? py_int(n.at("end_lane")) : lane;
            if (e.is_null())
                recs.push_back({T(n, "end"), curve ? 9 : 5, el, 0, 0, ecrit});
            else
                recs.push_back({T(n, "end"), curve ? 10 : 6, el, mod360(py_int(e)), 0, ecrit});
        }
    }
    std::stable_sort(recs.begin(), recs.end(), [](const Record& a, const Record& b) { return a.t != b.t ? a.t < b.t : a.lane < b.lane; });
    return recs;
}

std::string trigger_file(const std::vector<Value>& notes, const std::string& stage, int64_t length_ms) {
    std::vector<Record> recs = records(notes);
    Writer w;
    w.i32(ms_type(stage));
    w.i32(1);
    w.i32(0);
    w.i32(0);
    w.i32(0);
    w.i64(length_ms);
    w.i32(static_cast<int32_t>(recs.size()));
    w.i32(0);
    w.i32(0);
    w.i32(0);
    for (const Record& r : recs) {
        for (int64_t v : {r.t, r.t, r.type, r.lane, r.a1, r.a2, r.crit})
            w.i32(static_cast<int32_t>(v));
        for (int k = 0; k < 7; ++k)
            w.i32(0);
    }
    return std::move(w.d);
}

std::pair<std::string, std::vector<Value>> read_trigger(std::string_view data) {
    Reader r(data);
    int32_t mt = r.i32();
    r.p = 28;
    int32_t count = r.i32();
    std::string stage = mt == 0 ? "field" : mt == 1 ? "battle" : mt == 2 ? "event" : throw ValueError(std::to_string(mt));
    std::vector<Value> notes;
    std::vector<size_t> linear, curves; // open holds, oldest first (indices into notes)
    for (int32_t i = 0; i < count; ++i) {
        r.p = 44 + 56 * static_cast<size_t>(i);
        int32_t t = r.i32();
        r.i32();
        int32_t ty = r.i32(), lane = r.i32(), a1 = r.i32(), a2 = r.i32(), crit = r.i32();
        auto mod360 = [](int64_t v) { return ((v % 360) + 360) % 360; };
        Value note = Value::of_map();
        if (ty == 0 || ty == 1 || ty == 2) {
            note["t"] = t;
            note["type"] = ty == 0 ? "tap" : ty == 1 ? "slide" : "twin";
            note["lane"] = lane;
            if (ty)
                note["dir"] = Value(static_cast<int64_t>(mod360(a1)));
            if (ty == 2)
                note["dir2"] = Value(static_cast<int64_t>(mod360(a2)));
        } else if (ty == 3 || ty == 7) {
            note["t"] = t;
            note["type"] = ty == 3 ? "hold" : "curve";
            note["lane"] = lane;
            if (ty == 7) {
                note["path"] = Value::of_list();
                curves.push_back(notes.size());
            } else
                linear.push_back(notes.size());
        } else if (ty == 4 || ty == 5 || ty == 6 || ty == 8 || ty == 9 || ty == 10) {
            std::vector<size_t>& open = ty < 7 ? linear : curves;
            auto it = open.begin();
            if (ty < 7)
                while (it != open.end() && py_int(notes[*it].at("lane")) != lane)
                    ++it;
            if (it == open.end())
                throw ValueError(std::to_string(t) + " ms: a hold record (type " + std::to_string(ty) + ") with no open hold");
            Value& h = notes[*it];
            if (ty == 4) {
                if (stage == "field") {
                    Value tick;
                    if (crit) {
                        tick = Value::of_map();
                        tick["t"] = t;
                        tick["critical"] = true;
                    } else
                        tick = Value(t);
                    if (!h.has("ticks"))
                        h["ticks"] = Value::of_list();
                    h["ticks"].list.push_back(tick);
                }
            } else if (ty == 8) {
                Value p = Value::of_map();
                p["t"] = t;
                p["lane"] = lane;
                if (crit)
                    p["critical"] = true;
                h["path"].list.push_back(p);
            } else {
                h["end"] = t;
                if (ty == 6 || ty == 10)
                    h["end_dir"] = Value(static_cast<int64_t>(mod360(a1)));
                if (ty >= 9)
                    h["end_lane"] = lane;
                if (crit)
                    h["end_critical"] = true;
                open.erase(it);
            }
            continue;
        } else
            continue; // Info (11): in no game chart
        if (crit)
            note["critical"] = true;
        notes.push_back(std::move(note));
    }
    if (!linear.empty() || !curves.empty())
        throw ValueError(std::to_string(linear.size() + curves.size()) + " holds never end");
    return {stage, notes};
}

} // namespace dlcmaker::chart
