// SPDX-License-Identifier: GPL-2.0-or-later
#include "savefile.h"

#include "stb_image.h"
#include "stb_image_write.h"

// stb_image_write's Deflate (defined with its implementation, in the DLC maker's image.cpp; the header
// declares it only there).
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

#include <windows.h>
#include <bcrypt.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace saveeditor {

namespace {

// BackupCryptography.Decrypt's default password (the literal its callers pass when they have none).
constexpr char kPassword[] = "ニほヷボやぞァげラ1QlゥマルコベりvのェハヤM";
constexpr ULONG kIterations = 1000; // Rfc2898DeriveBytes(string, byte[]): SHA1, 1000 rounds

// --- MessagePack ----------------------------------------------------------------------------------

struct Reader {
    std::string_view d;
    size_t p = 0;
    uint8_t u8() {
        if (p >= d.size())
            throw SaveError("the save data ends early");
        return static_cast<uint8_t>(d[p++]);
    }
    uint64_t be(int n) {
        uint64_t v = 0;
        for (int k = 0; k < n; ++k)
            v = v << 8 | u8();
        return v;
    }
    std::string bytes(uint64_t n) {
        if (n > d.size() - p)
            throw SaveError("the save data ends early");
        std::string s(d.substr(p, static_cast<size_t>(n)));
        p += static_cast<size_t>(n);
        return s;
    }
};

Node read(Reader& r, int depth = 0) {
    if (depth > 64)
        throw SaveError("the save data nests too deep");
    Node n;
    uint8_t t = r.u8();
    n.tag = t;
    auto int_of = [&](int64_t v) { n.k = Node::K::Int; n.i = v; };
    auto str_of = [&](uint64_t len, Node::K k) { n.k = k; n.s = r.bytes(len); };
    auto arr_of = [&](uint64_t len) {
        n.k = Node::K::Array;
        for (uint64_t j = 0; j < len; ++j)
            n.a.push_back(read(r, depth + 1));
    };
    auto map_of = [&](uint64_t len) {
        n.k = Node::K::Map;
        for (uint64_t j = 0; j < len; ++j) {
            Node key = read(r, depth + 1);
            Node val = read(r, depth + 1);
            n.m.emplace_back(std::move(key), std::move(val));
        }
    };
    auto ext_of = [&](uint64_t len) {
        n.k = Node::K::Ext;
        n.ext = static_cast<int8_t>(r.u8());
        n.s = r.bytes(len);
    };
    if (t <= 0x7f) int_of(t);
    else if (t <= 0x8f) map_of(t & 0x0f);
    else if (t <= 0x9f) arr_of(t & 0x0f);
    else if (t <= 0xbf) str_of(t & 0x1f, Node::K::Str);
    else if (t >= 0xe0) int_of(static_cast<int8_t>(t));
    else switch (t) {
    case 0xc0: n.k = Node::K::Nil; break;
    case 0xc2: case 0xc3: n.k = Node::K::Bool; n.b = t == 0xc3; break;
    case 0xc4: str_of(r.be(1), Node::K::Bin); break;
    case 0xc5: str_of(r.be(2), Node::K::Bin); break;
    case 0xc6: str_of(r.be(4), Node::K::Bin); break;
    case 0xc7: ext_of(r.be(1)); break;
    case 0xc8: ext_of(r.be(2)); break;
    case 0xc9: ext_of(r.be(4)); break;
    case 0xca: {
        uint32_t bits = static_cast<uint32_t>(r.be(4));
        float f;
        std::memcpy(&f, &bits, 4);
        n.k = Node::K::Float;
        n.f = f;
        break;
    }
    case 0xcb: {
        uint64_t bits = r.be(8);
        std::memcpy(&n.f, &bits, 8);
        n.k = Node::K::Float;
        break;
    }
    case 0xcc: int_of(static_cast<int64_t>(r.be(1))); break;
    case 0xcd: int_of(static_cast<int64_t>(r.be(2))); break;
    case 0xce: int_of(static_cast<int64_t>(r.be(4))); break;
    case 0xcf: {
        uint64_t v = r.be(8);
        int_of(static_cast<int64_t>(v));
        n.u64 = v > static_cast<uint64_t>(INT64_MAX);
        break;
    }
    case 0xd0: int_of(static_cast<int8_t>(r.be(1))); break;
    case 0xd1: int_of(static_cast<int16_t>(r.be(2))); break;
    case 0xd2: int_of(static_cast<int32_t>(r.be(4))); break;
    case 0xd3: int_of(static_cast<int64_t>(r.be(8))); break;
    case 0xd4: ext_of(1); break;
    case 0xd5: ext_of(2); break;
    case 0xd6: ext_of(4); break;
    case 0xd7: ext_of(8); break;
    case 0xd8: ext_of(16); break;
    case 0xd9: str_of(r.be(1), Node::K::Str); break;
    case 0xda: str_of(r.be(2), Node::K::Str); break;
    case 0xdb: str_of(r.be(4), Node::K::Str); break;
    case 0xdc: arr_of(r.be(2)); break;
    case 0xdd: arr_of(r.be(4)); break;
    case 0xde: map_of(r.be(2)); break;
    case 0xdf: map_of(r.be(4)); break;
    default: throw SaveError("unknown MessagePack byte in the save data");
    }
    return n;
}

void put_be(std::string& out, uint64_t v, int n) {
    for (int k = n - 1; k >= 0; --k)
        out += static_cast<char>((v >> (8 * k)) & 0xff);
}

// Whether `v` fits the int format `t`.
bool int_fits(uint8_t t, int64_t v, bool u64) {
    if (u64)
        return t == 0xcf;
    if (t <= 0x7f) return v >= 0 && v <= 0x7f;
    if (t >= 0xe0) return v >= -32 && v < 0;
    switch (t) {
    case 0xcc: return v >= 0 && v <= 0xff;
    case 0xcd: return v >= 0 && v <= 0xffff;
    case 0xce: return v >= 0 && v <= 0xffffffffLL;
    case 0xcf: return v >= 0;
    case 0xd0: return v >= INT8_MIN && v <= INT8_MAX;
    case 0xd1: return v >= INT16_MIN && v <= INT16_MAX;
    case 0xd2: return v >= INT32_MIN && v <= INT32_MAX;
    case 0xd3: return true;
    }
    return false;
}

void write_int(std::string& out, uint8_t t, int64_t v, bool u64) {
    if (!int_fits(t, v, u64)) { // shortest form, as MessagePack-CSharp writes it
        if (u64) t = 0xcf;
        else if (v >= 0) t = v <= 0x7f ? 0x00 : v <= 0xff ? 0xcc : v <= 0xffff ? 0xcd : v <= 0xffffffffLL ? 0xce : 0xcf;
        else t = v >= -32 ? 0xe0 : v >= INT8_MIN ? 0xd0 : v >= INT16_MIN ? 0xd1 : v >= INT32_MIN ? 0xd2 : 0xd3;
    }
    uint64_t bits = static_cast<uint64_t>(v);
    if (t <= 0x7f || t >= 0xe0) { out += static_cast<char>(bits & 0xff); return; }
    out += static_cast<char>(t);
    int n = (t == 0xcc || t == 0xd0) ? 1 : (t == 0xcd || t == 0xd1) ? 2 : (t == 0xce || t == 0xd2) ? 4 : 8;
    put_be(out, bits, n);
}

// A length header: the original format `tag` when the length fits it, else the shortest of the fix
// form (`fix` | length, up to `fix_max`; none when `fix_max` is 0) and the 8 (when `f8`), 16 and 32
// bit forms.
void write_len(std::string& out, uint8_t tag, size_t len, uint8_t fix, size_t fix_max, uint8_t f8, uint8_t f16, uint8_t f32) {
    auto is_fix = [&](uint8_t t) { return fix_max && t >= fix && t <= fix + fix_max; };
    auto fits = [&](uint8_t t) {
        if (is_fix(t)) return len <= fix_max;
        if (f8 && t == f8) return len <= 0xff;
        if (t == f16) return len <= 0xffff;
        if (t == f32) return len <= 0xffffffffULL;
        return false;
    };
    uint8_t t = tag;
    if (!tag || !fits(tag))
        t = (fix_max && len <= fix_max) ? fix : (f8 && len <= 0xff) ? f8 : len <= 0xffff ? f16 : f32;
    if (is_fix(t)) {
        out += static_cast<char>(fix | len);
        return;
    }
    out += static_cast<char>(t);
    put_be(out, len, t == f8 ? 1 : t == f16 ? 2 : 4);
}

void write(std::string& out, const Node& n) {
    switch (n.k) {
    case Node::K::Nil: out += '\xc0'; break;
    case Node::K::Bool: out += n.b ? '\xc3' : '\xc2'; break;
    case Node::K::Int: write_int(out, n.tag, n.i, n.u64); break;
    case Node::K::Float:
        if (n.tag == 0xca) {
            float f = static_cast<float>(n.f);
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            out += '\xca';
            put_be(out, bits, 4);
        } else {
            uint64_t bits;
            std::memcpy(&bits, &n.f, 8);
            out += '\xcb';
            put_be(out, bits, 8);
        }
        break;
    case Node::K::Str: write_len(out, n.tag, n.s.size(), 0xa0, 31, 0xd9, 0xda, 0xdb); out += n.s; break;
    case Node::K::Bin: write_len(out, n.tag, n.s.size(), 0, 0, 0xc4, 0xc5, 0xc6); out += n.s; break;
    case Node::K::Ext: {
        static const size_t sizes[] = {1, 2, 4, 8, 16}; // fixext 1..16: 0xd4..0xd8
        int fixed = -1;
        for (int j = 0; j < 5; ++j)
            if (n.s.size() == sizes[j])
                fixed = j;
        bool was_var = n.tag >= 0xc7 && n.tag <= 0xc9;
        if (fixed >= 0 && !was_var)
            out += static_cast<char>(0xd4 + fixed);
        else
            write_len(out, was_var ? n.tag : 0, n.s.size(), 0, 0, 0xc7, 0xc8, 0xc9);
        out += static_cast<char>(n.ext);
        out += n.s;
        break;
    }
    case Node::K::Array:
        write_len(out, n.tag, n.a.size(), 0x90, 15, 0, 0xdc, 0xdd);
        for (const Node& c : n.a)
            write(out, c);
        break;
    case Node::K::Map:
        write_len(out, n.tag, n.m.size(), 0x80, 15, 0, 0xde, 0xdf);
        for (const auto& [key, val] : n.m) {
            write(out, key);
            write(out, val);
        }
        break;
    }
}

void json_str(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out += static_cast<char>(c);
    }
    out += '"';
}

