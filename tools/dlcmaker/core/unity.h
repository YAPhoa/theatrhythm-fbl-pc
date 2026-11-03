// SPDX-License-Identifier: GPL-2.0-or-later
// Unity asset bundles (UnityFS), the serialized files in them and their objects as type-tree values.
//
// A bundle is read whole (LZ4 or uncompressed blocks) and written back uncompressed, laid out as UnityPy
// 1.25 writes it (BundleFile.save(packer='none'), SerializedFile.save): the packs earlier (Python)
// releases built and the ones built here are the same bytes. Objects read and write through their type trees
// as UnityPy's read_typetree/save_typetree do: classes as maps in node order, arrays as lists, pairs as
// two-item lists, strings as their raw bytes, TypelessData as bytes.
#pragma once

#include "value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dlcmaker::unity {

struct TypeNode {
    uint16_t version = 0;
    uint8_t level = 0, type_flags = 0;
    std::string type, name;
    int32_t byte_size = 0, index = 0, meta_flag = 0;
    uint64_t ref_type_hash = 0;
    std::vector<TypeNode> children;
};

struct SerializedType {
    int32_t class_id = 0;
    bool stripped = false;
    int16_t script_index = -1;
    std::string script_id, old_hash; // 16 bytes each (script_id only where the format has it)
    TypeNode node;
    std::vector<int32_t> dependencies;
    std::string class_name, name_space, assembly; // reference types
};

struct Object {
    int64_t path_id = 0;
    int32_t type_id = 0;
    std::string data;
};

struct SerializedFile {
    uint32_t version = 0;
    bool big_endian = false;
    std::string reserved;
    int64_t unknown = 0;
    std::string unity_version;
    int32_t platform = 0;
    bool type_tree = true;
    std::vector<SerializedType> types;
    std::vector<Object> objects; // in the file's object table order
    struct Script {
        int32_t file = 0;
        int64_t id = 0;
    };
    std::vector<Script> scripts;
    struct External {
        std::string temp, guid, path;
        int32_t type = 0;
    };
    std::vector<External> externals;
    std::vector<SerializedType> ref_types;
    std::string user_info;

    static SerializedFile parse(std::string_view data);
    std::string save() const;

    const SerializedType& type_of(const Object& o) const;
    int class_id(const Object& o) const { return type_of(o).class_id; }
    Value read(const Object& o) const;     // read_typetree
    void write(Object& o, const Value& v); // save_typetree
};

// The class names the DLC maker looks for, by ClassIDType.
constexpr int kGameObject = 1, kTexture2D = 28, kTextAsset = 49, kAudioClip = 83, kMonoBehaviour = 114, kAssetBundle = 142;

struct Bundle {
    std::string signature;
    uint32_t version = 0;
    std::string player, engine;
    bool aligned = false; // the block info starts 16-aligned
    struct Node {
        std::string name;
        uint32_t flags = 0;
        std::string data;                     // a raw node (.resS, .resource)
        std::unique_ptr<SerializedFile> file; // a serialized file (CAB-...)
    };
    std::vector<Node> nodes;

    static Bundle parse(std::string_view data);
    std::string save() const;

    Node& node_ending(std::string_view suffix); // the one node whose name ends so (ValueError otherwise)
    // Every object of every serialized file, as UnityPy's env.objects lists them.
    struct Ref {
        SerializedFile* file;
        Object* obj;
        int class_id() const { return file->class_id(*obj); }
        Value read() const { return file->read(*obj); }
        void write(const Value& v) const { file->write(*obj, v); }
    };
    std::vector<Ref> objects();
};

std::string lz4_block_decompress(std::string_view src, size_t size);

} // namespace dlcmaker::unity
