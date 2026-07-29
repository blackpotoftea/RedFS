// redfs_verify -- checks RedFS's texture output two independent ways.
//
// RedFS builds DDS headers from RED4 texture metadata. Believing our own reading
// of the spec is not evidence, so each texture is checked twice:
//
//  1. Header, by a third party. Feed the DDS to DirectXTex (the texconv.dll that
//     ships with WolvenKit) and compare what DirectXTex parses out of it against
//     what RedFS claimed. If our header were malformed the two would disagree.
//
//  2. Payload, by arithmetic. Compute how many bytes a mip chain of that exact
//     format and extent must occupy, and compare against the size of the buffer
//     actually stored in the archive. Those two numbers come from completely
//     different places -- one from the CR2W metadata we decoded, the other from
//     the archive segment table -- so an exact match across hundreds of textures
//     in many formats means the format mapping is right. A wrong DXGI format or
//     mip count cannot produce a matching size by accident.
//
//   redfs_verify <path-to-texconv.dll> <game-dir> [count]

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <objbase.h>

#include "redfs.h"

namespace {

// Mirrors DirectX::TexMetadata as texconv.dll exposes it.
struct TexMetadata {
    int64_t width;
    int64_t height;
    int64_t depth;
    int64_t arraySize;
    int64_t mipLevels;
    int32_t miscFlags;
    int32_t miscFlags2;
    int32_t format;     // DXGI_FORMAT
    int32_t dimension;  // 2=1D, 3=2D, 4=3D
};

struct Blob {
    void* handle;
    int64_t length;
};

using GetMetadataFromDDSMemoryFn = TexMetadata(__stdcall*)(const uint8_t*, int32_t, int32_t);

GetMetadataFromDDSMemoryFn g_meta = nullptr;

bool load_texconv(const char* path) {
    HMODULE m = ::LoadLibraryA(path);
    if (!m) {
        std::fprintf(stderr, "cannot load %s (error %lu)\n", path, ::GetLastError());
        return false;
    }
    g_meta = reinterpret_cast<GetMetadataFromDDSMemoryFn>(
        reinterpret_cast<void*>(::GetProcAddress(m, "GetMetadataFromDDSMemory")));
    return g_meta != nullptr;
}

// --- independent mip-chain arithmetic ----------------------------------------
// Deliberately written from the DXGI format definitions rather than reused from
// the library, so it can disagree with RedFS.

struct FormatInfo {
    uint32_t dxgi;
    const char* name;
    uint32_t block;  // bytes per 4x4 block, or 0 when uncompressed
    uint32_t bpp;    // bits per pixel when uncompressed
};

const FormatInfo kFormats[] = {
    {2, "R32G32B32A32_FLOAT", 0, 128}, {10, "R16G16B16A16_FLOAT", 0, 64},
    {11, "R16G16B16A16_UNORM", 0, 64}, {28, "R8G8B8A8_UNORM", 0, 32},
    {29, "R8G8B8A8_UNORM_SRGB", 0, 32}, {49, "R8G8_UNORM", 0, 16},
    {54, "R16_FLOAT", 0, 16},          {61, "R8_UNORM", 0, 8},
    {65, "A8_UNORM", 0, 8},            {71, "BC1_UNORM", 8, 0},
    {72, "BC1_UNORM_SRGB", 8, 0},      {77, "BC3_UNORM", 16, 0},
    {78, "BC3_UNORM_SRGB", 16, 0},     {80, "BC4_UNORM", 8, 0},
    {83, "BC5_UNORM", 16, 0},          {95, "BC6H_UF16", 16, 0},
    {98, "BC7_UNORM", 16, 0},          {99, "BC7_UNORM_SRGB", 16, 0},
};

const FormatInfo* find_format(uint32_t dxgi) {
    for (const auto& f : kFormats)
        if (f.dxgi == dxgi) return &f;
    return nullptr;
}

// Bytes a full mip chain occupies, per D3D's surface-size rules.
uint64_t expected_bytes(const FormatInfo& f, uint32_t w, uint32_t h, uint32_t mips,
                        uint32_t slices, uint32_t depth) {
    uint64_t total = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = w >> m ? w >> m : 1;
        const uint32_t mh = h >> m ? h >> m : 1;
        const uint32_t md = depth >> m ? depth >> m : 1;
        uint64_t level;
        if (f.block) {
            const uint64_t bw = (mw + 3) / 4;
            const uint64_t bh = (mh + 3) / 4;
            level = bw * bh * f.block;
        } else {
            level = static_cast<uint64_t>(mw) * mh * f.bpp / 8;
        }
        total += level * md;
    }
    return total * (slices ? slices : 1);
}

