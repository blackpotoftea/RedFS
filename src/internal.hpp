// RedFS internals. Not part of the public ABI.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "redfs.h"

namespace redfs {

// --- unaligned little-endian reads ------------------------------------------
// Index and CR2W tables are read straight out of a file mapping at arbitrary
// offsets, so nothing may assume natural alignment.

template <typename T>
inline T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
inline uint16_t rd16(const uint8_t* p) { return rd<uint16_t>(p); }
inline uint32_t rd32(const uint8_t* p) { return rd<uint32_t>(p); }
inline uint64_t rd64(const uint8_t* p) { return rd<uint64_t>(p); }

// --- diagnostics -------------------------------------------------------------

void log(const char* fmt, ...);
// Records a message for redfs_last_error() and returns `status`, so callers can
// `return fail(REDFS_E_CORRUPT, "...")` in one line.
redfs_status fail(redfs_status status, const char* fmt, ...);

// --- oodle -------------------------------------------------------------------

namespace oodle {
// Resolves oo2ext_7_win64.dll: already loaded in the game process, else from
// <game_dir>/bin/x64. Idempotent, thread-safe.
bool load(const char* game_dir);
bool available();
// Returns decoded byte count, or a value != raw_len on failure.
int64_t decompress(const void* comp, int64_t comp_len, void* raw, int64_t raw_len);
}  // namespace oodle

// --- path hashing ------------------------------------------------------------

std::string sanitize_path(const char* path, size_t len);
uint64_t fnv1a64(const void* data, size_t len);

// --- archive -----------------------------------------------------------------

constexpr uint32_t kArchiveMagic = 0x52414452;  // 'RDAR'
constexpr uint32_t kKarkMagic    = 0x4B52414B;  // 'KARK'
constexpr uint32_t kCr2wMagic    = 0x57325243;  // 'CR2W'

constexpr size_t kIndexHeaderSize = 28;
constexpr size_t kEntryStride     = 56;
constexpr size_t kSegmentStride   = 16;

struct Segment {
    uint64_t offset;
    uint32_t zsize;  // bytes on disk
    uint32_t size;   // bytes once decoded
};

// One mounted .archive. The index stays mapped rather than copied, so a full
// depot costs tens of MB of address space instead of hundreds of MB of heap.
class Archive {
public:
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    Archive() = default;

    redfs_status open(const std::string& path);

    const std::string& path() const { return path_; }
    uint32_t entry_count() const { return entry_count_; }
    uint32_t segment_count() const { return segment_count_; }
    uint64_t index_bytes() const { return index_size_; }

    // Entry accessors, by index into this archive's file table.
    uint64_t entry_hash(uint32_t i) const { return rd64(entries_ + i * kEntryStride); }
    int64_t entry_timestamp(uint32_t i) const {
        return static_cast<int64_t>(rd64(entries_ + i * kEntryStride + 8));
    }
    uint32_t entry_seg_start(uint32_t i) const { return rd32(entries_ + i * kEntryStride + 20); }
    uint32_t entry_seg_end(uint32_t i) const { return rd32(entries_ + i * kEntryStride + 24); }
    const uint8_t* entry_sha1(uint32_t i) const { return entries_ + i * kEntryStride + 36; }

    Segment segment(uint32_t i) const {
        const uint8_t* p = segments_ + i * kSegmentStride;
        return Segment{rd64(p), rd32(p + 8), rd32(p + 12)};
    }

    // Decode one segment into `dst`, which must hold at least segment.size bytes.
    redfs_status read_segment(const Segment& seg, uint8_t* dst) const;

private:
    std::string path_;
    void* file_ = nullptr;   // HANDLE
    void* map_ = nullptr;    // HANDLE
    void* view_ = nullptr;   // index mapping base (allocation-granularity aligned)
    const uint8_t* entries_ = nullptr;
    const uint8_t* segments_ = nullptr;
    uint32_t entry_count_ = 0;
    uint32_t segment_count_ = 0;
    uint64_t index_size_ = 0;
};

// --- depot -------------------------------------------------------------------

struct Located {
    const Archive* archive;
    uint32_t entry;
    uint32_t archive_index;
};

}  // namespace redfs

