#include "fixtures.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace fixture {

// --- CR2W --------------------------------------------------------------------

std::vector<uint8_t> Cr2wBuilder::build(uint32_t version) {
    constexpr size_t kHeaderSize = 0xA0;  // magic + 36-byte header + 10 x 12-byte tables

    // Finalize the tables before measuring anything. Order matters: interning a
    // class name can add a name, and every name needs a string, so names must
    // settle before strings do. Getting this wrong writes the string blob before
    // the last few strings exist, and the reader then sees empty class names.
    for (const auto& imp : imports_) name(imp.class_name);
    for (const auto& cls : chunk_class_) name(cls);
    for (size_t i = 0; i < names_.size(); ++i) string(names_[i]);
    for (const auto& imp : imports_) string(imp.path);

    // Lay the sections out end to end, after the fixed header.
    const uint32_t strings_offset = static_cast<uint32_t>(kHeaderSize);
    const uint32_t strings_size = static_cast<uint32_t>(strings_.size());

    const uint32_t names_offset = strings_offset + strings_size;
    const uint32_t names_count = static_cast<uint32_t>(names_.size());

    const uint32_t imports_offset = names_offset + names_count * 8;
    const uint32_t imports_count = static_cast<uint32_t>(imports_.size());

    const uint32_t props_offset = imports_offset + imports_count * 8;
    const uint32_t chunks_offset = props_offset;  // no property table
    const uint32_t chunks_count = static_cast<uint32_t>(chunk_class_.size());

    const uint32_t buffers_offset = chunks_offset + chunks_count * 24;
    const uint32_t embedded_offset = buffers_offset;  // no buffer or embedded tables
    const uint32_t data_offset = embedded_offset;

    // Chunk bodies follow the tables, back to back.
    std::vector<uint32_t> body_offsets;
    uint32_t at = data_offset;
    for (const auto& body : bodies_) {
        body_offsets.push_back(at);
        at += static_cast<uint32_t>(body.size());
    }
    const uint32_t objects_end = at;

    Buf out;
    out.u32(0x57325243);  // 'CR2W'
    out.u32(version);
    out.u32(0);           // flags
    out.u64(0);           // timestamp
    out.u32(0);           // build
    out.u32(objects_end);
    out.u32(objects_end);  // buffers_end
    out.u32(0);            // crc32
    out.u32(chunks_count);

    auto table = [&out](uint32_t offset, uint32_t count) {
        out.u32(offset);
        out.u32(count);
        out.u32(0);  // crc32
    };
    table(strings_offset, strings_size);  // [0] count is BYTES for the string blob
    table(names_offset, names_count);     // [1]
    table(imports_offset, imports_count); // [2]
    table(props_offset, 0);               // [3]
    table(chunks_offset, chunks_count);   // [4]
    table(buffers_offset, 0);             // [5]
    table(embedded_offset, 0);            // [6]
    table(0, 0);                          // [7..9] unused
    table(0, 0);
    table(0, 0);

    // string blob
    out.raw(strings_.data(), strings_.size());

    // names: {u32 str_offset, u32 hash}
    for (const auto& n : names_) {
        out.u32(string_offsets_.at(n));
        out.u32(0);
    }

    // imports: {u32 str_offset, u16 class_name, u16 flags}
    for (const auto& imp : imports_) {
        out.u32(string_offsets_.at(imp.path));
        out.u16(name(imp.class_name));
        out.u16(0);
    }

    // chunks: {u16 class, u16 flags, u32 parent, u32 data_size, u32 data_offset,
    //          u32 template, u32 crc}
    for (size_t i = 0; i < chunks_count; ++i) {
        out.u16(name(chunk_class_[i]));
        out.u16(0);
        out.u32(0);
        out.u32(static_cast<uint32_t>(bodies_[i].size()));
        out.u32(body_offsets[i]);
        out.u32(0);
        out.u32(0);
    }

    for (const auto& body : bodies_) out.raw(body.bytes.data(), body.size());
    return out.bytes;
}

// --- archive -----------------------------------------------------------------

namespace {

// Reflected CRC-64, poly 0xC96C5795D7870F42, init and xorout all-ones -- the
// same algorithm the real packer uses (WolvenKit.Core/CRC/CRC64Algo.cs: the
// table is generated from this poly, and Compute() defaults crc to ~0 and
// returns ~crc).
//
// RedFS never validates this field; it only mixes it into the depot
// fingerprint as an opaque change detector. Matching the real polynomial is
// therefore not load-bearing -- it just keeps synthesized archives honest.
uint64_t crc64(const uint8_t* data, size_t len) {
    static const auto table = [] {
        std::array<uint64_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint64_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (c >> 1) ^ 0xC96C5795D7870F42ull : c >> 1;
            t[i] = c;
        }
        return t;
    }();

