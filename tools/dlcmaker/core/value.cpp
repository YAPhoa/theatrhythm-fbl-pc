// SPDX-License-Identifier: GPL-2.0-or-later
#include "value.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dlcmaker {

// --- values ---------------------------------------------------------------------------------------

Value::Value() = default;
Value::Value(std::nullptr_t) {}
Value::Value(bool v) : t(T::Bool), b(v) {}
Value::Value(int v) : t(T::Int), i(v) {}
Value::Value(unsigned v) : t(T::Int), i(v) {}
Value::Value(long v) : t(T::Int), i(v) {}
Value::Value(long long v) : t(T::Int), i(v) {}
Value::Value(unsigned long v) : t(T::Int), i(static_cast<int64_t>(v)) {}
Value::Value(unsigned long long v) : t(T::Int), i(static_cast<int64_t>(v)) {}
Value::Value(double v) : t(T::Float), f(v) {}
Value::Value(const char* v) : t(T::Str), s(v) {}
Value::Value(std::string v) : t(T::Str), s(std::move(v)) {}
Value::Value(std::string_view v) : t(T::Str), s(v) {}
Value::Value(const Value&) = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(const Value&) = default;
Value& Value::operator=(Value&&) noexcept = default;
Value::~Value() = default;

Value Value::bytes(std::string data) {
    Value v;
    v.t = T::Bytes;
    v.s = std::move(data);
    return v;
}

Value Value::of_list(std::vector<Value> items) {
    Value v;
    v.t = T::List;
    v.list = std::move(items);
    return v;
}

Value Value::of_map() {
    Value v;
    v.t = T::Map;
    return v;
}

namespace {

double number_of(const Value& v) {
    return v.t == Value::T::Float ? v.f : v.t == Value::T::Bool ? (v.b ? 1 : 0) : static_cast<double>(v.i);
}

bool key_is(const Value& k, std::string_view s) {
    return k.t == Value::T::Str && k.s == s;
}

} // namespace

bool operator==(const Value& a, const Value& b) {
    if (a.is_number() && b.is_number()) {
        if (a.t != Value::T::Float && b.t != Value::T::Float)
            return (a.t == Value::T::Bool ? a.b : a.i) == (b.t == Value::T::Bool ? b.b : b.i);
        return number_of(a) == number_of(b);
    }
    if (a.t != b.t)
        return false;
    switch (a.t) {
    case Value::T::Null:
        return true;
    case Value::T::Str:
    case Value::T::Bytes:
        return a.s == b.s;
    case Value::T::List:
        return a.list == b.list;
    case Value::T::Map:
        if (a.map.size() != b.map.size())
            return false;
        for (const Item& x : a.map) {
            const Value* y = b.find_v(x.k);
            if (!y || !(*y == x.v))
                return false;
        }
        return true;
    default:
        return false;
    }
}

const Value* Value::find(std::string_view key) const {
    for (const Item& it : map)
        if (key_is(it.k, key))
            return &it.v;
    return nullptr;
}

Value* Value::find(std::string_view key) {
    return const_cast<Value*>(static_cast<const Value*>(this)->find(key));
}

const Value* Value::find_v(const Value& key) const {
    for (const Item& it : map)
        if (it.k == key)
            return &it.v;
    return nullptr;
}

Value* Value::find_v(const Value& key) {
    return const_cast<Value*>(static_cast<const Value*>(this)->find_v(key));
}

const Value& Value::at(std::string_view key) const {
    if (const Value* v = find(key))
        return *v;
    throw ValueError("'" + std::string(key) + "'");
}

Value& Value::at(std::string_view key) {
    return const_cast<Value&>(static_cast<const Value*>(this)->at(key));
}

const Value& Value::at_v(const Value& key) const {
    if (const Value* v = find_v(key))
        return *v;
    throw ValueError(py_repr(key));
}

Value Value::get(std::string_view key, Value fallback) const {
    const Value* v = t == T::Map ? find(key) : nullptr;
    return v ? *v : fallback;
}

