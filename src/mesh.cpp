// Mesh chunk decoding: per-submesh LOD, material and bounding box.
//
// The bounding box is the reason this file exists, and the format does not store
// one. rendChunk carries vertex/index counts, a lodMask and stream offsets, but
// no bounds -- only CMesh has a box, and it covers the whole mesh. So the box
// per chunk has to be computed from the geometry:
//
//   positions live at renderBuffer[ chunkVertices.byteOffsets[0]
//                                   + i * vertexLayout.slotStrides[0] ]
//   as three int16s, dequantized by
//       p = i16 / 32767 * header.quantizationScale + header.quantizationOffset
//
// That costs one Kraken decompress of the geometry buffer per mesh, which is why
// cache.cpp exists.
//
// Boxes come out in mesh-local game space (Z up). No axis swap: these are meant
// to be compared against component transforms, not fed to a glTF writer.

#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace redfs {
namespace {

// Reads element `want` of a fixed-width array property as an unsigned value.
struct ElemPick {
    uint32_t want;
    uint64_t value;
    bool found;
};

int pick_elem(uint32_t index, const redfs_value* v, void* user) {
    auto* p = static_cast<ElemPick*>(user);
    if (index != p->want) return 1;
    if (v->kind == REDFS_KIND_UINT || v->kind == REDFS_KIND_BOOL)
        p->value = v->as.u;
    else if (v->kind == REDFS_KIND_INT)
        p->value = static_cast<uint64_t>(v->as.i);
    p->found = true;
    return 0;
}

uint64_t array_uint_at(const redfs_cr2w* f, const redfs_value* arr, uint32_t index,
                       uint64_t fallback) {
    if (!arr || arr->kind != REDFS_KIND_ARRAY) return fallback;
    ElemPick p{index, fallback, false};
    cr2w_walk_array(f, arr, pick_elem, &p);
    return p.found ? p.value : fallback;
}

uint64_t struct_uint(const redfs_cr2w* f, const redfs_value* parent, const char* path,
                     uint64_t fallback) {
    redfs_value v{};
    if (cr2w_get_in(f, parent, path, &v) != REDFS_OK) return fallback;
    if (v.kind == REDFS_KIND_UINT || v.kind == REDFS_KIND_BOOL) return v.as.u;
    if (v.kind == REDFS_KIND_INT) return static_cast<uint64_t>(v.as.i);
    return fallback;
}

float chunk_float(const redfs_cr2w* f, uint32_t chunk, const char* path, float fallback) {
    redfs_value v{};
    if (cr2w_find(f, chunk, path, &v) != REDFS_OK) return fallback;
    return v.kind == REDFS_KIND_FLOAT ? static_cast<float>(v.as.f) : fallback;
}

// lodMask is a bitfield of the detail levels a chunk appears in; report the
// lowest, 1-based, which is the level artists think of as "the" LOD.
uint32_t lowest_lod(uint32_t mask) {
    if (mask == 0) return 1;
    uint32_t lod = 1;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        ++lod;
    }
    return lod;
}

// Collects the per-chunk facts we can read straight out of the CR2W. The
// geometry pass fills in the boxes afterwards.
struct ChunkGather {
    const redfs_cr2w* f;
    std::vector<MeshChunk>* chunks;
};

int gather_chunk(uint32_t index, const redfs_value* v, void* user) {
    auto* g = static_cast<ChunkGather*>(user);
    if (v->kind != REDFS_KIND_STRUCT) return 1;

    MeshChunk c{};
    c.index = index;
    c.vertex_count = static_cast<uint32_t>(struct_uint(g->f, v, "numVertices", 0));
    c.index_count = static_cast<uint32_t>(struct_uint(g->f, v, "numIndices", 0));
    c.lod_mask = static_cast<uint32_t>(struct_uint(g->f, v, "lodMask", 1));
    c.lod = lowest_lod(c.lod_mask);

    redfs_value offsets{};
    if (cr2w_get_in(g->f, v, "chunkVertices.byteOffsets", &offsets) == REDFS_OK)
        c.position_offset = static_cast<uint32_t>(array_uint_at(g->f, &offsets, 0, 0));

    redfs_value strides{};
    if (cr2w_get_in(g->f, v, "chunkVertices.vertexLayout.slotStrides", &strides) == REDFS_OK)
        c.position_stride = static_cast<uint32_t>(array_uint_at(g->f, &strides, 0, 0));

    g->chunks->push_back(c);
    return 1;
}