std::string hex(std::string_view s) {
    static const char* digits = "0123456789abcdef";
    std::string h;
    for (unsigned char c : s) { h += digits[c >> 4]; h += digits[c & 15]; }
    return h;
}

std::string number_text(const Node& n) {
    if (n.k == Node::K::Int)
        return n.u64 ? std::to_string(static_cast<uint64_t>(n.i)) : std::to_string(n.i);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", n.f);
    return buf;
}

void json(std::string& out, const Node& n, int indent, int level) {
    auto nl = [&](int l) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(l * indent), ' ');
    };
    switch (n.k) {
    case Node::K::Nil: out += "null"; break;
    case Node::K::Bool: out += n.b ? "true" : "false"; break;
    case Node::K::Int: case Node::K::Float: {
        std::string t = number_text(n);
        out += (std::isfinite(n.f) || n.k == Node::K::Int) ? t : "null";
        break;
    }
    case Node::K::Str: json_str(out, n.s); break;
    case Node::K::Bin: json_str(out, "bin:" + hex(n.s)); break;
    case Node::K::Ext: {
        std::string ts = timestamp_text(n);
        json_str(out, ts.empty() ? "ext" + std::to_string(n.ext) + ":" + hex(n.s) : ts);
        break;
    }
    case Node::K::Array:
        out += '[';
        for (size_t j = 0; j < n.a.size(); ++j) {
            if (j) out += ',';
            nl(level + 1);
            json(out, n.a[j], indent, level + 1);
        }
        if (!n.a.empty()) nl(level);
        out += ']';
        break;
    case Node::K::Map:
        out += '{';
        for (size_t j = 0; j < n.m.size(); ++j) {
            if (j) out += ',';
            nl(level + 1);
            json_str(out, n.m[j].first.key_text());
            out += ": ";
            json(out, n.m[j].second, indent, level + 1);
        }
        if (!n.m.empty()) nl(level);
        out += '}';
        break;
    }
}

