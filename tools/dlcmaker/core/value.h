// SPDX-License-Identifier: GPL-2.0-or-later
// Values as the DLC maker keeps them: JSON (project files, the game's catalog), MessagePack (the
// game's tables) and Unity type trees. Maps keep their insertion order, ints and floats stay apart,
// and text comes out as Python wrote it (json.dumps, repr, format specs): the caches and files
// earlier (Python) releases wrote stay valid.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dlcmaker {

// A failure the user can act on, printed without more detail; the command exits with 1.
struct Exit : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A malformed value or file (what earlier (Python) releases raised as ValueError, KeyError or TypeError).
struct ValueError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Item;

struct Value {
    enum class T : uint8_t { Null, Bool, Int, Float, Str, Bytes, List, Map };
    T t = T::Null;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::string s;           // Str (UTF-8) and Bytes
    std::vector<Value> list; // List (and Python tuples)
    std::vector<Item> map;   // Map, in insertion order

    // Out of line: they need Item, which needs Value.
    Value();
    Value(std::nullptr_t);
    Value(bool v);
    Value(int v);
    Value(unsigned v);
    Value(long v);
    Value(long long v);
    Value(unsigned long v);
    Value(unsigned long long v);
    Value(double v);
    Value(const char* v);
    Value(std::string v);
    Value(std::string_view v);
    Value(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(const Value&);
    Value& operator=(Value&&) noexcept;
    ~Value();
    static Value bytes(std::string data);
    static Value of_list(std::vector<Value> items = {});
    static Value of_map();

    bool is_null() const { return t == T::Null; }
    bool is_str() const { return t == T::Str; }
    bool is_list() const { return t == T::List; }
    bool is_map() const { return t == T::Map; }
    bool is_number() const { return t == T::Int || t == T::Float || t == T::Bool; }
    bool is_int() const { return t == T::Int || t == T::Bool; } // isinstance(v, int)

    // Map access by key (a string key, or any value as a MessagePack key).
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    const Value* find_v(const Value& key) const;
    Value* find_v(const Value& key);
    bool has(std::string_view key) const { return find(key) != nullptr; }
    const Value& at(std::string_view key) const; // ValueError (KeyError) when missing
    Value& at(std::string_view key);
    const Value& at_v(const Value& key) const;
    Value get(std::string_view key, Value fallback = {}) const;
    Value& operator[](std::string_view key); // added at the end when missing, as d[k] = v does
    Value& slot(const Value& key);
    void set(Value key, Value v);
    bool erase(std::string_view key);
    size_t size() const;
};

struct Item {
    Value k, v;
};

bool operator==(const Value& a, const Value& b);
inline bool operator!=(const Value& a, const Value& b) { return !(a == b); }

// Python semantics.
bool truthy(const Value& v);
int64_t py_int(const Value& v);    // int(v)
double py_float(const Value& v);   // float(v)
std::string py_repr(const Value& v);
std::string py_str(const Value& v);
const char* py_type(const Value& v); // type(v).__name__

std::string float_repr(double v);              // repr(float)
std::string format_g(double v);                // f'{v:g}'
std::string format_fixed(double v, int digits); // f'{v:.Nf}'
double round_to(double v, int digits);         // round(v, digits)
int64_t round_half_even(double v);             // round(v)

// JSON as Python's json module reads and writes it.
struct JsonStyle {
    int indent = -1;         // -1: one line (", " and ": " separators); else "," and ": " with newlines
    bool ensure_ascii = true;
    bool sort_keys = false;
    bool compact = false;    // "," and ":" (separators=(',', ':'))
};
Value json_parse(std::string_view text); // ValueError with json.JSONDecodeError's message
std::string json_dump(const Value& v, const JsonStyle& style = {});

// UTF-8 helpers (strings may hold lone surrogates as 3-byte sequences, as Python's str can).
void put_utf8(std::string& out, uint32_t cp);
std::string utf16le_to_utf8(std::string_view bytes);
std::string utf8_to_utf16le(std::string_view s);
bool is_ascii(std::string_view s);
std::string ascii_lower(std::string s);

} // namespace dlcmaker
