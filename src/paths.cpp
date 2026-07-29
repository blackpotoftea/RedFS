// Hash -> depot path.
//
// Archives store only the 64-bit hash; the string is gone at cook time and FNV1a
// is one-way. So a reverse lookup can only ever be a dictionary of strings whose
// hash is known. RedFS fills one from three sources, cheapest first:
//
//   1. CR2W import tables. Every cooked resource names its dependencies as real
//      path strings. Reading any file therefore teaches us paths for free, and
//      this covers modded content that no shipped dictionary knows about.
//   2. A path list on disk -- WolvenKit's usedhashes.kark, or any plain text
//      file of one path per line. This is the bulk source.
//   3. Anything the caller adds by hand.
//
// Source 2 keeps only paths whose hash is actually present in the mounted depot.
// On a stock install that cuts the shipped list down by roughly three quarters,
// and for that source a hit does correspond to a file you can really read.
//
// Sources 1 and 3 are NOT filtered, and cannot be. paths_learn_imports is called
// from cr2w_parse, which has no depot -- redfs_cr2w_open genuinely has none to
// give -- and paths_add receives only a string. So an import naming a dependency
// you did not mount is still learned, and redfs_path_from_hash can return a path
// that a subsequent read reports as REDFS_E_NOT_FOUND. Treat a hit as "this is
// what the file is called", not as "this file is readable".

#include "internal.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>

namespace redfs {
namespace {

// Interned strings live in fixed blocks that are never moved and never freed, so
// a pointer handed to a caller stays valid for the lifetime of the process --
// which is exactly what redfs.h:195-196 promises.
//
// A single growing std::vector<char> cannot keep that promise. Every insert may
// reallocate, and interior pointers previously returned by redfs_path_from_hash
// become dangling. That is invisible in small tests -- two successive lookups
// with nothing in between look perfectly stable -- and the documented usage
// pattern (resolve a hash to a path, then open the mesh) is precisely the
// sequence that breaks it, because opening a mesh parses a CR2W and learns its
// imports.
class Arena {
public:
    const char* intern(const char* s, size_t len) {
        const size_t need = len + 1;
        if (need > kBlockSize) {
            // A path longer than a whole block gets its own exact allocation.
            // Deliberately not adopted as the current block: it is already full.
            blocks_.push_back(std::make_unique<char[]>(need));
            return write(blocks_.back().get(), s, len);
        }
        if (!cur_ || used_ + need > kBlockSize) {
            blocks_.push_back(std::make_unique<char[]>(kBlockSize));
            cur_ = blocks_.back().get();
            used_ = 0;
        }
        char* p = cur_ + used_;
        used_ += need;
        return write(p, s, len);
    }

    size_t bytes() const { return bytes_; }

private:
    char* write(char* p, const char* s, size_t len) {
        std::memcpy(p, s, len);
        p[len] = '\0';
        bytes_ += len + 1;
        return p;
    }

