// Persistent mesh cache.
//
// Chunk bounds cost a geometry decompress to produce and never change, because
// the archives they came from never change. So compute once, keep forever.
//
// The file is loaded whole at open and rewritten whole at flush. Its header
// carries a fingerprint of the archive set that produced it -- patch the game or
// add a mod and the fingerprint moves, the cache is dropped, and everything is
// recomputed rather than silently serving geometry for a mesh that has since
// been replaced.
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

#include <io.h>
#include <windows.h>

namespace redfs {
namespace {

constexpr uint32_t kMagic = 0x434D4652;  // 'RFMC'
constexpr uint32_t kVersion = 1;

struct Cache {
    std::mutex mutex;
    std::string file;
    uint64_t fingerprint = 0;
    // Read outside the lock on mesh_acquire's fast path. redfs.h declares the
    // cache calls not concurrency-safe, so a host following the contract cannot
    // race -- but a plain bool read outside the lock is still formally UB.
    std::atomic<bool> enabled{false};
    // The depot cache_open was called with. Compared for identity only and NEVER
    // dereferenced -- it may dangle after redfs_depot_close.
    const redfs_depot* owner = nullptr;
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

// Smallest on-disk footprint of each repeated element, straight from serialize()
// above. These turn every declared count into a bound the file itself has to pay
// for, which is what keeps a 68-byte cache file from asking for 56 MB of chunks.
// Deriving the bound from bytes remaining rather than picking a fixed cap also
// keeps the two halves in agreement: the writer applies no caps, so any fixed
// reader limit could reject a record serialize() legitimately produced -- and
// rejecting one truncates the rest of the file.
constexpr uint32_t kChunkBytes = 6 * 4 + 6 * 4;  // 5 x u32 + bounds_valid + 6 x f32
constexpr uint32_t kAppearanceBytes = 4 + 4;     // empty name + material count
constexpr uint32_t kMaterialBytes = 4;           // empty string

bool deserialize(Reader& r, Mesh* m) {
    m->hash = r.u64();
    m->lod_count = r.u32();
    const uint32_t chunk_count = r.u32();
    const uint32_t appearance_count = r.u32();
    for (int i = 0; i < 3; ++i) m->bbox_min[i] = r.f32();
    for (int i = 0; i < 3; ++i) m->bbox_max[i] = r.f32();
    if (!r.ok) return false;

    const size_t left = static_cast<size_t>(r.end - r.p);
    if (chunk_count > left / kChunkBytes || appearance_count > left / kAppearanceBytes)
        return false;

    // mesh_build guarantees >= 1 (max(1u, ...) and lowest_lod); a cache file is
    // external input and carries no such guarantee. Clamp rather than reject:
    // rejecting discards every later record in the file over a value that
    // repairs trivially, and redfs.h documents lod as 1-based.
    if (m->lod_count == 0) m->lod_count = 1;

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
        if (c.lod == 0) c.lod = 1;
    }
    m->appearances.resize(appearance_count);
    for (auto& a : m->appearances) {
        a.name = r.str();
        const uint32_t n = r.u32();
        if (!r.ok || n > static_cast<size_t>(r.end - r.p) / kMaterialBytes) return false;
        a.chunk_materials.resize(n);
        for (auto& mat : a.chunk_materials) mat = r.str();
        // Truncate AFTER reading, never before: the count is part of the stream,
        // so skipping entries here would desynchronise every later record.
        // internal.hpp documents chunk_materials as parallel to chunks and
        // mesh_build caps it at the chunk count; the load path has to as well.
        if (a.chunk_materials.size() > m->chunks.size())
            a.chunk_materials.resize(m->chunks.size());
    }
    return r.ok;
}

// What a load attempt did, so the caller can report it after unlocking. The log
// sink is host code, it runs on this thread, and it is free to call back into
// the cache -- which would re-enter a mutex this thread already holds.
struct LoadReport {
    enum class Result { kNoFile, kBadHeader, kWrongFingerprint, kLoaded };
    Result result = Result::kNoFile;
    uint32_t loaded = 0;
    uint32_t declared = 0;
};

LoadReport load_from_disk(Cache& c) {
    LoadReport rep;
    FILE* f = std::fopen(c.file.c_str(), "rb");
    if (!f) return rep;

    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 24) {
        std::fclose(f);
        return rep;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return rep;

    Reader r{buf.data(), buf.data() + buf.size()};
    if (r.u32() != kMagic || r.u32() != kVersion) {
        rep.result = LoadReport::Result::kBadHeader;
        return rep;
    }
    const uint64_t fingerprint = r.u64();
    if (fingerprint != c.fingerprint) {
        rep.result = LoadReport::Result::kWrongFingerprint;
        return rep;
    }
    rep.declared = r.u32();
    r.u32();  // reserved

    rep.result = LoadReport::Result::kLoaded;
    for (uint32_t i = 0; i < rep.declared && r.ok; ++i) {
        auto m = std::make_shared<Mesh>();
        // A partial record leaves the reader mid-field with no way to
        // resynchronise, so stopping is the only option; loaded vs declared is
        // what tells the caller the file was truncated.
        if (!deserialize(r, m.get())) break;
        m->finalize();  // public view derived before the object is published
        const uint64_t h = m->hash;
        c.entries[h] = std::const_pointer_cast<const Mesh>(m);
        ++rep.loaded;
    }
    return rep;
}

}  // namespace

uint64_t depot_fingerprint(const redfs_depot* depot) {
    // Per archive: path, entry count, index size, index CRC, declared file size.
    //
    // The CRC is the load-bearing one. Path, entry count and index size are all
    // blind to an archive whose files were replaced IN PLACE -- re-cook a mesh
    // with edited vertices and repack, and as long as the file, segment and
    // dependency counts are unchanged, the index is exactly as long as before and
    // all three of those inputs are byte-identical. The cache would then validate
    // and keep serving the old geometry forever.
    //
    // The packer's index CRC covers the entry table and the segment table, so it
    // moves when a file's content hash or a segment's offset/size moves. It is
    // also free: a header field already read at mount, so O(1) per archive rather
    // than the O(entries) a sweep of the per-entry SHA-1s would cost.
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&h](const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    };
    for (const auto* a : depot->archives) {
        mix(a->path().data(), a->path().size());
        const uint32_t n = a->entry_count();
        const uint64_t bytes = a->index_bytes();
        const uint64_t crc = a->index_crc();
        const uint64_t fsize = a->file_size();
        mix(&n, sizeof(n));
        mix(&bytes, sizeof(bytes));
        mix(&crc, sizeof(crc));
        mix(&fsize, sizeof(fsize));
    }
    return h;
}

