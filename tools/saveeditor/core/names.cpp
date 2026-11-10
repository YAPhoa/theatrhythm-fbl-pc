// SPDX-License-Identifier: GPL-2.0-or-later
#include "names.h"

#include "game.h"
#include "unity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace saveeditor {

using dlcmaker::Game;
using dlcmaker::Table;
using dlcmaker::Value;

namespace {

constexpr int64_t kNone = 4294967295; // the tables' "no ID"

int64_t num(const Value& v) {
    return v.is_number() ? dlcmaker::py_int(v) : (v.is_str() ? std::atoll(v.s.c_str()) : kNone);
}

// The texture in the bundle that holds `key` (as the DLC maker's game_image reads it), or an empty image.
// `name`: the one so named, where the bundle holds several (a summon's icon among its effects).
dlcmaker::image::Image texture(Game& game, const std::string& key, const std::string& name = "") {
    using namespace dlcmaker;
    if (!game.base().has(Key(key)))
        return {};
    unity::Bundle b = unity::Bundle::parse(game.read_bundle(game.bundle_path(Key(key))));
    for (auto& o : b.objects()) {
        if (o.class_id() != unity::kTexture2D)
            continue;
        Value t = o.read();
        if (!name.empty() && t.at("m_Name").s != name)
            continue;
        const Value& sd = t.at("m_StreamData");
        std::string data = b.node_ending(".resS").data.substr(static_cast<size_t>(py_int(sd.at("offset"))),
                                                              static_cast<size_t>(py_int(sd.at("size"))));
        return image::decode_texture(data, static_cast<int>(py_int(t.at("m_Width"))), static_cast<int>(py_int(t.at("m_Height"))),
                                     static_cast<int>(py_int(t.at("m_TextureFormat"))));
    }
    return {};
}

// `im` cut to what is drawn (not transparent), centred on a square and made 128 x 128.
dlcmaker::image::Image square_portrait(dlcmaker::image::Image im) {
    namespace image = dlcmaker::image;
    int box[4];
    if (im.w <= 0 || !image::bbox(im, box))
        return {};
    im = image::crop(im, box[0], box[1], box[2], box[3]);
    int side = std::max(im.w, im.h);
    image::Image sq{side, side, std::vector<uint8_t>(static_cast<size_t>(side) * side * 4, 0)};
    int ox = (side - im.w) / 2, oy = (side - im.h) / 2;
    for (int y = 0; y < im.h; ++y)
        std::copy_n(&im.rgba[static_cast<size_t>(y) * im.w * 4], static_cast<size_t>(im.w) * 4,
                    &sq.rgba[(static_cast<size_t>(y + oy) * side + ox) * 4]);
    return image::resize_lanczos(sq, 128, 128);
}

} // namespace

// The airship sprites: Sprite objects name them and hold a render data key; the SpriteAtlas maps each
// key to its rectangle in the atlas texture (from the bottom left; packed upright, settingsRaw 67).
void Names::airships(Game& game) {
    using namespace dlcmaker;
    std::string key = std::string(P) + "Atlas/Airship/Airship.spriteatlas";
    if (!game.base().has(Key(key)))
        return;
    image::Image atlas = texture(game, key);
    if (atlas.w <= 0)
        return;
    unity::Bundle b = unity::Bundle::parse(game.read_bundle(game.bundle_path(Key(key))));
    auto guid = [](const Value& k) { // [{data[0..3]}, file id]
        std::string s;
        for (const char* d : {"data[0]", "data[1]", "data[2]", "data[3]"})
            s += std::to_string(py_int(k.list.at(0).at(d))) + ".";
        return s + std::to_string(py_int(k.list.at(1)));
    };
    std::map<std::string, int64_t> sprites; // render data key -> airship ID
    std::map<std::string, const Value*> rects;
    std::vector<Value> keep;
    keep.reserve(64);
    for (auto& o : b.objects()) {
        if (o.class_id() != 213 && o.class_id() != 687078895) // Sprite, SpriteAtlas
            continue;
        keep.push_back(o.read());
        const Value& v = keep.back();
        if (o.class_id() == 213) {
            const std::string& name = v.at("m_Name").s;
            if (name.rfind("AIRSHIP", 0) == 0)
                sprites[guid(v.at("m_RenderDataKey"))] = 370000000 + std::atoll(name.c_str() + 7);
        } else {
            for (const Value& e : v.at("m_RenderDataMap").list)
                rects[guid(e.list.at(0))] = &e.list.at(1).at("textureRect");
        }
    }
    for (const auto& [k, id] : sprites) {
        auto r = rects.find(k);
        if (r == rects.end())
            continue;
        const Value& t = *r->second;
        auto f = [&](const char* n) { return static_cast<int>(std::lround(dlcmaker::py_float(t.at(n)))); };
        int x0 = f("x"), w = f("width"), h = f("height"), y0 = atlas.h - f("y") - h;
        x0 = std::clamp(x0, 0, atlas.w);
        y0 = std::clamp(y0, 0, atlas.h);
        image::Image sprite = image::crop(atlas, x0, y0, std::min(atlas.w, x0 + w), std::min(atlas.h, y0 + h));
        if (image::Image im = square_portrait(std::move(sprite)); im.w > 0)
            faces[id] = std::move(im);
    }
}

