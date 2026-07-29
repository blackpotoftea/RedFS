// Persistent mesh cache.
//
// Chunk bounds cost a geometry decompress to produce and never change, because
// the archives they came from never change. So compute once, keep forever.
//
// The file is append-friendly and loaded whole at open. Every record carries a
// fingerprint of the archive set that produced it -- patch the game or add a mod
// and the fingerprint moves, the cache is dropped, and everything is recomputed
// rather than silently serving geometry for a mesh that has since been replaced.
//
// Format (little-endian):
//   'RFMC' u32 | version u32 | fingerprint u64 | entry_count u32 | reserved u32
//   entries, each:
//     u64 hash | u32 lod_count | u32 chunk_count | u32 appearance_count
//     f32 bbox_min[3] | f32 bbox_max[3]
//     chunk_count      x { u32 index, lod_mask, lod, vertex_count, index_count,
//                          u32 bounds_valid, f32 min[3], f32 max[3] }
//     appearance_count x { u32 name_len, bytes, u32 material_count,
//                          material_count x { u32 len, bytes } }

#include "internal.hpp"

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace redfs {
namespace {

constexpr uint32_t kMagic = 0x434D4652;  // 'RFMC'
constexpr uint32_t kVersion = 1;

struct Cache {
    std::mutex mutex;
    std::string file;
    uint64_t fingerprint = 0;
    bool enabled = false;
    bool dirty = false;
    // shared_ptr, not unique_ptr: an open handle holds its own reference, so
    // clearing the cache cannot free a mesh a caller is still reading.
    std::unordered_map<uint64_t, std::shared_ptr<const Mesh>> entries;
};

Cache& cache() {
    static Cache* c = new Cache();  // never destroyed; see Worker in api.cpp
    return *c;
}

// --- writing -----------------------------------------------------------------

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}
void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 8);
}
void put_f32(std::vector<uint8_t>& out, float v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}
void put_str(std::vector<uint8_t>& out, const std::string& s) {
    put_u32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

void serialize(std::vector<uint8_t>& out, const Mesh& m) {
    put_u64(out, m.hash);
    put_u32(out, m.lod_count);
    put_u32(out, static_cast<uint32_t>(m.chunks.size()));
    put_u32(out, static_cast<uint32_t>(m.appearances.size()));
    for (int i = 0; i < 3; ++i) put_f32(out, m.bbox_min[i]);
    for (int i = 0; i < 3; ++i) put_f32(out, m.bbox_max[i]);

    for (const auto& c : m.chunks) {
        put_u32(out, c.index);
        put_u32(out, c.lod_mask);
        put_u32(out, c.lod);
        put_u32(out, c.vertex_count);
        put_u32(out, c.index_count);
        put_u32(out, c.bounds_valid ? 1u : 0u);
        for (int i = 0; i < 3; ++i) put_f32(out, c.bbox_min[i]);
        for (int i = 0; i < 3; ++i) put_f32(out, c.bbox_max[i]);
    }
    for (const auto& a : m.appearances) {
        put_str(out, a.name);
        put_u32(out, static_cast<uint32_t>(a.chunk_materials.size()));
        for (const auto& mat : a.chunk_materials) put_str(out, mat);
    }
}

// --- reading -----------------------------------------------------------------

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    bool need(size_t n) {
        if (!ok || static_cast<size_t>(end - p) < n) {
            ok = false;
            return false;
        }
        return true;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        const uint32_t v = rd32(p);
        p += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        const uint64_t v = rd64(p);
        p += 8;
        return v;
    }
    float f32() {
        if (!need(4)) return 0.f;
        float v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    std::string str() {
        const uint32_t n = u32();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
};

bool deserialize(Reader& r, Mesh* m) {
    m->hash = r.u64();
    m->lod_count = r.u32();
    const uint32_t chunk_count = r.u32();
    const uint32_t appearance_count = r.u32();
    for (int i = 0; i < 3; ++i) m->bbox_min[i] = r.f32();
    for (int i = 0; i < 3; ++i) m->bbox_max[i] = r.f32();
    if (!r.ok || chunk_count > (1u << 20) || appearance_count > (1u << 16)) return false;

    m->chunks.resize(chunk_count);
    for (auto& c : m->chunks) {
        c.index = r.u32();
        c.lod_mask = r.u32();
        c.lod = r.u32();
        c.vertex_count = r.u32();
        c.index_count = r.u32();
        c.bounds_valid = r.u32() != 0;
        for (int i = 0; i < 3; ++i) c.bbox_min[i] = r.f32();
        for (int i = 0; i < 3; ++i) c.bbox_max[i] = r.f32();
    }
    m->appearances.resize(appearance_count);
    for (auto& a : m->appearances) {
        a.name = r.str();
        const uint32_t n = r.u32();
        if (!r.ok || n > (1u << 20)) return false;
        a.chunk_materials.resize(n);
        for (auto& mat : a.chunk_materials) mat = r.str();
    }
    return r.ok;
}

void load_from_disk(Cache& c) {
    FILE* f = std::fopen(c.file.c_str(), "rb");
    if (!f) return;

    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 24) {
        std::fclose(f);
        return;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return;

    Reader r{buf.data(), buf.data() + buf.size()};
    if (r.u32() != kMagic || r.u32() != kVersion) {
        log("mesh cache %s has an unrecognised header; starting fresh", c.file.c_str());
        return;
    }
    const uint64_t fingerprint = r.u64();
    if (fingerprint != c.fingerprint) {
        log("mesh cache %s was built for a different archive set; discarding", c.file.c_str());
        return;
    }
    const uint32_t count = r.u32();
    r.u32();  // reserved

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        auto m = std::make_shared<Mesh>();
        if (!deserialize(r, m.get())) break;
        // Same rule as mesh_build: the public view is derived before the object
        // is published, so a cached mesh is immutable from here on.
        m->finalize();
        const uint64_t h = m->hash;
        c.entries[h] = std::const_pointer_cast<const Mesh>(m);
        ++loaded;
    }
    log("mesh cache: loaded %u entries from %s", loaded, c.file.c_str());
}

}  // namespace

