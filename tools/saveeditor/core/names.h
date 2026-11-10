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

namespace dlcmaker {
struct Game;
}

namespace saveeditor {

struct Names {
    std::unordered_map<int64_t, std::string> by_id;
    std::unordered_map<int64_t, int64_t> sort;     // the game's own order, where its table has one
    std::map<int64_t, dlcmaker::image::Image> faces; // character, moogle or airship ID -> its 128 x 128 picture
    bool loaded = false;

    // Reads every table it names IDs from. Returns "" or what went wrong (not a dump, a missing table).
    std::string load(const std::string& game_dir);
    std::string get(int64_t id) const;

private:
    void airships(dlcmaker::Game& game);
};

// What kind of thing an ID is (its first digits): "Character", "Item", ... or "" when unknown.
const char* kind_of(int64_t id);

} // namespace saveeditor