const char* kind_of(int64_t id) {
    switch (id / 1000000) {
    case 50: return "World";
    case 60: return "Quest stage";
    case 70: return "Quest";
    case 150: return "Profile card illustration";
    case 160: return "Song";
    case 180: return "Series";
    case 210: return "Theater movie";
    case 230: return "Character";
    case 240: return "Ability";
    case 250: return "Monster";
    case 270: return "Item";
    case 280: return "Summon";
    case 290: return "Summon stone effect";
    case 300: return "CollectaCard";
    case 320: return "Rhythpo reward";
    case 360: return "Moogle";
    case 370: return "Airship";
    case 390: return "Feat";
    case 430: return "Tip";
    case 460: return "Help";
    }
    return "";
}

std::string Names::get(int64_t id) const {
    auto it = by_id.find(id);
    return it == by_id.end() ? "" : it->second;
}

std::string Names::load(const std::string& game_dir) {
    by_id.clear();
    sort.clear();
    faces.clear();
    loaded = false;
    try {
        Game game(game_dir);
        // Plain text tables: ID prefix, text prefab, term path.
        struct Plain { int64_t base; const char* prefab; const char* term; };
        static const Plain plain[] = {
            {160000000, "Table/MusicTable", "Table/MusicTable/MusicName/MUSIC"},
            {180000000, "Table/Table_Series", "Table/Table_Series/SeriesName/SERIES"},
            {230000000, "Table/Table_Character", "Table/Table_Character/CharacterName/CHARACTER"},
            {240000000, "Table/Table_Ability", "Table/Table_Ability/AbilityName/ABILITY"},
            {250000000, "Table/Table_Monster", "Table/Table_Monster/MonsterName/MONSTER"},
            {270000000, "Table/Item", "Table/Item/ItemName/ITEM"},
            {280000000, "Table/Table_SummonMonster", "Table/Table_SummonMonster/SummonMonsterName/SUMMONMONSTER"},
            {360000000, "Table/Table_Moogle", "Table/Table_Moogle/MoogleName/MOOGLE"},
            {370000000, "Table/Table_AirShip", "Table/Table_AirShip/AirShipName/AIRSHIP"},
            {390000000, "Table/Table_Achievement", "Table/Table_Achievement/AcievementName/ACHIEVEMENT"},
            {430000000, "Table/Table_Tips", "Table/Table_Tips/TipsName/TIPS"},
            {460000000, "Table/Table_Help", "Table/Table_Help/HelpTitleName/HELP"},
        };
        for (const Plain& p : plain) {
            Value texts = game.texts(p.prefab);
            std::string prefix = p.term;
            for (const auto& it : texts.map) {
                if (!it.k.is_str() || it.k.s.rfind(prefix, 0) != 0 || !it.v.is_str() || it.v.s.empty())
                    continue;
                std::string digits = it.k.s.substr(prefix.size());
                if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
                    continue;
                by_id[p.base + std::atoll(digits.c_str())] = it.v.s;
            }
        }
        for (const char* t : {"CharacterTable", "ItemTable", "MoogleTable", "AirShipTable", "MusicTable", "CollecaTable", "QuestWorldTable"})
            for (const auto& [id, row] : game.table(t).rows)
                if (const Value* o = row.find("SortOrder"))
                    sort[id] = num(*o);
        auto name = [&](int64_t id) { return id == kNone ? std::string() : get(id); };
        for (const auto& [id, row] : game.table("TheaterTable").rows)
            by_id[id] = name(num(row.get("MusicID")));
        for (const auto& [id, row] : game.table("QuestWorldTable").rows)
            by_id[id] = name(num(row.get("SeriesID")));
        for (const auto& [id, row] : game.table("QuestSquareTable").rows) {
            std::string world = name(num(row.get("QuestWorld"))), song = name(num(row.get("Music")));
            by_id[id] = song.empty() ? world : world.empty() ? song : song + " (" + world + ")";
        }
        // A card or illustration: the character, summon, monster, moogle or song on it.
        auto subject = [&](const Value& row) {
            for (const char* k : {"CharacterID", "SummonMonsterID", "MoogleID", "MusicID"})
                if (const Value* v = row.find(k))
                    if (std::string n = name(num(*v)); !n.empty())
                        return n;
            if (const Value* v = row.find("MonsterID")) {
                if (v->is_list()) {
                    for (const Value& m : v->list)
                        if (std::string n = name(num(m)); !n.empty())
                            return n;
                } else if (std::string n = name(num(*v)); !n.empty()) {
                    return n;
                }
            }
            return std::string();
        };
        for (const auto& [id, row] : game.table("CollecaTable").rows) {
            static const char* rarity[] = {"", " (Rare)", " (Premium)"};
            int64_t r = num(row.get("CollecaRarity"));
            std::string s = "No. " + std::to_string(num(row.get("CardNumber"))) + " " + subject(row);
            by_id[id] = s + (r >= 0 && r < 3 ? rarity[r] : "");
        }
        for (const auto& [id, row] : game.table("ProficaIllustTable").rows) {
            std::string s = subject(row);
            by_id[id] = s.empty() ? "Illustration " + std::to_string(num(row.get("Group"))) : s;
            sort[id] = num(row.get("Group")) * 1000 + num(row.get("Order"));
        }
        for (const auto& [id, row] : game.table("RhythpoRewardTable").rows) {
            std::string what = name(num(row.get("RewardIDValue")));
            int64_t count = num(row.get("GetCount"));
            by_id[id] = std::to_string(num(row.get("TargetRhythpo"))) + " Rhythpo" +
                        (what.empty() ? "" : ": " + what + (count > 1 ? " x" + std::to_string(count) : ""));
        }
        // Portraits: one small bundle per character (a few seconds for all of them).
        for (const auto& [id, row] : game.table("CharacterTable").rows) {
            char key[160];
            std::snprintf(key, sizeof key, "%sTextures/CharacterFaceStandard/CHARACTER%07lld.png", dlcmaker::P,
                          static_cast<long long>(id - 230000000));
            try {
                if (dlcmaker::image::Image im = texture(game, key); im.w > 0)
                    faces[id] = std::move(im);
            } catch (const std::exception&) {
                // that one shows without a portrait
            }
        }
        // Moogle styles: the 512 x 512 art cut to the moogle and made square.
        for (const auto& [id, row] : game.table("MoogleTable").rows) {
            char key[160];
            std::snprintf(key, sizeof key, "%sTextures/Moogle/MOOGLE%07lld.png", dlcmaker::P, static_cast<long long>(id - 360000000));
            try {
                if (dlcmaker::image::Image im = square_portrait(texture(game, key)); im.w > 0)
                    faces[id] = std::move(im);
            } catch (const std::exception&) {
                // that one shows without a portrait
            }
        }
        // Summons: each has an icon (what the summon stones show), in a bundle with its effects' textures.
        auto key_name = [](long long n) {
            char s[32];
            std::snprintf(s, sizeof s, "SUMMONMONSTER%07lld", n);
            return std::string(s);
        };
        for (const auto& [id, row] : game.table("SummonMonsterTable").rows) {
            char key[200];
            long long n = static_cast<long long>(id - 280000000);
            std::snprintf(key, sizeof key, "%sSummonMonster/SUMMONMONSTER%07lld/Icon/SUMMONMONSTER%07lld.png", dlcmaker::P, n, n);
            try {
                if (dlcmaker::image::Image im = square_portrait(texture(game, key, key_name(n))); im.w > 0)
                    faces[id] = std::move(im);
            } catch (const std::exception&) {
                // that one shows by name
            }
        }
        // Airships: the party screen's sprite atlas, one sprite AIRSHIPnnnnnnn per airship.
        try {
            airships(game);
        } catch (const std::exception&) {
            // they show by name
        }
        loaded = true;
        return "";
    } catch (const std::exception& e) {
        by_id.clear();
        sort.clear();
        faces.clear();
        return e.what();
    }
}

} // namespace saveeditor