struct Collector {
    std::vector<uint64_t> hashes;
    uint64_t seen = 0;
    uint64_t stride = 1;
    uint32_t want = 0;
};

int collect(const redfs_file_info* info, void* user) {
    auto* c = static_cast<Collector*>(user);
    if (c->seen++ % c->stride == 0 && c->hashes.size() < c->want) c->hashes.push_back(info->hash);
    return c->hashes.size() < c->want;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: redfs_verify <texconv.dll> <game-dir> [count]\n");
        return 2;
    }
    // ConvertFromDds saves PNG through WIC, which needs COM on this thread.
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (!load_texconv(argv[1])) return 1;
    std::printf("texconv loaded\n");
    std::fflush(stdout);

    const uint32_t want = argc >= 4 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 24;

    redfs_depot* d = nullptr;
    redfs_status st = redfs_depot_open(argv[2], REDFS_SCAN_ALL, &d);
    if (st != REDFS_OK) {
        std::fprintf(stderr, "depot_open: %s (%s)\n", redfs_status_string(st), redfs_last_error());
        return 1;
    }

    // Sweep the depot for real textures rather than relying on known paths.
    Collector c;
    c.want = 200000;
    c.stride = redfs_depot_file_count(d) / 120000 + 1;
    redfs_enumerate(d, collect, &c);

    uint32_t checked = 0, header_mismatch = 0, size_mismatch = 0, unknown_format = 0, verbose = 0;
    for (uint64_t h : c.hashes) {
        if (checked >= want) break;

        redfs_texture_desc t{};
        if (redfs_texture_desc_of(d, h, &t) != REDFS_OK) continue;  // not a texture

        redfs_blob dds{};
        if (redfs_texture_read_dds(d, h, &dds) != REDFS_OK) {
            std::printf("FAIL  0x%016" PRIX64 "  could not build DDS: %s\n", h, redfs_last_error());
            ++header_mismatch;
            ++checked;
            continue;
        }

        // check 1: DirectXTex agrees with us about what this image is
        //
        // arraySize and miscFlags are part of this. They were declared above and
        // never compared, and that gap is exactly how a cubemap encoding bug --
        // every emitted cube declaring six times the faces its payload held --
        // sat behind a clean "0 header mismatches" result. A surface count the
        // loader disagrees with is a header mismatch like any other.
        const TexMetadata m = g_meta(dds.data, static_cast<int32_t>(dds.size), 0);
        // On disk arraySize counts cubes; loaders multiply by 6 for a cubemap.
        // RedFS reports slice_count in faces, so compare in faces.
        const bool is_cube = (m.miscFlags & 0x4) != 0;  // TEX_MISC_TEXTURECUBE
        const int64_t faces = m.arraySize * (is_cube ? 6 : 1);
        const bool header_ok = m.width == static_cast<int64_t>(t.width) &&
                               m.height == static_cast<int64_t>(t.height) &&
                               m.mipLevels == static_cast<int64_t>(t.mip_count) &&
                               m.format == static_cast<int32_t>(t.dxgi_format) &&
                               faces == static_cast<int64_t>(t.slice_count) &&
                               is_cube == (t.is_cubemap != 0);
        if (!header_ok) {
            ++header_mismatch;
            std::printf("FAIL  0x%016" PRIX64 "  redfs %ux%u mips=%u dxgi=%u slices=%u cube=%u, "
                        "directxtex %lldx%lld mips=%lld dxgi=%d faces=%lld cube=%d\n",
                        h, t.width, t.height, t.mip_count, t.dxgi_format, t.slice_count,
                        t.is_cubemap, static_cast<long long>(m.width),
                        static_cast<long long>(m.height), static_cast<long long>(m.mipLevels),
                        m.format, static_cast<long long>(faces), is_cube ? 1 : 0);
        }

        // check 2: the stored payload is exactly the size that format demands
        const FormatInfo* fmt = find_format(t.dxgi_format);
        if (!fmt) {
            ++unknown_format;
            std::printf("SKIP  0x%016" PRIX64 "  dxgi %u not in the reference table\n", h,
                        t.dxgi_format);
        } else {
            const uint64_t want_bytes =
                expected_bytes(*fmt, t.width, t.height, t.mip_count, t.slice_count, t.depth);
            if (want_bytes != t.data_size) {
                ++size_mismatch;
                std::printf("FAIL  0x%016" PRIX64 "  %-20s %ux%u mips=%u slices=%u: archive holds "
                            "%" PRIu64 " bytes, %s needs %" PRIu64 "\n",
                            h, fmt->name, t.width, t.height, t.mip_count, t.slice_count,
                            t.data_size, fmt->name, want_bytes);
            } else if (verbose < 12) {
                std::printf("PASS  0x%016" PRIX64 "  %-20s %4ux%-4u mips=%-2u slices=%-2u  "
                            "%" PRIu64 " bytes exactly\n",
                            h, fmt->name, t.width, t.height, t.mip_count, t.slice_count,
                            t.data_size);
                ++verbose;
            }
        }

        redfs_blob_free(&dds);
        ++checked;
    }

    std::printf("\n%u textures checked\n", checked);
    std::printf("  %u header mismatches vs DirectXTex\n", header_mismatch);
    std::printf("  %u payload size mismatches\n", size_mismatch);
    std::printf("  %u skipped (format not in the reference table)\n", unknown_format);

    // --- meshes --------------------------------------------------------------
    //
    // Chunk bounds are computed by dequantizing vertex positions, so they need
    // an independent check too. CMesh stores its own whole-mesh box, written by
    // CDPR's cooker and never read by our code path -- so the union of the
    // chunk boxes must fall inside it. A wrong stride, a wrong quantization
    // scale or a misread offset all push vertices outside that box immediately.
    //
    // Containment rather than equality: the stored box legitimately includes
    // padding and covers morph/LOD range the resident chunks do not.

    uint32_t meshes = 0, escaped = 0, empty_bounds = 0;
    for (uint64_t h : c.hashes) {
        if (meshes >= want) break;

        redfs_mesh* m = nullptr;
        if (redfs_mesh_open(d, h, &m) != REDFS_OK) continue;  // not a mesh

        float lo[3], hi[3];
        redfs_mesh_bounds(m, lo, hi);
        const uint32_t n = redfs_mesh_chunk_count(m);

        float ulo[3] = {1e30f, 1e30f, 1e30f}, uhi[3] = {-1e30f, -1e30f, -1e30f};
        uint32_t with_bounds = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const redfs_mesh_chunk* ch = redfs_mesh_chunk_at(m, i);
            if (!ch) continue;
            // The API says so directly now. This used to infer "no bounds" from
            // "all six floats are zero", which also discards any real chunk
            // centred on the origin with zero extent.
            if (!ch->bounds_valid) continue;
            ++with_bounds;
            for (int a = 0; a < 3; ++a) {
                ulo[a] = (std::min)(ulo[a], ch->bbox_min[a]);
                uhi[a] = (std::max)(uhi[a], ch->bbox_max[a]);
            }
        }

        if (with_bounds == 0) {
            ++empty_bounds;
        } else {
            // Tolerance has to scale with the mesh. Positions are int16 over the
            // axis extent, so one quantization step is extent/32767 -- six
            // millimetres on a 400-unit mesh, and a computed bound can sit half a
            // step outside the cooker's own rounding. A fixed epsilon would flag
            // large meshes for being large.
            bool inside = true;
            for (int a = 0; a < 3; ++a) {
                // Two independent error sources, both real:
                //   - quantization: positions are int16 over the axis extent, so
                //     one step is extent/32767 (6 mm on a 400-unit mesh).
                //   - float resolution: a mesh authored at world coordinate 2800
                //     has ~2.4e-4 between representable floats there, and our
                //     dequantize-then-add lands a few ULPs from whatever the
                //     cooker computed.
                // A fixed epsilon would flag big meshes for being big and
                // far-from-origin meshes for being far from the origin.
                const float extent = hi[a] - lo[a];
                const float magnitude = (std::max)(std::fabs(lo[a]), std::fabs(hi[a]));
                const float eps =
                    (std::max)({1e-4f, extent / 32767.0f, magnitude * 1e-6f});
                if (ulo[a] < lo[a] - eps || uhi[a] > hi[a] + eps) inside = false;
            }
            if (!inside) {
                ++escaped;
                if (escaped <= 8)
                    std::printf("FAIL  0x%016" PRIX64 "  chunk union (%.3f %.3f %.3f)..(%.3f %.3f "
                                "%.3f) escapes CMesh box (%.3f %.3f %.3f)..(%.3f %.3f %.3f)\n",
                                h, ulo[0], ulo[1], ulo[2], uhi[0], uhi[1], uhi[2], lo[0], lo[1],
                                lo[2], hi[0], hi[1], hi[2]);
            } else if (meshes < 6) {
                std::printf("PASS  0x%016" PRIX64 "  %2u chunks, %u lods, union inside CMesh box\n",
                            h, n, redfs_mesh_lod_count(m));
            }
        }
        redfs_mesh_close(m);
        ++meshes;
    }

    std::printf("\n%u meshes checked\n", meshes);
    std::printf("  %u chunk unions escaping the stored CMesh box\n", escaped);
    std::printf("  %u with no computable bounds (geometry absent)\n", empty_bounds);

    redfs_depot_close(d);
    return (header_mismatch || size_mismatch || escaped) ? 1 : 0;
}
