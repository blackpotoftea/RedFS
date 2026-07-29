// Minimal CR2W reader.
//
// CR2W is the container every cooked RED4 resource lives in. Full deserialization
// needs the game's RTTI (thousands of classes), but the container itself is
// self-describing: every property carries its own name and RED type name as
// indices into the file's own string table. So a reader that knows *no* classes
// can still walk the whole object graph and pull out named fields -- which is
// all a mod needs to find a texture's dimensions or a mesh's geometry buffer.
//
// Layout:
//   0x00 u32 'CR2W'
//   0x04 u32 version (163..195)   u32 flags   u64 timestamp   u32 build
//        u32 objects_end  u32 buffers_end  u32 crc32  u32 num_chunks
//   0x28 10 x { u32 offset; u32 item_count; u32 crc32 }
//   0xA0 string table (NUL-terminated, indexed by offset relative to table start)
//        names[8]  imports[8]  properties[16]  chunks[24]  buffers[24]  embedded[16]
//
// Chunk data is a flat TLV stream:
//   u8 0
//   repeat { u16 name_idx; u16 type_idx; u32 size_including_these_4; bytes }
//   until name_idx == 0
// Nested structs use the exact same encoding, which is what makes dotted paths
// like "header.sizeInfo.width" work.

#include "internal.hpp"

#include <memory>

namespace redfs {
namespace {

constexpr size_t kCr2wHeaderSize = 0xA0;

bool starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

// CDPR's LEB128 variant: first octet keeps the sign in bit 7 and the
// continuation flag in bit 6; later octets are plain LEB128.
int32_t read_vlq(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return 0;
    uint8_t b = *p++;
    const bool negative = (b & 0x80) != 0;
    int32_t value = b & 0x3F;
    int shift = 6;
    bool more = (b & 0x40) != 0;
    while (more && p < end && shift < 32) {
        b = *p++;
        value |= static_cast<int32_t>(b & 0x7F) << shift;
        shift += 7;
        more = (b & 0x80) != 0;
    }
    return negative ? -value : value;
}

// One step of the TLV walk.
// Default to empty strings rather than null: `name` and `type` are always passed
// to strcmp-family functions, so an unfilled Prop should compare as "no match"
// rather than crash.
struct Prop {
    const char* name = "";
    const char* type = "";
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

// Iterates the properties of a struct body. `body` points at the leading zero
// byte. Returns false once the terminator is reached or the stream runs out.
class PropWalker {
public:
    PropWalker(const redfs_cr2w* f, const uint8_t* body, const uint8_t* end)
        : f_(f), p_(body), end_(end) {
        if (p_ < end_) ++p_;  // skip the leading zero
    }