Value& Value::operator[](std::string_view key) {
    if (t == T::Null)
        t = T::Map;
    if (Value* v = find(key))
        return *v;
    map.push_back({Value(key), Value()});
    return map.back().v;
}

Value& Value::slot(const Value& key) {
    if (t == T::Null)
        t = T::Map;
    if (Value* v = find_v(key))
        return *v;
    map.push_back({key, Value()});
    return map.back().v;
}

void Value::set(Value key, Value v) {
    if (t == T::Null)
        t = T::Map;
    if (Value* old = find_v(key)) {
        *old = std::move(v);
        return;
    }
    map.push_back({std::move(key), std::move(v)});
}

bool Value::erase(std::string_view key) {
    for (auto it = map.begin(); it != map.end(); ++it)
        if (key_is(it->k, key)) {
            map.erase(it);
            return true;
        }
    return false;
}

size_t Value::size() const {
    return t == T::List ? list.size() : t == T::Map ? map.size() : t == T::Str || t == T::Bytes ? s.size() : 0;
}

// --- Python semantics -----------------------------------------------------------------------------

bool truthy(const Value& v) {
    switch (v.t) {
    case Value::T::Null:
        return false;
    case Value::T::Bool:
        return v.b;
    case Value::T::Int:
        return v.i != 0;
    case Value::T::Float:
        return v.f != 0;
    default:
        return v.size() != 0;
    }
}

const char* py_type(const Value& v) {
    static const char* names[] = {"NoneType", "bool", "int", "float", "str", "bytes", "list", "dict"};
    return names[static_cast<int>(v.t)];
}

namespace {

std::string strip_spaces(std::string_view s) {
    size_t a = s.find_first_not_of(" \t\n\r\f\v"), b = s.find_last_not_of(" \t\n\r\f\v");
    return a == std::string_view::npos ? "" : std::string(s.substr(a, b - a + 1));
}

} // namespace

int64_t py_int(const Value& v) {
    switch (v.t) {
    case Value::T::Bool:
        return v.b;
    case Value::T::Int:
        return v.i;
    case Value::T::Float:
        if (!std::isfinite(v.f))
            throw ValueError("cannot convert float " + float_repr(v.f) + " to integer");
        return static_cast<int64_t>(std::trunc(v.f));
    case Value::T::Str: {
        std::string s = strip_spaces(v.s);
        int64_t out = 0;
        const char* begin = s.data() + (!s.empty() && s[0] == '+' ? 1 : 0);
        auto r = std::from_chars(begin, s.data() + s.size(), out);
        if (s.empty() || r.ec != std::errc() || r.ptr != s.data() + s.size())
            throw ValueError("invalid literal for int() with base 10: " + py_repr(v));
        return out;
    }
    default:
        throw ValueError(std::string("int() argument must be a string, a bytes-like object or a real number, not '") +
                         py_type(v) + "'");
    }
}

double py_float(const Value& v) {
    switch (v.t) {
    case Value::T::Bool:
        return v.b;
    case Value::T::Int:
        return static_cast<double>(v.i);
    case Value::T::Float:
        return v.f;
    case Value::T::Str: {
        std::string s = strip_spaces(v.s), l = ascii_lower(s);
        std::string body = l.empty() || (l[0] != '+' && l[0] != '-') ? l : l.substr(1);
        double sign = !l.empty() && l[0] == '-' ? -1 : 1;
        if (body == "inf" || body == "infinity")
            return sign * HUGE_VAL;
        if (body == "nan")
            return std::nan("");
        char* end = nullptr;
        double out = s.empty() || s.find_first_of("xXpP") != std::string::npos ? 0 : std::strtod(s.c_str(), &end);
        if (s.empty() || !end || *end)
            throw ValueError("could not convert string to float: " + py_repr(v));
        return out;
    }
    default:
        throw ValueError(std::string("float() argument must be a string or a real number, not '") + py_type(v) + "'");
    }
}