// Appearance gathering: each element is a handle to a meshMeshAppearance chunk.
struct AppearanceGather {
    const redfs_cr2w* f;
    Mesh* mesh;
};

struct MaterialGather {
    std::vector<std::string>* names;
};

int gather_material(uint32_t, const redfs_value* v, void* user) {
    auto* g = static_cast<MaterialGather*>(user);
    g->names->emplace_back(v->kind == REDFS_KIND_NAME && v->as.s ? v->as.s : "");
    return 1;
}

int gather_appearance(uint32_t, const redfs_value* v, void* user) {
    auto* g = static_cast<AppearanceGather*>(user);
    if (v->kind != REDFS_KIND_HANDLE || v->as.chunk < 0) return 1;

    MeshAppearance app;
    redfs_value name{};
    if (cr2w_find(g->f, static_cast<uint32_t>(v->as.chunk), "name", &name) == REDFS_OK &&
        name.kind == REDFS_KIND_NAME && name.as.s)
        app.name = name.as.s;

    redfs_value materials{};
    if (cr2w_find(g->f, static_cast<uint32_t>(v->as.chunk), "chunkMaterials", &materials) ==
            REDFS_OK &&
        materials.kind == REDFS_KIND_ARRAY) {
        MaterialGather mg{&app.chunk_materials};
        cr2w_walk_array(g->f, &materials, gather_material, &mg);
    }

    g->mesh->appearances.push_back(std::move(app));
    return 1;
}

// Sweeps one chunk's position stream and reduces it to a box.
void compute_bounds(MeshChunk& c, const uint8_t* geometry, uint64_t geometry_size,
                    const float scale[3], const float offset[3]) {
    c.bbox_min[0] = c.bbox_min[1] = c.bbox_min[2] = 0.f;
    c.bbox_max[0] = c.bbox_max[1] = c.bbox_max[2] = 0.f;
    if (c.vertex_count == 0 || c.position_stride < 6) return;

    const uint64_t span =
        static_cast<uint64_t>(c.position_offset) + static_cast<uint64_t>(c.vertex_count - 1) *
                                                       c.position_stride + 6;
    if (span > geometry_size) return;  // truncated or streamed-out geometry

    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                   -std::numeric_limits<float>::max()};

    const uint8_t* p = geometry + c.position_offset;
    for (uint32_t i = 0; i < c.vertex_count; ++i, p += c.position_stride) {
        for (int axis = 0; axis < 3; ++axis) {
            const int16_t q = static_cast<int16_t>(rd16(p + axis * 2));
            const float value = (q / 32767.0f) * scale[axis] + offset[axis];
            lo[axis] = (std::min)(lo[axis], value);
            hi[axis] = (std::max)(hi[axis], value);
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        c.bbox_min[axis] = lo[axis];
        c.bbox_max[axis] = hi[axis];
    }
    c.bounds_valid = true;
}

}  // namespace

