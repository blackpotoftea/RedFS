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
#include <unordered_set>

#include <io.h>
#include <windows.h>

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

// Every intern goes through here, which is why the length bound lives here and
// not at any one caller. glob_match costs O(min(|pattern|,|entry|) x |entry|)
// against an entry with a long single-character run, and this dictionary is
// process-global: one absurd entry taxes every later redfs_find for the life of
// the process, under the same lock. Measured on a 50k-character entry: 810 ms
// per search; 64k: 1.3 s; 256k: 21.9 s.
//
// Of the three routes that intern, only paths_add is fed by HOST code:
// paths_load takes an on-disk list, and paths_learn_imports takes CR2W import
// tables read straight out of whatever .archive a downloaded mod ships.
//
// 1024 is generous: WolvenKit's usedhashes.kark decompresses to 1,723,496 lines
// with a mean of 76.6 characters and a longest of 199. Nothing real is near it.
constexpr size_t kMaxPathLength = 1024;

void add_locked(Dictionary& d, const char* path, size_t len, uint64_t hash) {
    if (len > kMaxPathLength) {
        log("paths: ignoring a %zu-character path (limit %zu)", len, kMaxPathLength);
        return;
    }
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
    const uint64_t compressed = static_cast<uint64_t>(raw.size()) - 8;  // len > 8 checked above

    // The resize below commits and zero-fills whatever this header declares,
    // before Oodle is ever asked whether the remaining bytes could decode to it,
    // so the declared size has to be bounded first. What it must be bounded
    // AGAINST is the expansion ratio, not an absolute size: the case this stops
    // is a nine-byte file claiming gigabytes, and that is an expansion of ~10^8.
    //
    // An absolute cap measures the wrong quantity. The 256 MB one that stood
    // here was sized at "roughly twice usedhashes.kark" and rejected WolvenKit's
    // red.kark -- 417 MiB from 86 MB on disk, an expansion of 4.9x, the LOWEST
    // of any list WolvenKit ships -- while still accepting noderefs.kark at
    // 53.6x. Measured across the resources WolvenKit ships: 4.9x, 6.0x, 15-20x,
    // 26.1x, 39.8x, 53.6x. 256x leaves nearly 5x headroom over the worst real
    // case and still stops the nine-byte file at 256 bytes.
    constexpr uint64_t kMaxExpansion = 256;
    // Backstop on the other axis, and the one that actually bounds the
    // allocation below. Note where the two meet: the ratio can only decide when
    // compressed * 256 < kMaxDecoded, i.e. for inputs under 2 MiB. Above that
    // this constant decides alone -- including for red.kark at 86 MB. That is
    // not a flaw in the ratio, whose job is the tiny-file-declares-gigabytes
    // case; it is the reason this number, not the ratio, is what to think about
    // when asking how much RedFS may commit before Oodle has validated anything.
    //
    // 512 MiB, not more: this constant IS the worst case, so raising it raises
    // the exact thing it bounds. A 4 MiB file declaring 256x clears the ratio
    // check and gets the whole ceiling committed and zero-filled at the resize
    // below before Oodle sees a byte. 512 MiB clears the largest real list
    // (red.kark, 417 MiB).
    constexpr uint64_t kMaxDecoded = 512ull << 20;

    if (decoded_size == 0)
        return fail(REDFS_E_CORRUPT, "%s declares a decompressed size of zero", file);
    // REDFS_E_UNSUPPORTED, not REDFS_E_CORRUPT: a list past either bound is a
    // perfectly valid file this build declines to hold, and calling that
    // "corrupt data" sends the caller to check the file instead of the limit.
    // Both numbers go in the message so one read ends the question.
    if (decoded_size > kMaxDecoded)
        return fail(REDFS_E_UNSUPPORTED,
                    "%s decompresses to %u bytes, past the %llu byte limit for one path list",
                    file, decoded_size, static_cast<unsigned long long>(kMaxDecoded));
    if (decoded_size > compressed * kMaxExpansion)
        return fail(REDFS_E_UNSUPPORTED,
                    "%s claims %u bytes from %llu compressed, an expansion of %.1fx past the %llux "
                    "this accepts",
                    file, decoded_size, static_cast<unsigned long long>(compressed),
                    static_cast<double>(decoded_size) / static_cast<double>(compressed),
                    static_cast<unsigned long long>(kMaxExpansion));
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

namespace {

// '*' spans separators, unlike a shell glob -- see redfs.h. Both arguments are
// already normalised (lowercase, '\' separators), so a raw char compare is the
// case-insensitive one.
//
// Iterative rather than the obvious recursion: `pattern` is caller input, and
// the recursive form recurses once per '*' backtrack, so "*a*a*a*a*a*a*b"
// against a long non-matching path is a stack overflow rather than a slow
// match. This is the standard two-pointer form -- remember where the last '*'
// was and where the text stood, and on a mismatch resume one character later.
//
// The '*' test MUST come before the literal compare. Reversed, a '*' in the
// PATTERN facing a literal '*' in the TEXT satisfies `*pattern == *text` and is
// consumed one-for-one, so `star` is never set and the backtrack point is lost
// for the rest of the match -- glob_match("*", "*a") is false. Entries can hold
// a '*': sanitize_path strips neither wildcard, and paths_add and
// paths_learn_imports intern arbitrary strings.
bool glob_match(const char* pattern, const char* text) {
    const char* star = nullptr;   // last '*' seen in the pattern
    const char* resume = nullptr; // where in the text to retry after it

    while (*text) {
        if (*pattern == '*') {
            star = pattern++;  // provisionally match nothing
            resume = text;
        } else if (*pattern == '?' || *pattern == *text) {
            ++pattern;
            ++text;
        } else if (star) {
            pattern = star + 1;  // that star must swallow one more character
            text = ++resume;
        } else {
            return false;
        }
    }
    // Trailing stars may match the empty remainder; nothing else may.
    while (*pattern == '*') ++pattern;
    return *pattern == '\0';
}

}  // namespace

redfs_status paths_find(const redfs_depot* depot, const char* pattern, redfs_find_fn fn,
                        void* user, uint32_t* out_matched) {
    // Zeroed before any early return, so a caller that ignores the status still
    // reads 0 rather than whatever the variable held.
    if (out_matched) *out_matched = 0;
    if (!pattern || !fn) return REDFS_E_INVALID_ARG;

    // Trim the pattern's own trailing noise FIRST, then test for a separator,
    // then sanitize. Both other orders break the trailing-separator shorthand:
    // the RAW pattern's last byte is whatever followed the separator (quote,
    // space, CR/LF), and the SANITIZED pattern has already had its separator
    // eaten. Either way the shorthand degrades to an exact match on a directory
    // name no file can equal -- a silent 0 with REDFS_OK.
    //
    // Tab is trimmed here rather than by widening sanitize_path's set, which
    // would change redfs_hash for every caller.
    const char* end = pattern + std::strlen(pattern);
    const auto is_noise = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\'';
    };
    while (end > pattern && is_noise(end[-1])) --end;
    const bool ends_in_separator = end > pattern && (end[-1] == '\\' || end[-1] == '/');

    // Normalised the same way the entries were, so a caller may paste a path
    // straight out of WolvenKit and only replace a component with '*'.
    std::string pat = sanitize_path(pattern, static_cast<size_t>(end - pattern));
    if (pat.empty()) {
        // Separators only. Under the rule above that means everything beneath
        // the root, i.e. everything.
        if (!ends_in_separator) return fail(REDFS_E_INVALID_ARG, "empty search pattern");
        pat = "*";
    } else if (ends_in_separator) {
        // sanitize_path guarantees pat does not already end in a separator, so
        // this cannot produce a doubled one.
        pat += "\\*";
    }

    // Matches are collected under the lock and delivered after it is released.
    // The documented thing to do from the callback is read one of the files it
    // hands you -- and a read parses a CR2W, which calls paths_learn_imports,
    // which takes this same non-recursive mutex. Calling out from inside the
    // lock would deadlock on the first caller who used the API for its purpose.
    //
    // So the whole scan happens under the lock, and every concurrent read that
    // parses a CR2W waits it out: ~100-150 ms over a 544k-entry dictionary on
    // the reference machine, and roughly independent of the pattern, because
    // every entry is matched however specific it is. Holding iterators instead
    // of copying is not an option: a concurrent add reallocates `sorted` out
    // from under them.
    //
    // A small fixed reserve, deliberately. Scaling it to the dictionary
    // (sorted.size()/16, a 1-in-16 hit rate) costs ~532 KB on every call
    // including one that matches nothing, and still leaves 7 reallocations on
    // "*". Narrow patterns are the common query; 256 entries covers one at 4 KB.
    std::vector<Dictionary::Entry> hits;
    bool dictionary_empty = false;
    {
        Dictionary& d = dict();
        std::lock_guard<std::mutex> lock(d.mutex);
        // pending is not in `sorted` yet: without this a path added moments ago
        // is unfindable, and an all-pending dictionary reads as empty below.
        merge_pending(d);
        dictionary_empty = d.sorted.empty();
        hits.reserve(256);
        for (const auto& e : d.sorted) {
            if (!glob_match(pat.c_str(), e.str)) continue;
            // Only the on-disk list is filtered at load; imports and paths_add
            // are not, so without this a hit means "this is what some file is
            // called", not "this file is here".
            if (depot && !depot->locate(e.hash, nullptr)) continue;
            hits.push_back(e);
        }
    }

    // Nothing to search is not the same answer as nothing matched, and reporting
    // it as success is how a caller who has not loaded a list concludes their
    // pattern is wrong. redfs_cache_warm already refuses the structurally
    // identical call rather than returning a 0 it computed nothing for.
    //
    // The message covers both ways of arriving here, because they are not the
    // same mistake: never having loaded a list, and having loaded one that
    // resolved nothing against the mounted depot -- a list for another game
    // version, or the wrong archives mounted. Saying only the first sends that
    // caller to load the list they just loaded.
    if (dictionary_empty)
        return fail(REDFS_E_NO_DICTIONARY,
                    "the path dictionary is empty -- either no list was loaded, or the one that "
                    "was resolved no files in this depot. redfs_find searches names learned by "
                    "redfs_path_load / redfs_path_add / import learning, not the depot index");

    // The TOTAL, not the number delivered: the scan has already finished by the
    // time `fn` runs, so the count is free, and a caller who stopped early still
    // learns how much they stopped short of. Deliveries are the caller's own to
    // count.
    if (out_matched) *out_matched = static_cast<uint32_t>(hits.size());

    for (const auto& e : hits)
        if (!fn(e.hash, e.str, user)) break;
    return REDFS_OK;
}

