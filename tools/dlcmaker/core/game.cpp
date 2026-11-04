// SPDX-License-Identifier: GPL-2.0-or-later
#include "game.h"

#include "unity.h"

#include <algorithm>
#include <cstring>

namespace dlcmaker {

const char* const P = "PlatformCommon/RegionCommon/LanguageCommon/";
const char* const TEXT = "PS4/RegionCommon/LanguageCommon/Text/";

// --- MessagePack (the subset the game's tables use) -----------------------------------------------

namespace {

std::string be(uint64_t v, int n) {
    std::string out;
    for (int k = n - 1; k >= 0; --k)
        out += static_cast<char>(v >> (8 * k));
    return out;
}

void pack_into(std::string& out, const Value& o) {
    switch (o.t) {
    case Value::T::Null:
        out += '\xc0';
        return;
    case Value::T::Bool:
        out += o.b ? '\xc3' : '\xc2';
        return;
    case Value::T::Int:
        if (o.i >= 0 && o.i < 128)
            out += static_cast<char>(o.i);
        else if (o.i >= -32 && o.i < 0)
            out += static_cast<char>(static_cast<int8_t>(o.i));
        else if (o.i >= 0 && o.i < (int64_t(1) << 32))
            out += '\xce', out += be(static_cast<uint64_t>(o.i), 4);
        else
            out += '\xd3', out += be(static_cast<uint64_t>(o.i), 8);
        return;
    case Value::T::Float: {
        uint64_t u;
        std::memcpy(&u, &o.f, 8);
        out += '\xcb', out += be(u, 8);
        return;
    }
    case Value::T::Str: {
        size_t n = o.s.size();
        if (n < 32)
            out += static_cast<char>(0xa0 | n);
        else if (n < 256)
            out += '\xd9', out += static_cast<char>(n);
        else
            out += '\xda', out += be(n, 2);
        out += o.s;
        return;
    }
    case Value::T::List: {
        size_t n = o.list.size();
        if (n < 16)
            out += static_cast<char>(0x90 | n);
        else
            out += '\xdc', out += be(n, 2);
        for (const Value& x : o.list)
            pack_into(out, x);
        return;
    }
    case Value::T::Map: {
        size_t n = o.map.size();
        if (n < 16)
            out += static_cast<char>(0x80 | n);
        else
            out += '\xde', out += be(n, 2);
        for (const Item& it : o.map)
            pack_into(out, it.k), pack_into(out, it.v);
        return;
    }
    case Value::T::Bytes:
        break;
    }
    throw ValueError("bytes cannot be packed");
}

// Text as Python's decode('utf8', 'replace'): each malformed sequence becomes U+FFFD.
std::string utf8_replace(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        size_t ok = n ? 1 : 0;
        while (ok && ok < n && i + ok < s.size() && (static_cast<unsigned char>(s[i + ok]) & 0xC0) == 0x80)
            ++ok;
        if (n && ok == n) {
            out.append(s.substr(i, n));
            i += n;
        } else {
            out += "\xEF\xBF\xBD";
            i += ok ? ok : 1;
        }
    }
    return out;
}

Value unpack(Reader& r) {
    unsigned t = r.u8();
    auto items = [&](size_t n, bool map) {
        Value v = map ? Value::of_map() : Value::of_list();
        for (size_t k = 0; k < n; ++k) {
            if (map) {
                Value key = unpack(r);
                Value val = unpack(r);
                v.set(std::move(key), std::move(val));
            } else
                v.list.push_back(unpack(r));
        }
        return v;
    };
    if (t <= 0x7f)
        return Value(static_cast<int64_t>(t));
    if (t >= 0xe0)
        return Value(static_cast<int64_t>(t) - 256);
    if (t >= 0x80 && t <= 0x8f)
        return items(t & 15, true);
    if (t >= 0x90 && t <= 0x9f)
        return items(t & 15, false);
    if (t >= 0xa0 && t <= 0xbf)
        return Value(utf8_replace(r.bytes(t & 31)));
    switch (t) {
    case 0xc0:
        return Value();
    case 0xc2:
        return Value(false);
    case 0xc3:
        return Value(true);
    case 0xc4:
    case 0xc5:
    case 0xc6: {
        size_t n = static_cast<size_t>(r.uint(t == 0xc4 ? 1 : t == 0xc5 ? 2 : 4));
        return Value::bytes(std::string(r.bytes(n)));
    }
    case 0xca:
        return Value(static_cast<double>(r.f32()));
    case 0xcb:
        return Value(r.f64());
    case 0xcc:
    case 0xcd:
    case 0xce:
    case 0xcf:
        return Value(static_cast<int64_t>(r.uint(size_t(1) << (t - 0xcc))));
    case 0xd0:
        return Value(static_cast<int64_t>(r.i8()));
    case 0xd1:
        return Value(static_cast<int64_t>(r.i16()));
    case 0xd2:
        return Value(static_cast<int64_t>(r.i32()));
    case 0xd3:
        return Value(r.i64());
    case 0xd9:
    case 0xda:
    case 0xdb: {
        size_t n = static_cast<size_t>(r.uint(t == 0xd9 ? 1 : t == 0xda ? 2 : 4));
        return Value(utf8_replace(r.bytes(n)));
    }
    case 0xdc:
    case 0xdd:
        return items(static_cast<size_t>(r.uint(t == 0xdc ? 2 : 4)), false);
    case 0xde:
    case 0xdf:
        return items(static_cast<size_t>(r.uint(t == 0xde ? 2 : 4)), true);
    default:
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%x", t);
        throw ValueError(buf);
    }
}

} // namespace