redfs_status cache_open(const redfs_depot* depot, const char* file) {
    if (!depot || !file) return REDFS_E_INVALID_ARG;
    Cache& c = cache();

    LoadReport rep;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.file = file;
        c.fingerprint = depot_fingerprint(depot);
        c.entries.clear();
        c.enabled.store(true, std::memory_order_relaxed);
        c.owner = depot;
        c.dirty = false;
        rep = load_from_disk(c);
        path = c.file;
    }

    // Lock released before any of this reaches the host's sink.
    switch (rep.result) {
        case LoadReport::Result::kNoFile:
            break;
        case LoadReport::Result::kBadHeader:
            log("mesh cache %s has an unrecognised header; starting fresh", path.c_str());
            break;
        case LoadReport::Result::kWrongFingerprint:
            log("mesh cache %s was built for a different archive set; discarding", path.c_str());
            break;
        case LoadReport::Result::kLoaded:
            if (rep.loaded != rep.declared)
                log("mesh cache: loaded %u of %u entries from %s (file is truncated or corrupt)",
                    rep.loaded, rep.declared, path.c_str());
            else
                log("mesh cache: loaded %u entries from %s", rep.loaded, path.c_str());
            break;
    }
    return REDFS_OK;
}

redfs_status cache_flush() {
    Cache& c = cache();

    // Built under the lock, emitted after it. See LoadReport.
    char message[512] = {0};
    bool failed = false;

    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (!c.enabled.load(std::memory_order_relaxed) || !c.dirty) return REDFS_OK;

        std::vector<uint8_t> out;
        out.reserve(1 << 20);
        put_u32(out, kMagic);
        put_u32(out, kVersion);
        put_u64(out, c.fingerprint);
        put_u32(out, static_cast<uint32_t>(c.entries.size()));
        put_u32(out, 0);  // reserved
        for (const auto& [hash, mesh] : c.entries) serialize(out, *mesh);

        // Write to a sibling and rename over the original, so a crash mid-write
        // cannot corrupt a cache that was previously good.
        const std::string tmp = c.file + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            std::snprintf(message, sizeof(message), "cannot write %s", tmp.c_str());
            failed = true;
        } else {
            // fwrite reports bytes accepted into the FILE buffer, not bytes that
            // reached the disk: on a network share, a synced folder or a
            // removable drive the failure surfaces only at flush or close, so
            // fwrite alone would call a truncated file a success and then
            // promote it over a good cache.
            bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
            if (ok) ok = std::fflush(f) == 0 && ::_commit(::_fileno(f)) == 0;
            if (std::fclose(f) != 0) ok = false;  // close either way, then judge

            if (!ok) {
                std::remove(tmp.c_str());
                std::snprintf(message, sizeof(message), "cannot write %s", tmp.c_str());
                failed = true;
            } else if (!::MoveFileExA(tmp.c_str(), c.file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                // Replace in one step. Deleting the destination first opens a
                // window where neither file exists, and a rename that then fails
                // -- a scanner holding the path is enough -- destroys the good
                // cache instead of preserving it.
                std::remove(tmp.c_str());
                std::snprintf(message, sizeof(message), "cannot replace %s (error %lu)",
                              c.file.c_str(), ::GetLastError());
                failed = true;
            } else {
                c.dirty = false;
                std::snprintf(message, sizeof(message),
                              "mesh cache: wrote %zu entries (%zu bytes)", c.entries.size(),
                              out.size());
            }
        }
    }

    if (failed) return fail(REDFS_E_IO, "%s", message);
    log("%s", message);
    return REDFS_OK;
}

