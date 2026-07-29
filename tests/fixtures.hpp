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

    // An unsigned value under the narrowest Uint8/Uint16/Uint32/Uint64 that
    // holds it -- never narrower than `min_width` bytes, and under `type`
    // instead of the matching Uint* when one is given.
    //
    // Widening by value is not a convenience. A builder that hardcodes the width
    // stock cooks use truncates the value a test passed *because* it is out of
    // range: mipCount is Uint8 in every shipped texture, so a regression test
    // for a 4-billion-iteration hang wrote 0xFFFFFFFB as 251 and passed against
    // the unfixed reader. `min_width` keeps the stock width for ordinary values,
    // so fixtures stay byte-identical to a real cook until a test asks for
    // something a real cook would not write.
    //
    // `type` is the other half of what untrusted content controls. A property's
    // declared type name and its payload length are separate fields in the file,
    // so a hostile archive can label one byte "Uint32", four bytes "Uint8", or a
    // number "Float" -- and the reader's size and type checks are exactly what
    // such a file tests. Nothing else here can state a type that disagrees with
    // the bytes behind it.
    void prop_in_uint(const std::string& n, uint64_t v, uint32_t min_width = 0,
                      const std::string& type = "");

    // An array of fixed-width elements, already encoded by the caller.
    //
    // `declared_count` is written verbatim and is NOT derived from `bytes`: the
    // count is a u32 in the file and a hostile archive picks it freely, so a
    // test can say "claim 0xFFFFFFFF elements, write three". Everything that
    // trusts the declared count -- sizing an allocation, bounding a loop,
    // answering a length query without walking -- is wrong on exactly that file,
    // and a fixture that could only state the truth made the distinction
    // invisible.
    void prop_array(const std::string& prop_name, const std::string& elem_type,
                    uint32_t declared_count, const void* elements, size_t bytes) {
        Buf v;
        v.u32(declared_count);
        v.raw(elements, bytes);
        Buf& target = stack_.empty() ? bodies_.back() : *stack_.back();
        target.u16(name(prop_name));
        target.u16(name("array:" + elem_type));
        target.u32(static_cast<uint32_t>(v.size()) + 4);
        target.raw(v.bytes.data(), v.size());
    }

    // An array of CNames, the shape meshMeshAppearance.chunkMaterials uses.
    void prop_array_cname(const std::string& prop_name, const std::vector<std::string>& values) {
        prop_array_cname(prop_name, values, static_cast<uint32_t>(values.size()));
    }
    // The same, with a header count of the caller's choosing. Deriving the count
    // from `values` is what a cook does; the two-argument form above is that.
    // This one is how a test says "claim four billion names, write three" --
    // the shape the material budget in mesh.cpp exists to survive, and the shape
    // no amount of care in a test could produce while the count was derived.
    void prop_array_cname(const std::string& prop_name, const std::vector<std::string>& values,
                          uint32_t declared_count) {
        std::vector<uint8_t> encoded;
        for (const auto& s : values) {
            const uint16_t idx = name(s);
            encoded.insert(encoded.end(), reinterpret_cast<const uint8_t*>(&idx),
                           reinterpret_cast<const uint8_t*>(&idx) + 2);
        }
        prop_array(prop_name, "CName", declared_count, encoded.data(), encoded.size());
    }

    // Overrides the extent chunk `index` declares in the chunk table.
    //
    // build() derives both fields from the body it just wrote, so every chunk it
    // emits sits inside the file by construction -- which leaves cr2w_find's
    // check that a chunk does not run past the end of the blob with no input
    // that reaches it. The chunk table is 24 bytes of header per chunk, wholly
    // independent of the bytes it points at, and untrusted content sets it to
    // whatever it likes.
    void chunk_extent(uint32_t index, uint32_t data_offset, uint32_t data_size) {
        chunk_extents_[index] = Extent{data_offset, data_size};
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
    struct Extent {
        uint32_t offset, size;
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
    std::map<uint32_t, Extent> chunk_extents_;
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
        // Set by segment_range(); otherwise build() writes the real range.
        bool range_declared = false;
        uint32_t range_first = 0, range_last = 0;
    };

    void add(uint64_t hash, std::vector<uint8_t> main,
             std::vector<std::vector<uint8_t>> buffers = {}) {
        File f;
        f.hash = hash;
        f.segments.push_back(std::move(main));
        for (auto& b : buffers) f.segments.push_back(std::move(b));
        files_.push_back(std::move(f));
    }

    // Overrides the [first, last) segment range file `index` declares.
    //
    // build() derives the range from the segments it just wrote, so the ranges
    // it emits are always valid -- and the guards written for invalid ones have
    // no input that reaches them: resolve_part rejects `first >= last` or a
    // `last` past the segment count outright, and fill_info has to clamp instead
    // of reject, because an entry claiming last = 0xFFFFFFFF costs one
    // enumerate entry_count x segment_count iterations with no allocation and no
    // fault for a watchdog to notice. The range is 8 bytes of index entry,
    // independent of the payload behind it.
    void segment_range(size_t index, uint32_t first, uint32_t last) {
        files_[index].range_declared = true;
        files_[index].range_first = first;
        files_[index].range_last = last;
    }

    std::vector<uint8_t> build() const;

    // Writes to disk; returns the path. Used for depot-level tests.
    static bool write(const std::string& path, const std::vector<uint8_t>& bytes);

private:
    std::vector<File> files_;
};

