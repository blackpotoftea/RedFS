// Synthetic .archive and CR2W builders.
//
// The point of this file: every other check in the project needs an 85 GB game
// install, which makes them integration tests. These builders produce byte-exact
// containers from nothing, so the parsers can be tested against *known* inputs
// with known expected outputs -- including malformed ones, which no real install
// will ever hand you.
//
// The layouts here are the same ones documented in docs/done/archive-format.md
// and docs/done/cr2w-format.md, written independently of the reader. If a
// builder and the reader disagree, one of them has the format wrong.
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace fixture {

// --- little-endian writer ----------------------------------------------------

struct Buf {
    std::vector<uint8_t> bytes;

    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void i64(int64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void raw(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    void zeros(size_t n) { bytes.insert(bytes.end(), n, 0); }
    void cstr(const std::string& s) {
        raw(s.data(), s.size());
        u8(0);
    }
    void pad_to(size_t offset) {
        if (bytes.size() < offset) zeros(offset - bytes.size());
    }
    size_t size() const { return bytes.size(); }

    // Patch a u32 already written.
    void patch_u32(size_t at, uint32_t v) { std::memcpy(bytes.data() + at, &v, 4); }
    void patch_u64(size_t at, uint64_t v) { std::memcpy(bytes.data() + at, &v, 8); }
};

// --- CR2W --------------------------------------------------------------------

// Builds a CR2W document. Strings are interned; names and imports index into the
// string table; chunks carry a TLV property stream.
class Cr2wBuilder {
public:
    // Interns a string, returning its offset relative to the string table start.
    uint32_t string(const std::string& s) {
        auto it = string_offsets_.find(s);
        if (it != string_offsets_.end()) return it->second;
        const uint32_t off = static_cast<uint32_t>(strings_.size());
        strings_.insert(strings_.end(), s.begin(), s.end());
        strings_.push_back('\0');
        string_offsets_[s] = off;
        return off;
    }

    // Interns a name, returning its index in the name table. Index 0 must stay
    // the empty string: a zero name index terminates a property stream.
    uint16_t name(const std::string& s) {
        for (uint16_t i = 0; i < names_.size(); ++i)
            if (names_[i] == s) return i;
        names_.push_back(s);
        return static_cast<uint16_t>(names_.size() - 1);
    }

    // Adds an import (a dependency path). Returns its 1-based reference index.
    uint16_t import(const std::string& depot_path, const std::string& class_name) {
        imports_.push_back({depot_path, class_name});
        return static_cast<uint16_t>(imports_.size());
    }

    // --- property stream construction ---
    //
    // Values are appended to a chunk body. `begin_chunk` starts one, and it is
    // terminated by `end_chunk`.

    void begin_chunk(const std::string& class_name) {
        chunk_class_.push_back(class_name);
        bodies_.emplace_back();
        bodies_.back().u8(0);  // the leading zero every struct body opens with
    }
    void end_chunk() { bodies_.back().u16(0); }

    // A property whose value bytes the caller supplies.
    void prop(const std::string& prop_name, const std::string& type, const void* data, size_t len) {
        Buf& b = bodies_.back();
        b.u16(name(prop_name));
        b.u16(name(type));
        b.u32(static_cast<uint32_t>(len) + 4);  // size INCLUDES these 4 bytes
        b.raw(data, len);
    }

    void prop_u8(const std::string& n, uint8_t v) { prop(n, "Uint8", &v, 1); }
    void prop_u16(const std::string& n, uint16_t v) { prop(n, "Uint16", &v, 2); }
    void prop_u32(const std::string& n, uint32_t v) { prop(n, "Uint32", &v, 4); }
    void prop_u64(const std::string& n, uint64_t v) { prop(n, "Uint64", &v, 8); }
    void prop_i32(const std::string& n, int32_t v) { prop(n, "Int32", &v, 4); }
    void prop_f32(const std::string& n, float v) { prop(n, "Float", &v, 4); }
    void prop_bool(const std::string& n, bool v) {
        uint8_t b = v ? 1 : 0;
        prop(n, "Bool", &b, 1);
    }
    void prop_cname(const std::string& n, const std::string& value) {
        const uint16_t idx = name(value);
        prop(n, "CName", &idx, 2);
    }
    // Enums serialize exactly like CName: a name-table index, typed as the enum.
    void prop_enum(const std::string& n, const std::string& enum_type,
                   const std::string& value) {
        const uint16_t idx = name(value);
        prop(n, enum_type, &idx, 2);
    }
    void prop_handle(const std::string& n, const std::string& type, int32_t chunk) {
        const int32_t encoded = chunk + 1;  // 0 means null
        prop(n, type, &encoded, 4);
    }
    void prop_deferred_buffer(const std::string& n, uint32_t buffer_index,
                              const std::string& type = "serializationDeferredDataBuffer") {
        const uint16_t encoded = static_cast<uint16_t>(buffer_index + 1);
        prop(n, type, &encoded, 2);
    }
    void prop_data_buffer(const std::string& n, uint32_t buffer_index) {
        const uint32_t encoded = 0x80000000u | (buffer_index + 1);
        prop(n, "DataBuffer", &encoded, 4);
    }
    void prop_rref(const std::string& n, const std::string& type, uint16_t import_ref) {
        prop(n, type, &import_ref, 2);
    }

    // A nested struct: same encoding as a chunk body, inlined as a value.
    void begin_struct(const std::string& prop_name, const std::string& type) {
        pending_.push_back({prop_name, type, {}});
        pending_.back().body.u8(0);
        stack_.push_back(&pending_.back().body);
    }
    void end_struct() {
        Pending p = std::move(pending_.back());
        pending_.pop_back();
        stack_.pop_back();
        p.body.u16(0);
        // Append into whatever is now the innermost target.
        Buf& target = stack_.empty() ? bodies_.back() : *stack_.back();
        target.u16(name(p.name));
        target.u16(name(p.type));
        target.u32(static_cast<uint32_t>(p.body.size()) + 4);
        target.raw(p.body.bytes.data(), p.body.size());
    }

    // Struct-aware property emission: routes to the innermost open struct.
    void prop_in(const std::string& prop_name, const std::string& type, const void* data,
                 size_t len) {
        Buf& target = stack_.empty() ? bodies_.back() : *stack_.back();
        target.u16(name(prop_name));
        target.u16(name(type));
        target.u32(static_cast<uint32_t>(len) + 4);
        target.raw(data, len);
    }
    void prop_in_u16(const std::string& n, uint16_t v) { prop_in(n, "Uint16", &v, 2); }
    void prop_in_u32(const std::string& n, uint32_t v) { prop_in(n, "Uint32", &v, 4); }
    void prop_in_f32(const std::string& n, float v) { prop_in(n, "Float", &v, 4); }

    // An array of fixed-width elements, already encoded by the caller.
    void prop_array(const std::string& prop_name, const std::string& elem_type, uint32_t count,
                    const void* elements, size_t bytes) {
        Buf v;
        v.u32(count);
        v.raw(elements, bytes);
        Buf& target = stack_.empty() ? bodies_.back() : *stack_.back();
        target.u16(name(prop_name));
        target.u16(name("array:" + elem_type));
        target.u32(static_cast<uint32_t>(v.size()) + 4);
        target.raw(v.bytes.data(), v.size());
    }

    // An array of CNames, the shape meshMeshAppearance.chunkMaterials uses.
    void prop_array_cname(const std::string& prop_name, const std::vector<std::string>& values) {
        std::vector<uint8_t> encoded;
        for (const auto& s : values) {
            const uint16_t idx = name(s);
            encoded.insert(encoded.end(), reinterpret_cast<const uint8_t*>(&idx),
                           reinterpret_cast<const uint8_t*>(&idx) + 2);
        }
        prop_array(prop_name, "CName", static_cast<uint32_t>(values.size()), encoded.data(),
                   encoded.size());
    }

    // Serializes the whole document. Not const: interning class names and import
    // paths can still add to the name and string tables, and that has to happen
    // before either table's size is known.
    std::vector<uint8_t> build(uint32_t version = 195);

private:
    struct Import {
        std::string path, class_name;
    };
    struct Pending {
        std::string name, type;
        Buf body;
    };

    std::vector<char> strings_{'\0'};  // offset 0 is the empty string
    std::map<std::string, uint32_t> string_offsets_{{"", 0}};
    std::vector<std::string> names_{""};  // index 0 must be empty
    std::vector<Import> imports_;
    std::vector<std::string> chunk_class_;
    std::deque<Buf> bodies_;
    // A deque, not a vector: `stack_` holds pointers into these, and nesting
    // begin_struct() would reallocate a vector and leave them dangling.
    std::deque<Pending> pending_;
    std::vector<Buf*> stack_;
};

// --- archive -----------------------------------------------------------------

// Builds a .archive. Files are added as a main blob plus optional buffers; each
// becomes one segment. Everything is stored uncompressed (zsize == size) so the
// tests do not need Oodle.
class ArchiveBuilder {
public:
    struct File {
        uint64_t hash;
        std::vector<std::vector<uint8_t>> segments;  // [0] is main, rest are buffers
    };

    void add(uint64_t hash, std::vector<uint8_t> main,
             std::vector<std::vector<uint8_t>> buffers = {}) {
        File f;
        f.hash = hash;
        f.segments.push_back(std::move(main));
        for (auto& b : buffers) f.segments.push_back(std::move(b));
        files_.push_back(std::move(f));
    }

    std::vector<uint8_t> build() const;

    // Writes to disk; returns the path. Used for depot-level tests.
    static bool write(const std::string& path, const std::vector<uint8_t>& bytes);

private:
    std::vector<File> files_;
};

// A minimal but structurally complete CBitmapTexture, for the texture path.
std::vector<uint8_t> make_texture_cr2w(uint32_t width, uint32_t height, uint32_t mips,
                                       const char* compression, const char* raw_format,
                                       bool gamma);

// A minimal but structurally complete CMesh with `chunks` submeshes.
std::vector<uint8_t> make_mesh_cr2w(uint32_t chunk_count, uint32_t verts_per_chunk,
                                    float quant_scale, float quant_offset);

// The matching geometry buffer for make_mesh_cr2w: int16 positions at stride 8.
std::vector<uint8_t> make_mesh_geometry(uint32_t chunk_count, uint32_t verts_per_chunk);

}  // namespace fixture