void paths_enable() {
    Dictionary& d = dict();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.enabled.store(true, std::memory_order_relaxed);
}

// --- path cache --------------------------------------------------------------
//
// The dictionary costs minutes to fill from import tables and nothing to keep,
// so it is persisted. Unlike the mesh cache it is NEVER discarded: a hash ->
// name mapping is a fact about the string, so nothing a user installs can make a
// restored entry wrong, and redfs.h already says a hit means "what this is
// called", not "this is readable".
//
// The fingerprints answer a different question -- "which archives have I already
// read" -- one per archive, so installing a mod costs harvesting that mod. Per
// archive also makes mount ORDER irrelevant, which depot_fingerprint is not.
//
// Format (little-endian):
//   'RFPC' u32 | version u32 | archive_count u32 | path_count u32
//   archive_count x u64 archive fingerprint
//   path_count    x { u32 len, bytes }   -- no trailing NUL
//
// Hashes are not stored: every intern route stores the string in the form it
// hashed, so fnv1a64(stored) recovers the key. See docs/done/path-cache.md.

namespace {

constexpr uint32_t kPathMagic = 0x43504652;  // 'RFPC'
constexpr uint32_t kPathVersion = 1;
constexpr long kPathHeaderBytes = 16;

struct PathCache {
    std::mutex mutex;  // lock order: this one, THEN Dictionary::mutex
    std::string file;
    bool enabled = false;
    // Archive fingerprints already harvested into the dictionary. Only grows.
    std::unordered_set<uint64_t> harvested;
    // What the last successful write held. Both it and the dictionary only ever
    // grow, so matching sizes mean the file is current -- and a restore, which
    // goes through the same insert path as everything else, leaves them matching
    // rather than tripping a dirty flag and rewriting 40 MB on every run.
    uint32_t written_paths = 0;
    uint32_t written_archives = 0;
    // For when the counts agree but the file is still not what a flush would
    // write: a truncated file declares whatever it declares.
    bool stale_file = false;
};

PathCache& path_cache() {
    static PathCache* c = new PathCache();  // never destroyed; see api.cpp
    return *c;
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}
void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 8);
}