namespace {

std::string repr_str(std::string_view s, char prefix = 0) {
    bool single = s.find('\'') == std::string_view::npos || s.find('"') != std::string_view::npos;
    char q = single ? '\'' : '"';
    std::string out;
    if (prefix)
        out += prefix;
    out += q;
    for (unsigned char c : s) {
        if (c == static_cast<unsigned char>(q) || c == '\\')
            out += '\\', out += static_cast<char>(c);
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c < 0x20 || c == 0x7f || (prefix && c >= 0x80)) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        } else
            out += static_cast<char>(c);
    }
    return out + q;
}

} // namespace

std::string py_repr(const Value& v) {
    switch (v.t) {
    case Value::T::Null:
        return "None";
    case Value::T::Bool:
        return v.b ? "True" : "False";
    case Value::T::Int:
        return std::to_string(v.i);
    case Value::T::Float:
        return float_repr(v.f);
    case Value::T::Str:
        return repr_str(v.s);
    case Value::T::Bytes:
        return repr_str(v.s, 'b');
    case Value::T::List: {
        std::string out = "[";
        for (size_t k = 0; k < v.list.size(); ++k)
            out += (k ? ", " : "") + py_repr(v.list[k]);
        return out + "]";
    }
    case Value::T::Map: {
        std::string out = "{";
        for (size_t k = 0; k < v.map.size(); ++k)
            out += (k ? ", " : "") + py_repr(v.map[k].k) + ": " + py_repr(v.map[k].v);
        return out + "}";
    }
    }
    return "";
}

std::string py_str(const Value& v) {
    return v.t == Value::T::Str ? v.s : py_repr(v);
}

std::string float_repr(double v) {
    if (std::isnan(v))
        return "nan";
    if (std::isinf(v))
        return v < 0 ? "-inf" : "inf";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    size_t e = sci.find('e');
    std::string mant = sci.substr(0, e);
    int exp = std::atoi(sci.c_str() + e + 1);
    bool neg = mant[0] == '-';
    std::string digits;
    for (char c : mant)
        if (c >= '0' && c <= '9')
            digits += c;
    int decpt = exp + 1, n = static_cast<int>(digits.size());
    std::string out = neg ? "-" : "";
    if (decpt <= -4 || decpt > 16) {
        out += digits.substr(0, 1);
        if (n > 1)
            out += "." + digits.substr(1);
        char eb[16];
        std::snprintf(eb, sizeof(eb), "e%c%02d", decpt - 1 < 0 ? '-' : '+', std::abs(decpt - 1));
        return out + eb;
    }
    if (decpt <= 0)
        return out + "0." + std::string(static_cast<size_t>(-decpt), '0') + digits;
    if (decpt >= n)
        return out + digits + std::string(static_cast<size_t>(decpt - n), '0') + ".0";
    return out + digits.substr(0, static_cast<size_t>(decpt)) + "." + digits.substr(static_cast<size_t>(decpt));
}

std::string format_g(double v) {
    if (std::isnan(v))
        return "nan";
    if (std::isinf(v))
        return v < 0 ? "-inf" : "inf";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string format_fixed(double v, int digits) {
    if (std::isnan(v))
        return "nan";
    if (std::isinf(v))
        return v < 0 ? "-inf" : "inf";
    char buf[512];
    auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, digits);
    return std::string(buf, r.ptr);
}

double round_to(double v, int digits) {
    if (!std::isfinite(v))
        return v;
    return std::strtod(format_fixed(v, digits).c_str(), nullptr);
}

int64_t round_half_even(double v) {
    return static_cast<int64_t>(std::nearbyint(v));
}

// --- UTF-8 ----------------------------------------------------------------------------------------

