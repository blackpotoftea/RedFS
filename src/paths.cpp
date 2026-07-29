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
// Only paths whose hash is actually present in the mounted depot are kept. On a
// stock install that cuts the shipped list down by roughly three quarters, and
// it means a hit always corresponds to a file you can really read.

#include "internal.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace redfs {
namespace {

struct Dictionary {
    std::mutex mutex;
    // One blob of NUL-terminated strings; entries point into it by offset, so
    // there is no per-path allocation.
    std::vector<char> strings;
    struct Entry {
        uint64_t hash;
        uint32_t offset;
    };
    std::vector<Entry> sorted;    // by hash, binary-searched
    std::vector<Entry> pending;   // added since the last sort
    bool enabled = false;
};

Dictionary& dict() {
    static Dictionary* d = new Dictionary();  // never destroyed; see api.cpp
    return *d;
}

void merge_pending(Dictionary& d) {
    if (d.pending.empty()) return;
    d.sorted.insert(d.sorted.end(), d.pending.begin(), d.pending.end());
    d.pending.clear();
    std::sort(d.sorted.begin(), d.sorted.end(),
              [](const Dictionary::Entry& a, const Dictionary::Entry& b) { return a.hash < b.hash; });
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
    const uint32_t offset = static_cast<uint32_t>(d.strings.size());
    d.strings.insert(d.strings.end(), path, path + len);
    d.strings.push_back('\0');
    d.pending.push_back(Dictionary::Entry{hash, offset});
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
    if (decoded_size == 0 || decoded_size > (1u << 31))
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
    std::lock_guard<std::mutex> lock(d.mutex);
    d.enabled = true;

    // Keeping only paths that resolve in this depot trims the shipped list hard
    // and guarantees a hit means a readable file.
    uint32_t kept = 0, seen = 0;
    const char* p = reinterpret_cast<const char*>(text.data());
    const char* end = p + text.size();
    // Reserve only for the first load. Reloading a list adds almost nothing --
    // every path is already interned -- so reserving again would grow capacity
    // on each call without ever using it.
    if (d.strings.empty()) d.strings.reserve(text.size() / 3);

    while (p < end) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
        const char* line_end = nl ? nl : end;
        size_t len = static_cast<size_t>(line_end - p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) --len;

        if (len) {
            ++seen;
            // The list is already normalised, so hash it as-is; fall back to the
            // sanitizing hash if that does not resolve.
            uint64_t h = fnv1a64(p, len);
            bool present = !depot || depot->locate(h, nullptr) ||
                           [&] {
                               const std::string s = sanitize_path(p, len);
                               const uint64_t h2 = fnv1a64(s.data(), s.size());
                               if (depot->locate(h2, nullptr)) {
                                   h = h2;
                                   return true;
                               }
                               return false;
                           }();
            if (present) {
                add_locked(d, p, len, h);
                ++kept;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    merge_pending(d);

    log("paths: %u of %u entries from %s resolve in this depot (%.1f MB of strings)", kept, seen,
        file, d.strings.size() / 1048576.0);
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
    if (!d.enabled) return;
    std::lock_guard<std::mutex> lock(d.mutex);
    for (const auto& imp : f->imports) {
        const char* s = f->str(imp.str_offset);
        const size_t len = std::strlen(s);
        if (len) add_locked(d, s, len, fnv1a64(s, len));
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
    return d.strings.data() + it->offset;
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
    d.enabled = true;
}

}  // namespace redfs
