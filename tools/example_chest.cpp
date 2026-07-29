// Worked example, and a compile-check for the header-only C++ wrapper.
//
// Shows the query this library exists for: given only a depot path, work out
// which submeshes of a body are the chest -- without knowing anything about the
// mesh, the mod that supplied it, or which region numbers anyone chose.
//
//   example_chest [game-dir] [depot-path]

#include <cstdio>
#include <string>

#include "redfs.hpp"

int main(int argc, char** argv) {
    const char* game_dir = argc >= 2 ? argv[1] : nullptr;
    const char* mesh_path =
        argc >= 3 ? argv[2]
                  : "base\\characters\\common\\player_base_bodies\\player_female_average"
                    "\\t0_000_pwa_base__full.mesh";

    auto depot = redfs::Depot::open(game_dir);
    if (!depot) {
        std::printf("no depot: %s\n", redfs::last_error().c_str());
        return 1;
    }
    std::printf("%llu files across %u archives\n",
                static_cast<unsigned long long>(depot->file_count()), depot->archive_count());

    // Remember decoded meshes between runs so the geometry pass happens once.
    depot->enable_cache("redfs_mesh.cache");

    auto mesh = depot->mesh(mesh_path);
    if (!mesh) {
        std::printf("not a mesh: %s\n", redfs::last_error().c_str());
        return 2;
    }

    const auto [lo, hi] = mesh->bounds();
    const float height = hi[2] - lo[2];

    // "Chest" as a query rather than a hardcoded chunk list: LOD 1 only, and the
    // upper third of whatever this mesh happens to span.
    const float chest_floor = lo[2] + height * 0.66f;

    std::printf("\nmesh spans z %.3f..%.3f; treating z > %.3f as upper body\n", lo[2], hi[2],
                chest_floor);
    std::printf("%-6s %-5s %-9s %-24s %s\n", "chunk", "lod", "verts", "material", "z range");

    uint64_t chest_mask = 0;
    for (const auto& c : mesh->chunks()) {
        if (c.lod != 1) continue;  // one copy, not every detail level
        const bool is_chest = c.bbox_min[2] > chest_floor;
        if (is_chest) chest_mask |= (1ull << c.index);
        std::printf("%-6u %-5u %-9u %-24s %.3f .. %.3f%s\n", c.index, c.lod, c.vertex_count,
                    std::string(mesh->chunk_material(0, c.index)).c_str(), c.bbox_min[2],
                    c.bbox_max[2], is_chest ? "   <-- chest" : "");
    }

    // A chunkMask is exactly this: bit i set means chunk i is drawn.
    std::printf("\nchunkMask selecting those chunks: %llu\n",
                static_cast<unsigned long long>(chest_mask));

    std::printf("appearances (%u):", mesh->appearance_count());
    for (uint32_t a = 0; a < mesh->appearance_count() && a < 6; ++a)
        std::printf(" %s", std::string(mesh->appearance_name(a)).c_str());
    std::printf("\n");

    redfs::Depot::flush_cache();
    return 0;
}
