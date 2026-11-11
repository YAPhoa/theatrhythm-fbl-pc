// SPDX-License-Identifier: GPL-2.0-or-later
// English names for the IDs a save holds (characters, items, songs, worlds, cards...), and the
// characters' portraits, read from the player's decrypted dump with the DLC maker's core. Without a
// dump the editor shows the IDs alone.
#pragma once

#include "image.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dlcmaker {
struct Game;
}

namespace saveeditor {

struct Names {
    std::unordered_map<int64_t, std::string> by_id;
    std::unordered_map<int64_t, int64_t> sort;     // the game's own order, where its table has one
    std::map<int64_t, dlcmaker::image::Image> faces; // character, moogle or airship ID -> its 128 x 128 picture
    std::unordered_map<int64_t, std::string> effect_text;           // summon stone effect: "EXP received +{}%"
    std::unordered_map<int64_t, std::vector<double>> effect_values; // its value at each rarity (0.05 = 5%)
    std::map<int64_t, int64_t> music_series; // song -> the series the game files it under (SortSeriesID)
    std::map<int64_t, int64_t> music_type;   // song -> its stage type (MSType: 0 FMS, 1 BMS, 2 EMS)
    bool loaded = false;

    // Reads every table it names IDs from. Returns "" or what went wrong (not a dump, a missing table).
    std::string load(const std::string& game_dir);
    std::string get(int64_t id) const;
    // A summon stone effect at a rarity: "EXP received +10%" (its name alone when unknown).
    std::string effect(int64_t id, int64_t rarity) const;

private:
    void airships(dlcmaker::Game& game);
};

// What kind of thing an ID is (its first digits): "Character", "Item", ... or "" when unknown.
const char* kind_of(int64_t id);

} // namespace saveeditor