    uint64_t crc = ~0ull;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ table[static_cast<uint8_t>(crc ^ data[i])];
    return ~crc;
}

// Stand-in for the per-entry SHA-1. It must be derived from CONTENT, not from
// the entry index: the whole point of the index CRC is to notice an archive
// whose files were replaced in place, and that only works if replacing a
// file's bytes changes the bytes of its index entry. An index-derived
// placeholder makes the CRC blind to exactly the case it exists to catch.
void content_digest(const std::vector<std::vector<uint8_t>>& segments, uint8_t out[20]) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const auto& s : segments)
        for (uint8_t b : s) h = (h ^ b) * 0x00000100000001B3ull;
    for (int i = 0; i < 20; ++i) {
        out[i] = static_cast<uint8_t>(h);
        h = (h ^ static_cast<uint8_t>(i)) * 0x00000100000001B3ull;
    }
}

}  // namespace

std::vector<uint8_t> ArchiveBuilder::build() const {
    constexpr size_t kHeaderEnd = 0xAC;  // Header.EXTENDED_SIZE

    Buf out;
    out.zeros(kHeaderEnd);  // header patched at the end

    // Segment payloads, stored raw so no Oodle is needed.
    struct Seg {
        uint64_t offset;
        uint32_t zsize, size;
    };
    std::vector<Seg> segments;
    std::vector<std::pair<uint32_t, uint32_t>> file_ranges;  // [start, end) per file

    for (const auto& f : files_) {
        const uint32_t start = static_cast<uint32_t>(segments.size());
        for (const auto& data : f.segments) {
            segments.push_back(Seg{static_cast<uint64_t>(out.size()),
                                   static_cast<uint32_t>(data.size()),
                                   static_cast<uint32_t>(data.size())});
            out.raw(data.data(), data.size());
            while (out.size() % 8) out.u8(0);  // keep offsets tidy
        }
        file_ranges.push_back({start, static_cast<uint32_t>(segments.size())});
    }

    const uint64_t index_position = out.size();

    // index header (28 bytes)
    out.u32(8);   // file_table_offset
    out.u32(0);   // file_table_size
    out.u64(0);   // crc
    out.u32(static_cast<uint32_t>(files_.size()));
    out.u32(static_cast<uint32_t>(segments.size()));
    out.u32(0);   // dependency_count

    // file entries (56 bytes each)
    for (size_t i = 0; i < files_.size(); ++i) {
        out.u64(files_[i].hash);
        out.i64(0);  // timestamp
        out.u32(0);  // num_inline_buffer_segments
        out.u32(file_ranges[i].first);
        out.u32(file_ranges[i].second);
        out.u32(0);  // deps start
        out.u32(0);  // deps end
        uint8_t digest[20];
        content_digest(files_[i].segments, digest);
        for (int b = 0; b < 20; ++b) out.u8(digest[b]);
    }

    // segments (16 bytes each)
    for (const auto& s : segments) {
        out.u64(s.offset);
        out.u32(s.zsize);
        out.u32(s.size);
    }

    const uint32_t index_size = static_cast<uint32_t>(out.size() - index_position);

    // The real packer CRCs the index BODY -- the three counts, every file entry,
    // every segment descriptor, every dependency -- and stores it in the header
    // field at index_position + 8 (ArchiveWriter.WriteIndex). Mirror that, so a
    // rebuild with different file content yields a different CRC even when the
    // entry count, segment count and index length are all unchanged.
    {
        const size_t body = static_cast<size_t>(index_position) + 16;
        const uint64_t crc = crc64(out.bytes.data() + body, out.bytes.size() - body);
        std::memcpy(out.bytes.data() + static_cast<size_t>(index_position) + 8, &crc, 8);
    }

    // patch the header now that positions are known
    Buf head;
    head.u32(0x52414452);  // 'RDAR'
    head.u32(12);          // version
    head.u64(index_position);
    head.u32(index_size);
    head.u64(0);  // debug position
    head.u32(0);  // debug size
    head.u64(out.size());
    head.u32(0);  // custom_data_length
    std::memcpy(out.bytes.data(), head.bytes.data(), head.size());

    return out.bytes;
}

bool ArchiveBuilder::write(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return n == bytes.size();
}

// --- higher-level fixtures ---------------------------------------------------