// Bounds-checked cursor over the loaded file. Every read either fits or clears
// `ok`, so a truncated file stops the walk instead of reading past the buffer.
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
};

// What a load attempt did, reported after the locks are released. The log sink
// is host code running on this thread and is free to call back into RedFS.
struct LoadReport {
    enum class Result { kNoFile, kBadHeader, kUnreadable, kLoaded };
    Result result = Result::kNoFile;
    uint32_t loaded = 0;    // records read
    uint32_t declared = 0;  // records the header promised
    uint32_t refused = 0;   // read, but empty or past kMaxPathLength
    uint32_t inserted = 0;  // by how much the dictionary actually grew
    uint32_t archives = 0;  // coverage digests kept
    bool dictionary_was_empty = false;
    bool dropped_coverage = false;
};

LoadReport load_from_disk(PathCache& c) {
    LoadReport rep;
    FILE* f = std::fopen(c.file.c_str(), "rb");
    if (!f) return rep;

    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    // Also the >2 GB guard: ftell returns -1 there, and a cache file that large
    // is not one this wrote. Either way it is not a header.
    if (len < kPathHeaderBytes) {
        std::fclose(f);
        rep.result = len < 0 ? LoadReport::Result::kBadHeader : LoadReport::Result::kNoFile;
        return rep;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    // NOT "no file": reporting an I/O failure as absence makes it look like a
    // first run, and the next flush then overwrites the file that failed to read.
    if (got != buf.size()) {
        rep.result = LoadReport::Result::kUnreadable;
        return rep;
    }

    Reader r{buf.data(), buf.data() + buf.size()};
    if (r.u32() != kPathMagic || r.u32() != kPathVersion) {
        rep.result = LoadReport::Result::kBadHeader;
        return rep;
    }
    const uint32_t archive_count = r.u32();
    rep.declared = r.u32();

    // Bounded by what the file pays for, not by a fixed cap the writer would not
    // respect: a digest costs 8 bytes, so a 16-byte file cannot ask for a billion.
    const size_t left = static_cast<size_t>(r.end - r.p);
    if (archive_count > left / 8) {
        rep.result = LoadReport::Result::kBadHeader;
        return rep;
    }
    rep.result = LoadReport::Result::kLoaded;
    for (uint32_t i = 0; i < archive_count && r.ok; ++i) {
        c.harvested.insert(r.u64());
        ++rep.archives;
    }

    // One lock for the whole restore, and NOT paths_load's route: that one
    // filters against the depot, and these entries are documented unfiltered.
    // Filtering here returns a SMALLER dictionary on the second run than the
    // first, with nothing in the log.
    Dictionary& d = dict();
    std::lock_guard<std::mutex> lock(d.mutex);
    merge_pending(d);  // so the before/after counts below are true counts
    const size_t before = d.sorted.size();
    rep.dictionary_was_empty = before == 0;

    for (uint32_t i = 0; i < rep.declared && r.ok; ++i) {
        const uint32_t n = r.u32();
        if (!r.need(n)) break;
        const char* s = reinterpret_cast<const char*>(r.p);
        r.p += n;
        ++rep.loaded;
        // Refused here, not in add_locked, which logs -- and this thread holds
        // BOTH mutexes. The sink is host code and may call back in. Counted
        // instead, and reported once the locks drop.
        //
        // An empty record is corruption: nothing in the dictionary is empty.
        if (!n || n > kMaxPathLength) {
            ++rep.refused;
            continue;
        }
        // intern() copies and NUL-terminates, so un-terminated bytes are fine.
        add_locked(d, s, n, fnv1a64(s, n));
    }
    merge_pending(d);
    rep.inserted = static_cast<uint32_t>(d.sorted.size() - before);
    return rep;
}

}  // namespace

