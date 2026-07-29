// Hash -> depot path.
//
// Archives store only the 64-bit FNV1a hash and it is one-way, so a reverse
// lookup can only ever be a dictionary of strings whose hash is known. Three
// sources fill it: CR2W import tables (free on every parse, and the only source
// covering paths a mod invented), a path list on disk, and paths_add. redfs.h
// documents what callers may assume about each.
//
// Only the on-disk list is filtered against the depot. The other two cannot be:
// paths_learn_imports runs inside cr2w_parse, which has no depot to hand it, and
// paths_add receives only a string. A hit therefore means "this is what the file
// is called", not "this file is readable".

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
// exactly what redfs_path_from_hash promises in redfs.h.
//
// A single growing std::vector<char> cannot keep that promise: any insert may
// reallocate and dangle interior pointers already returned. Small tests miss it,
// while the documented usage pattern hits it -- resolve a hash, then open the
// mesh, and opening the mesh parses a CR2W and learns its imports.
class Arena {
public:
    const char* intern(const char* s, size_t len) {
        const size_t need = len + 1;
        if (need > kBlockSize) {
            // Oversized path gets an exact block of its own. Not adopted as
            // cur_: it has no room left.
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
    // This vector reallocates; the blocks it owns never move.
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
    // Read without the lock for the early-out in paths_learn_imports.
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

    // Sort only the new run and merge it in. Re-sorting the whole dictionary on
    // every flush makes a full load quadratic: introsort cannot exploit the fact
    // that all but the trailing few thousand elements are already ordered.
    //
    // This lowers the constant, not the complexity class -- still O(N/B) merges,
    // and `contains` still scans `pending` linearly. At the documented dictionary
    // size (~544k kept entries) neither is worth the extra structure.
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
    // The resize below commits and zero-fills whatever this declares, before
    // Oodle is ever asked whether the remaining bytes could decode to that -- so
    // under a loose bound a nine-byte file makes us allocate gigabytes. 256 MB is
    // roughly twice WolvenKit's usedhashes.kark, which decompresses to ~135 MB.
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

        const char* p = reinterpret_cast<const char*>(text.data());
        const char* end = p + text.size();

        while (p < end) {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
            const char* line_end = nl ? nl : end;
            size_t len = static_cast<size_t>(line_end - p);
            while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) --len;

            if (len) {
                ++seen;
                // The list is already normalised, so hash it as-is and only fall
                // back to the sanitizing hash if that does not resolve. Whichever
                // form produced the winning hash must also be the form interned:
                // a line taking the fallback typically has a leading quote or
                // space, which the trim above leaves but sanitize_path removes,
                // so interning the raw line would hand callers back text that is
                // not the canonical path.
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
        // Sanitize before hashing. Import strings are archive content and are not
        // guaranteed canonical; hashing "Base/Foo.mesh" raw files it under a key
        // redfs_hash can never produce, so the entry is permanently dead and the
        // real path stays unresolvable.
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