redfs_status mesh_build(const redfs_depot* depot, uint64_t hash, Mesh* out) {
    // 1. the resource itself
    std::vector<uint8_t> storage;
    uint64_t size = 0;
    redfs_status st = read_part(depot, hash, REDFS_PART_MAIN, nullptr, 0, &size);
    if (st != REDFS_OK) return st;
    storage.resize(static_cast<size_t>(size));
    st = read_part(depot, hash, REDFS_PART_MAIN, storage.data(), size, &size);
    if (st != REDFS_OK) return st;

    redfs_cr2w f;
    st = cr2w_parse(storage.data(), size, &f);
    if (st != REDFS_OK) return st;

    if (f.chunks.empty()) return fail(REDFS_E_CORRUPT, "CR2W has no chunks");
    const char* root = f.name(f.chunks[0].class_name);
    if (std::strcmp(root, "CMesh") != 0)
        return fail(REDFS_E_UNSUPPORTED, "root chunk is %s, not CMesh", root);

    out->hash = hash;

    // 2. the render blob, reached through the mesh's own handle
    int32_t blob = -1;
    redfs_value handle{};
    if (cr2w_find(&f, 0, "renderResourceBlob", &handle) == REDFS_OK &&
        handle.kind == REDFS_KIND_HANDLE && handle.as.chunk >= 0)
        blob = handle.as.chunk;
    if (blob < 0) {
        for (uint32_t i = 0; i < f.chunks.size(); ++i)
            if (std::strcmp(f.name(f.chunks[i].class_name), "rendRenderMeshBlob") == 0) {
                blob = static_cast<int32_t>(i);
                break;
            }
    }
    if (blob < 0) return fail(REDFS_E_UNSUPPORTED, "CMesh has no rendRenderMeshBlob");
    const uint32_t blob_chunk = static_cast<uint32_t>(blob);

    // 3. whole-mesh bounds, straight from CMesh
    out->bbox_min[0] = chunk_float(&f, 0, "boundingBox.Min.X", 0.f);
    out->bbox_min[1] = chunk_float(&f, 0, "boundingBox.Min.Y", 0.f);
    out->bbox_min[2] = chunk_float(&f, 0, "boundingBox.Min.Z", 0.f);
    out->bbox_max[0] = chunk_float(&f, 0, "boundingBox.Max.X", 0.f);
    out->bbox_max[1] = chunk_float(&f, 0, "boundingBox.Max.Y", 0.f);
    out->bbox_max[2] = chunk_float(&f, 0, "boundingBox.Max.Z", 0.f);

    // 4. quantization -- positions are int16 in a box the header describes
    const float scale[3] = {chunk_float(&f, blob_chunk, "header.quantizationScale.X", 1.f),
                            chunk_float(&f, blob_chunk, "header.quantizationScale.Y", 1.f),
                            chunk_float(&f, blob_chunk, "header.quantizationScale.Z", 1.f)};
    const float offset[3] = {chunk_float(&f, blob_chunk, "header.quantizationOffset.X", 0.f),
                             chunk_float(&f, blob_chunk, "header.quantizationOffset.Y", 0.f),
                             chunk_float(&f, blob_chunk, "header.quantizationOffset.Z", 0.f)};

    // 5. per-chunk facts
    redfs_value chunk_infos{};
    if (cr2w_find(&f, blob_chunk, "header.renderChunkInfos", &chunk_infos) != REDFS_OK ||
        chunk_infos.kind != REDFS_KIND_ARRAY)
        return fail(REDFS_E_UNSUPPORTED, "rendRenderMeshBlob has no renderChunkInfos");

    ChunkGather cg{&f, &out->chunks};
    cr2w_walk_array(&f, &chunk_infos, gather_chunk, &cg);

    redfs_value lods{};
    out->lod_count = 1;
    if (cr2w_find(&f, blob_chunk, "header.renderLODs", &lods) == REDFS_OK &&
        lods.kind == REDFS_KIND_ARRAY)
        out->lod_count = (std::max)(1u, static_cast<uint32_t>(lods.as.u));
    for (const auto& c : out->chunks) out->lod_count = (std::max)(out->lod_count, c.lod);

    // 6. appearances and their per-chunk materials
    redfs_value appearances{};
    if (cr2w_find(&f, 0, "appearances", &appearances) == REDFS_OK &&
        appearances.kind == REDFS_KIND_ARRAY) {
        AppearanceGather ag{&f, out};
        cr2w_walk_array(&f, &appearances, gather_appearance, &ag);
    }

    // 7. the expensive part: decompress the geometry and sweep each chunk
    redfs_value render_buffer{};
    uint32_t buffer_index = 0;
    if (cr2w_find(&f, blob_chunk, "renderBuffer", &render_buffer) == REDFS_OK &&
        render_buffer.kind == REDFS_KIND_BUFFER)
        buffer_index = render_buffer.as.buffer;

    uint64_t geometry_size = 0;
    st = read_part(depot, hash, buffer_index, nullptr, 0, &geometry_size);
    if (st != REDFS_OK) {
        // No geometry available: the header facts are still valid, boxes are not.
        log("mesh 0x%016llX: geometry buffer unreadable, bounds omitted",
            static_cast<unsigned long long>(hash));
        return REDFS_OK;
    }

    std::vector<uint8_t> geometry(static_cast<size_t>(geometry_size));
    st = read_part(depot, hash, buffer_index, geometry.data(), geometry_size, &geometry_size);
    if (st != REDFS_OK) return st;

    for (auto& c : out->chunks) compute_bounds(c, geometry.data(), geometry_size, scale, offset);
    return REDFS_OK;
}

}  // namespace redfs