    bool next(Prop* out) {
        if (p_ + 8 > end_) return false;
        const uint16_t name_idx = rd16(p_);
        if (name_idx == 0) return false;
        const uint16_t type_idx = rd16(p_ + 2);
        const uint32_t sz = rd32(p_ + 4);
        if (sz < 4) return false;
        const uint32_t payload = sz - 4;
        if (p_ + 8 + payload > end_) return false;

        out->name = f_->name(name_idx);
        out->type = f_->name(type_idx);
        out->data = p_ + 8;
        out->size = payload;
        p_ += 8 + payload;
        return true;
    }

private:
    const redfs_cr2w* f_;
    const uint8_t* p_;
    const uint8_t* end_;
};

// A struct body always opens with a zero byte and closes with a u16 zero.
bool looks_like_struct(const uint8_t* data, uint32_t size) {
    return size >= 3 && data[0] == 0;
}

// Serialized width of a fixed-size element type, or 0 when the type is
// variable-width (a struct, which has to be measured by scanning its TLV).
uint32_t fixed_width(const char* type) {
    struct Entry {
        const char* name;
        uint32_t size;
    };
    static const Entry kFixed[] = {
        {"Bool", 1},   {"Int8", 1},    {"Uint8", 1},   {"Int16", 2},  {"Uint16", 2},
        {"CName", 2},  {"Int32", 4},   {"Uint32", 4},  {"Float", 4},  {"Int64", 8},
        {"Uint64", 8}, {"Double", 8},  {"TweakDBID", 8}, {"NodeRef", 8},
    };
    for (const auto& e : kFixed)
        if (std::strcmp(type, e.name) == 0) return e.size;
    if (starts_with(type, "handle:") || starts_with(type, "whandle:")) return 4;
    if (starts_with(type, "rRef:") || starts_with(type, "raRef:")) return 2;
    return 0;
}

// Walks a struct body to its terminator and returns the byte just past it, or
// nullptr if the stream does not parse.
const uint8_t* struct_end(const uint8_t* body, const uint8_t* limit) {
    if (body >= limit) return nullptr;
    const uint8_t* p = body + 1;  // leading zero
    for (;;) {
        if (p + 2 > limit) return nullptr;
        const uint16_t name_idx = rd16(p);
        if (name_idx == 0) return p + 2;
        if (p + 8 > limit) return nullptr;
        const uint32_t sz = rd32(p + 4);
        if (sz < 4) return nullptr;
        const uint32_t payload = sz - 4;

        // Bound-check the advance BEFORE making it, in 64-bit.
        //
        // Writing this as `p += 8 + (sz - 4)` and checking `p > limit` afterwards
        // is what PropWalker::next deliberately avoids: `8 + (sz - 4)` is
        // evaluated entirely in 32-bit (the literal 8 promotes to unsigned), so
        // sz == 0xFFFFFFFC gives 8 + 0xFFFFFFF8 == 0x100000000 -> 0. The pointer
        // then never moves, the after-the-fact check cannot fire because p is
        // unchanged, and the loop spins forever on a file a mod could ship.
        if (static_cast<uint64_t>(limit - (p + 8)) < payload) return nullptr;
        p += 8 + static_cast<size_t>(payload);
    }
}

// The element type of an "array:X" / "static:N,X" / "[N]X" type name.
//
// All three spellings must be handled here because cr2w_decode accepts all three
// as REDFS_KIND_ARRAY. Missing one means the whole array type name is returned as
// its own element type, and every element is then sized and decoded as if it were
// that array -- which is what "[N]X" used to do.
const char* element_type(const char* array_type) {
    if (starts_with(array_type, "array:")) return array_type + 6;
    if (starts_with(array_type, "static:")) {
        // "static:5,Uint32" -- skip the count
        const char* comma = std::strchr(array_type, ',');
        return comma ? comma + 1 : array_type + 7;
    }
    if (array_type[0] == '[') {
        // "[3]Float" -- a RED4 fixed-size array; skip past the bracketed count.
        const char* close = std::strchr(array_type, ']');
        if (close) return close + 1;
    }
    return array_type;
}

// End of a CString element: a VLQ length prefix, then the characters. UTF-16
// when the prefix is positive, UTF-8 when negative.
const uint8_t* cstring_end(const uint8_t* p, const uint8_t* limit) {
    const uint8_t* cursor = p;
    const int32_t prefix = read_vlq(cursor, limit);
    const uint64_t chars = static_cast<uint64_t>(prefix < 0 ? -static_cast<int64_t>(prefix)
                                                            : prefix);
    const uint64_t bytes = prefix > 0 ? chars * 2 : chars;
    if (static_cast<uint64_t>(limit - cursor) < bytes) return nullptr;
    return cursor + bytes;
}

// How the elements of an array are laid out.
//
// The type name alone is not always enough. fixed_width knows the primitives and
// the pointer-ish types; CString is self-describing; a struct is a TLV body. What
// is left is a fixed-width type whose name we cannot recognise -- in practice an
// enum, which serialises as a 2-byte name-table index exactly like CName but
// under a per-enum type name we have no table for.
//
// For that last case the payload itself settles it: an enum array divides evenly
// into equal elements, and a struct array essentially never does, because struct
// elements vary in size. So a clean division into a small power-of-two stride is
// strong evidence of a uniform type, and it is checked BEFORE falling back to the
// TLV walk. Guessing "struct" first would misfire on any enum whose low byte is
// zero (name index >= 256), which looks exactly like a struct's leading zero.
struct ElementLayout {
    enum Kind { kFixed, kCString, kStruct } kind;
    uint64_t stride;  // meaningful when kind == kFixed
};

ElementLayout classify_elements(const char* elem, const uint8_t* first, const uint8_t* limit,
                                uint32_t count) {
    if (const uint32_t w = fixed_width(elem)) return {ElementLayout::kFixed, w};
    if (std::strcmp(elem, "CString") == 0) return {ElementLayout::kCString, 0};

    if (count > 0) {
        const uint64_t avail = static_cast<uint64_t>(limit - first);
        if (avail % count == 0) {
            const uint64_t stride = avail / count;
            // Enums are 2; the bound is deliberately tight so a struct array that
            // happens to divide evenly is not mistaken for a uniform one.
            if (stride >= 1 && stride <= 8) return {ElementLayout::kFixed, stride};
        }
    }
    return {ElementLayout::kStruct, 0};
}

}  // namespace

redfs_status cr2w_walk_array(const redfs_cr2w* f, const redfs_value* array, redfs_elem_fn fn,
                             void* user) {
    if (!array || !fn) return REDFS_E_INVALID_ARG;
    if (array->kind != REDFS_KIND_ARRAY) return REDFS_E_INVALID_ARG;

    if (array->size < 4) return REDFS_E_CORRUPT;
    const char* elem = element_type(array->type);
    const uint32_t count = static_cast<uint32_t>(array->as.u);
    const uint8_t* p = array->data + 4;  // past the element count
    const uint8_t* limit = array->data + array->size;
    const ElementLayout layout = classify_elements(elem, p, limit, count);

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* end = nullptr;
        switch (layout.kind) {
            case ElementLayout::kFixed:
                if (static_cast<uint64_t>(limit - p) < layout.stride) return REDFS_E_CORRUPT;
                end = p + layout.stride;
                break;
            case ElementLayout::kCString:
                end = cstring_end(p, limit);
                break;
            case ElementLayout::kStruct:
                end = struct_end(p, limit);
                break;
        }
        if (!end || end <= p) return REDFS_E_CORRUPT;  // no progress means malformed

        redfs_value v{};
        cr2w_decode(f, elem, p, static_cast<uint32_t>(end - p), &v);
        if (!fn(i, &v, user)) break;
        p = end;
    }
    return REDFS_OK;
}

void cr2w_decode(const redfs_cr2w* f, const char* type, const uint8_t* data, uint32_t size,
                 redfs_value* out) {
    out->type = type;
    out->data = data;
    out->size = size;
    out->kind = REDFS_KIND_RAW;
    out->as.u = 0;

    auto eq = [&](const char* s) { return std::strcmp(type, s) == 0; };

    if (eq("Bool") && size >= 1) {
        out->kind = REDFS_KIND_BOOL;
        out->as.u = data[0] ? 1u : 0u;
    } else if (eq("Int8") && size >= 1) {
        out->kind = REDFS_KIND_INT;
        out->as.i = static_cast<int8_t>(data[0]);
    } else if (eq("Uint8") && size >= 1) {
        out->kind = REDFS_KIND_UINT;
        out->as.u = data[0];
    } else if (eq("Int16") && size >= 2) {
        out->kind = REDFS_KIND_INT;
        out->as.i = static_cast<int16_t>(rd16(data));
    } else if (eq("Uint16") && size >= 2) {
        out->kind = REDFS_KIND_UINT;
        out->as.u = rd16(data);
    } else if (eq("Int32") && size >= 4) {
        out->kind = REDFS_KIND_INT;
        out->as.i = static_cast<int32_t>(rd32(data));
    } else if (eq("Uint32") && size >= 4) {
        out->kind = REDFS_KIND_UINT;
        out->as.u = rd32(data);
    } else if (eq("Int64") && size >= 8) {
        out->kind = REDFS_KIND_INT;
        out->as.i = static_cast<int64_t>(rd64(data));
    } else if ((eq("Uint64") || eq("TweakDBID") || eq("NodeRef")) && size >= 8) {
        out->kind = REDFS_KIND_UINT;
        out->as.u = rd64(data);
    } else if (eq("Float") && size >= 4) {
        out->kind = REDFS_KIND_FLOAT;
        float v;
        std::memcpy(&v, data, 4);
        out->as.f = v;
    } else if (eq("Double") && size >= 8) {
        out->kind = REDFS_KIND_FLOAT;
        double v;
        std::memcpy(&v, data, 8);
        out->as.f = v;
    } else if (eq("CName") && size >= 2) {
        out->kind = REDFS_KIND_NAME;
        out->as.s = f->name(rd16(data));
    } else if (eq("CString")) {
        // Decoded strings are owned by the handle so redfs_value::as.s stays
        // valid as long as the caller holds it. Cached by source pointer: the
        // same property decoded twice must not allocate twice, or a per-frame
        // query grows the handle without bound until it is closed.
        auto* mut = const_cast<redfs_cr2w*>(f);
        auto cached = mut->string_cache.find(data);
        if (cached != mut->string_cache.end()) {
            out->kind = REDFS_KIND_STRING;
            out->as.s = cached->second->c_str();
        } else {
            // Build the value first and only take ownership once it is decoded,
            // so a payload that fails validation does not leave an entry behind.
            std::string decoded;
            const uint8_t* p = data;
            const int32_t prefix = read_vlq(p, data + size);
            const uint64_t chars =
                static_cast<uint64_t>(prefix < 0 ? -static_cast<int64_t>(prefix) : prefix);

            if (prefix > 0) {  // UTF-16
                if (static_cast<uint64_t>(data + size - p) >= chars * 2) {
                    decoded.reserve(static_cast<size_t>(chars));
                    for (uint64_t i = 0; i < chars; ++i) {
                        const uint16_t wc = rd16(p + i * 2);
                        decoded.push_back(wc < 0x80 ? static_cast<char>(wc) : '?');
                    }
                }
            } else if (static_cast<uint64_t>(data + size - p) >= chars) {
                decoded.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(chars));
            }

            auto owned = std::make_unique<std::string>(std::move(decoded));
            out->kind = REDFS_KIND_STRING;
            out->as.s = owned->c_str();
            mut->string_cache.emplace(data, owned.get());
            mut->owned_strings.push_back(std::move(owned));
        }
    } else if (_stricmp(type, "SerializationDeferredDataBuffer") == 0 && size >= 2) {
        // Spelled with a leading lowercase 's' in texture resources and with a
        // capital elsewhere, so this one has to be matched case-insensitively.
        //
        // Index 0 means null, the same convention handles and rRef use. It must
        // be rejected BEFORE the subtraction: 0 - 1 is 0xFFFFFFFF, which is
        // REDFS_PART_MAIN, so a null buffer would quietly resolve to segment 0
        // and hand back the CR2W document as if it were payload -- a DDS whose
        // pixels are the document, or mesh bounds swept from it and cached.
        const uint32_t index = rd16(data);
        if (index > 0) {
            out->kind = REDFS_KIND_BUFFER;
            out->as.buffer = index - 1u;
        }
        // else: no buffer attached; leave the value RAW so callers see no index.
    } else if (eq("DataBuffer") && size >= 4) {
        const uint32_t v = rd32(data);
        if (v > 0x80000000u) {
            out->kind = REDFS_KIND_BUFFER;
            out->as.buffer = (v ^ 0x80000000u) - 1u;
        }
        // v == 0x80000000 is a null buffer; anything else is inline bytes.
    } else if (starts_with(type, "handle:") || starts_with(type, "whandle:")) {
        if (size >= 4) {
            out->kind = REDFS_KIND_HANDLE;
            out->as.chunk = static_cast<int32_t>(rd32(data)) - 1;
        }
    } else if (starts_with(type, "rRef:") || starts_with(type, "raRef:")) {
        if (size >= 2) {
            const uint32_t idx = rd16(data);
            out->kind = REDFS_KIND_STRING;
            out->as.s = (idx > 0 && idx <= f->imports.size())
                            ? f->str(f->imports[idx - 1].str_offset)
                            : "";
        }
    } else if (starts_with(type, "array:") || starts_with(type, "static:") ||
               starts_with(type, "[")) {
        if (size >= 4) {
            out->kind = REDFS_KIND_ARRAY;
            out->as.u = rd32(data);
        }
    } else if (size == 2) {
        // Nothing else is exactly two bytes: this is an enum, stored as a name index.
        out->kind = REDFS_KIND_NAME;
        out->as.s = f->name(rd16(data));
    } else if (looks_like_struct(data, size)) {
        out->kind = REDFS_KIND_STRUCT;
    }
}