void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
        out += static_cast<char>(0xC0 | cp >> 6), out += static_cast<char>(0x80 | (cp & 0x3F));
    else if (cp < 0x10000)
        out += static_cast<char>(0xE0 | cp >> 12), out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)),
            out += static_cast<char>(0x80 | (cp & 0x3F));
    else
        out += static_cast<char>(0xF0 | cp >> 18), out += static_cast<char>(0x80 | (cp >> 12 & 0x3F)),
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
}

namespace {

// The code point at s[i] (advancing i); a stray byte reads as itself.
uint32_t next_cp(std::string_view s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i++]);
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = extra == 3 ? c & 0x07 : extra == 2 ? c & 0x0F : extra == 1 ? c & 0x1F : c;
    for (int k = 0; k < extra && i < s.size(); ++k)
        cp = cp << 6 | (static_cast<unsigned char>(s[i++]) & 0x3F);
    return cp;
}

} // namespace

std::string utf16le_to_utf8(std::string_view b) {
    std::string out;
    for (size_t k = 0; k + 1 < b.size(); k += 2) {
        uint32_t u = static_cast<unsigned char>(b[k]) | static_cast<unsigned char>(b[k + 1]) << 8;
        if (u >= 0xD800 && u < 0xDC00 && k + 3 < b.size()) {
            uint32_t l = static_cast<unsigned char>(b[k + 2]) | static_cast<unsigned char>(b[k + 3]) << 8;
            if (l >= 0xDC00 && l < 0xE000) {
                put_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00));
                k += 2;
                continue;
            }
        }
        put_utf8(out, u);
    }
    return out;
}

std::string utf8_to_utf16le(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = next_cp(s, i);
        auto put = [&](uint32_t u) {
            out += static_cast<char>(u & 0xFF);
            out += static_cast<char>(u >> 8);
        };
        if (cp >= 0x10000)
            put(0xD800 + ((cp - 0x10000) >> 10)), put(0xDC00 + ((cp - 0x10000) & 0x3FF));
        else
            put(cp);
    }
    return out;
}

