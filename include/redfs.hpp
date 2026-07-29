// RedFS -- C++ conveniences over the C ABI in redfs.h.
//
// Header-only and optional: everything here is a thin wrapper, so mixing the two
// layers is fine. The C ABI is what stays stable across compilers; this is what
// you actually want to type.
//
//   redfs::Depot depot = redfs::Depot::open().value();
//   auto dds = depot.texture_dds("base\\icon\\common\\ico_scanner.xbm");
//   device.CreateTextureFromMemory(dds->data(), dds->size());
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "redfs.h"

namespace redfs {

using Status = redfs_status;
using FileInfo = redfs_file_info;
using TextureDesc = redfs_texture_desc;
using MeshDesc = redfs_mesh_desc;
using AudioFormat = redfs_audio_format;
using Value = redfs_value;
using Kind = redfs_kind;

inline const char* to_string(Status s) { return redfs_status_string(s); }
inline std::string last_error() { return redfs_last_error(); }

/// A depot path, hashed the way the engine hashes it.
inline uint64_t hash(std::string_view path) { return redfs_hash_n(path.data(), path.size()); }

/// Decimal form of a key, for hosts that cannot hold a uint64 exactly.
inline std::string hash_string(std::string_view path) {
    return std::to_string(hash(path));
}
inline uint64_t hash_parse(const std::string& decimal) {
    return redfs_hash_parse(decimal.c_str());
}

/// Reverse lookup; empty when the hash is not in the dictionary.
/// See Depot::load_paths.
inline std::string_view path_of(uint64_t key) {
    const char* p = redfs_path_from_hash(key);
    return p ? std::string_view{p} : std::string_view{};
}

/// Owning byte buffer returned by the read APIs.
class Blob {
public:
    Blob() = default;
    explicit Blob(redfs_blob b) : b_(b) {}
    Blob(const Blob&) = delete;
    Blob& operator=(const Blob&) = delete;
    Blob(Blob&& o) noexcept : b_(std::exchange(o.b_, redfs_blob{})) {}
    Blob& operator=(Blob&& o) noexcept {
        if (this != &o) {
            reset();
            b_ = std::exchange(o.b_, redfs_blob{});
        }
        return *this;
    }
    ~Blob() { reset(); }

    const uint8_t* data() const { return b_.data; }
    uint8_t* data() { return b_.data; }
    size_t size() const { return static_cast<size_t>(b_.size); }
    bool empty() const { return b_.size == 0; }
    explicit operator bool() const { return b_.data != nullptr; }

    std::span<const uint8_t> bytes() const { return {b_.data, size()}; }
    std::string_view text() const { return {reinterpret_cast<const char*>(b_.data), size()}; }