// Opaque to the C ABI; defined here so the format helpers can use it.
struct redfs_depot {
    std::vector<redfs::Archive*> archives;

    // hash -> (archive, entry), sorted by hash. 16 bytes per file.
    struct Ref {
        uint64_t hash;
        uint32_t archive;
        uint32_t entry;
    };
    std::vector<Ref> refs;

    ~redfs_depot();
    // `out` may be null when only presence matters.
    bool locate(uint64_t hash, redfs::Located* out) const;
};

namespace redfs {

// Segment span for a file: [first, last) in the archive's segment table.
struct PartRange {
    const Archive* archive;
    uint32_t first;
    uint32_t last;
};
redfs_status resolve_part(const redfs_depot* depot, uint64_t hash, uint32_t part, PartRange* out);

// `abort` lets a long read be cut short between segments. The async worker
// points it at its shutdown flag so that closing the game does not have to wait
// out a queue of large reads; synchronous callers pass null. Oodle cannot be
// interrupted mid-block, so the granularity is one segment -- which is what
// bounds shutdown latency.
redfs_status read_part(const redfs_depot* depot, uint64_t hash, uint32_t part, uint8_t* dst,
                       uint64_t capacity, uint64_t* out_size,
                       const std::atomic<bool>* abort = nullptr);

// --- CR2W --------------------------------------------------------------------

struct Cr2wChunk {
    uint16_t class_name;
    uint32_t data_offset;
    uint32_t data_size;
};

struct Cr2wImport {
    uint32_t str_offset;
    uint16_t class_name;
    uint16_t flags;
};

struct Cr2wBuffer {
    uint32_t index;
    uint32_t offset;
    uint32_t disk_size;
    uint32_t mem_size;
};

}  // namespace redfs

struct redfs_cr2w {
    const uint8_t* base = nullptr;
    uint64_t size = 0;
    const char* strings = nullptr;  // base + string table offset
    uint32_t strings_size = 0;
    std::vector<uint32_t> names;  // offsets into the string table
    std::vector<redfs::Cr2wImport> imports;
    std::vector<redfs::Cr2wChunk> chunks;
    std::vector<redfs::Cr2wBuffer> buffers;
    // CString values are decoded on demand; the handle owns them so that
    // redfs_value::as.s stays valid for as long as the caller holds the CR2W.
    // Keyed by source pointer so decoding the same property twice reuses the
    // result instead of growing the handle on every query.
    //
    // Mutated by cr2w_decode, which means a CR2W handle is SINGLE-THREADED --
    // see the threading note in redfs.h. Depot reads remain safe to share; it is
    // only an individual redfs_cr2w* that must not be touched from two threads.
    std::vector<std::unique_ptr<std::string>> owned_strings;
    std::unordered_map<const uint8_t*, std::string*> string_cache;

    // Both accessors must bounds-check the *offset*, not just the index. A
    // corrupt name table holds arbitrary offsets, and every returned pointer is
    // handed to strcmp -- so an unchecked one reads until it hits an unmapped
    // page. cr2w_parse additionally guarantees the table ends in a NUL, which is
    // what makes an in-range offset safe to walk.
    const char* name(uint32_t i) const {
        return i < names.size() ? str(names[i]) : "";
    }
    const char* str(uint32_t off) const {
        // The null check covers a default-constructed handle, which callers
        // should never see but which nothing structurally prevents. Both
        // accessors are contracted to return a valid C string, never null:
        // every result reaches strcmp.
        return (strings && off < strings_size) ? strings + off : "";
    }
};

