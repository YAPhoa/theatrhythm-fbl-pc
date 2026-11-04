// SPDX-License-Identifier: GPL-2.0-or-later
#include "unity.h"

#include "util.h"

#include <algorithm>
#include <map>

namespace dlcmaker::unity {

namespace {

// Unity's common string buffer (the type-tree strings stored as an offset with the top bit set), as
// UnityPy's Tpk table has it.
constexpr char kCommonStrings[] =
#include "common_strings.inc"
    ;

const std::map<uint32_t, std::string>& common_strings() {
    static const std::map<uint32_t, std::string> table = [] {
        std::map<uint32_t, std::string> t;
        std::string_view all(kCommonStrings, sizeof(kCommonStrings) - 1);
        for (size_t at = 0; at < all.size();) {
            size_t end = all.find('\0', at);
            t[static_cast<uint32_t>(at)] = std::string(all.substr(at, end - at));
            at = end + 1;
        }
        return t;
    }();
    return table;
}

constexpr int32_t kAlignBytes = 0x4000;

bool aligned(int32_t meta) {
    return (meta & kAlignBytes) != 0;
}

// --- type trees -----------------------------------------------------------------------------------

TypeNode parse_blob(Reader& r, uint32_t version) {
    int32_t count = r.i32(), strings = r.i32();
    size_t node_size = version >= 19 ? 32 : 24;
    Reader nodes(r.bytes(node_size * static_cast<size_t>(count)), r.big);
    std::string_view buf = r.bytes(static_cast<size_t>(strings));
    auto str = [&](uint32_t v) -> std::string {
        if (!(v & 0x80000000u)) {
            size_t end = buf.find('\0', v);
            return std::string(buf.substr(v, end == std::string_view::npos ? std::string_view::npos : end - v));
        }
        auto it = common_strings().find(v & 0x7FFFFFFF);
        return it == common_strings().end() ? std::to_string(v & 0x7FFFFFFF) : it->second;
    };
    std::vector<TypeNode> flat;
    for (int32_t k = 0; k < count; ++k) {
        TypeNode n;
        n.version = nodes.u16();
        n.level = nodes.u8();
        n.type_flags = nodes.u8();
        n.type = str(nodes.u32());
        n.name = str(nodes.u32());
        n.byte_size = nodes.i32();
        n.index = nodes.i32();
        n.meta_flag = nodes.i32();
        if (version >= 19)
            n.ref_type_hash = nodes.u64();
        flat.push_back(std::move(n));
    }
    // Levels to a tree: each node is a child of the last node one level up.
    std::vector<TypeNode*> stack;
    TypeNode root;
    if (flat.empty())
        return root;
    root = std::move(flat[0]);
    stack.push_back(&root);
    for (size_t k = 1; k < flat.size(); ++k) {
        while (stack.size() > 1 && stack.back()->level >= flat[k].level)
            stack.pop_back();
        stack.back()->children.push_back(std::move(flat[k]));
        stack.push_back(&stack.back()->children.back());
    }
    return root;
}

// TypeTreeNode.dump_blob: strings in first-use order, a common string by its offset (but the one at
// offset 0, which UnityPy's truth test skips).
void dump_blob(Writer& w, const TypeNode& root, uint32_t version) {
    static const std::map<std::string, uint32_t> common = [] {
        std::map<std::string, uint32_t> m;
        for (auto& [off, s] : common_strings())
            m.emplace(s, off);
        return m;
    }();
    Writer nodes(w.big);
    std::string buf;
    std::map<std::string, uint32_t> offsets;
    auto str = [&](const std::string& s) -> uint32_t {
        auto it = offsets.find(s);
        if (it != offsets.end())
            return it->second;
        auto c = common.find(s);
        uint32_t off;
        if (c != common.end() && c->second)
            off = c->second | 0x80000000u;
        else {
            off = static_cast<uint32_t>(buf.size());
            buf += s;
            buf += '\0';
        }
        offsets[s] = off;
        return off;
    };
    int32_t count = 0;
    auto walk = [&](auto& self, const TypeNode& n) -> void {
        nodes.u16(n.version);
        nodes.u8(n.level);
        nodes.u8(n.type_flags);
        nodes.u32(str(n.type));
        nodes.u32(str(n.name));
        nodes.i32(n.byte_size);
        nodes.i32(n.index);
        nodes.i32(n.meta_flag);
        if (version >= 19)
            nodes.u64(n.ref_type_hash);
        ++count;
        for (const TypeNode& c : n.children)
            self(self, c);
    };
    walk(walk, root);
    w.i32(count);
    w.i32(static_cast<int32_t>(buf.size()));
    w.bytes(nodes.d);
    w.bytes(buf);
}

SerializedType parse_type(Reader& r, const SerializedFile& f, bool is_ref) {
    SerializedType t;
    uint32_t v = f.version;
    t.class_id = r.i32();
    if (v >= 16)
        t.stripped = r.u8() != 0;
    if (v >= 17)
        t.script_index = r.i16();
    if (v >= 13) {
        if ((is_ref && t.script_index >= 0) || (v < 16 && t.class_id < 0) || (v >= 16 && t.class_id == 114))
            t.script_id = std::string(r.bytes(16));
        t.old_hash = std::string(r.bytes(16));
    }
    if (f.type_tree) {
        if (v >= 23)
            throw ValueError("serialized file format " + std::to_string(v) + " is not supported");
        if (v < 12 && v != 10)
            throw ValueError("old type trees are not supported");
        t.node = parse_blob(r, v);
        if (v >= 21) {
            if (is_ref) {
                t.class_name = r.cstring();
                t.name_space = r.cstring();
                t.assembly = r.cstring();
            } else {
                int32_t n = r.i32();
                for (int32_t k = 0; k < n; ++k)
                    t.dependencies.push_back(r.i32());
            }
        }
    }
    return t;
}

void write_type(Writer& w, const SerializedFile& f, const SerializedType& t, bool is_ref) {
    uint32_t v = f.version;
    w.i32(t.class_id);
    if (v >= 16)
        w.u8(t.stripped);
    if (v >= 17)
        w.i16(t.script_index);
    if (v >= 13) {
        if ((is_ref && t.script_index >= 0) || (v < 16 && t.class_id < 0) || (v >= 16 && t.class_id == 114))
            w.bytes(t.script_id);
        w.bytes(t.old_hash);
    }
    if (f.type_tree) {
        dump_blob(w, t.node, v);
        if (v >= 21) {
            if (is_ref) {
                w.cstring(t.class_name);
                w.cstring(t.name_space);
                w.cstring(t.assembly);
            } else {
                w.i32(static_cast<int32_t>(t.dependencies.size()));
                for (int32_t d : t.dependencies)
                    w.i32(d);
            }
        }
    }
}

// --- type-tree values (UnityPy's TypeTreeHelper) --------------------------------------------------

enum class Prim { None, I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, Bool, String, Typeless };

Prim prim_of(const std::string& t) {
    static const std::map<std::string, Prim> m = {
        {"SInt8", Prim::I8},       {"UInt8", Prim::U8},
        {"char", Prim::U8},        {"short", Prim::I16},
        {"SInt16", Prim::I16},     {"unsigned short", Prim::U16},
        {"UInt16", Prim::U16},     {"int", Prim::I32},
        {"SInt32", Prim::I32},     {"unsigned int", Prim::U32},
        {"UInt32", Prim::U32},     {"Type*", Prim::U32},
        {"long long", Prim::I64},  {"SInt64", Prim::I64},
        {"unsigned long long", Prim::U64}, {"UInt64", Prim::U64},
        {"FileSize", Prim::U64},   {"float", Prim::F32},
        {"double", Prim::F64},     {"bool", Prim::Bool},
        {"string", Prim::String},  {"TypelessData", Prim::Typeless},
    };
    auto it = m.find(t);
    return it == m.end() ? Prim::None : it->second;
}

Value read_prim(Prim p, Reader& r) {
    switch (p) {
    case Prim::I8:
        return Value(static_cast<int64_t>(r.i8()));
    case Prim::U8:
        return Value(static_cast<int64_t>(r.u8()));
    case Prim::I16:
        return Value(static_cast<int64_t>(r.i16()));
    case Prim::U16:
        return Value(static_cast<int64_t>(r.u16()));
    case Prim::I32:
        return Value(static_cast<int64_t>(r.i32()));
    case Prim::U32:
        return Value(static_cast<int64_t>(r.u32()));
    case Prim::I64:
        return Value(r.i64());
    case Prim::U64:
        return Value(static_cast<int64_t>(r.u64()));
    case Prim::F32:
        return Value(static_cast<double>(r.f32()));
    case Prim::F64:
        return Value(r.f64());
    case Prim::Bool:
        return Value(r.u8() != 0);
    case Prim::String: {
        int32_t n = r.i32();
        if (n < 0)
            throw ValueError("negative string length");
        std::string s(r.bytes(static_cast<size_t>(n)));
        r.align(4);
        return Value(std::move(s));
    }
    case Prim::Typeless: {
        int32_t n = r.i32();
        if (n < 0)
            throw ValueError("negative TypelessData length");
        return Value::bytes(std::string(r.bytes(static_cast<size_t>(n))));
    }
    case Prim::None:
        break;
    }
    return Value();
}

int64_t int_of(const Value& v) {
    if (v.t == Value::T::Int)
        return v.i;
    if (v.t == Value::T::Bool)
        return v.b;
    throw ValueError(std::string("an integer is required, not ") + py_type(v));
}

void write_prim(Prim p, Writer& w, const Value& v) {
    switch (p) {
    case Prim::I8:
    case Prim::U8:
        w.uint(static_cast<uint64_t>(int_of(v)), 1);
        return;
    case Prim::I16:
    case Prim::U16:
        w.uint(static_cast<uint64_t>(int_of(v)), 2);
        return;
    case Prim::I32:
    case Prim::U32:
        w.uint(static_cast<uint64_t>(int_of(v)), 4);
        return;
    case Prim::I64:
    case Prim::U64:
        w.uint(static_cast<uint64_t>(int_of(v)), 8);
        return;
    case Prim::F32:
        w.f32(static_cast<float>(py_float(v)));
        return;
    case Prim::F64:
        w.f64(py_float(v));
        return;
    case Prim::Bool:
        w.u8(truthy(v));
        return;
    case Prim::String:
        if (v.t != Value::T::Str && v.t != Value::T::Bytes)
            throw ValueError(std::string("a string is required, not ") + py_type(v));
        w.i32(static_cast<int32_t>(v.s.size()));
        w.bytes(v.s);
        w.align(4);
        return;
    case Prim::Typeless:
        if (v.t == Value::T::List) {
            w.i32(static_cast<int32_t>(v.list.size()));
            for (const Value& b : v.list)
                w.u8(static_cast<uint8_t>(int_of(b)));
        } else {
            w.i32(static_cast<int32_t>(v.s.size()));
            w.bytes(v.s);
        }
        return;
    case Prim::None:
        break;
    }
}

bool is_array(const TypeNode& n) {
    return !n.children.empty() && n.children[0].type == "Array";
}

void unsupported(const TypeNode& n) {
    if (n.type == "ReferencedObject" || n.type == "ManagedReferencesRegistry")
        throw ValueError("type trees with managed references are not supported (" + n.name + ")");
}

Value read_value(const TypeNode& n, Reader& r);

// read_value_array: `count` items of `n`, aligned once at the end when `n` is.
Value read_array(const TypeNode& n, Reader& r, int32_t count) {
    unsupported(n);
    if (count < 0)
        throw ValueError("Negative length read from TypeTree");
    bool align = aligned(n.meta_flag);
    Value out = Value::of_list();
    out.list.reserve(static_cast<size_t>(count));
    Prim p = prim_of(n.type);
    if (p != Prim::None) {
        for (int32_t k = 0; k < count; ++k)
            out.list.push_back(read_prim(p, r));
    } else if (n.type == "pair") {
        const TypeNode &a = n.children[0], &b = n.children[1];
        Prim pa = prim_of(a.type), pb = prim_of(b.type);
        for (int32_t k = 0; k < count; ++k) {
            Value first = pa != Prim::None ? read_prim(pa, r) : read_value(a, r);
            Value second = pb != Prim::None ? read_prim(pb, r) : read_value(b, r);
            out.list.push_back(Value::of_list({std::move(first), std::move(second)}));
        }
    } else if (is_array(n)) {
        if (aligned(n.children[0].meta_flag))
            align = true;
        const TypeNode& sub = n.children[0].children[1];
        for (int32_t k = 0; k < count; ++k) {
            int32_t m = r.i32();
            if (aligned(sub.meta_flag))
                out.list.push_back(read_array(sub, r, m));
            else {
                Value inner = Value::of_list();
                for (int32_t j = 0; j < m; ++j)
                    inner.list.push_back(read_value(sub, r));
                out.list.push_back(std::move(inner));
            }
        }
    } else {
        for (int32_t k = 0; k < count; ++k) {
            Value m = Value::of_map();
            for (const TypeNode& c : n.children) {
                unsupported(c);
                m.map.push_back({Value(c.name), read_value(c, r)});
            }
            out.list.push_back(std::move(m));
        }
    }
    if (align)
        r.align(4);
    return out;
}

Value read_value(const TypeNode& n, Reader& r) {
    unsupported(n);
    bool align = aligned(n.meta_flag);
    Value v;
    if (Prim p = prim_of(n.type); p != Prim::None)
        v = read_prim(p, r);
    else if (n.type == "pair") {
        Value first = read_value(n.children[0], r);
        Value second = read_value(n.children[1], r);
        v = Value::of_list({std::move(first), std::move(second)});
    } else if (is_array(n)) {
        if (aligned(n.children[0].meta_flag))
            align = true;
        int32_t size = r.i32();
        if (size < 0)
            throw ValueError("Negative length read from TypeTree");
        const TypeNode& sub = n.children[0].children[1];
        if (aligned(sub.meta_flag))
            v = read_array(sub, r, size);
        else {
            v = Value::of_list();
            for (int32_t k = 0; k < size; ++k)
                v.list.push_back(read_value(sub, r));
        }
    } else {
        v = Value::of_map();
        for (const TypeNode& c : n.children)
            v.map.push_back({Value(c.name), read_value(c, r)});
    }
    if (align)
        r.align(4);
    return v;
}

void write_value(const TypeNode& n, Writer& w, const Value& v) {
    unsupported(n);
    bool align = aligned(n.meta_flag);
    if (Prim p = prim_of(n.type); p != Prim::None)
        write_prim(p, w, v);
    else if (n.type == "pair") {
        if (v.t != Value::T::List || v.list.size() != 2)
            throw ValueError("a pair needs two values");
        write_value(n.children[0], w, v.list[0]);
        write_value(n.children[1], w, v.list[1]);
    } else if (is_array(n)) {
        if (aligned(n.children[0].meta_flag))
            align = true;
        const TypeNode& sub = n.children[0].children[1];
        if (v.t == Value::T::Bytes) { // a byte array kept as bytes
            w.i32(static_cast<int32_t>(v.s.size()));
            for (char c : v.s)
                write_value(sub, w, Value(static_cast<int64_t>(static_cast<unsigned char>(c))));
        } else {
            if (v.t != Value::T::List)
                throw ValueError(std::string("a list is required for ") + n.name + ", not " + py_type(v));
            w.i32(static_cast<int32_t>(v.list.size()));
            for (const Value& item : v.list)
                write_value(sub, w, item);
        }
    } else {
        if (v.t != Value::T::Map)
            throw ValueError(std::string("a dict is required for ") + n.name + ", not " + py_type(v));
        for (const TypeNode& c : n.children)
            write_value(c, w, v.at(c.name));
    }
    if (align)
        w.align(4);
}

} // namespace

// --- serialized files -----------------------------------------------------------------------------

SerializedFile SerializedFile::parse(std::string_view data) {
    SerializedFile f;
    Reader r(data, true);
    uint32_t metadata_size = r.u32(), file_size = r.u32();
    f.version = r.u32();
    uint64_t data_offset = r.u32();
    (void)metadata_size, (void)file_size;
    if (f.version < 9)
        throw ValueError("serialized file format " + std::to_string(f.version) + " is not supported");
    f.big_endian = r.u8() != 0;
    f.reserved = std::string(r.bytes(3));
    if (f.version >= 22) {
        r.u32();
        r.i64();
        data_offset = r.u64();
        f.unknown = r.i64();
    }
    r.big = f.big_endian;
    if (f.version >= 7)
        f.unity_version = r.cstring();
    if (f.version >= 8)
        f.platform = r.i32();
    if (f.version >= 13)
        f.type_tree = r.u8() != 0;
    int32_t count = r.i32();
    for (int32_t k = 0; k < count; ++k)
        f.types.push_back(parse_type(r, f, false));
    if (f.version < 14)
        throw ValueError("serialized file format " + std::to_string(f.version) + " is not supported");
    count = r.i32();
    for (int32_t k = 0; k < count; ++k) {
        Object o;
        r.align(4);
        o.path_id = r.i64();
        uint64_t start = f.version >= 22 ? r.u64() : r.u32();
        uint32_t size = r.u32();
        o.type_id = r.i32();
        if (f.version < 16)
            r.u16();
        if (f.version < 17 && f.version >= 11)
            r.i16();
        if (f.version == 15 || f.version == 16)
            r.u8();
        if (start + data_offset + size > data.size())
            throw ValueError("object data past the end of the file");
        o.data = std::string(data.substr(static_cast<size_t>(data_offset + start), size));
        f.objects.push_back(std::move(o));
    }
    if (f.version >= 11) {
        count = r.i32();
        for (int32_t k = 0; k < count; ++k) {
            Script s;
            s.file = r.i32();
            r.align(4);
            s.id = r.i64();
            f.scripts.push_back(s);
        }
    }
    count = r.i32();
    for (int32_t k = 0; k < count; ++k) {
        External e;
        e.temp = r.cstring();
        e.guid = std::string(r.bytes(16));
        e.type = r.i32();
        e.path = r.cstring();
        f.externals.push_back(std::move(e));
    }
    if (f.version >= 20) {
        count = r.i32();
        for (int32_t k = 0; k < count; ++k)
            f.ref_types.push_back(parse_type(r, f, true));
    }
    f.user_info = r.cstring();
    for (const Object& o : f.objects)
        if (o.type_id < 0 || static_cast<size_t>(o.type_id) >= f.types.size())
            throw ValueError("object with an unknown type");
    return f;
}

std::string SerializedFile::save() const {
    Writer meta(big_endian), objs(big_endian);
    meta.cstring(unity_version);
    meta.i32(platform);
    meta.u8(type_tree);
    meta.i32(static_cast<int32_t>(types.size()));
    for (const SerializedType& t : types)
        write_type(meta, *this, t, false);
    meta.i32(static_cast<int32_t>(objects.size()));
    std::vector<const Object*> order;
    for (const Object& o : objects)
        order.push_back(&o);
    std::stable_sort(order.begin(), order.end(), [](const Object* a, const Object* b) { return a->path_id < b->path_id; });
    for (const Object* o : order) {
        meta.align(4);
        meta.i64(o->path_id);
        if (version >= 22)
            meta.u64(objs.d.size());
        else
            meta.u32(static_cast<uint32_t>(objs.d.size()));
        meta.u32(static_cast<uint32_t>(o->data.size()));
        objs.bytes(o->data);
        meta.i32(o->type_id);
        objs.align(8);
    }
    meta.i32(static_cast<int32_t>(scripts.size()));
    for (const Script& s : scripts) {
        meta.i32(s.file);
        meta.align(4);
        meta.i64(s.id);
    }
    meta.i32(static_cast<int32_t>(externals.size()));
    for (const External& e : externals) {
        meta.cstring(e.temp);
        meta.bytes(e.guid);
        meta.i32(e.type);
        meta.cstring(e.path);
    }
    if (version >= 20) {
        meta.i32(static_cast<int32_t>(ref_types.size()));
        for (const SerializedType& t : ref_types)
            write_type(meta, *this, t, true);
    }
    meta.cstring(user_info);

    Writer w(true);
    size_t header = 16 + (version < 22 ? 4 : 4 + 28);
    uint64_t data_offset = header + meta.d.size();
    data_offset += (16 - data_offset % 16) % 16;
    uint64_t file_size = data_offset + objs.d.size();
    if (version < 22) {
        w.u32(static_cast<uint32_t>(meta.d.size()));
        w.u32(static_cast<uint32_t>(file_size));
        w.u32(version);
        w.u32(static_cast<uint32_t>(data_offset));
        w.u8(big_endian);
        w.bytes(reserved);
    } else {
        w.u32(0);
        w.u32(0);
        w.u32(version);
        w.u32(0);
        w.u8(big_endian);
        w.bytes(reserved);
        w.u32(static_cast<uint32_t>(meta.d.size()));
        w.u64(file_size);
        w.u64(data_offset);
        w.i64(unknown);
    }
    w.bytes(meta.d);
    w.align(16);
    w.bytes(objs.d);
    return std::move(w.d);
}

const SerializedType& SerializedFile::type_of(const Object& o) const {
    return types.at(static_cast<size_t>(o.type_id));
}

Value SerializedFile::read(const Object& o) const {
    Reader r(o.data, big_endian);
    Value v = read_value(type_of(o).node, r);
    if (r.p != o.data.size())
        throw ValueError("Expected to read " + std::to_string(o.data.size()) + " bytes, but only read " + std::to_string(r.p) + " bytes");
    return v;
}

void SerializedFile::write(Object& o, const Value& v) {
    Writer w(big_endian);
    write_value(type_of(o).node, w, v);
    o.data = std::move(w.d);
}

// --- bundles --------------------------------------------------------------------------------------

std::string lz4_block_decompress(std::string_view src, size_t size) {
    std::string out;
    out.reserve(size);
    size_t i = 0;
    auto byte = [&]() -> unsigned {
        if (i >= src.size())
            throw ValueError("truncated LZ4 block");
        return static_cast<unsigned char>(src[i++]);
    };
    while (i < src.size()) {
        unsigned token = byte();
        size_t n = token >> 4;
        if (n == 15)
            for (unsigned b = 255; b == 255;)
                n += (b = byte());
        if (i + n > src.size())
            throw ValueError("truncated LZ4 block");
        out.append(src.substr(i, n));
        i += n;
        if (i >= src.size())
            break;
        size_t offset = byte();
        offset |= static_cast<size_t>(byte()) << 8;
        if (!offset || offset > out.size())
            throw ValueError("bad LZ4 match offset");
        n = token & 15;
        if (n == 15)
            for (unsigned b = 255; b == 255;)
                n += (b = byte());
        n += 4;
        size_t start = out.size() - offset;
        for (size_t k = 0; k < n; ++k) // matches may overlap their own output
            out += out[start + k];
    }
    if (out.size() != size)
        throw ValueError("LZ4 block gave " + std::to_string(out.size()) + " bytes, expected " + std::to_string(size));
    return out;
}

namespace {

std::string decompress(std::string_view data, size_t size, uint32_t flags) {
    switch (flags & 0x3F) {
    case 0:
        return std::string(data);
    case 2:
    case 3:
        return lz4_block_decompress(data, size);
    default:
        throw ValueError("unsupported bundle compression " + std::to_string(flags & 0x3F));
    }
}

// A node is a serialized file when its header reads as one (as UnityPy's check_file_type decides).
bool looks_serialized(std::string_view d) {
    if (d.size() < 48)
        return false;
    Reader r(d, true);
    uint32_t meta = r.u32(), size = r.u32(), version = r.u32(), offset = r.u32();
    if (version >= 22) {
        r.p = 20;
        meta = r.u32();
        uint64_t s64 = r.u64(), o64 = r.u64();
        return version < 100 && s64 == d.size() && o64 <= s64 && meta < s64;
    }
    return version >= 9 && version < 100 && size == d.size() && offset <= size && meta < size;
}

} // namespace

Bundle Bundle::parse(std::string_view data) {
    Bundle b;
    Reader r(data, true);
    b.signature = r.cstring();
    if (b.signature != "UnityFS")
        throw ValueError("not a UnityFS bundle");
    b.version = r.u32();
    b.player = r.cstring();
    b.engine = r.cstring();
    r.i64();
    uint32_t info_packed = r.u32(), info_size = r.u32(), flags = r.u32();
    if (b.version >= 7) {
        r.align(16);
        b.aligned = true;
    }
    size_t start = r.p;
    std::string_view info_raw;
    if (flags & 0x80) {
        if (info_packed > data.size())
            throw ValueError("bad bundle header");
        info_raw = data.substr(data.size() - info_packed);
    } else {
        info_raw = r.bytes(info_packed);
        start = r.p;
    }
    std::string info = decompress(info_raw, info_size, flags);
    Reader ir(info, true, 16);
    int32_t blocks = ir.i32();
    struct Block {
        uint32_t size, packed;
        uint16_t flags;
    };
    std::vector<Block> list;
    for (int32_t k = 0; k < blocks; ++k) {
        Block bl;
        bl.size = ir.u32();
        bl.packed = ir.u32();
        bl.flags = ir.u16();
        list.push_back(bl);
    }
    int32_t count = ir.i32();
    struct Dir {
        uint64_t offset, size;
        uint32_t flags;
        std::string name;
    };
    std::vector<Dir> dirs;
    for (int32_t k = 0; k < count; ++k) {
        Dir d;
        d.offset = ir.u64();
        d.size = ir.u64();
        d.flags = ir.u32();
        d.name = ir.cstring();
        dirs.push_back(std::move(d));
    }
    // ArchiveFlags.BlockInfoNeedPaddingAtStart counts only from Unity 2021.3.2 (the game is 2021.2).
    std::string stream;
    Reader br(data, true, start);
    for (const Block& bl : list)
        stream += decompress(br.bytes(bl.packed), bl.size, bl.flags);
    for (const Dir& d : dirs) {
        if (d.offset + d.size > stream.size())
            throw ValueError("bundle node past the end of its data");
        Node n;
        n.name = d.name;
        n.flags = d.flags;
        std::string_view bytes = std::string_view(stream).substr(static_cast<size_t>(d.offset), static_cast<size_t>(d.size));
        if (looks_serialized(bytes))
            n.file = std::make_unique<SerializedFile>(SerializedFile::parse(bytes));
        else
            n.data = std::string(bytes);
        b.nodes.push_back(std::move(n));
    }
    return b;
}

std::string Bundle::save() const {
    std::string files;
    struct Entry {
        uint64_t offset, size;
        uint32_t flags;
        const std::string* name;
    };
    std::vector<Entry> entries;
    for (const Node& n : nodes) {
        std::string data = n.file ? n.file->save() : n.data;
        entries.push_back({files.size(), data.size(), n.flags, &n.name});
        files += data;
    }
    Writer info(true);
    info.bytes(std::string(16, '\0'));
    info.i32(1);
    info.u32(static_cast<uint32_t>(files.size()));
    info.u32(static_cast<uint32_t>(files.size()));
    info.u16(64);
    info.i32(static_cast<int32_t>(entries.size()));
    for (const Entry& e : entries) {
        info.u64(e.offset);
        info.u64(e.size);
        info.u32(e.flags);
        info.cstring(*e.name);
    }
    Writer w(true);
    w.cstring(signature);
    w.u32(version);
    w.cstring(player);
    w.cstring(engine);
    size_t size_at = w.d.size();
    w.u64(0);
    w.u32(static_cast<uint32_t>(info.d.size()));
    w.u32(static_cast<uint32_t>(info.d.size()));
    w.u32(64);
    if (aligned)
        w.align(16);
    w.bytes(info.d);
    w.bytes(files);
    w.put_u64(size_at, w.d.size());
    return std::move(w.d);
}

Bundle::Node& Bundle::node_ending(std::string_view suffix) {
    Node* found = nullptr;
    int n = 0;
    for (Node& node : nodes)
        if (node.name.size() >= suffix.size() && node.name.compare(node.name.size() - suffix.size(), suffix.size(), suffix) == 0)
            found = &node, ++n;
    if (n != 1)
        throw ValueError("expected one bundle file ending in " + std::string(suffix) + ", found " + std::to_string(n));
    return *found;
}

std::vector<Bundle::Ref> Bundle::objects() {
    std::vector<Ref> out;
    for (Node& n : nodes)
        if (n.file)
            for (Object& o : n.file->objects)
                out.push_back({n.file.get(), &o});
    return out;
}

} // namespace dlcmaker::unity