    void reset() { redfs_blob_free(&b_); }

private:
    redfs_blob b_{};
};

/// A parsed CR2W document. Borrows the bytes it was opened over.
class Cr2w {
public:
    Cr2w() = default;
    explicit Cr2w(redfs_cr2w* h) : h_(h) {}
    Cr2w(const Cr2w&) = delete;
    Cr2w& operator=(const Cr2w&) = delete;
    Cr2w(Cr2w&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Cr2w& operator=(Cr2w&& o) noexcept {
        if (this != &o) {
            if (h_) redfs_cr2w_close(h_);
            h_ = std::exchange(o.h_, nullptr);
        }
        return *this;
    }
    ~Cr2w() {
        if (h_) redfs_cr2w_close(h_);
    }

    static std::optional<Cr2w> open(std::span<const uint8_t> data) {
        redfs_cr2w* h = nullptr;
        if (redfs_cr2w_open(data.data(), data.size(), &h) != REDFS_OK) return std::nullopt;
        return Cr2w{h};
    }

    explicit operator bool() const { return h_ != nullptr; }

    std::string_view root_type() const { return redfs_cr2w_root_type(h_); }
    uint32_t chunk_count() const { return redfs_cr2w_chunk_count(h_); }
    std::string_view chunk_type(uint32_t i) const { return redfs_cr2w_chunk_type(h_, i); }
    int32_t find_chunk(const char* type) const { return redfs_cr2w_find_chunk(h_, type); }

    /// Depot paths of every resource this file references.
    std::vector<std::string_view> imports() const {
        std::vector<std::string_view> out;
        const uint32_t n = redfs_cr2w_import_count(h_);
        out.reserve(n);
        for (uint32_t i = 0; i < n; ++i) out.emplace_back(redfs_cr2w_import_path(h_, i));
        return out;
    }

    /// Address nested properties with a dotted path: "header.sizeInfo.width".
    std::optional<Value> get(uint32_t chunk, const char* prop_path) const {
        Value v{};
        if (redfs_cr2w_get(h_, chunk, prop_path, &v) != REDFS_OK) return std::nullopt;
        return v;
    }

    std::optional<uint64_t> get_uint(uint32_t chunk, const char* prop_path) const {
        auto v = get(chunk, prop_path);
        if (!v) return std::nullopt;
        if (v->kind == REDFS_KIND_UINT || v->kind == REDFS_KIND_BOOL) return v->as.u;
        if (v->kind == REDFS_KIND_INT) return static_cast<uint64_t>(v->as.i);
        return std::nullopt;
    }

    std::optional<std::string_view> get_name(uint32_t chunk, const char* prop_path) const {
        auto v = get(chunk, prop_path);
        if (!v || (v->kind != REDFS_KIND_NAME && v->kind != REDFS_KIND_STRING))
            return std::nullopt;
        return std::string_view{v->as.s};
    }

    /// Visit every property of a chunk (or of a nested struct). `fn` returns
    /// false to stop early.
    template <typename Fn>
    void walk(uint32_t chunk, const char* prop_path, Fn&& fn) const {
        auto trampoline = [](const char* name, const Value* v, void* user) -> int {
            return (*static_cast<Fn*>(user))(std::string_view{name}, *v) ? 1 : 0;
        };
        redfs_cr2w_walk(h_, chunk, prop_path, trampoline, &fn);
    }

    redfs_cr2w* handle() const { return h_; }

private:
    redfs_cr2w* h_ = nullptr;
};

/// A decoded mesh: chunk table, LODs, appearances and per-chunk bounds.
///
/// A chunk index is a bit in a component's chunkMask, so `chunks()` is directly
/// queryable against a live entity. Chunks repeat per LOD -- filter on `lod`.
class Mesh {
public:
    using Chunk = redfs_mesh_chunk;

    Mesh() = default;
    explicit Mesh(redfs_mesh* h) : h_(h) {}
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Mesh& operator=(Mesh&& o) noexcept {
        if (this != &o) {
            if (h_) redfs_mesh_close(h_);
            h_ = std::exchange(o.h_, nullptr);
        }
        return *this;
    }
    ~Mesh() {
        if (h_) redfs_mesh_close(h_);
    }

    explicit operator bool() const { return h_ != nullptr; }

    uint32_t chunk_count() const { return redfs_mesh_chunk_count(h_); }
    const Chunk* chunk(uint32_t i) const { return redfs_mesh_chunk_at(h_, i); }
    uint32_t lod_count() const { return redfs_mesh_lod_count(h_); }

    std::span<const Chunk> chunks() const {
        const uint32_t n = chunk_count();
        const Chunk* first = n ? redfs_mesh_chunk_at(h_, 0) : nullptr;
        return {first, n};
    }

    std::pair<std::array<float, 3>, std::array<float, 3>> bounds() const {
        std::array<float, 3> lo{}, hi{};
        redfs_mesh_bounds(h_, lo.data(), hi.data());
        return {lo, hi};
    }

    uint32_t appearance_count() const { return redfs_mesh_appearance_count(h_); }
    std::string_view appearance_name(uint32_t i) const {
        return redfs_mesh_appearance_name(h_, i);
    }
    int32_t find_appearance(const char* name) const {
        return redfs_mesh_find_appearance(h_, name);
    }
    std::string_view chunk_material(uint32_t appearance, uint32_t chunk) const {
        return redfs_mesh_chunk_material(h_, appearance, chunk);
    }

