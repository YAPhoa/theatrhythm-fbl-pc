// SPDX-License-Identifier: GPL-2.0-or-later
// Read-only access to the player's decrypted dump: the Addressables catalog (and a writer for a pack's
// own), bundle decryption and the base tables and texts. Nothing from the game goes into a project;
// packs are built from the player's own dump.
#pragma once

#include "util.h"
#include "value.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dlcmaker {

extern const char* const P;    // the shared assets' key prefix
extern const char* const TEXT; // the text sources' key prefix

// The game's tables: MessagePack {"LoadTableDataDictionary": {id: [row]}}.
Value msgpack_unpack(std::string_view data);
std::string msgpack_pack(const Value& v);

// A catalog key: a string or an int.
struct Key {
    bool is_int = false;
    int64_t i = 0;
    std::string s;
    Key() = default;
    Key(std::string str) : s(std::move(str)) {}
    Key(const char* str) : s(str) {}
    bool operator==(const Key& o) const { return is_int == o.is_int && (is_int ? i == o.i : s == o.s); }
    static Key of_int(int64_t v) {
        Key k;
        k.is_int = true;
        k.i = v;
        return k;
    }
};
struct KeyHash {
    size_t operator()(const Key& k) const { return k.is_int ? std::hash<int64_t>()(k.i) : std::hash<std::string>()(k.s); }
};

using Entry = std::array<int32_t, 7>; // internal id, provider, dependency key, dependency hash, extra data, primary key, type

// The base catalog as read from the dump.
struct CatalogBase {
    Value json;
    std::vector<std::pair<Key, std::vector<int32_t>>> buckets;
    std::vector<Key> keys;                                      // by_key's keys, in order
    std::unordered_map<Key, std::vector<int32_t>, KeyHash> by_key;
    std::vector<Entry> entries;
    std::vector<std::string> types;
    std::string extra;
    const Value& internal_ids() const { return json.at("m_InternalIds"); }
    explicit CatalogBase(const fs::path& path);
    const std::vector<int32_t>& at(const Key& k) const;
    bool has(const Key& k) const { return by_key.count(k) != 0; }
};

// A pack's catalog: base entries copied under new keys, plain DLC bundles added.
struct Catalog {
    std::shared_ptr<const CatalogBase> base;
    explicit Catalog(std::shared_ptr<const CatalogBase> b) : base(std::move(b)) {}

    void copy(const Key& src, const Key& dst);
    void add(const Key& key, const std::string& internal_id, const std::string& bundle, const std::string& type_name, uint64_t bundle_size = 0);
    void write(const fs::path& path, const std::string& locator) const;
    int type_index(const std::string& name) const;

private:
    std::vector<Key> keys_;
    std::unordered_map<Key, int32_t, KeyHash> key_index_;
    std::vector<std::vector<int32_t>> out_buckets_;
    std::vector<std::array<int64_t, 7>> out_entries_;
    std::map<std::pair<int32_t, int32_t>, int32_t> entry_map_;
    std::vector<std::string> ids_;
    std::unordered_map<std::string, int32_t> id_index_;
    std::string extra_;

    int32_t key_id(const Key& k);
    int32_t internal(const std::string& s);
    int32_t copy_entry(int32_t e, int32_t primary);
    int32_t options(const std::string& name, uint64_t size);
};

// Base table rows by id, in the table's order.
struct Table {
    std::vector<std::pair<int64_t, Value>> rows;
    std::unordered_map<int64_t, size_t> index;
    const Value* get(int64_t id) const;
};

struct Game {
    fs::path dir, aa;
    std::string secret;

    explicit Game(const std::string& game_dir); // Exit when it is not a decrypted dump
    Catalog catalog() const { return Catalog(base_); }
    const CatalogBase& base() const { return *base_; }
    fs::path bundle_path(const Key& key) const;      // the base bundle file holding `key`
    std::string read_bundle(const fs::path& path) const; // decrypted when it is
    const Table& table(const std::string& name);         // Data/<name>.bytes
    Value texts(const std::string& name);                 // Text/<name>.prefab as {term: English text}

private:
    std::shared_ptr<const CatalogBase> base_;
    std::map<std::string, Table> tables_;
};

std::string decrypt_bundle(std::string_view data, const std::string& secret);
std::string game_secret(const fs::path& game_dir);

} // namespace dlcmaker