// --- crypto ---------------------------------------------------------------------------------------

struct Alg {
    BCRYPT_ALG_HANDLE h = nullptr;
    Alg(const wchar_t* id, ULONG flags) {
        if (BCryptOpenAlgorithmProvider(&h, id, nullptr, flags) < 0)
            throw SaveError("Windows cryptography is not available");
    }
    ~Alg() { BCryptCloseAlgorithmProvider(h, 0); }
};

std::string derive_key(std::string_view salt) {
    Alg sha1(BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    unsigned char key[16];
    std::string pw = kPassword;
    if (BCryptDeriveKeyPBKDF2(sha1.h, reinterpret_cast<PUCHAR>(pw.data()), static_cast<ULONG>(pw.size()),
                              reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())), static_cast<ULONG>(salt.size()),
                              kIterations, key, sizeof key, 0) < 0)
        throw SaveError("could not derive the save key");
    return std::string(reinterpret_cast<char*>(key), sizeof key);
}

// AES-128-CBC with PKCS7 padding.
std::string aes(bool encrypt, std::string_view key, std::string iv, std::string_view data) {
    Alg aes(BCRYPT_AES_ALGORITHM, 0);
    BCryptSetProperty(aes.h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                      sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    BCRYPT_KEY_HANDLE k = nullptr;
    if (BCryptGenerateSymmetricKey(aes.h, &k, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                   static_cast<ULONG>(key.size()), 0) < 0)
        throw SaveError("could not set the save key");
    std::string out(data.size() + 16, '\0');
    ULONG n = 0;
    auto in = reinterpret_cast<PUCHAR>(const_cast<char*>(data.data()));
    auto o = reinterpret_cast<PUCHAR>(out.data());
    auto ivp = reinterpret_cast<PUCHAR>(iv.data());
    NTSTATUS st = encrypt ? BCryptEncrypt(k, in, static_cast<ULONG>(data.size()), nullptr, ivp, 16, o, static_cast<ULONG>(out.size()), &n, BCRYPT_BLOCK_PADDING)
                          : BCryptDecrypt(k, in, static_cast<ULONG>(data.size()), nullptr, ivp, 16, o, static_cast<ULONG>(out.size()), &n, BCRYPT_BLOCK_PADDING);
    BCryptDestroyKey(k);
    if (st < 0)
        throw SaveError("this is not a Theatrhythm save file (it does not decrypt)");
    out.resize(n);
    return out;
}

uint32_t adler_size_le(std::string_view s) {
    uint32_t v = 0;
    std::memcpy(&v, s.data(), 4);
    return v;
}

} // namespace