    redfs_mesh* handle() const { return h_; }

private:
    redfs_mesh* h_ = nullptr;
};

/// The mounted archives of one game install.
class Depot {
public:
    Depot() = default;
    explicit Depot(redfs_depot* h) : h_(h) {}
    Depot(const Depot&) = delete;
    Depot& operator=(const Depot&) = delete;
    Depot(Depot&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Depot& operator=(Depot&& o) noexcept {
        if (this != &o) {
            if (h_) redfs_depot_close(h_);
            h_ = std::exchange(o.h_, nullptr);
        }
        return *this;
    }
    ~Depot() {
        if (h_) redfs_depot_close(h_);
    }

    /// game_dir == nullptr auto-detects from the running process, which is what
    /// you want from inside the game.
    static std::optional<Depot> open(const char* game_dir = nullptr,
                                     uint32_t flags = REDFS_SCAN_ALL) {
        redfs_depot* h = nullptr;
        if (redfs_depot_open(game_dir, flags, &h) != REDFS_OK) return std::nullopt;
        return Depot{h};
    }

    explicit operator bool() const { return h_ != nullptr; }
    redfs_depot* handle() const { return h_; }

    Status mount(const char* archive_path) { return redfs_depot_mount(h_, archive_path); }

    uint32_t archive_count() const { return redfs_depot_archive_count(h_); }
    std::string_view archive_path(uint32_t i) const { return redfs_depot_archive_path(h_, i); }
    uint64_t file_count() const { return redfs_depot_file_count(h_); }
    uint64_t index_bytes() const { return redfs_depot_index_bytes(h_); }

    bool exists(uint64_t key) const { return redfs_exists(h_, key) != 0; }
    bool exists(std::string_view path) const { return exists(hash(path)); }

    std::optional<FileInfo> stat(uint64_t key) const {
        FileInfo fi{};
        if (redfs_stat(h_, key, &fi) != REDFS_OK) return std::nullopt;
        return fi;
    }
    std::optional<FileInfo> stat(std::string_view path) const { return stat(hash(path)); }

    /// Whole file: the resource plus every attached buffer.
    std::optional<Blob> read(uint64_t key, uint32_t part = REDFS_PART_ALL) const {
        redfs_blob b{};
        if (redfs_read(h_, key, part, &b) != REDFS_OK) return std::nullopt;
        return Blob{b};
    }
    std::optional<Blob> read(std::string_view path, uint32_t part = REDFS_PART_ALL) const {
        return read(hash(path), part);
    }

    /// Just the CR2W document, without the bulk payload.
    std::optional<Blob> read_main(std::string_view path) const {
        return read(hash(path), REDFS_PART_MAIN);
    }
    std::optional<Blob> read_buffer(std::string_view path, uint32_t index) const {
        return read(hash(path), index);
    }

    /// Parse a resource's CR2W. The returned pair keeps the bytes alive.
    std::optional<std::pair<Blob, Cr2w>> open_resource(uint64_t key) const {
        auto blob = read(key, REDFS_PART_MAIN);
        if (!blob) return std::nullopt;
        auto cr2w = Cr2w::open(blob->bytes());
        if (!cr2w) return std::nullopt;
        return std::make_pair(std::move(*blob), std::move(*cr2w));
    }
    std::optional<std::pair<Blob, Cr2w>> open_resource(std::string_view path) const {
        return open_resource(hash(path));
    }

    // --- textures ------------------------------------------------------------

    std::optional<TextureDesc> texture_desc(uint64_t key) const {
        TextureDesc d{};
        if (redfs_texture_desc_of(h_, key, &d) != REDFS_OK) return std::nullopt;
        return d;
    }
    std::optional<TextureDesc> texture_desc(std::string_view path) const {
        return texture_desc(hash(path));
    }

    /// A complete in-memory DDS, ready for CreateDDSTextureFromMemory.
    std::optional<Blob> texture_dds(uint64_t key) const {
        redfs_blob b{};
        if (redfs_texture_read_dds(h_, key, &b) != REDFS_OK) return std::nullopt;
        return Blob{b};
    }
    std::optional<Blob> texture_dds(std::string_view path) const { return texture_dds(hash(path)); }

    /// Raw pixels plus the descriptor, for callers filling their own
    /// D3D11_SUBRESOURCE_DATA.
    std::optional<std::pair<TextureDesc, Blob>> texture_raw(uint64_t key) const {
        TextureDesc d{};
        redfs_blob b{};
        if (redfs_texture_read_raw(h_, key, &d, &b) != REDFS_OK) return std::nullopt;
        return std::make_pair(d, Blob{b});
    }
    std::optional<std::pair<TextureDesc, Blob>> texture_raw(std::string_view path) const {
        return texture_raw(hash(path));
    }

    // --- audio ---------------------------------------------------------------

    std::optional<AudioFormat> audio_format(std::string_view path) const {
        AudioFormat f{};
        if (redfs_audio_probe(h_, hash(path), &f) != REDFS_OK) return std::nullopt;
        return f;
    }

    // --- meshes --------------------------------------------------------------

    /// Cheap: header facts only, no geometry read.
    std::optional<MeshDesc> mesh_desc(std::string_view path) const {
        MeshDesc m{};
        if (redfs_mesh_desc_of(h_, hash(path), &m) != REDFS_OK) return std::nullopt;
        return m;
    }

    /// Full decode including per-chunk bounding boxes. Costs a geometry
    /// decompress unless the mesh is already cached -- see enable_cache.
    std::optional<Mesh> mesh(uint64_t key) const {
        redfs_mesh* m = nullptr;
        if (redfs_mesh_open(h_, key, &m) != REDFS_OK) return std::nullopt;
        return Mesh{m};
    }
    std::optional<Mesh> mesh(std::string_view path) const { return mesh(hash(path)); }

    // --- hash -> path --------------------------------------------------------

    /// Load a path dictionary (WolvenKit's usedhashes.kark, or plain text).
    /// Returns how many entries resolve in this depot. Also switches on
    /// learning paths from CR2W import tables as files are read.
    std::optional<uint32_t> load_paths(const char* list_file) const {
        uint32_t kept = 0;
        if (redfs_path_load(h_, list_file, &kept) != REDFS_OK) return std::nullopt;
        return kept;
    }

    // --- mesh cache ----------------------------------------------------------

    /// Remember decoded meshes in `file`, across process restarts. Discards
    /// itself automatically if the mounted archive set changes.
    Status enable_cache(const char* file) const { return redfs_cache_open(h_, file); }
    static Status flush_cache() { return redfs_cache_flush(); }
    static void close_cache() { redfs_cache_close(); }
    static uint32_t cached_mesh_count() { return redfs_cache_entry_count(); }

    /// Precompute a known set up front, skipping anything already cached.
    std::optional<uint32_t> warm_cache(std::span<const uint64_t> hashes) const {
        uint32_t computed = 0;
        if (redfs_cache_warm(h_, hashes.data(), static_cast<uint32_t>(hashes.size()), &computed) !=
            REDFS_OK)
            return std::nullopt;
        return computed;
    }

    // --- enumeration ---------------------------------------------------------

    /// Visit every file in the depot. `fn` returns false to stop early.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        auto trampoline = [](const FileInfo* info, void* user) -> int {
            return (*static_cast<Fn*>(user))(*info) ? 1 : 0;
        };
        redfs_enumerate(h_, trampoline, &fn);
    }

    // --- async ---------------------------------------------------------------

    /// Read on RedFS's worker thread. `fn` runs on that worker, not here.
    template <typename Fn>
    Status read_async(uint64_t key, uint32_t part, Fn fn) const {
        auto* boxed = new Fn(std::move(fn));
        return redfs_read_async(
            h_, key, part,
            [](Status st, redfs_blob b, void* user) {
                auto* f = static_cast<Fn*>(user);
                (*f)(st, Blob{b});
                delete f;
            },
            boxed);
    }

    /// Block until every queued async read has finished.
    static void drain() { redfs_drain(); }

private:
    redfs_depot* h_ = nullptr;
};

}  // namespace redfs