namespace redfs {

redfs_status cr2w_parse(const void* data, uint64_t size, redfs_cr2w* out);

// Walks the TLV property stream of a chunk (or a nested struct) and resolves a
// dotted path such as "header.sizeInfo.width".
redfs_status cr2w_find(const redfs_cr2w* f, uint32_t chunk, const char* prop_path,
                       redfs_value* out);
redfs_status cr2w_walk(const redfs_cr2w* f, uint32_t chunk, const char* prop_path,
                       redfs_prop_fn fn, void* user);
redfs_status cr2w_get_in(const redfs_cr2w* f, const redfs_value* parent, const char* prop_path,
                         redfs_value* out);
redfs_status cr2w_walk_array(const redfs_cr2w* f, const redfs_value* array, redfs_elem_fn fn,
                             void* user);
redfs_status cr2w_walk_in(const redfs_cr2w* f, const redfs_value* parent, const char* prop_path,
                          redfs_prop_fn fn, void* user);

// Decodes one property's bytes into a typed value.
void cr2w_decode(const redfs_cr2w* f, const char* type, const uint8_t* data, uint32_t size,
                 redfs_value* out);

// --- mesh --------------------------------------------------------------------

// Extends the public redfs_mesh_chunk with the stream locations needed to
// compute the box, which callers never see.
struct MeshChunk {
    uint32_t index = 0;
    uint32_t lod_mask = 1;
    uint32_t lod = 1;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    float bbox_min[3] = {0, 0, 0};
    float bbox_max[3] = {0, 0, 0};
    bool bounds_valid = false;

    uint32_t position_offset = 0;  // into the render buffer
    uint32_t position_stride = 0;
};

struct MeshAppearance {
    std::string name;
    std::vector<std::string> chunk_materials;  // parallel to chunks
};

// A fully decoded mesh. IMMUTABLE once built.
//
// That immutability is the whole design. The cache hands the same object to
// every caller, so anything that mutates it after publication is a data race --
// and redfs_mesh_chunk_at returns an interior pointer callers may hold, so a
// vector that grows after publication is a use-after-free waiting to happen.
// public_chunks is therefore populated at CONSTRUCTION (mesh_build, and
// cache deserialize), never on open.
//
// Ownership is shared: the cache holds a reference and so does every open
// handle, so closing the cache cannot pull the object out from under a caller
// who is still using it.
struct MeshData {
    uint64_t hash = 0;
    uint32_t lod_count = 1;
    float bbox_min[3] = {0, 0, 0};
    float bbox_max[3] = {0, 0, 0};
    std::vector<MeshChunk> chunks;
    std::vector<MeshAppearance> appearances;

    // Mirrors `chunks` in the public struct layout, so redfs_mesh_chunk_at can
    // return a pointer straight into it.
    std::vector<redfs_mesh_chunk> public_chunks;

    // Derives public_chunks from chunks. Call once, before publication.
    void finalize();
};

}  // namespace redfs

// The handle a caller holds. Deliberately not the mesh itself: it exists to own
// a reference, so redfs_mesh_close always means "drop my reference" regardless
// of whether the cache is on, and an outstanding handle keeps the data alive
// past redfs_cache_close.
struct redfs_mesh {
    std::shared_ptr<const redfs::MeshData> data;
};

namespace redfs {

using Mesh = MeshData;

// Reads and fully decodes a mesh, geometry sweep included.
redfs_status mesh_build(const redfs_depot* depot, uint64_t hash, Mesh* out);

// --- mesh cache --------------------------------------------------------------

// Returns a cached mesh if one exists, otherwise builds and records it. The
// result is shared: the caller's reference keeps it alive independently of the
// cache, so cache_close cannot invalidate a live handle.
redfs_status mesh_acquire(const redfs_depot* depot, uint64_t hash,
                          std::shared_ptr<const Mesh>* out);

// Drops every cached entry and re-fingerprints against the depot as it is NOW.
// Called when the archive set changes; without it a mount after cache_open
// serves pre-mount geometry and flushes new entries under the stale label.
void cache_invalidate(const redfs_depot* depot);
redfs_status cache_open(const redfs_depot* depot, const char* file);
redfs_status cache_flush();
void cache_close();
uint32_t cache_entry_count();

// A fingerprint of the mounted archive set; the cache is discarded when it moves.
uint64_t depot_fingerprint(const redfs_depot* depot);

// --- path dictionary ---------------------------------------------------------

redfs_status paths_load(const redfs_depot* depot, const char* file, uint32_t* out_kept);
void paths_add(const char* path);
void paths_enable();
void paths_learn_imports(const redfs_cr2w* f);
const char* path_from_hash(uint64_t hash);
uint32_t paths_count();

// --- blobs -------------------------------------------------------------------

redfs_status blob_alloc(uint64_t size, redfs_blob* out);

}  // namespace redfs