    static constexpr size_t kBlockSize = 1u << 20;
    // Only the vector of owning pointers ever reallocates; the blocks it points
    // at do not move, so `cur_` and every interned pointer survive that.
    std::vector<std::unique_ptr<char[]>> blocks_;
    char* cur_ = nullptr;
    size_t used_ = 0;
    size_t bytes_ = 0;
};

struct Dictionary {
    std::mutex mutex;
    Arena strings;
    struct Entry {
        uint64_t hash;
        const char* str;  // into the arena; stable forever
    };
    std::vector<Entry> sorted;    // by hash, binary-searched
    std::vector<Entry> pending;   // added since the last merge
    // Read without the lock for the early-out in paths_learn_imports, so it has
    // to be atomic -- same reasoning as g_log in api.cpp.
    std::atomic<bool> enabled{false};
};

Dictionary& dict() {
    static Dictionary* d = new Dictionary();  // never destroyed; see api.cpp
    return *d;
}

void merge_pending(Dictionary& d) {
    if (d.pending.empty()) return;
    const auto by_hash = [](const Dictionary::Entry& a, const Dictionary::Entry& b) {
        return a.hash < b.hash;
    };

    // Sort only the new run and merge it in. Re-sorting the whole dictionary
    // every 4096 inserts made a full load quadratic: std::sort is introsort and
    // does not exploit the fact that all but the trailing few thousand elements
    // are already ordered.
    //
    // Honest about what this buys: inplace_merge is linear per call, but it is
    // still called O(N/B) times, so the total remains O(N^2/B) -- a much smaller
    // constant, not a better complexity class. Making it genuinely linear needs a
    // merge threshold proportional to N, which in turn needs `contains` to stop
    // scanning `pending` linearly. At the documented dictionary size (~544k
    // kept entries) neither is worth the extra structure.
    std::sort(d.pending.begin(), d.pending.end(), by_hash);
    const size_t mid = d.sorted.size();
    d.sorted.insert(d.sorted.end(), d.pending.begin(), d.pending.end());
    d.pending.clear();
    std::inplace_merge(d.sorted.begin(), d.sorted.begin() + mid, d.sorted.end(), by_hash);
    d.sorted.erase(std::unique(d.sorted.begin(), d.sorted.end(),
                               [](const Dictionary::Entry& a, const Dictionary::Entry& b) {
                                   return a.hash == b.hash;
                               }),
                   d.sorted.end());
}

// Caller holds the lock.
bool contains(Dictionary& d, uint64_t hash) {
    auto it = std::lower_bound(
        d.sorted.begin(), d.sorted.end(), hash,
        [](const Dictionary::Entry& e, uint64_t h) { return e.hash < h; });
    if (it != d.sorted.end() && it->hash == hash) return true;
    for (const auto& e : d.pending)
        if (e.hash == hash) return true;
    return false;
}

void add_locked(Dictionary& d, const char* path, size_t len, uint64_t hash) {
    if (contains(d, hash)) return;
    d.pending.push_back(Dictionary::Entry{hash, d.strings.intern(path, len)});
    if (d.pending.size() > 4096) merge_pending(d);
}

// Reads a path list: either a raw KARK stream (as WolvenKit ships it) or plain
// text. Returns the decompressed bytes.
redfs_status read_list(const char* file, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(file, "rb");
    if (!f) return fail(REDFS_E_IO, "cannot open %s", file);
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 8) {
        std::fclose(f);
        return fail(REDFS_E_CORRUPT, "%s is too small to be a path list", file);
    }
    std::vector<uint8_t> raw(static_cast<size_t>(len));
    const size_t got = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    if (got != raw.size()) return fail(REDFS_E_IO, "short read from %s", file);

    if (rd32(raw.data()) != kKarkMagic) {  // plain text
        *out = std::move(raw);
        return REDFS_OK;
    }

    const uint32_t decoded_size = rd32(raw.data() + 4);
    // 256 MB is already about twice the largest list this project documents, and
    // the resize below both commits and zero-fills whatever this says. The old
    // bound of 2^31 let a nine-byte file demand 2 GB before Oodle was ever asked
    // whether the remaining bytes could decode to that.
    if (decoded_size == 0 || decoded_size > (1u << 28))
        return fail(REDFS_E_CORRUPT, "%s declares an implausible size of %u", file, decoded_size);
    if (!oodle::available())
        return fail(REDFS_E_OODLE, "%s is Kraken-compressed but Oodle is unavailable", file);

    out->resize(decoded_size);
    const int64_t produced = oodle::decompress(raw.data() + 8, static_cast<int64_t>(raw.size()) - 8,
                                               out->data(), decoded_size);
    if (produced != static_cast<int64_t>(decoded_size))
        return fail(REDFS_E_OODLE, "%s decoded to %lld bytes, expected %u", file,
                    static_cast<long long>(produced), decoded_size);
    return REDFS_OK;
}

}  // namespace

