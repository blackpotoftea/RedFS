// RedFS -- C++ conveniences over the C ABI in redfs.h.
//
// Header-only and optional: everything here is a thin wrapper, so mixing the two
// layers is fine. The C ABI is what stays stable across compilers; this is what
// you actually want to type.
//
//   redfs::Depot depot = redfs::Depot::open().value();
//   auto dds = depot.texture_dds("base\\icon\\common\\ico_scanner.xbm");
//   DirectX::CreateDDSTextureFromMemory(device, dds->data(), dds->size(), ...);
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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

/// True when the loaded RedFS.dll matches the header this was compiled against.
///
/// Worth checking because a mismatch is silent: redfs_mesh_chunk has only ever
/// grown by appending, so field reads still land correctly and only the STRIDE
/// is wrong -- chunks() then walks a 48-byte array at 44-byte steps and returns
/// plausible garbage from the second element on. Depot::open checks this for
/// you; call it directly if you use the C API.
inline bool abi_ok() { return redfs_abi_version() == REDFS_ABI_VERSION; }
inline std::string last_error() { return redfs_last_error(); }

inline uint64_t hash(std::string_view path) { return redfs_hash_n(path.data(), path.size()); }

inline std::string hash_string(std::string_view path) {
    return std::to_string(hash(path));
}
inline uint64_t hash_parse(const std::string& decimal) {
    return redfs_hash_parse(decimal.c_str());
}

/// Empty rather than null when the hash is unknown. The view does not borrow
/// anything you own: redfs.h interns these strings for the life of the process,
/// so it stays valid however the dictionary grows afterwards.
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

    /// The views borrow this Cr2w, not the vector.
    std::vector<std::string_view> imports() const {
        std::vector<std::string_view> out;
        const uint32_t n = redfs_cr2w_import_count(h_);
        out.reserve(n);
        for (uint32_t i = 0; i < n; ++i) out.emplace_back(redfs_cr2w_import_path(h_, i));
        return out;
    }

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

    /// `fn` returns false to stop early.
    template <typename Fn>
    void walk(uint32_t chunk, const char* prop_path, Fn&& fn) const {
        // remove_reference_t: with a forwarding reference and an lvalue callable
        // Fn deduces to L&, and `L&*` is not a type -- MSVC C2528. Without it,
        // an inline lambda compiles and a named one does not.
        //
        // remove_cv_t on top of that: a CONST named callable (or a const
        // std::function) deduces Fn to `const L&`, making `&fn` a `const L*`
        // that will not convert to the void* the C ABI takes. Same symptom --
        // inline compiles, named does not -- one step further along.
        using Callable = std::remove_cv_t<std::remove_reference_t<Fn>>;
        auto trampoline = [](const char* name, const Value* v, void* user) -> int {
            return (*static_cast<Callable*>(user))(std::string_view{name}, *v) ? 1 : 0;
        };
        redfs_cr2w_walk(h_, chunk, prop_path, trampoline,
                        const_cast<void*>(static_cast<const void*>(&fn)));
    }

    redfs_cr2w* handle() const { return h_; }

private:
    redfs_cr2w* h_ = nullptr;
};

/// One file: its bytes, its document and its buffers, owned together.
///
/// Replaces holding a Blob and a Cr2w in the right order by hand -- the document
/// borrows the bytes, so the pairing was always a rule the caller had to keep.
/// Opens any file, not only CR2W documents: for a .wem or .bnk, type() is empty
/// and cr2w() is null while data()/size() still work.
class Resource {
public:
    Resource() = default;
    explicit Resource(redfs_resource* h) : h_(h) {}
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    Resource(Resource&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Resource& operator=(Resource&& o) noexcept {
        if (this != &o) {
            if (h_) redfs_close(h_);
            h_ = std::exchange(o.h_, nullptr);
        }
        return *this;
    }
    ~Resource() {
        if (h_) redfs_close(h_);
    }

    explicit operator bool() const { return h_ != nullptr; }

    /// "CMesh", "CBitmapTexture", or empty when the file is not a CR2W document.
    std::string_view type() const { return redfs_resource_type(h_); }
    uint64_t key() const { return redfs_resource_hash(h_); }

    /// The main segment. Borrows this Resource -- do not let it outlive one.
    std::span<const uint8_t> bytes() const {
        return {redfs_resource_data(h_), static_cast<size_t>(redfs_resource_size(h_))};
    }

    /// Null for a non-CR2W file. Borrowed, like bytes().
    const redfs_cr2w* cr2w() const { return redfs_resource_cr2w(h_); }

    uint32_t buffer_count() const { return redfs_resource_buffer_count(h_); }

    /// 0..buffer_count-1, the same numbering as Value::as.buffer and the
    /// descriptors' buffer_index. nullopt past the end -- never a different
    /// segment, and never the document.
    std::optional<Blob> buffer(uint32_t index) const {
        redfs_blob b{};
        if (redfs_resource_buffer(h_, index, &b) != REDFS_OK) return std::nullopt;
        return Blob{b};
    }

    redfs_resource* handle() const { return h_; }

private:
    redfs_resource* h_ = nullptr;
};

/// A decoded mesh: chunk table, LODs, appearances and per-chunk bounds.
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