redfs_status path_cache_open(const redfs_depot* depot, const char* file) {
    if (!depot || !file) return REDFS_E_INVALID_ARG;
    PathCache& c = path_cache();

    // Before the lock, since flush takes the same one. Everything the previous
    // cache learned but did not write would otherwise be discarded: c.file,
    // c.harvested and the bookkeeping are all about to be overwritten.
    path_cache_flush();

    LoadReport rep;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.file = file;
        c.harvested.clear();
        c.enabled = true;
        c.written_paths = 0;
        c.written_archives = 0;
        c.stale_file = false;

        // Import learning comes on with the cache. HERE, not in the restore:
        // the first run has no file and returns early, so learning stayed off
        // for exactly the run that had everything to learn.
        {
            Dictionary& d = dict();  // lock order: PathCache, then Dictionary
            std::lock_guard<std::mutex> dlock(d.mutex);
            d.enabled.store(true, std::memory_order_relaxed);
        }

        rep = load_from_disk(c);
        path = c.file;

        if (rep.result == LoadReport::Result::kLoaded) {
            // Is the file exactly what a flush would write? Every record read,
            // none refused, and -- when the dictionary started empty, the only
            // case where the arithmetic is exact -- each one produced an entry,
            // which rules out duplicates a size check would read as clean.
            const bool faithful =
                rep.loaded == rep.declared && rep.refused == 0 &&
                (!rep.dictionary_was_empty || rep.inserted == rep.loaded);

            if (!faithful) {
                // Digests are at the FRONT of the file and paths at the back,
                // so a tail cut restores EVERY digest and only some paths.
                // Keeping them publishes full coverage over a partial
                // dictionary -- pending says "nothing to do", the lost paths
                // never come back, and the rewrite below makes it permanent.
                // Coverage is cheap to re-derive; the paths are not.
                c.harvested.clear();
                rep.dropped_coverage = rep.archives != 0;
                rep.archives = 0;
            }
            // What the FILE holds, not the dictionary: entries added before
            // this call are not in it and would otherwise never be written.
            c.written_paths = rep.loaded;
            c.written_archives = rep.archives;
            c.stale_file = !faithful;
        }
    }

    switch (rep.result) {
        case LoadReport::Result::kNoFile:
            break;
        case LoadReport::Result::kBadHeader:
            log("path cache %s has an unrecognised header; starting fresh", path.c_str());
            break;
        case LoadReport::Result::kUnreadable:
            log("path cache %s could not be read; starting fresh, and it will be overwritten",
                path.c_str());
            break;
        case LoadReport::Result::kLoaded:
            if (rep.loaded != rep.declared)
                log("path cache: restored %u of %u paths from %s (file is truncated or corrupt)",
                    rep.loaded, rep.declared, path.c_str());
            else if (rep.refused)
                log("path cache: restored %u paths from %s, refusing %u unusable records",
                    rep.loaded - rep.refused, path.c_str(), rep.refused);
            else
                log("path cache: restored %u paths and %u harvested archives from %s", rep.loaded,
                    rep.archives, path.c_str());
            // Its own line: this is the one that costs the caller time.
            if (rep.dropped_coverage)
                log("path cache: the restore was incomplete, so every archive will be harvested "
                    "again rather than trusting coverage recorded for paths that did not survive");
            break;
    }
    return REDFS_OK;
}

