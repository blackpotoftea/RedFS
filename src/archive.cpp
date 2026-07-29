// .archive container: index parsing and segment decoding.
//
// Layout (version 12), all little-endian:
//
//   0x00  u32 magic 'RDAR'      u32 version
//         u64 index_position    u32 index_size
//         u64 debug_position    u32 debug_size
//         u64 file_size
//   0x28  u32 custom_data_length     -- LXRS path footer, if the archive has one
//
//   at index_position:                                     -- 28-byte header
//         u32 file_table_offset  u32 file_table_size  u64 crc
//         u32 entry_count  u32 segment_count  u32 dependency_count
//         entry_count   x 56 bytes  { u64 hash; i64 time; u32 inline_bufs;
//                                     u32 seg_start; u32 seg_end;
//                                     u32 dep_start; u32 dep_end; u8 sha1[20] }
//         segment_count x 16 bytes  { u64 offset; u32 zsize; u32 size }
//         dependency_count x u64
//
// A segment is raw when zsize == size, otherwise it is
//   'KARK' | u32 raw_size | Kraken bitstream.

#include "internal.hpp"

#include <algorithm>

#include <windows.h>

namespace redfs {
namespace {

// Windows requires file-mapping offsets to be allocation-granularity aligned.
uint64_t alloc_granularity() {
    static const uint64_t g = [] {
        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        return static_cast<uint64_t>(si.dwAllocationGranularity);
    }();
    return g;
}

}  // namespace

// --- path hashing ------------------------------------------------------------

std::string sanitize_path(const char* path, size_t len) {
    std::string out;
    if (!path || len == 0) return out;

    auto is_trim = [](char c) {
        return c == '\'' || c == '"' || c == '/' || c == '\\' || c == ' ' || c == '\n' || c == '\r';
    };
    size_t b = 0, e = len;
    while (b < e && is_trim(path[b])) ++b;
    while (e > b && is_trim(path[e - 1])) --e;

    out.reserve(e - b);
    for (size_t i = b; i < e; ++i) {
        char c = path[i];
        if (c == '/' || c == '\\') {
            // collapse runs of separators; never emit a leading one
            if (out.empty() || out.back() == '\\') continue;
            out.push_back('\\');
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}

uint64_t fnv1a64(const void* data, size_t len) {
    constexpr uint64_t kOffset = 0xCBF29CE484222325ull;
    constexpr uint64_t kPrime = 0x00000100000001B3ull;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = kOffset;
    for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * kPrime;
    return h;
}

// --- archive -----------------------------------------------------------------

Archive::~Archive() {
    if (view_) ::UnmapViewOfFile(view_);
    if (map_) ::CloseHandle(map_);
    if (file_ && file_ != INVALID_HANDLE_VALUE) ::CloseHandle(file_);
}

redfs_status Archive::open(const std::string& path) {
    path_ = path;

    // The game holds these open while running, so share aggressively.
    HANDLE fh = ::CreateFileA(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE)
        return fail(REDFS_E_IO, "cannot open %s (error %lu)", path.c_str(), ::GetLastError());
    file_ = fh;

    HANDLE mh = ::CreateFileMappingW(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mh) return fail(REDFS_E_IO, "CreateFileMapping failed for %s", path.c_str());
    map_ = mh;

    // header
    uint8_t header[40];
    {
        void* v = ::MapViewOfFile(mh, FILE_MAP_READ, 0, 0, sizeof(header));
        if (!v) return fail(REDFS_E_IO, "cannot map header of %s", path.c_str());
        std::memcpy(header, v, sizeof(header));
        ::UnmapViewOfFile(v);
    }
    if (rd32(header) != kArchiveMagic)
        return fail(REDFS_E_CORRUPT, "%s is not a RED archive", path.c_str());

    const uint64_t index_pos = rd64(header + 8);
    const uint32_t index_size = rd32(header + 16);
    if (index_size < kIndexHeaderSize)
        return fail(REDFS_E_CORRUPT, "%s has a truncated index", path.c_str());

    // Map only the index window. MapViewOfFile demands an allocation-granularity
    // aligned offset, so round down and keep the delta.
    const uint64_t gran = alloc_granularity();
    const uint64_t aligned = index_pos & ~(gran - 1);
    const uint64_t delta = index_pos - aligned;
    const uint64_t span = delta + index_size;

    view_ = ::MapViewOfFile(mh, FILE_MAP_READ, static_cast<DWORD>(aligned >> 32),
                            static_cast<DWORD>(aligned & 0xFFFFFFFFu), static_cast<SIZE_T>(span));
    if (!view_) return fail(REDFS_E_IO, "cannot map index of %s (error %lu)", path.c_str(), ::GetLastError());

    const uint8_t* idx = static_cast<const uint8_t*>(view_) + delta;
    entry_count_ = rd32(idx + 16);
    segment_count_ = rd32(idx + 20);
    const uint32_t dep_count = rd32(idx + 24);

    const uint64_t need = kIndexHeaderSize + static_cast<uint64_t>(entry_count_) * kEntryStride +
                          static_cast<uint64_t>(segment_count_) * kSegmentStride +
                          static_cast<uint64_t>(dep_count) * 8ull;
    if (need > index_size)
        return fail(REDFS_E_CORRUPT, "%s index claims %llu bytes but is %u", path.c_str(),
                    static_cast<unsigned long long>(need), index_size);

    entries_ = idx + kIndexHeaderSize;
    segments_ = entries_ + static_cast<size_t>(entry_count_) * kEntryStride;
    index_size_ = index_size;
    return REDFS_OK;
}

redfs_status Archive::read_segment(const Segment& seg, uint8_t* dst) const {
    const uint64_t gran = alloc_granularity();
    const uint64_t aligned = seg.offset & ~(gran - 1);
    const uint64_t delta = seg.offset - aligned;
    const uint64_t span = delta + seg.zsize;

    const void* v = ::MapViewOfFile(map_, FILE_MAP_READ, static_cast<DWORD>(aligned >> 32),
                                    static_cast<DWORD>(aligned & 0xFFFFFFFFu),
                                    static_cast<SIZE_T>(span));
    if (!v) return fail(REDFS_E_IO, "cannot map segment at %llu of %s (error %lu)",
                        static_cast<unsigned long long>(seg.offset), path_.c_str(), ::GetLastError());

    const uint8_t* src = static_cast<const uint8_t*>(v) + delta;
    redfs_status st = REDFS_OK;

    if (seg.zsize == seg.size) {
        std::memcpy(dst, src, seg.size);
    } else if (seg.zsize >= 8 && rd32(src) == kKarkMagic) {
        uint32_t raw = rd32(src + 4);
        // The KARK header is authoritative when it disagrees with the index.
        if (raw > seg.size) raw = seg.size;
        if (!oodle::available()) {
            st = fail(REDFS_E_OODLE, "oo2ext_7_win64.dll is not available");
        } else {
            const int64_t got = oodle::decompress(src + 8, static_cast<int64_t>(seg.zsize) - 8, dst,
                                                  static_cast<int64_t>(raw));
            if (got != static_cast<int64_t>(raw))
                st = fail(REDFS_E_OODLE, "Kraken decode returned %lld, expected %u",
                          static_cast<long long>(got), raw);
            else if (raw < seg.size)
                std::memset(dst + raw, 0, seg.size - raw);
        }
    } else {
        // Not compressed after all despite zsize != size; copy what is there.
        std::memcpy(dst, src, (std::min)(seg.zsize, seg.size));
        if (seg.zsize < seg.size) std::memset(dst + seg.zsize, 0, seg.size - seg.zsize);
    }

    ::UnmapViewOfFile(const_cast<void*>(v));
    return st;
}

}  // namespace redfs

// --- depot -------------------------------------------------------------------

redfs_depot::~redfs_depot() {
    for (auto* a : archives) delete a;
}

bool redfs_depot::locate(uint64_t hash, redfs::Located* out) const {
    auto it = std::lower_bound(refs.begin(), refs.end(), hash,
                               [](const Ref& r, uint64_t h) { return r.hash < h; });
    if (it == refs.end() || it->hash != hash) return false;
    // refs is built from `archives`, so this index is always valid -- but the
    // invariant lives in reindex(), a long way from here, and a stale ref would
    // be a use-after-free rather than a miss.
    if (it->archive >= archives.size()) return false;
    if (out) {
        out->archive = archives[it->archive];
        out->archive_index = it->archive;
        out->entry = it->entry;
    }
    return true;
}

namespace redfs {

redfs_status resolve_part(const redfs_depot* depot, uint64_t hash, uint32_t part, PartRange* out) {
    Located loc{};
    if (!depot->locate(hash, &loc)) return REDFS_E_NOT_FOUND;

    const Archive* ar = loc.archive;
    if (!ar) return REDFS_E_NOT_FOUND;
    const uint32_t start = ar->entry_seg_start(loc.entry);
    const uint32_t end = ar->entry_seg_end(loc.entry);
    if (start >= end || end > ar->segment_count()) return REDFS_E_CORRUPT;

    out->archive = ar;
    if (part == REDFS_PART_ALL) {
        out->first = start;
        out->last = end;
    } else if (part == REDFS_PART_MAIN) {
        out->first = start;
        out->last = start + 1;
    } else {
        // buffer i == segment start + 1 + i
        const uint32_t seg = start + 1 + part;
        if (seg >= end) return REDFS_E_RANGE;
        out->first = seg;
        out->last = seg + 1;
    }
    return REDFS_OK;
}

redfs_status read_part(const redfs_depot* depot, uint64_t hash, uint32_t part, uint8_t* dst,
                       uint64_t capacity, uint64_t* out_size, const std::atomic<bool>* abort) {
    PartRange r{};
    redfs_status st = resolve_part(depot, hash, part, &r);
    if (st != REDFS_OK) return st;

    uint64_t total = 0;
    for (uint32_t i = r.first; i < r.last; ++i) total += r.archive->segment(i).size;
    if (out_size) *out_size = total;
    if (!dst) return REDFS_OK;
    if (capacity < total) return REDFS_E_RANGE;

    uint64_t at = 0;
    for (uint32_t i = r.first; i < r.last; ++i) {
        // Checked per segment rather than per byte: a Kraken block cannot be
        // interrupted once started, so this is the finest granularity available.
        // It is what keeps shutdown from having to wait out a whole multi-buffer
        // read of a large resource.
        if (abort && abort->load(std::memory_order_relaxed)) return REDFS_E_CANCELLED;

        const Segment seg = r.archive->segment(i);
        st = r.archive->read_segment(seg, dst + at);
        if (st != REDFS_OK) return st;
        at += seg.size;
    }
    return REDFS_OK;
}

}  // namespace redfs