// The parts of a cooked texture that a cook derives but a mod just writes.
// Every field defaults to what a stock cook produces, so the file agrees with
// itself unless a test asks otherwise.
struct TextureOverrides {
    // mipCount's payload width and declared type name. Stock is Uint8, which is
    // the default, and a bigger value widens on its own -- these are for the
    // cases a value cannot express: a small mip count declared Uint32, or a
    // mipCount typed as something the reader does not read as a number at all,
    // which is what forces its fallback path.
    uint32_t mip_count_width = 1;
    std::string mip_count_type;
    // The attached buffer textureData names. Stock is 0, the first buffer after
    // the main segment; naming one with no segment behind it is a file the
    // out-of-range part path has to reject rather than resolve elsewhere.
    uint32_t buffer_index = 0;
    // The chunk renderResourceBlobPC points at. Stock is chunk 1, the blob this
    // builder emits right after; any other value is a dangling handle, and the
    // fallback that searches by class name is only reachable through one.
    int32_t blob_chunk = 1;
};

// A minimal but structurally complete CBitmapTexture, for the texture path.
//
// `texture_type` and `slices` default to a plain 2D surface. Pass "TEXTYPE_CUBE"
// with slices = 6 to build a cubemap -- the DDS encoding of those differs from
// the 2D case in ways nothing could test while this was hardcoded.
//
// width, height, mips and slices are written at the width the value needs, not
// the width a cook would use, so passing a deliberately absurd one reaches the
// reader intact instead of arriving silently truncated.
std::vector<uint8_t> make_texture_cr2w(uint32_t width, uint32_t height, uint32_t mips,
                                       const char* compression, const char* raw_format, bool gamma,
                                       const char* texture_type = "TEXTYPE_2D",
                                       uint32_t slices = 1,
                                       const TextureOverrides& ov = TextureOverrides{});

// The same for a mesh: what the geometry actually is, versus what the file says
// about it. A declared count of 0 means "write the truth".
struct MeshOverrides {
    // Element counts the arrays declare. Whether a reader reports what the
    // header claims or what it could actually walk is invisible while the two
    // always agree -- and the difference is a loop bound handed to a caller.
    uint32_t declared_chunk_count = 0;       // renderChunkInfos
    uint32_t declared_appearance_count = 0;  // appearances
    uint32_t declared_material_count = 0;    // chunkMaterials
    // renderLODs is absent from a stock cook of this shape, and mesh_build reads
    // a missing array as one LOD, so it stays absent unless `lod_count` asks for
    // elements. `declared_lod_count` then lies about how many there are.
    uint32_t lod_count = 0;
    uint32_t declared_lod_count = 0;
    // slotStrides[0], the stride the bounds sweep steps the position stream by.
    // Stock is 8. A stride under 6 cannot hold a position and a huge one runs
    // the sweep off the end of the buffer; both are refused by guards that no
    // fixture could feed while this was 8.
    uint32_t position_stride = 8;
    // The chunk the single appearance handle resolves to. Stock is chunk 2, the
    // meshMeshAppearance this builder emits.
    int32_t appearance_chunk = 2;
};

// A minimal but structurally complete CMesh with `chunks` submeshes.
//
// numVertices widens past its stock Uint16 when the count demands it, for the
// same reason the texture builder widens mipCount: a vertex count is a loop
// bound, and truncating one to fit the stock width is how a test for an absurd
// count ends up asserting against an ordinary one.
std::vector<uint8_t> make_mesh_cr2w(uint32_t chunk_count, uint32_t verts_per_chunk,
                                    float quant_scale, float quant_offset,
                                    const MeshOverrides& ov = MeshOverrides{});

// The matching geometry buffer for make_mesh_cr2w: int16 positions at `stride`,
// which must match MeshOverrides::position_stride for the bounds to mean
// anything. A stride under 6 truncates the position it is meant to hold; that is
// the input, not an accident, so it is written as asked.
std::vector<uint8_t> make_mesh_geometry(uint32_t chunk_count, uint32_t verts_per_chunk,
                                        uint32_t stride = 8);

}  // namespace fixture