Value msgpack_unpack(std::string_view data) {
    Reader r(data, true);
    return unpack(r);
}

std::string msgpack_pack(const Value& v) {
    std::string out;
    pack_into(out, v);
    return out;
}

// --- catalog --------------------------------------------------------------------------------------

namespace {

int32_t le32(std::string_view b, size_t at) {
    if (at + 4 > b.size())
        throw ValueError("catalog data ends early");
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k)
        v |= static_cast<uint32_t>(static_cast<unsigned char>(b[at + k])) << (8 * k);
    return static_cast<int32_t>(v);
}

void put32(std::string& out, int64_t v) {
    for (int k = 0; k < 4; ++k)
        out += static_cast<char>(static_cast<uint64_t>(v) >> (8 * k));
}

Key key_at(std::string_view kd, size_t off) {
    unsigned t = static_cast<unsigned char>(kd.at(off++));
    if (t == 0 || t == 1) {
        int32_t n = le32(kd, off);
        std::string_view raw = kd.substr(off + 4, static_cast<size_t>(n));
        return Key(t == 0 ? std::string(raw) : utf16le_to_utf8(raw));
    }
    if (t == 4)
        return Key::of_int(le32(kd, off));
    throw ValueError("key type " + std::to_string(t));
}

size_t extra_len(std::string_view xd, size_t off) {
    if (static_cast<unsigned char>(xd.at(off)) != 7)
        throw ValueError("extra type " + std::to_string(static_cast<unsigned char>(xd[off])));
    size_t a = static_cast<unsigned char>(xd.at(off + 1)), c = static_cast<unsigned char>(xd.at(off + 2 + a));
    return 7 + a + c + static_cast<size_t>(le32(xd, off + 3 + a + c));
}

} // namespace

CatalogBase::CatalogBase(const fs::path& path) {
    json = json_parse(read_file(path));
    std::string kd = base64_decode(json.at("m_KeyDataString").s), bd = base64_decode(json.at("m_BucketDataString").s);
    std::string ed = base64_decode(json.at("m_EntryDataString").s);
    extra = base64_decode(json.at("m_ExtraDataString").s);
    int32_t n = le32(bd, 0);
    size_t o = 4;
    for (int32_t k = 0; k < n; ++k) {
        int32_t doff = le32(bd, o), cnt = le32(bd, o + 4);
        o += 8;
        std::vector<int32_t> es;
        for (int32_t j = 0; j < cnt; ++j, o += 4)
            es.push_back(le32(bd, o));
        buckets.emplace_back(key_at(kd, static_cast<size_t>(doff)), std::move(es));
    }
    for (auto& [k, es] : buckets) {
        auto [it, fresh] = by_key.insert_or_assign(k, es);
        if (fresh)
            keys.push_back(k);
    }
    int32_t ne = le32(ed, 0);
    for (int32_t k = 0; k < ne; ++k) {
        Entry e;
        for (int j = 0; j < 7; ++j)
            e[j] = le32(ed, 4 + static_cast<size_t>(k) * 28 + 4 * j);
        entries.push_back(e);
    }
    for (const Value& t : json.at("m_resourceTypes").list)
        types.push_back(t.at("m_ClassName").s);
}

const std::vector<int32_t>& CatalogBase::at(const Key& k) const {
    auto it = by_key.find(k);
    if (it == by_key.end())
        throw ValueError("'" + (k.is_int ? std::to_string(k.i) : k.s) + "'");
    return it->second;
}