uint64_t depot_fingerprint(const redfs_depot* depot) {
    // Path + entry count + index size of every mounted archive. Cheap, and it
    // moves whenever the set of archives or their contents change.
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&h](const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    };
    for (const auto* a : depot->archives) {
        mix(a->path().data(), a->path().size());
        const uint32_t n = a->entry_count();
        const uint64_t bytes = a->index_bytes();
        mix(&n, sizeof(n));
        mix(&bytes, sizeof(bytes));
    }
    return h;
}

redfs_status cache_open(const redfs_depot* depot, const char* file) {
    if (!depot || !file) return REDFS_E_INVALID_ARG;
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);

    c.file = file;
    c.fingerprint = depot_fingerprint(depot);
    c.entries.clear();
    c.enabled = true;
    c.dirty = false;
    load_from_disk(c);
    return REDFS_OK;
}

redfs_status cache_flush() {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (!c.enabled || !c.dirty) return REDFS_OK;

    std::vector<uint8_t> out;
    out.reserve(1 << 20);
    put_u32(out, kMagic);
    put_u32(out, kVersion);
    put_u64(out, c.fingerprint);
    put_u32(out, static_cast<uint32_t>(c.entries.size()));
    put_u32(out, 0);  // reserved
    for (const auto& [hash, mesh] : c.entries) serialize(out, *mesh);

    // Write to a sibling then rename, so a crash mid-write cannot corrupt a
    // cache that was previously good.
    const std::string tmp = c.file + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return fail(REDFS_E_IO, "cannot write %s", tmp.c_str());
    const size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (wrote != out.size()) {
        std::remove(tmp.c_str());
        return fail(REDFS_E_IO, "short write to %s", tmp.c_str());
    }
    std::remove(c.file.c_str());
    if (std::rename(tmp.c_str(), c.file.c_str()) != 0)
        return fail(REDFS_E_IO, "cannot replace %s", c.file.c_str());

    c.dirty = false;
    log("mesh cache: wrote %zu entries (%zu bytes)", c.entries.size(), out.size());
    return REDFS_OK;
}

void cache_close() {
    cache_flush();
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.entries.clear();
    c.enabled = false;
}

uint32_t cache_entry_count() {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    return static_cast<uint32_t>(c.entries.size());
}

redfs_status mesh_acquire(const redfs_depot* depot, uint64_t hash,
                          std::shared_ptr<const Mesh>* out) {
    Cache& c = cache();

    if (c.enabled) {
        std::lock_guard<std::mutex> lock(c.mutex);
        auto it = c.entries.find(hash);
        if (it != c.entries.end()) {
            *out = it->second;  // caller gets its own reference
            return REDFS_OK;
        }
    }

    // Built privately and only published once complete, so nothing ever observes
    // a half-decoded mesh and nothing mutates one after it is shared.
    auto mesh = std::make_shared<Mesh>();
    const redfs_status st = mesh_build(depot, hash, mesh.get());
    if (st != REDFS_OK) return st;

    if (c.enabled) {
        std::lock_guard<std::mutex> lock(c.mutex);
        // Another thread may have inserted it while we were building; keep
        // whichever won so all callers observe one object per hash.
        auto it = c.entries.find(hash);
        if (it == c.entries.end()) {
            it = c.entries.emplace(hash, std::const_pointer_cast<const Mesh>(mesh)).first;
            c.dirty = true;
        }
        *out = it->second;
        return REDFS_OK;
    }

    *out = std::move(mesh);
    return REDFS_OK;
}

void cache_invalidate(const redfs_depot* depot) {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (!c.enabled) return;

    // Both halves are required. Re-fingerprinting alone would leave the stale
    // in-memory entries live and merely relabel them on flush -- turning a
    // reproducible staleness bug into an intermittent one. Dropping entries
    // alone would let the next flush write fresh data under the old label.
    const uint64_t before = c.fingerprint;
    c.fingerprint = depot_fingerprint(depot);
    if (c.fingerprint == before) return;  // archive set unchanged; keep the cache

    if (!c.entries.empty())
        log("mesh cache: archive set changed, dropping %zu entries", c.entries.size());
    c.entries.clear();
    c.dirty = false;  // nothing worth writing under the new fingerprint yet
}

}  // namespace redfs