bool is_ascii(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// --- JSON -----------------------------------------------------------------------------------------

namespace {

struct JsonParser {
    std::string_view s;
    size_t p = 0;

    [[noreturn]] void fail(const std::string& what, size_t at) const {
        // json.JSONDecodeError: positions count characters, lines and columns from 1.
        size_t chars = 0, line = 1, col_start = 0;
        for (size_t k = 0; k < at && k < s.size(); ++k) {
            if ((static_cast<unsigned char>(s[k]) & 0xC0) != 0x80)
                ++chars;
            if (s[k] == '\n')
                ++line, col_start = chars;
        }
        throw ValueError(what + ": line " + std::to_string(line) + " column " + std::to_string(chars - col_start + 1) +
                         " (char " + std::to_string(chars) + ")");
    }

    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
            ++p;
    }

    bool lit(std::string_view word) {
        if (s.substr(p, word.size()) != word)
            return false;
        p += word.size();
        return true;
    }

    std::string string() {
        size_t start = p - 1;
        std::string out;
        for (;;) {
            if (p >= s.size())
                fail("Unterminated string starting at", start);
            unsigned char c = static_cast<unsigned char>(s[p]);
            if (c == '"') {
                ++p;
                return out;
            }
            if (c < 0x20)
                fail("Invalid control character at", p);
            if (c != '\\') {
                out += static_cast<char>(c);
                ++p;
                continue;
            }
            if (p + 1 >= s.size())
                fail("Unterminated string starting at", start);
            char e = s[p + 1];
            const char* simple = std::strchr("\"\\/bfnrt", e);
            if (e && simple) {
                static const char map[] = "\"\\/\b\f\n\r\t";
                out += map[simple - "\"\\/bfnrt"];
                p += 2;
                continue;
            }
            if (e != 'u')
                fail("Invalid \\escape", p);
            auto hex4 = [&](size_t at) -> int {
                if (at + 4 > s.size())
                    return -1;
                int v = 0;
                for (size_t k = at; k < at + 4; ++k) {
                    char h = s[k];
                    int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (d < 0)
                        return -1;
                    v = v * 16 + d;
                }
                return v;
            };
            int u = hex4(p + 2);
            if (u < 0)
                fail("Invalid \\uXXXX escape", p + 1);
            p += 6;
            if (u >= 0xD800 && u < 0xDC00 && s.substr(p, 2) == "\\u") {
                int l = hex4(p + 2);
                if (l >= 0xDC00 && l < 0xE000) {
                    put_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00));
                    p += 6;
                    continue;
                }
            }
            put_utf8(out, static_cast<uint32_t>(u));
        }
    }

    Value value() {
        if (p >= s.size())
            fail("Expecting value", p);
        char c = s[p];
        if (c == '"') {
            ++p;
            return Value(string());
        }
        if (c == '{') {
            ++p;
            Value m = Value::of_map();
            ws();
            if (p < s.size() && s[p] == '}') {
                ++p;
                return m;
            }
            for (;;) {
                if (p >= s.size() || s[p] != '"')
                    fail("Expecting property name enclosed in double quotes", p);
                ++p;
                std::string key = string();
                ws();
                if (p >= s.size() || s[p] != ':')
                    fail("Expecting ':' delimiter", p);
                ++p;
                ws();
                Value v = value();
                m.set(Value(std::move(key)), std::move(v));
                ws();
                if (p < s.size() && s[p] == '}') {
                    ++p;
                    return m;
                }
                if (p >= s.size() || s[p] != ',')
                    fail("Expecting ',' delimiter", p);
                ++p;
                ws();
            }
        }
        if (c == '[') {
            ++p;
            Value l = Value::of_list();
            ws();
            if (p < s.size() && s[p] == ']') {
                ++p;
                return l;
            }
            for (;;) {
                l.list.push_back(value());
                ws();
                if (p < s.size() && s[p] == ']') {
                    ++p;
                    return l;
                }
                if (p >= s.size() || s[p] != ',')
                    fail("Expecting ',' delimiter", p);
                ++p;
                ws();
            }
        }
        if (lit("null"))
            return Value();
        if (lit("true"))
            return Value(true);
        if (lit("false"))
            return Value(false);
        if (lit("NaN"))
            return Value(std::nan(""));
        if (lit("Infinity"))
            return Value(HUGE_VAL);
        if (lit("-Infinity"))
            return Value(-HUGE_VAL);
        // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)?
        size_t start = p, q = p;
        if (q < s.size() && s[q] == '-')
            ++q;
        auto digit = [&](size_t k) { return k < s.size() && s[k] >= '0' && s[k] <= '9'; };
        if (!digit(q))
            fail("Expecting value", p);
        if (s[q] == '0')
            ++q;
        else
            while (digit(q))
                ++q;
        bool is_float = false;
        if (q < s.size() && s[q] == '.' && digit(q + 1)) {
            is_float = true;
            for (++q; digit(q);)
                ++q;
        }
        if (q < s.size() && (s[q] == 'e' || s[q] == 'E')) {
            size_t r = q + 1;
            if (r < s.size() && (s[r] == '+' || s[r] == '-'))
                ++r;
            if (digit(r)) {
                is_float = true;
                for (q = r; digit(q);)
                    ++q;
            }
        }
        std::string text(s.substr(start, q - start));
        p = q;
        if (is_float)
            return Value(std::strtod(text.c_str(), nullptr));
        int64_t i = 0;
        auto r = std::from_chars(text.data(), text.data() + text.size(), i);
        if (r.ec != std::errc())
            return Value(std::strtod(text.c_str(), nullptr)); // past int64: kept as a float
        return Value(i);
    }
};