redfs_status path_cache_flush() {
    PathCache& c = path_cache();

    char message[512] = {0};
    bool failed = false;

    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (!c.enabled) return REDFS_OK;

        std::vector<const char*> strings;
        {
            Dictionary& d = dict();
            std::lock_guard<std::mutex> dlock(d.mutex);
            merge_pending(d);
            if (!c.stale_file && d.sorted.size() == c.written_paths &&
                c.harvested.size() == c.written_archives)
                return REDFS_OK;  // nothing learned since the last write
            strings.reserve(d.sorted.size());
            for (const auto& e : d.sorted) strings.push_back(e.str);
        }
        // The image is built with the dictionary lock released, which is safe
        // only because Arena never moves or frees an interned string. A
        // concurrent add just leaves the cache dirty for the next flush.

        std::vector<uint8_t> out;
        out.reserve(strings.size() * 64 + 64);
        put_u32(out, kPathMagic);
        put_u32(out, kPathVersion);
        put_u32(out, static_cast<uint32_t>(c.harvested.size()));
        put_u32(out, static_cast<uint32_t>(strings.size()));

        // Sorted: an unordered_set's iteration order is not a promise, and the
        // same depot should give the same bytes. `strings` is already ordered.
        std::vector<uint64_t> fps(c.harvested.begin(), c.harvested.end());
        std::sort(fps.begin(), fps.end());
        for (uint64_t fp : fps) put_u64(out, fp);

        for (const char* s : strings) {
            const size_t n = std::strlen(s);
            put_u32(out, static_cast<uint32_t>(n));
            out.insert(out.end(), s, s + n);
        }

        // Sibling plus rename, so a crash mid-write cannot corrupt a file that
        // was previously good. At 40 MB that window is not theoretical.
        const std::string tmp = c.file + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            std::snprintf(message, sizeof(message), "cannot write %s", tmp.c_str());
            failed = true;
        } else {
            // fwrite reports bytes buffered, not bytes on disk -- on a share or
            // a synced folder the failure surfaces only at flush or close, so
            // fwrite alone would promote a truncated file over a good one.
            bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
            if (ok) ok = std::fflush(f) == 0 && ::_commit(::_fileno(f)) == 0;
            if (std::fclose(f) != 0) ok = false;  // close either way, then judge

            if (!ok) {
                std::remove(tmp.c_str());
                std::snprintf(message, sizeof(message), "cannot write %s", tmp.c_str());
                failed = true;
            } else if (!::MoveFileExA(tmp.c_str(), c.file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                // One step: deleting the destination first leaves a window with
                // no file at all, and a rename that then fails (a scanner
                // holding the path is enough) destroys the good copy.
                std::remove(tmp.c_str());
                std::snprintf(message, sizeof(message), "cannot replace %s (error %lu)",
                              c.file.c_str(), ::GetLastError());
                failed = true;
            } else {
                c.written_paths = static_cast<uint32_t>(strings.size());
                c.written_archives = static_cast<uint32_t>(c.harvested.size());
                c.stale_file = false;
                std::snprintf(message, sizeof(message),
                              "path cache: wrote %zu paths and %zu harvested archives (%zu bytes)",
                              strings.size(), c.harvested.size(), out.size());
            }
        }
    }

    if (failed) return fail(REDFS_E_IO, "%s", message);
    log("%s", message);
    return REDFS_OK;
}