std::vector<uint8_t> make_texture_cr2w(uint32_t width, uint32_t height, uint32_t mips,
                                       const char* compression, const char* raw_format,
                                       bool gamma) {
    Cr2wBuilder b;

    // chunk 0: CBitmapTexture, with setup and a handle to the blob
    b.begin_chunk("CBitmapTexture");
    b.prop_u32("width", width);
    b.prop_u32("height", height);
    b.begin_struct("setup", "STextureGroupSetup");
    {
        const uint16_t comp = b.name(compression);
        b.prop_in("compression", "ETextureCompression", &comp, 2);
        const uint16_t rf = b.name(raw_format);
        b.prop_in("rawFormat", "ETextureRawFormat", &rf, 2);
        const uint8_t g = gamma ? 1 : 0;
        b.prop_in("isGamma", "Bool", &g, 1);
    }
    b.end_struct();
    b.begin_struct("renderTextureResource", "rendRenderTextureResource");
    {
        const int32_t handle = 2;  // chunk 1, +1
        b.prop_in("renderResourceBlobPC", "handle:rendIRenderTextureBlob", &handle, 4);
    }
    b.end_struct();
    b.end_chunk();

    // chunk 1: rendRenderTextureBlobPC
    b.begin_chunk("rendRenderTextureBlobPC");
    b.begin_struct("header", "rendRenderTextureBlobHeader");
    {
        b.prop_in_u32("version", 2);
        b.begin_struct("sizeInfo", "rendRenderTextureBlobSizeInfo");
        b.prop_in_u16("width", static_cast<uint16_t>(width));
        b.prop_in_u16("height", static_cast<uint16_t>(height));
        b.end_struct();
        b.begin_struct("textureInfo", "rendRenderTextureBlobTextureInfo");
        {
            const uint16_t type = b.name("TEXTYPE_2D");
            b.prop_in("type", "GpuWrapApieTextureType", &type, 2);
            b.prop_in_u16("sliceCount", 1);
            // Stock cooks store mipCount as Uint8, but the property's type name
            // travels in the file, so a hostile archive can declare Uint32 and
            // any value that fits. Widen automatically so a test can express
            // that without every caller caring.
            if (mips > 0xFF) {
                b.prop_in_u32("mipCount", mips);
            } else {
                const uint8_t mc = static_cast<uint8_t>(mips);
                b.prop_in("mipCount", "Uint8", &mc, 1);
            }
        }
        b.end_struct();
    }
    b.end_struct();
    b.prop_deferred_buffer("textureData", 0);
    b.end_chunk();

    return b.build();
}