redfs_status paths_load(const redfs_depot* depot, const char* file, uint32_t* out_kept) {
    if (!file) return REDFS_E_INVALID_ARG;
    if (out_kept) *out_kept = 0;

    std::vector<uint8_t> text;
    const redfs_status st = read_list(file, &text);
    if (st != REDFS_OK) return st;

    Dictionary& d = dict();
    uint32_t kept = 0, seen = 0;
    size_t interned = 0;

    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.enabled.store(true, std::memory_order_relaxed);

        // Keeping only paths that resolve in this depot trims the shipped list
        // hard, and for this source a hit does mean a readable file.
        const char* p = reinterpret_cast<const char*>(text.data());
        const char* end = p + text.size();

        while (p < end) {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
            const char* line_end = nl ? nl : end;
            size_t len = static_cast<size_t>(line_end - p);
            while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) --len;

            if (len) {
                ++seen;
                // The list is already normalised, so hash it as-is; fall back to
                // the sanitizing hash if that does not resolve.
                //
                // Whichever form produced the winning hash is also the form that
                // gets interned. Storing the raw line under a sanitized key gave
                // callers back text that was not the canonical path -- including
                // any leading quote or space, which the trim above does not strip
                // but sanitize_path does, and which is exactly the kind of line
                // that takes this fallback.
                uint64_t h = fnv1a64(p, len);
                const char* text_ptr = p;
                size_t text_len = len;
                std::string canonical;

                bool present = depot->locate(h, nullptr);
                if (!present) {
                    canonical = sanitize_path(p, len);
                    const uint64_t h2 = fnv1a64(canonical.data(), canonical.size());
                    if (depot->locate(h2, nullptr)) {
                        h = h2;
                        text_ptr = canonical.data();
                        text_len = canonical.size();
                        present = true;
                    }
                }
                if (present) {
                    add_locked(d, text_ptr, text_len, h);
                    ++kept;
                }
            }
            if (!nl) break;
            p = nl + 1;
        }
        merge_pending(d);
        interned = d.strings.bytes();
    }

    // Emitted with the lock released: the sink is host code on this thread and
    // may call straight back into redfs_path_count.
    log("paths: %u of %u entries from %s resolve in this depot (%.1f MB of strings)", kept, seen,
        file, interned / 1048576.0);
    if (out_kept) *out_kept = kept;
    return REDFS_OK;
}

void paths_add(const char* path) {
    if (!path || !*path) return;
    const std::string s = sanitize_path(path, std::strlen(path));
    if (s.empty()) return;
    Dictionary& d = dict();
    std::lock_guard<std::mutex> lock(d.mutex);
    add_locked(d, s.data(), s.size(), fnv1a64(s.data(), s.size()));
}

// Called after every CR2W parse: import tables are free path strings, and they
// are the only source that covers paths invented by mods.
void paths_learn_imports(const redfs_cr2w* f) {
    Dictionary& d = dict();
    if (!d.enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(d.mutex);
    for (const auto& imp : f->imports) {
        const char* s = f->str(imp.str_offset);
        const size_t len = std::strlen(s);
        if (!len) continue;
        // Sanitize before hashing. Import strings are archive content and are
        // not guaranteed canonical; hashing "Base/Foo.mesh" raw files it under a
        // key redfs_hash can never produce, so the entry is permanently dead and
        // the real path stays unresolvable. paths_add and redfs_hash both
        // sanitize -- this was the one source that did not.
        const std::string canonical = sanitize_path(s, len);
        if (canonical.empty()) continue;
        add_locked(d, canonical.data(), canonical.size(),
                   fnv1a64(canonical.data(), canonical.size()));
    }
}

const char* path_from_hash(uint64_t hash) {
    Dictionary& d = dict();
    std::lock_guard<std::mutex> lock(d.mutex);
    merge_pending(d);
    auto it = std::lower_bound(
        d.sorted.begin(), d.sorted.end(), hash,
        [](const Dictionary::Entry& e, uint64_t h) { return e.hash < h; });
    if (it == d.sorted.end() || it->hash != hash) return nullptr;
    // Points into the arena, which never moves or frees, so this stays valid for
    // the lifetime of the process as documented.
    return it->str;
}

uint32_t paths_count() {
    Dictionary& d = dict();
    std::lock_guard<std::mutex> lock(d.mutex);
    merge_pending(d);
    return static_cast<uint32_t>(d.sorted.size());
}

void paths_enable() {
    Dictionary& d = dict();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.enabled.store(true, std::memory_order_relaxed);
}

}  // namespace redfs