void dump_str(std::string& out, std::string_view s, bool ascii) {
    out += '"';
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"':
            out += "\\\"", ++i;
            continue;
        case '\\':
            out += "\\\\", ++i;
            continue;
        case '\n':
            out += "\\n", ++i;
            continue;
        case '\r':
            out += "\\r", ++i;
            continue;
        case '\t':
            out += "\\t", ++i;
            continue;
        case '\b':
            out += "\\b", ++i;
            continue;
        case '\f':
            out += "\\f", ++i;
            continue;
        default:
            break;
        }
        if (c < 0x20 || (ascii && c >= 0x7F)) {
            uint32_t cp = next_cp(s, i);
            char buf[16];
            if (cp >= 0x10000)
                std::snprintf(buf, sizeof(buf), "\\u%04x\\u%04x", 0xD800 + ((cp - 0x10000) >> 10), 0xDC00 + ((cp - 0x10000) & 0x3FF));
            else
                std::snprintf(buf, sizeof(buf), "\\u%04x", cp);
            out += buf;
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }
    out += '"';
}

std::string json_float(double f) {
    if (std::isnan(f))
        return "NaN";
    if (std::isinf(f))
        return f < 0 ? "-Infinity" : "Infinity";
    return float_repr(f);
}

std::string key_text(const Value& k) {
    switch (k.t) {
    case Value::T::Str:
        return k.s;
    case Value::T::Int:
        return std::to_string(k.i);
    case Value::T::Float:
        return json_float(k.f);
    case Value::T::Bool:
        return k.b ? "true" : "false";
    case Value::T::Null:
        return "null";
    default:
        throw ValueError(std::string("keys must be str, int, float, bool or None, not ") + py_type(k));
    }
}

void dump(std::string& out, const Value& v, const JsonStyle& st, int level) {
    auto newline = [&](int lv) {
        if (st.indent >= 0) {
            out += '\n';
            out.append(static_cast<size_t>(st.indent * lv), ' ');
        }
    };
    const char* item_sep = st.compact || st.indent >= 0 ? "," : ", ";
    const char* key_sep = st.compact ? ":" : ": ";
    switch (v.t) {
    case Value::T::Null:
        out += "null";
        return;
    case Value::T::Bool:
        out += v.b ? "true" : "false";
        return;
    case Value::T::Int:
        out += std::to_string(v.i);
        return;
    case Value::T::Float:
        out += json_float(v.f);
        return;
    case Value::T::Str:
        dump_str(out, v.s, st.ensure_ascii);
        return;
    case Value::T::Bytes:
        throw ValueError("Object of type bytes is not JSON serializable");
    case Value::T::List:
        if (v.list.empty()) {
            out += "[]";
            return;
        }
        out += '[';
        for (size_t k = 0; k < v.list.size(); ++k) {
            if (k)
                out += item_sep;
            newline(level + 1);
            dump(out, v.list[k], st, level + 1);
        }
        newline(level);
        out += ']';
        return;
    case Value::T::Map: {
        if (v.map.empty()) {
            out += "{}";
            return;
        }
        std::vector<const Item*> items;
        for (const Item& it : v.map)
            items.push_back(&it);
        if (st.sort_keys)
            std::stable_sort(items.begin(), items.end(), [](const Item* a, const Item* b) { return key_text(a->k) < key_text(b->k); });
        out += '{';
        for (size_t k = 0; k < items.size(); ++k) {
            if (k)
                out += item_sep;
            newline(level + 1);
            dump_str(out, key_text(items[k]->k), st.ensure_ascii);
            out += key_sep;
            dump(out, items[k]->v, st, level + 1);
        }
        newline(level);
        out += '}';
        return;
    }
    }
}

} // namespace

Value json_parse(std::string_view text) {
    JsonParser p{text};
    if (text.substr(0, 3) == "\xEF\xBB\xBF")
        throw ValueError("Unexpected UTF-8 BOM (decode using utf-8-sig): line 1 column 1 (char 0)");
    p.ws();
    Value v = p.value();
    p.ws();
    if (p.p != text.size())
        p.fail("Extra data", p.p);
    return v;
}

std::string json_dump(const Value& v, const JsonStyle& style) {
    std::string out;
    dump(out, v, style, 0);
    return out;
}

} // namespace dlcmaker
