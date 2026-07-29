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
//
// Two independent bounds are needed here, and neither is redundant.
//
// Nothing stops N appearance handles from all resolving to the SAME chunk, and
// each one re-walks that chunk's chunkMaterials in full. Handles cost 4 bytes of
// array payload and CNames cost 2, so a ~512 KB crafted .mesh buys 65536 x
// 131072 string constructions -- billions of allocations that are retained, not
// transient, because every appearance is moved into the mesh. Worse, a variant
// tuned to stop just short of bad_alloc gets serialized into the on-disk cache
// and replayed on every subsequent load.
//
// `chunk_materials` is documented as parallel to the chunk list, and
// redfs_mesh_chunk_material refuses any index past chunks.size() anyway, so
// truncating there is free. But that alone is not enough: chunks.size() comes
// from the same attacker-controlled file (renderChunkInfos elements cost 3
// bytes), so the product stays quadratic. The aggregate budget is what actually
// bounds the work.
constexpr size_t kMaterialBudget = 1u << 20;  // total strings across all appearances

struct AppearanceGather {
    const redfs_cr2w* f;
    Mesh* mesh;
    size_t chunk_count = 0;   // upper bound per appearance
    size_t budget = kMaterialBudget;
    bool warned = false;
};

struct MaterialGather {
    std::vector<std::string>* names;
    size_t limit;  // stop accepting past this many
};

int gather_material(uint32_t, const redfs_value* v, void* user) {
    auto* g = static_cast<MaterialGather*>(user);
    if (g->names->size() >= g->limit) return 0;  // stop the walk
    g->names->emplace_back(v->kind == REDFS_KIND_NAME && v->as.s ? v->as.s : "");
    return 1;
}

int gather_appearance(uint32_t, const redfs_value* v, void* user) {
    auto* g = static_cast<AppearanceGather*>(user);
    if (v->kind != REDFS_KIND_HANDLE || v->as.chunk < 0) return 1;

    if (g->budget == 0) {
        if (!g->warned) {
            log("mesh appearances exceed the material budget of %zu names; truncating",
                kMaterialBudget);
            g->warned = true;
        }
        return 0;
    }

    MeshAppearance app;
    redfs_value name{};
    if (cr2w_find(g->f, static_cast<uint32_t>(v->as.chunk), "name", &name) == REDFS_OK &&
        name.kind == REDFS_KIND_NAME && name.as.s)
        app.name = name.as.s;

    redfs_value materials{};
    if (cr2w_find(g->f, static_cast<uint32_t>(v->as.chunk), "chunkMaterials", &materials) ==
            REDFS_OK &&
        materials.kind == REDFS_KIND_ARRAY) {
        const size_t cap = (std::min)(g->chunk_count, g->budget);
        MaterialGather mg{&app.chunk_materials, cap};
        cr2w_walk_array(g->f, &materials, gather_material, &mg);
        g->budget -= (std::min)(g->budget, app.chunk_materials.size());
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

    // A NaN or infinite quantization term makes every dequantized value NaN, and
    // both std::min and std::max return their first argument when the comparison
    // is false -- so the sentinels survive untouched and the chunk would publish
    // an inverted FLT_MAX..-FLT_MAX box marked valid, then cache it. Infinity is
    // enough on its own: 0/32767 * inf is NaN.
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(scale[axis]) || !std::isfinite(offset[axis])) return;

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
    // The input guard above is not sufficient, because finite inputs can still
    // produce a non-finite result: scale = offset = FLT_MAX are both finite, and
    // a vertex at q = 32767 dequantizes to FLT_MAX + FLT_MAX, which overflows to
    // +inf. min(FLT_MAX, +inf) then returns FLT_MAX and leaves `lo` sitting on
    // its sentinel -- reproducing the exact inverted box the input guard was
    // added to prevent, and caching it. Validate what was actually computed.
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(lo[axis]) || !std::isfinite(hi[axis])) return;

    for (int axis = 0; axis < 3; ++axis) {
        c.bbox_min[axis] = lo[axis];
        c.bbox_max[axis] = hi[axis];
    }
    c.bounds_valid = true;
}

}  // namespace