Node* Node::find(std::string_view key) {
    for (auto& [k2, v] : m)
        if (k2.k == K::Str && k2.s == key)
            return &v;
    return nullptr;
}

const Node* Node::find(std::string_view key) const {
    return const_cast<Node*>(this)->find(key);
}

std::string Node::key_text() const {
    if (k == K::Str)
        return s;
    if (k == K::Int || k == K::Float)
        return number_text(*this);
    return "?";
}

Node unpack(std::string_view data) {
    Reader r{data};
    Node n = read(r);
    if (r.p != data.size())
        throw SaveError("the save data has bytes after its end");
    return n;
}

std::string pack(const Node& n) {
    std::string out;
    write(out, n);
    return out;
}

std::string to_json(const Node& n, int indent) {
    std::string out;
    json(out, n, indent, 0);
    return out;
}

std::string timestamp_text(const Node& n) {
    if (n.k != Node::K::Ext || n.ext != -1)
        return "";
    int64_t sec = 0;
    uint32_t nsec = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(n.s.data());
    auto be = [&](int off, int len) {
        uint64_t v = 0;
        for (int j = 0; j < len; ++j)
            v = v << 8 | p[off + j];
        return v;
    };
    if (n.s.size() == 4) sec = static_cast<int64_t>(be(0, 4));
    else if (n.s.size() == 8) { uint64_t v = be(0, 8); nsec = static_cast<uint32_t>(v >> 34); sec = static_cast<int64_t>(v & 0x3ffffffffULL); }
    else if (n.s.size() == 12) { nsec = static_cast<uint32_t>(be(0, 4)); sec = static_cast<int64_t>(be(4, 8)); }
    else return "";
    (void)nsec;
    if (sec < -62135596800LL) // DateTime.MinValue: never set
        return "never";
    __time64_t t = sec;
    std::tm tm{};
    if (_gmtime64_s(&tm, &t) != 0)
        return "never";
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

std::string decrypt_file(std::string_view file) {
    if (file.size() < 4 + 32 + 16)
        throw SaveError("this is not a Theatrhythm save file (too short)");
    uint32_t n = adler_size_le(file);
    if (n != file.size() - 4 || (n - 32) % 16)
        throw SaveError("this is not a Theatrhythm save file (its length does not match)");
    std::string_view blob = file.substr(4);
    std::string salt = std::string(blob.substr(0, 8)) + std::string(blob.substr(24, 8));
    std::string iv = std::string(blob.substr(16, 8)) + std::string(blob.substr(8, 8));
    std::string plain = aes(false, derive_key(salt), iv, blob.substr(32));
    int len = 0;
    char* out = stbi_zlib_decode_noheader_malloc(plain.data(), static_cast<int>(plain.size()), &len);
    if (!out)
        throw SaveError("the save data does not decompress");
    std::string data(out, static_cast<size_t>(len));
    std::free(out);
    return data;
}

std::string encrypt_file(std::string_view msgpack) {
    int len = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(msgpack.data())),
                                          static_cast<int>(msgpack.size()), &len, 8);
    if (!z || len < 6)
        throw SaveError("could not compress the save data");
    std::string raw(reinterpret_cast<char*>(z) + 2, static_cast<size_t>(len - 6)); // zlib minus header and Adler-32
    std::free(z);
    unsigned char rnd[32];
    if (BCryptGenRandom(nullptr, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw SaveError("no random numbers for the save's salt");
    std::string salt(reinterpret_cast<char*>(rnd), 16), iv(reinterpret_cast<char*>(rnd) + 16, 16);
    std::string body = aes(true, derive_key(salt), iv, raw);
    std::string blob = salt.substr(0, 8) + iv.substr(8, 8) + iv.substr(0, 8) + salt.substr(8, 8) + body;
    std::string file(4, '\0');
    uint32_t n = static_cast<uint32_t>(blob.size());
    std::memcpy(file.data(), &n, 4);
    return file + blob;
}

} // namespace saveeditor