void cache_close() {
    cache_flush();
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.entries.clear();
    c.enabled.store(false, std::memory_order_relaxed);
    c.owner = nullptr;
}

bool cache_is_open(const redfs_depot* depot) {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.enabled.load(std::memory_order_relaxed) && c.owner == depot;
}

uint32_t cache_entry_count() {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    return static_cast<uint32_t>(c.entries.size());
}

redfs_status mesh_acquire(const redfs_depot* depot, uint64_t hash,
                          std::shared_ptr<const Mesh>* out) {
    Cache& c = cache();

    // Entries are keyed by hash alone, a hash means different bytes in different
    // depots, and there is one cache per process. Anything asked for on behalf of
    // a depot the cache does not belong to must bypass it entirely, or depot B is
    // served depot A's geometry and B's results are flushed under A's
    // fingerprint.
    const bool usable = c.enabled.load(std::memory_order_relaxed) && c.owner == depot;

    if (usable) {
        std::lock_guard<std::mutex> lock(c.mutex);
        auto it = c.entries.find(hash);
        if (it != c.entries.end()) {
            *out = it->second;
            return REDFS_OK;
        }
    }

    // Built privately and published only once complete: nothing ever observes a
    // half-decoded mesh, and nothing mutates one after it is shared.
    auto mesh = std::make_shared<Mesh>();
    const redfs_status st = mesh_build(depot, hash, mesh.get());
    if (st != REDFS_OK) return st;

    if (usable) {
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
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (!c.enabled.load(std::memory_order_relaxed)) return;
        // Mounting into a depot the cache does not belong to says nothing about
        // the archive set the cache was built from.
        if (c.owner != depot) return;

        // Both halves are required: re-fingerprinting alone leaves the stale
        // in-memory entries live and merely relabels them on flush, and dropping
        // entries alone lets the next flush write fresh data under the old label.
        const uint64_t before = c.fingerprint;
        c.fingerprint = depot_fingerprint(depot);
        if (c.fingerprint == before) return;  // archive set unchanged; keep the cache

        dropped = c.entries.size();
        c.entries.clear();
        c.dirty = false;  // nothing worth writing under the new fingerprint yet
    }
    if (dropped) log("mesh cache: archive set changed, dropping %zu entries", dropped);
}

}  // namespace redfs