// Builds the public view once, before the object is shared. Doing this lazily on
// open would mutate an object the cache has already handed to other callers.
void MeshData::finalize() {
    public_chunks.clear();
    public_chunks.reserve(chunks.size());
    for (const auto& c : chunks) {
        redfs_mesh_chunk p{};
        p.index = c.index;
        p.lod_mask = c.lod_mask;
        p.lod = c.lod;
        p.vertex_count = c.vertex_count;
        p.index_count = c.index_count;
        for (int i = 0; i < 3; ++i) {
            p.bbox_min[i] = c.bbox_min[i];
            p.bbox_max[i] = c.bbox_max[i];
        }
        p.bounds_valid = c.bounds_valid ? 1u : 0u;
        public_chunks.push_back(p);
    }
}

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
    //
    // Unlike the per-chunk boxes, this one is stored rather than computed, so it
    // is archive content and can be anything -- including NaN or infinity, which
    // makes every comparison a caller writes against it false and silently turns
    // a chunk filter into a no-op. Clamp non-finite to 0 here rather than inside
    // chunk_float: compute_bounds below feeds the quantization terms through the
    // same helper and RELIES on seeing a non-finite value so it can skip the
    // sweep.
    auto finite_or_zero = [](float v) { return std::isfinite(v) ? v : 0.f; };
    out->bbox_min[0] = finite_or_zero(chunk_float(&f, 0, "boundingBox.Min.X", 0.f));
    out->bbox_min[1] = finite_or_zero(chunk_float(&f, 0, "boundingBox.Min.Y", 0.f));
    out->bbox_min[2] = finite_or_zero(chunk_float(&f, 0, "boundingBox.Min.Z", 0.f));
    out->bbox_max[0] = finite_or_zero(chunk_float(&f, 0, "boundingBox.Max.X", 0.f));
    out->bbox_max[1] = finite_or_zero(chunk_float(&f, 0, "boundingBox.Max.Y", 0.f));
    out->bbox_max[2] = finite_or_zero(chunk_float(&f, 0, "boundingBox.Max.Z", 0.f));

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
        lods.kind == REDFS_KIND_ARRAY) {
        // Walked, not declared. The leading u32 is whatever the file claims, and
        // a truncated array can claim 4 billion -- which redfs_mesh_lod_count
        // would then hand back verbatim as a loop bound.
        uint32_t n = 0;
        cr2w_walk_array(
            &f, &lods,
            [](uint32_t, const redfs_value*, void* user) {
                ++*static_cast<uint32_t*>(user);
                return 1;
            },
            &n);
        out->lod_count = (std::max)(1u, n);
    }
    for (const auto& c : out->chunks) out->lod_count = (std::max)(out->lod_count, c.lod);

    // 6. appearances and their per-chunk materials
    redfs_value appearances{};
    if (cr2w_find(&f, 0, "appearances", &appearances) == REDFS_OK &&
        appearances.kind == REDFS_KIND_ARRAY) {
        AppearanceGather ag{&f, out, out->chunks.size()};
        cr2w_walk_array(&f, &appearances, gather_appearance, &ag);
    }

    // 7. the expensive part: decompress the geometry and sweep each chunk
    redfs_value render_buffer{};
    uint32_t buffer_index = 0;
    if (cr2w_find(&f, blob_chunk, "renderBuffer", &render_buffer) == REDFS_OK &&
        render_buffer.kind == REDFS_KIND_BUFFER)
        buffer_index = render_buffer.as.buffer;
    else
        // Attached buffer 0 is the usual answer, but when it is the wrong one the
        // sweep below produces a confident box from unrelated bytes -- and caches
        // it. Leave a trace so that outcome is diagnosable.
        log("mesh 0x%016llX: no renderBuffer property; assuming attached buffer 0",
            static_cast<unsigned long long>(hash));

    uint64_t geometry_size = 0;
    st = read_part(depot, hash, buffer_index, nullptr, 0, &geometry_size);
    if (st != REDFS_OK) {
        // No geometry available: the header facts are still valid, boxes are not.
        log("mesh 0x%016llX: geometry buffer unreadable, bounds omitted",
            static_cast<unsigned long long>(hash));
        out->finalize();
        return REDFS_OK;
    }

    std::vector<uint8_t> geometry(static_cast<size_t>(geometry_size));
    st = read_part(depot, hash, buffer_index, geometry.data(), geometry_size, &geometry_size);
    if (st != REDFS_OK) return st;

    for (auto& c : out->chunks) compute_bounds(c, geometry.data(), geometry_size, scale, offset);

    // Last step before the object can be shared: everything above mutates it.
    out->finalize();
    return REDFS_OK;
}

}  // namespace redfs