namespace {

// Resolves one dotted path segment inside a struct body.
bool descend(const redfs_cr2w* f, const uint8_t*& body, const uint8_t*& end, const char* name,
             size_t name_len, redfs_value* out) {
    PropWalker w(f, body, end);
    Prop prop{};
    while (w.next(&prop)) {
        if (std::strncmp(prop.name, name, name_len) != 0 || prop.name[name_len] != '\0') continue;
        cr2w_decode(f, prop.type, prop.data, prop.size, out);
        body = prop.data;
        end = prop.data + prop.size;
        return true;
    }
    return false;
}

}  // namespace

redfs_status cr2w_get_in(const redfs_cr2w* f, const redfs_value* parent, const char* prop_path,
                         redfs_value* out) {
    if (!parent || parent->kind != REDFS_KIND_STRUCT) return REDFS_E_INVALID_ARG;
    if (!prop_path || !*prop_path) {
        *out = *parent;
        return REDFS_OK;
    }

    const uint8_t* body = parent->data;
    const uint8_t* end = parent->data + parent->size;
    const char* p = prop_path;
    while (*p) {
        const char* dot = std::strchr(p, '.');
        const size_t len = dot ? static_cast<size_t>(dot - p) : std::strlen(p);
        if (len == 0) return REDFS_E_INVALID_ARG;
        if (!descend(f, body, end, p, len, out)) return REDFS_E_NOT_FOUND;
        if (!dot) return REDFS_OK;
        if (out->kind != REDFS_KIND_STRUCT) return REDFS_E_NOT_FOUND;
        p = dot + 1;
    }
    return REDFS_E_INVALID_ARG;
}

