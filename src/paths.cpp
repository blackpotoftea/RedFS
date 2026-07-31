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

}  // namespace redfs