int32_t Catalog::key_id(const Key& k) {
    auto it = key_index_.find(k);
    if (it != key_index_.end())
        return it->second;
    int32_t id = static_cast<int32_t>(keys_.size());
    key_index_[k] = id;
    keys_.push_back(k);
    out_buckets_.emplace_back();
    return id;
}

int32_t Catalog::internal(const std::string& s) {
    auto it = id_index_.find(s);
    if (it != id_index_.end())
        return it->second;
    int32_t id = static_cast<int32_t>(ids_.size());
    id_index_[s] = id;
    ids_.push_back(s);
    return id;
}

int Catalog::type_index(const std::string& name) const {
    for (size_t k = 0; k < base->types.size(); ++k)
        if (base->types[k] == name)
            return static_cast<int>(k);
    throw ValueError("'" + name + "' is not in list");
}

int32_t Catalog::copy_entry(int32_t e, int32_t primary) {
    auto found = entry_map_.find({e, primary});
    if (found != entry_map_.end())
        return found->second;
    const Entry& en = base->entries.at(static_cast<size_t>(e));
    int32_t dep = en[2], di = en[4];
    int32_t ndep = -1;
    if (dep >= 0) {
        const auto& [dk, des] = base->buckets.at(static_cast<size_t>(dep));
        ndep = key_id(dk);
        for (int32_t x : des) {
            int32_t n = copy_entry(x, ndep);
            auto& b = out_buckets_[static_cast<size_t>(ndep)];
            if (std::find(b.begin(), b.end(), n) == b.end())
                b.push_back(n);
        }
    }
    int32_t ndi = -1;
    if (di >= 0) {
        ndi = static_cast<int32_t>(extra_.size());
        extra_ += base->extra.substr(static_cast<size_t>(di), extra_len(base->extra, static_cast<size_t>(di)));
    }
    int32_t idx = static_cast<int32_t>(out_entries_.size());
    entry_map_[{e, primary}] = idx;
    out_entries_.push_back({internal(base->internal_ids().list.at(static_cast<size_t>(en[0])).s), en[1], ndep, en[3], ndi, primary, en[6]});
    return idx;
}

void Catalog::copy(const Key& src, const Key& dst) {
    int32_t nk = key_id(dst);
    for (int32_t e : base->at(src)) {
        int32_t n = copy_entry(e, nk);
        out_buckets_[static_cast<size_t>(nk)].push_back(n);
    }
}

int32_t Catalog::options(const std::string& name, uint64_t size) {
    Value o = Value::of_map();
    o["m_Hash"] = "";
    o["m_Crc"] = 0;
    o["m_Timeout"] = 0;
    o["m_ChunkedTransfer"] = false;
    o["m_RedirectLimit"] = -1;
    o["m_RetryCount"] = 0;
    o["m_BundleName"] = name;
    o["m_AssetLoadMode"] = 0;
    o["m_BundleSize"] = Value(static_cast<int64_t>(size));
    o["m_UseCrcForCachedBundles"] = false;
    o["m_UseUWRForLocalBundles"] = false;
    o["m_ClearOtherCachedVersionsWhenLoaded"] = false;
    JsonStyle compact;
    compact.compact = true; // separators=(',', ':')
    std::string u = utf8_to_utf16le(json_dump(o, compact));
    const std::string a = "Unity.ResourceManager, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null";
    const std::string cl = "UnityEngine.ResourceManagement.ResourceProviders.AssetBundleRequestOptions";
    int32_t off = static_cast<int32_t>(extra_.size());
    extra_ += '\x07';
    extra_ += static_cast<char>(a.size());
    extra_ += a;
    extra_ += static_cast<char>(cl.size());
    extra_ += cl;
    put32(extra_, static_cast<int64_t>(u.size()));
    extra_ += u;
    return off;
}

void Catalog::add(const Key& key, const std::string& internal_id, const std::string& bundle, const std::string& type_name,
                  uint64_t bundle_size) {
    int32_t bk = key_id(Key(bundle + ".bundle"));
    if (out_buckets_[static_cast<size_t>(bk)].empty()) {
        out_buckets_[static_cast<size_t>(bk)].push_back(static_cast<int32_t>(out_entries_.size()));
        int32_t id = internal("{DLC.LoadPath}/" + bundle + ".bundle");
        int32_t opt = options(bundle, bundle_size);
        out_entries_.push_back({id, 1, -1, 0, opt, bk, type_index("UnityEngine.ResourceManagement.ResourceProviders.IAssetBundleResource")});
    }
    int32_t k = key_id(key);
    out_buckets_[static_cast<size_t>(k)].push_back(static_cast<int32_t>(out_entries_.size()));
    int32_t id = internal(internal_id);
    out_entries_.push_back({id, 2, bk, 0, -1, k, type_index(type_name)});
}