    /// The span BORROWS this Mesh -- do not let it outlive the handle. In
    /// particular `depot.mesh(p)->chunks()` dangles: the optional temporary dies
    /// at the end of the full expression. Bind the Mesh to a named variable
    /// first. (With a cache open the data survives on the cache's reference and
    /// the mistake appears to work, which is worse than a clean crash.)
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

    static std::optional<Depot> open(const char* game_dir = nullptr,
                                     uint32_t flags = REDFS_SCAN_ALL) {
        // Refusing here is the only point at which an ABI mismatch is still
        // diagnosable -- see abi_ok. It is also the only failure in this header
        // that never reaches the DLL, so last_error() will not explain it.
        if (!abi_ok()) return std::nullopt;
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

    std::optional<Blob> read(uint64_t key, uint32_t part = REDFS_PART_ALL) const {
        redfs_blob b{};
        if (redfs_read(h_, key, part, &b) != REDFS_OK) return std::nullopt;
        return Blob{b};
    }
    std::optional<Blob> read(std::string_view path, uint32_t part = REDFS_PART_ALL) const {
        return read(hash(path), part);
    }

    /// One entry per file whose path matches `pattern`. Reads nothing.
    ///
    /// nullopt on failure rather than an empty vector, so "nothing was ever
    /// loaded" (REDFS_E_NO_DICTIONARY) and "nothing matched" stay
    /// distinguishable. last_error() says which -- except the null-handle case
    /// below, which never reaches the DLL.
    ///
    /// The views borrow nothing you own -- redfs.h interns dictionary strings for
    /// the life of the process -- so the vector is safe to keep and to outlive
    /// this Depot.
    struct Match {
        uint64_t key;
        std::string_view path;
    };
    std::optional<std::vector<Match>> find(std::string_view pattern) const {
        // A null handle would reach redfs_find as "no depot", which is a real
        // mode meaning "search unfiltered" -- so a moved-from or default Depot
        // would silently widen the query instead of failing like every other
        // method here does. find_unfiltered below is how you ask for it.
        if (!h_) return std::nullopt;
        return collect_in(h_, pattern);
    }

    /// Callback form, for when you do not want the vector materialised on your
    /// side. `fn` returns false to stop DELIVERY -- the search itself has already
    /// finished, see redfs.h -- and the result is the total match count, which is
    /// what it would have been either way. nullopt on failure.
    template <typename Fn>
    std::optional<uint32_t> find(std::string_view pattern, Fn&& fn) const {
        if (!h_) return std::nullopt;  // see the overload above
        return find_in(h_, pattern, std::forward<Fn>(fn));
    }

    /// Search the dictionary WITHOUT filtering to this depot -- the C API's
    /// null-depot mode. Reports paths learned from CR2W imports or added by hand
    /// that no mounted archive holds, which is how you see what a mod calls
    /// things before deciding whether to mount it.
    ///
    /// Static because it needs no depot; the filtered overloads above refuse a
    /// null handle rather than silently widening into this.
    static std::optional<std::vector<Match>> find_unfiltered(std::string_view pattern) {
        return collect_in(nullptr, pattern);
    }

    template <typename Fn>
    static std::optional<uint32_t> find_unfiltered(std::string_view pattern, Fn&& fn) {
        return find_in(nullptr, pattern, std::forward<Fn>(fn));
    }

private:
    /// Shared body of the collecting searches; `depot` null means unfiltered.
    static std::optional<std::vector<Match>> collect_in(const redfs_depot* depot,
                                                        std::string_view pattern) {
        const std::string pat{pattern};
        std::vector<Match> out;
        auto sink = [](uint64_t key, const char* path, void* user) -> int {
            static_cast<std::vector<Match>*>(user)->push_back({key, std::string_view{path}});
            return 1;
        };
        if (redfs_find(depot, pat.c_str(), sink, &out, nullptr) != REDFS_OK) return std::nullopt;
        return out;
    }

    /// Shared body of the callback-form searches. See Cr2w::walk for why
    /// remove_cv_t/remove_reference_t and the const_cast are all required.
    template <typename Fn>
    static std::optional<uint32_t> find_in(const redfs_depot* depot, std::string_view pattern,
                                           Fn&& fn) {
        const std::string pat{pattern};
        using Callable = std::remove_cv_t<std::remove_reference_t<Fn>>;
        auto trampoline = [](uint64_t key, const char* path, void* user) -> int {
            return (*static_cast<Callable*>(user))(key, std::string_view{path}) ? 1 : 0;
        };
        uint32_t matched = 0;
        if (redfs_find(depot, pat.c_str(), trampoline,
                       const_cast<void*>(static_cast<const void*>(&fn)), &matched) != REDFS_OK)
            return std::nullopt;
        return matched;
    }

public:

    std::optional<Blob> read_main(std::string_view path) const {
        return read(hash(path), REDFS_PART_MAIN);
    }
    std::optional<Blob> read_buffer(std::string_view path, uint32_t index) const {
        return read(hash(path), index);
    }

    /// DEPRECATED in favour of Depot::open. Returns the Blob as well as the Cr2w
    /// because the Cr2w only borrows those bytes -- drop the Blob and the
    /// document is reading freed memory. Resource makes that unrepresentable
    /// rather than making you remember it, and it does not fail on a file that
    /// has no CR2W at all.
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

    /// One file: its bytes, its document and its buffers, with no part number to
    /// get wrong. See redfs.h. nullopt if the file is not in the depot or its
    /// main segment will not read; a file that simply has no CR2W still opens,
    /// with type() empty and cr2w() null.
    ///
    /// Named `resource`, not `open`, because Depot::open is already the static
    /// factory that mounts an install -- and an instance overload of it resolves
    /// to the factory for a `const char*` argument, silently, since that matches
    /// better than string_view.
    std::optional<Resource> resource(uint64_t key) const {
        redfs_resource* r = nullptr;
        if (redfs_open(h_, key, &r) != REDFS_OK) return std::nullopt;
        return Resource{r};
    }
    std::optional<Resource> resource(std::string_view path) const { return resource(hash(path)); }

    // --- textures ------------------------------------------------------------

    std::optional<TextureDesc> texture_desc(uint64_t key) const {
        TextureDesc d{};
        if (redfs_texture_desc_of(h_, key, &d) != REDFS_OK) return std::nullopt;
        return d;
    }
    std::optional<TextureDesc> texture_desc(std::string_view path) const {
        return texture_desc(hash(path));
    }

    std::optional<Blob> texture_dds(uint64_t key) const {
        redfs_blob b{};
        if (redfs_texture_read_dds(h_, key, &b) != REDFS_OK) return std::nullopt;
        return Blob{b};
    }
    std::optional<Blob> texture_dds(std::string_view path) const { return texture_dds(hash(path)); }

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

    std::optional<MeshDesc> mesh_desc(std::string_view path) const {
        MeshDesc m{};
        if (redfs_mesh_desc_of(h_, hash(path), &m) != REDFS_OK) return std::nullopt;
        return m;
    }

    std::optional<Mesh> mesh(uint64_t key) const {
        redfs_mesh* m = nullptr;
        if (redfs_mesh_open(h_, key, &m) != REDFS_OK) return std::nullopt;
        return Mesh{m};
    }
    std::optional<Mesh> mesh(std::string_view path) const { return mesh(hash(path)); }

    // --- hash -> path --------------------------------------------------------

    /// The uint32_t is redfs.h's out_kept: how many of the file's paths resolve
    /// in this depot, not how many it contained.
    std::optional<uint32_t> load_paths(const char* list_file) const {
        uint32_t kept = 0;
        if (redfs_path_load(h_, list_file, &kept) != REDFS_OK) return std::nullopt;
        return kept;
    }

    // --- mesh cache ----------------------------------------------------------

    /// Opening is per-depot but the rest is static, mirroring redfs.h: there is
    /// one cache per process, owned by whichever depot opened it.
    Status enable_cache(const char* file) const { return redfs_cache_open(h_, file); }
    static Status flush_cache() { return redfs_cache_flush(); }
    static void close_cache() { redfs_cache_close(); }
    static uint32_t cached_mesh_count() { return redfs_cache_entry_count(); }

    std::optional<uint32_t> warm_cache(std::span<const uint64_t> hashes) const {
        uint32_t computed = 0;
        if (redfs_cache_warm(h_, hashes.data(), static_cast<uint32_t>(hashes.size()), &computed) !=
            REDFS_OK)
            return std::nullopt;
        return computed;
    }

    // --- path cache ----------------------------------------------------------

    /// Restores the hash -> path dictionary from disk instead of relearning it,
    /// and records which archives it has already read. Unlike the mesh cache it
    /// is never discarded, only merged into -- see redfs.h.
    Status enable_path_cache(const char* file) const { return redfs_path_cache_open(h_, file); }
    static Status flush_path_cache() { return redfs_path_cache_flush(); }
    static void close_path_cache() { redfs_path_cache_close(); }

    /// Indices of the mounted archives not yet harvested into the dictionary.
    /// Empty on failure, which for this call is only "no path cache is open".
    std::vector<uint32_t> unharvested_archives() const {
        uint32_t n = 0;
        if (redfs_path_cache_pending(h_, nullptr, 0, &n) != REDFS_OK) return {};
        const uint32_t capacity = n;
        std::vector<uint32_t> out(capacity);
        // Sized from the first call's TOTAL, so the second cannot overflow it.
        if (capacity && redfs_path_cache_pending(h_, out.data(), capacity, &n) != REDFS_OK)
            return {};
        // min, not n: a mount between the calls raises the total, and resizing
        // UP would zero-fill the tail and report archive 0 as needing work.
        out.resize(n < capacity ? n : capacity);
        return out;
    }

    /// Call after finishing one archive, not after the whole sweep.
    Status mark_archive_harvested(uint32_t archive_index) const {
        return redfs_path_cache_mark(h_, archive_index);
    }

    // --- enumeration ---------------------------------------------------------

    /// `fn` returns false to stop early.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        // See Cr2w::walk for why remove_reference_t and the const_cast are both
        // required here.
        using Callable = std::remove_cv_t<std::remove_reference_t<Fn>>;
        auto trampoline = [](const FileInfo* info, void* user) -> int {
            return (*static_cast<Callable*>(user))(*info) ? 1 : 0;
        };
        redfs_enumerate(h_, trampoline, const_cast<void*>(static_cast<const void*>(&fn)));
    }

    // --- async ---------------------------------------------------------------

    /// `fn` runs on RedFS's worker thread, not here. It is moved onto the heap
    /// and destroyed by the callback, so it may be a temporary -- but whatever
    /// it captures by reference has to still be alive when the worker runs it.
    ///
    /// This Depot need not outlive the read: destroying it cancels anything
    /// still queued against it and the callback fires with REDFS_E_CANCELLED
    /// rather than data.
    template <typename Fn>
    Status read_async(uint64_t key, uint32_t part, Fn fn) const {
        auto* boxed = new Fn(std::move(fn));
        const Status st = redfs_read_async(
            h_, key, part,
            [](Status s, redfs_blob b, void* user) {
                auto* f = static_cast<Fn*>(user);
                (*f)(s, Blob{b});
                delete f;
            },
            boxed);
        // The callback owns `boxed` only once the job is queued. On any other
        // status redfs.h guarantees it will not fire, so nothing would ever free
        // it -- one leaked closure, plus whatever it captured, per refused call.
        if (st != REDFS_OK) delete boxed;
        return st;
    }

    static void drain() { redfs_drain(); }

private:
    redfs_depot* h_ = nullptr;
};

}  // namespace redfs