redfs_status cr2w_find(const redfs_cr2w* f, uint32_t chunk, const char* prop_path,
                       redfs_value* out) {
    if (chunk >= f->chunks.size()) return REDFS_E_RANGE;
    const Cr2wChunk& c = f->chunks[chunk];
    if (static_cast<uint64_t>(c.data_offset) + c.data_size > f->size) return REDFS_E_CORRUPT;

    redfs_value root{};
    root.type = f->name(c.class_name);
    root.data = f->base + c.data_offset;
    root.size = c.data_size;
    root.kind = REDFS_KIND_STRUCT;

    return cr2w_get_in(f, &root, prop_path, out);
}

redfs_status cr2w_walk_in(const redfs_cr2w* f, const redfs_value* parent, const char* prop_path,
                          redfs_prop_fn fn, void* user) {
    if (!fn) return REDFS_E_INVALID_ARG;

    redfs_value root{};
    redfs_status st = cr2w_get_in(f, parent, prop_path, &root);
    if (st != REDFS_OK) return st;
    if (root.kind != REDFS_KIND_STRUCT) return REDFS_E_INVALID_ARG;

    PropWalker w(f, root.data, root.data + root.size);
    Prop prop{};
    while (w.next(&prop)) {
        redfs_value v{};
        cr2w_decode(f, prop.type, prop.data, prop.size, &v);
        if (!fn(prop.name, &v, user)) break;
    }
    return REDFS_OK;
}