std::vector<uint8_t> make_mesh_cr2w(uint32_t chunk_count, uint32_t verts_per_chunk,
                                    float quant_scale, float quant_offset) {
    Cr2wBuilder b;

    // chunk 0: CMesh
    b.begin_chunk("CMesh");
    b.begin_struct("boundingBox", "Box");
    {
        b.begin_struct("Min", "Vector4");
        b.prop_in_f32("X", -quant_scale + quant_offset);
        b.prop_in_f32("Y", -quant_scale + quant_offset);
        b.prop_in_f32("Z", -quant_scale + quant_offset);
        b.end_struct();
        b.begin_struct("Max", "Vector4");
        b.prop_in_f32("X", quant_scale + quant_offset);
        b.prop_in_f32("Y", quant_scale + quant_offset);
        b.prop_in_f32("Z", quant_scale + quant_offset);
        b.end_struct();
    }
    b.end_struct();
    b.prop_handle("renderResourceBlob", "handle:IRenderResourceBlob", 1);
    {
        // appearances: one handle, to chunk 2
        const int32_t app_handle = 3;  // chunk 2, +1
        b.prop_array("appearances", "handle:meshMeshAppearance", 1, &app_handle, 4);
    }
    b.end_chunk();

    // chunk 1: rendRenderMeshBlob
    b.begin_chunk("rendRenderMeshBlob");
    b.begin_struct("header", "rendRenderMeshBlobHeader");
    {
        b.prop_in_u32("version", 20);
        b.begin_struct("quantizationScale", "Vector4");
        b.prop_in_f32("X", quant_scale);
        b.prop_in_f32("Y", quant_scale);
        b.prop_in_f32("Z", quant_scale);
        b.end_struct();
        b.begin_struct("quantizationOffset", "Vector4");
        b.prop_in_f32("X", quant_offset);
        b.prop_in_f32("Y", quant_offset);
        b.prop_in_f32("Z", quant_offset);
        b.end_struct();
        b.prop_in_u32("vertexBufferSize", chunk_count * verts_per_chunk * 8);
        b.prop_in_u32("indexBufferSize", 0);
        b.prop_in_u32("indexBufferOffset", chunk_count * verts_per_chunk * 8);

        // renderChunkInfos: an array of rendChunk structs, variable width, so it
        // is assembled by hand rather than through prop_array.
        Buf arr;
        arr.u32(chunk_count);
        for (uint32_t c = 0; c < chunk_count; ++c) {
            Buf body;
            body.u8(0);
            // chunkVertices
            {
                Buf cv;
                cv.u8(0);
                {  // vertexLayout
                    Buf vl;
                    vl.u8(0);
                    {  // slotStrides: static:8,Uint8 -- count then 8 bytes
                        Buf sv;
                        sv.u32(8);
                        sv.u8(8);  // slot 0 stride
                        for (int k = 1; k < 8; ++k) sv.u8(0);
                        vl.u16(b.name("slotStrides"));
                        vl.u16(b.name("static:8,Uint8"));
                        vl.u32(static_cast<uint32_t>(sv.size()) + 4);
                        vl.raw(sv.bytes.data(), sv.size());
                    }
                    vl.u16(0);  // terminate vertexLayout
                    cv.u16(b.name("vertexLayout"));
                    cv.u16(b.name("GpuWrapApiVertexLayoutDesc"));
                    cv.u32(static_cast<uint32_t>(vl.size()) + 4);
                    cv.raw(vl.bytes.data(), vl.size());
                }
                {  // byteOffsets: static:5,Uint32
                    Buf bo;
                    bo.u32(5);
                    bo.u32(c * verts_per_chunk * 8);  // this chunk's position stream
                    for (int k = 1; k < 5; ++k) bo.u32(0);
                    cv.u16(b.name("byteOffsets"));
                    cv.u16(b.name("static:5,Uint32"));
                    cv.u32(static_cast<uint32_t>(bo.size()) + 4);
                    cv.raw(bo.bytes.data(), bo.size());
                }
                cv.u16(0);  // terminate chunkVertices
                body.u16(b.name("chunkVertices"));
                body.u16(b.name("rendVertexBufferChunk"));
                body.u32(static_cast<uint32_t>(cv.size()) + 4);
                body.raw(cv.bytes.data(), cv.size());
            }
            {
                const uint16_t nv = static_cast<uint16_t>(verts_per_chunk);
                body.u16(b.name("numVertices"));
                body.u16(b.name("Uint16"));
                body.u32(2 + 4);
                body.raw(&nv, 2);
            }
            {
                const uint32_t ni = verts_per_chunk * 3;
                body.u16(b.name("numIndices"));
                body.u16(b.name("Uint32"));
                body.u32(4 + 4);
                body.raw(&ni, 4);
            }
            {
                const uint8_t lod = 1;
                body.u16(b.name("lodMask"));
                body.u16(b.name("Uint8"));
                body.u32(1 + 4);
                body.raw(&lod, 1);
            }
            body.u16(0);  // terminate this rendChunk
            arr.raw(body.bytes.data(), body.size());
        }
        b.prop_in("renderChunkInfos", "array:rendChunk", arr.bytes.data(), arr.size());
    }
    b.end_struct();
    b.prop_data_buffer("renderBuffer", 0);
    b.end_chunk();

    // chunk 2: meshMeshAppearance
    b.begin_chunk("meshMeshAppearance");
    b.prop_cname("name", "default");
    {
        std::vector<std::string> mats;
        for (uint32_t c = 0; c < chunk_count; ++c) mats.push_back("mat_" + std::to_string(c));
        b.prop_array_cname("chunkMaterials", mats);
    }
    b.end_chunk();

    return b.build();
}

std::vector<uint8_t> make_mesh_geometry(uint32_t chunk_count, uint32_t verts_per_chunk) {
    // Stride 8: three int16 positions plus two padding bytes, matching the real
    // layout observed on shipping meshes.
    Buf out;
    for (uint32_t c = 0; c < chunk_count; ++c) {
        for (uint32_t v = 0; v < verts_per_chunk; ++v) {
            // Spread each chunk over a distinct, predictable band so the tests can
            // assert exact bounds. Chunk c occupies quantized z in
            // [c * step, (c+1) * step).
            const int16_t span = 32767;
            const int16_t lo = static_cast<int16_t>(-span + (2 * span / chunk_count) * c);
            const int16_t hi = static_cast<int16_t>(lo + (2 * span / chunk_count) - 1);
            const int16_t z = (v == 0) ? lo : hi;
            out.u16(static_cast<uint16_t>(static_cast<int16_t>(v == 0 ? -span : span)));  // x
            out.u16(static_cast<uint16_t>(static_cast<int16_t>(0)));                      // y
            out.u16(static_cast<uint16_t>(z));                                            // z
            out.u16(0);                                                                   // pad
        }
    }
    return out.bytes;
}

}  // namespace fixture