void Catalog::write(const fs::path& path, const std::string& locator) const {
    std::string kout;
    put32(kout, static_cast<int64_t>(keys_.size()));
    std::vector<int64_t> offsets;
    for (const Key& k : keys_) {
        offsets.push_back(static_cast<int64_t>(kout.size()));
        if (k.is_int) {
            kout += '\x04';
            put32(kout, k.i);
            continue;
        }
        std::string raw = is_ascii(k.s) ? k.s : utf8_to_utf16le(k.s);
        kout += is_ascii(k.s) ? '\x00' : '\x01';
        put32(kout, static_cast<int64_t>(raw.size()));
        kout += raw;
    }
    std::string bout;
    put32(bout, static_cast<int64_t>(keys_.size()));
    for (size_t i = 0; i < keys_.size(); ++i) {
        put32(bout, offsets[i]);
        put32(bout, static_cast<int64_t>(out_buckets_[i].size()));
        for (int32_t e : out_buckets_[i])
            put32(bout, e);
    }
    std::string eout;
    put32(eout, static_cast<int64_t>(out_entries_.size()));
    for (const auto& e : out_entries_)
        for (int64_t x : e)
            put32(eout, x);
    Value c = base->json;
    c["m_LocatorId"] = locator;
    Value ids = Value::of_list();
    for (const std::string& s : ids_)
        ids.list.push_back(Value(s));
    c["m_InternalIds"] = std::move(ids);
    c["m_KeyDataString"] = base64_encode(kout);
    c["m_BucketDataString"] = base64_encode(bout);
    c["m_EntryDataString"] = base64_encode(eout);
    c["m_ExtraDataString"] = base64_encode(extra_);
    write_file(path, json_dump(c));
}

// --- the dump -------------------------------------------------------------------------------------

namespace {

constexpr int kKeyLiteral = 1152; // the string literal AssetBundleCryptographyParam.get_EncryptionKey decodes

std::string literal(std::string_view metadata, int index) {
    Reader r(metadata);
    uint32_t sanity = r.u32();
    int32_t version = r.i32();
    if (sanity != 0xFAB11BAF || version != 29)
        throw ValueError("unsupported metadata (version " + std::to_string(version) + ")");
    uint32_t table = r.u32(), table_size = r.u32(), data = r.u32();
    if (static_cast<uint32_t>(index) * 8 + 8 > table_size)
        throw ValueError("no string literal " + std::to_string(index));
    Reader t(metadata, false, table + static_cast<size_t>(index) * 8);
    uint32_t length = t.u32(), offset = t.u32();
    return std::string(metadata.substr(data + static_cast<size_t>(offset), length));
}

// AssetBundleCryptography.DecryptKey, then get_EncryptionKey's Substring(2, Length - 4): base64 of
// big-endian 16-bit words; each pair XORed with the previous character's bits 8-11 (0x5963 to start)
// gives one character, last character first.
std::string password(std::string_view encoded) {
    std::string raw = base64_decode(encoded);
    std::vector<uint32_t> words;
    for (size_t i = 0; i + 1 < raw.size(); i += 2)
        words.push_back(static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 8 | static_cast<unsigned char>(raw[i + 1]));
    std::vector<uint32_t> chars(words.size() / 2);
    uint32_t previous = 0x5963;
    for (size_t k = 0; k < chars.size(); ++k) {
        uint32_t value = previous ^ words[2 * k + 1] ^ words[2 * k];
        chars[chars.size() - 1 - k] = value;
        previous = value >> 8 & 0xf;
    }
    std::string out;
    for (uint32_t c : chars)
        put_utf8(out, c);
    // [2:-2] of the characters
    std::vector<size_t> starts;
    for (size_t i = 0; i < out.size(); ++i)
        if ((static_cast<unsigned char>(out[i]) & 0xC0) != 0x80)
            starts.push_back(i);
    if (starts.size() < 4)
        return "";
    return out.substr(starts[2], starts[starts.size() - 2] - starts[2]);
}

} // namespace

std::string game_secret(const fs::path& game_dir) {
    return password(literal(read_file(game_dir / "Media" / "Metadata" / "global-metadata.dat"), kKeyLiteral));
}

