// SPDX-License-Identifier: GPL-2.0-or-later
// The game's save files (backup.inf, musicBackup.inf, proficaBackup.inf) as Gummi.Backup writes them:
// a u32 length, then BackupCryptography's block (salt and IV interleaved in its first 32 bytes, then
// AES-128-CBC with a PBKDF2-SHA1 key from the game's password) around raw Deflate around MessagePack.
// Nodes keep the format byte each value was read with, so an unchanged file writes back byte for byte.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace saveeditor {

struct SaveError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Node {
    enum class K : uint8_t { Nil, Bool, Int, Float, Str, Bin, Ext, Array, Map };
    K k = K::Nil;
    uint8_t tag = 0;    // the format byte it was read with (0: new or changed, written in the shortest form)
    bool b = false;
    int64_t i = 0;      // Int (the bit pattern of a uint64 above INT64_MAX when u64)
    bool u64 = false;
    double f = 0;       // Float (tag 0xca: float32)
    std::string s;      // Str (UTF-8), Bin, Ext data
    int8_t ext = 0;     // Ext type (-1: timestamp)
    std::vector<Node> a;
    std::vector<std::pair<Node, Node>> m;

    bool is_map() const { return k == K::Map; }
    bool is_array() const { return k == K::Array; }
    bool is_scalar() const { return k != K::Map && k != K::Array; }
    Node* find(std::string_view key);
    const Node* find(std::string_view key) const;
    std::string key_text() const;   // a map key as text (string, or the int in decimal)
};

Node unpack(std::string_view data);       // SaveError when malformed
std::string pack(const Node& n);
std::string to_json(const Node& n, int indent = 1);
std::string timestamp_text(const Node& n); // an Ext -1 as "YYYY-MM-DD hh:mm:ss UTC", else ""

// The file's bytes <-> the MessagePack inside. SaveError when it does not decrypt.
std::string decrypt_file(std::string_view file);
std::string encrypt_file(std::string_view msgpack);

} // namespace saveeditor