redfs_status cr2w_walk(const redfs_cr2w* f, uint32_t chunk, const char* prop_path,
                       redfs_prop_fn fn, void* user) {
    if (!fn) return REDFS_E_INVALID_ARG;

    redfs_value root{};
    redfs_status st = cr2w_find(f, chunk, prop_path, &root);
    if (st != REDFS_OK) return st;
    if (root.kind != REDFS_KIND_STRUCT) return REDFS_E_INVALID_ARG;

    PropWalker w(f, root.data, root.data + root.size);
    Prop prop{};
    while (w.next(&prop)) {
        redfs_value v{};
        cr2w_decode(f, prop.type, prop.data, prop.size, &v);
        if (!fn(prop.name, &v, user)) break;
    }
    return REDFS_OK;
}

redfs_status cr2w_parse(const void* data, uint64_t size, redfs_cr2w* out) {
    const uint8_t* b = static_cast<const uint8_t*>(data);
    if (size < kCr2wHeaderSize) return fail(REDFS_E_CORRUPT, "CR2W blob too small (%llu bytes)",
                                            static_cast<unsigned long long>(size));
    if (rd32(b) != kCr2wMagic) return fail(REDFS_E_CORRUPT, "not a CR2W file");

    const uint32_t version = rd32(b + 4);
    if (version < 163 || version > 195)
        return fail(REDFS_E_UNSUPPORTED, "CR2W version %u is outside the supported range", version);

    struct Table {
        uint32_t offset, count;
    } tables[10];
    for (int i = 0; i < 10; ++i) {
        const uint8_t* t = b + 0x28 + i * 12;
        tables[i] = {rd32(t), rd32(t + 4)};
    }

    // Table 0 is the string blob: `count` is its size in bytes, not an item count.
    const Table& strings = tables[0];
    if (static_cast<uint64_t>(strings.offset) + strings.count > size)
        return fail(REDFS_E_CORRUPT, "CR2W string table runs past the end of the file");
    // Every string in a well-formed file is NUL-terminated, so the table's last
    // byte is a NUL. Requiring that turns "the offset is in range" into "the
    // string is safe to walk", which is what every strcmp downstream relies on.
    //
    // Indexed in 64-bit to match the range check above. Left in 32-bit, the two
    // disagree once offset + count crosses 2^32: the guard passes on the true
    // sum while the index wraps to a small value, so the invariant this line
    // exists to establish is never actually checked. Unreachable through the
    // depot today (a main segment is uint32-sized), but the mismatch is a trap
    // for whoever later relaxes the input path.
    const uint64_t last = static_cast<uint64_t>(strings.offset) + strings.count - 1;
    if (strings.count == 0 || b[last] != '\0')
        return fail(REDFS_E_CORRUPT, "CR2W string table is not NUL-terminated");

    out->base = b;
    out->size = size;
    out->strings = reinterpret_cast<const char*>(b + strings.offset);
    out->strings_size = strings.count;

    auto in_bounds = [&](const Table& t, uint32_t stride) {
        return static_cast<uint64_t>(t.offset) + static_cast<uint64_t>(t.count) * stride <= size;
    };

    if (!in_bounds(tables[1], 8) || !in_bounds(tables[2], 8) || !in_bounds(tables[4], 24) ||
        !in_bounds(tables[5], 24))
        return fail(REDFS_E_CORRUPT, "CR2W table runs past the end of the file");

    out->names.reserve(tables[1].count);
    for (uint32_t i = 0; i < tables[1].count; ++i)
        out->names.push_back(rd32(b + tables[1].offset + i * 8));

    out->imports.reserve(tables[2].count);
    for (uint32_t i = 0; i < tables[2].count; ++i) {
        const uint8_t* p = b + tables[2].offset + i * 8;
        out->imports.push_back(Cr2wImport{rd32(p), rd16(p + 4), rd16(p + 6)});
    }

    out->chunks.reserve(tables[4].count);
    for (uint32_t i = 0; i < tables[4].count; ++i) {
        const uint8_t* p = b + tables[4].offset + i * 24;
        out->chunks.push_back(Cr2wChunk{rd16(p), rd32(p + 12), rd32(p + 8)});
    }

    out->buffers.reserve(tables[5].count);
    for (uint32_t i = 0; i < tables[5].count; ++i) {
        const uint8_t* p = b + tables[5].offset + i * 24;
        out->buffers.push_back(Cr2wBuffer{rd32(p + 4), rd32(p + 8), rd32(p + 12), rd32(p + 16)});
    }

    // Import tables are the one free source of real path strings, and the only
    // one that knows paths a mod invented. No-op unless the dictionary is on.
    paths_learn_imports(out);

    return REDFS_OK;
}

}  // namespace redfs