std::string decrypt_bundle(std::string_view data, const std::string& secret) {
    if (data.size() < 48 || data.size() % 16)
        throw ValueError("not an encrypted bundle");
    // readAESParameter + setupAES: the header's 8-byte quarters are salt[0:8], iv[8:16], iv[0:8],
    // salt[8:16]; the key is PBKDF2-HMAC-SHA1(secret, salt, 1000).
    std::string salt = std::string(data.substr(0, 8)) + std::string(data.substr(24, 8));
    std::string iv = std::string(data.substr(16, 8)) + std::string(data.substr(8, 8));
    std::string key = pbkdf2_sha1(secret, salt, 1000, 16);
    std::string plain = aes128_cbc_decrypt(key, iv, data.substr(32));
    unsigned pad = plain.empty() ? 0 : static_cast<unsigned char>(plain.back());
    bool ok = pad >= 1 && pad <= 16 && plain.size() >= pad && plain.rfind(std::string("UnityFS\0", 8), 0) == 0;
    for (size_t k = 0; ok && k < pad; ++k)
        ok = static_cast<unsigned char>(plain[plain.size() - 1 - k]) == pad;
    if (!ok)
        throw ValueError("cannot decrypt it: another game version?");
    plain.resize(plain.size() - pad);
    return plain;
}

const Value* Table::get(int64_t id) const {
    auto it = index.find(id);
    return it == index.end() ? nullptr : &rows[it->second].second;
}

Game::Game(const std::string& game_dir) {
    std::string d = !game_dir.empty() ? game_dir : env("THEATRHYTHM_GAME_DIR").value_or("");
    dir = py_path(d);
    aa = dir / "Media" / "StreamingAssets" / "aa";
    if (!is_file(aa / "catalog.json"))
        throw Exit("not a decrypted game dump: " + u8(dir) + " (pass --game DIR or set THEATRHYTHM_GAME_DIR)");
    secret = game_secret(dir);
    base_ = std::make_shared<const CatalogBase>(aa / "catalog.json");
}

fs::path Game::bundle_path(const Key& key) const {
    const CatalogBase& c = *base_;
    const Entry& e = c.entries.at(static_cast<size_t>(c.at(key).at(0)));
    const Entry& dep = c.entries.at(static_cast<size_t>(c.buckets.at(static_cast<size_t>(e[2])).second.at(0)));
    const std::string& id = c.internal_ids().list.at(static_cast<size_t>(dep[0])).s;
    size_t cut = id.find("}/");
    if (cut == std::string::npos)
        throw ValueError("not a bundle path: " + id);
    return aa / py_path(id.substr(cut + 2));
}

std::string Game::read_bundle(const fs::path& path) const {
    std::string raw = read_file(path);
    return raw.rfind("UnityFS", 0) == 0 ? raw : decrypt_bundle(raw, secret);
}

const Table& Game::table(const std::string& name) {
    auto found = tables_.find(name);
    if (found != tables_.end())
        return found->second;
    unity::Bundle b = unity::Bundle::parse(read_bundle(bundle_path(Key(std::string(P) + "Data/" + name + ".bytes"))));
    Table t;
    for (auto& o : b.objects()) {
        if (o.class_id() != unity::kTextAsset)
            continue;
        Value rows = msgpack_unpack(o.read().at("m_Script").s).at("LoadTableDataDictionary");
        for (Item& it : rows.map) {
            if (!it.k.is_int() || it.v.list.empty())
                continue;
            t.index[it.k.i] = t.rows.size();
            t.rows.emplace_back(it.k.i, std::move(it.v.list[0]));
        }
        break;
    }
    return tables_.emplace(name, std::move(t)).first->second;
}

Value Game::texts(const std::string& name) {
    unity::Bundle b = unity::Bundle::parse(read_bundle(bundle_path(Key(std::string(TEXT) + name + ".prefab"))));
    Value out = Value::of_map();
    for (auto& o : b.objects()) {
        if (o.class_id() != unity::kMonoBehaviour)
            continue;
        Value t = o.read();
        const Value& src = t.at("mSource");
        size_t en = 0;
        bool found = false;
        for (const Value& l : src.at("mLanguages").list) {
            if (l.get("Code") == Value("en-US")) {
                found = true;
                break;
            }
            ++en;
        }
        if (!found)
            throw ValueError("'en-US' is not in list");
        for (const Value& term : src.at("mTerms").list)
            out.set(term.at("Term"), term.at("Languages").list.at(en));
        return out;
    }
    return out;
}

} // namespace dlcmaker