void path_cache_close() {
    // Guarded, not a bare flush call, for two reasons. The reset below must
    // happen even when the write throws -- flush builds ~40 MB in memory, so
    // bad_alloc is its realistic failure, and unwinding leaves a cache that
    // still reports itself open. And this is the only place the status is
    // visible at all: redfs.h declares this void and redfs_shutdown discards
    // it, so without the line an unwritable target loses a session in silence.
    redfs_status st = REDFS_OK;
    try {
        st = path_cache_flush();
    } catch (...) {
        st = REDFS_E_IO;
    }
    if (st != REDFS_OK)
        log("path cache: the final write failed (%s); nothing learned this session was saved",
            redfs_status_string(st));

    PathCache& c = path_cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.harvested.clear();
    c.enabled = false;
    c.written_paths = 0;
    c.written_archives = 0;
    c.stale_file = false;
    // The dictionary is NOT cleared: closing means "stop persisting", not
    // "forget", and interned pointers callers hold must stay valid.
}

redfs_status path_cache_pending(const redfs_depot* depot, uint32_t* out_indices, uint32_t capacity,
                                uint32_t* out_count) {
    if (!depot || (!out_indices && capacity)) return REDFS_E_INVALID_ARG;
    if (out_count) *out_count = 0;

    PathCache& c = path_cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    // Without a cache the answer is always "all of them", from a call that
    // computed nothing -- and a caller acting on it re-teaches every run.
    // redfs_cache_warm refuses the structurally identical call.
    if (!c.enabled)
        return fail(REDFS_E_INVALID_ARG,
                    "redfs_path_cache_pending requires an open path cache -- without one no "
                    "archive has been recorded as harvested and the answer is meaningless");

    uint32_t total = 0;
    for (uint32_t i = 0; i < depot->archives.size(); ++i) {
        if (c.harvested.count(archive_fingerprint(depot->archives[i]))) continue;
        // The TOTAL, not the number delivered, so a first call with capacity 0
        // sizes a buffer the second is guaranteed to fit.
        if (total < capacity) out_indices[total] = i;
        ++total;
    }
    if (out_count) *out_count = total;
    return REDFS_OK;
}

redfs_status path_cache_mark(const redfs_depot* depot, uint32_t archive_index) {
    if (!depot) return REDFS_E_INVALID_ARG;
    if (archive_index >= depot->archives.size())
        return fail(REDFS_E_INVALID_ARG, "archive index %u is past the %zu mounted", archive_index,
                    depot->archives.size());

    PathCache& c = path_cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (!c.enabled)
        return fail(REDFS_E_INVALID_ARG, "redfs_path_cache_mark requires an open path cache");
    c.harvested.insert(archive_fingerprint(depot->archives[archive_index]));
    return REDFS_OK;
}

}  // namespace redfs
