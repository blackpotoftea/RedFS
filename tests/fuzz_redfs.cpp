// Mutation fuzzer for the binary parsers.
//
// RedFS parses data it did not write. A corrupt archive, a truncated CR2W, or a
// deliberately hostile mod must produce an error, never a crash and never a read
// outside the buffer. There is no libFuzzer in this toolchain (VS ships only
// clang-format and clang-tidy), so this is a self-contained mutation loop --
// which is enough, because the input space that matters is "valid file with
// something broken in it" rather than "arbitrary bytes".
//
// Run it under ASan to make out-of-bounds reads fatal and visible:
//   cmake -B build-asan -DREDFS_SANITIZE=address && build-asan/redfs_fuzz
//
// Deterministic: the same seed replays the same corruptions, so a failure can be
// reproduced and bisected.

#include "fixtures.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "redfs.h"

namespace {

// xorshift64* -- deterministic and dependency-free. std::mt19937 would do, but
// this keeps the corruption sequence stable across standard library versions.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }
    uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
};

// The corruptions worth trying, aimed at what actually breaks parsers: length
// and offset fields, not random noise in the middle of a payload.
enum Mutation {
    kFlipByte,
    kFlipBit,
    kZeroRegion,
    kMaxOutU32,     // a size or offset field becomes enormous
    kNegativeU32,   // ...or 0xFFFFFFFF, which underflows a subtraction
    kTruncate,
    kDuplicateByte,
    kMutationCount
};

void mutate(std::vector<uint8_t>& data, Rng& rng) {
    if (data.empty()) return;
    switch (rng.below(kMutationCount)) {
        case kFlipByte:
            data[rng.below((uint32_t)data.size())] = static_cast<uint8_t>(rng.next());
            break;
        case kFlipBit: {
            const uint32_t at = rng.below((uint32_t)data.size());
            data[at] ^= static_cast<uint8_t>(1u << rng.below(8));
            break;
        }
        case kZeroRegion: {
            const uint32_t at = rng.below((uint32_t)data.size());
            const uint32_t len = rng.below(64) + 1;
            for (uint32_t i = at; i < data.size() && i < at + len; ++i) data[i] = 0;
            break;
        }
        case kMaxOutU32: {
            if (data.size() < 4) break;
            const uint32_t at = rng.below((uint32_t)data.size() - 3);
            const uint32_t v = 0x7FFFFFFFu;
            std::memcpy(data.data() + at, &v, 4);
            break;
        }
        case kNegativeU32: {
            if (data.size() < 4) break;
            const uint32_t at = rng.below((uint32_t)data.size() - 3);
            const uint32_t v = 0xFFFFFFFFu;
            std::memcpy(data.data() + at, &v, 4);
            break;
        }
        case kTruncate:
            data.resize(rng.below((uint32_t)data.size()) + 1);
            break;
        case kDuplicateByte: {
            const uint32_t at = rng.below((uint32_t)data.size());
            data[at] = data[rng.below((uint32_t)data.size())];
            break;
        }
        default:
            break;
    }
}

// --- targets -----------------------------------------------------------------
//
// Each target consumes one mutated input and must return without crashing. Any
// status is acceptable; the contract is memory safety, not success.

void fuzz_cr2w(const std::vector<uint8_t>& data) {
    redfs_cr2w* f = nullptr;
    if (redfs_cr2w_open(data.data(), data.size(), &f) != REDFS_OK) return;

    // Exercise the whole query surface against a corrupt document -- the parse
    // succeeding is exactly when a bad offset does damage.
    redfs_cr2w_root_type(f);
    const uint32_t chunks = redfs_cr2w_chunk_count(f);
    for (uint32_t i = 0; i < chunks && i < 64; ++i) redfs_cr2w_chunk_type(f, i);

    const uint32_t imports = redfs_cr2w_import_count(f);
    for (uint32_t i = 0; i < imports && i < 64; ++i) {
        redfs_cr2w_import_path(f, i);
        redfs_cr2w_import_type(f, i);
    }

    for (uint32_t i = 0; i < chunks && i < 16; ++i) {
        redfs_value v{};
        redfs_cr2w_get(f, i, "header", &v);
        redfs_cr2w_get(f, i, "header.sizeInfo.width", &v);
        redfs_cr2w_get(f, i, "setup.compression", &v);

        // Walk everything, and descend into any array we find.
        redfs_cr2w_walk(
            f, i, nullptr,
            [](const char*, const redfs_value* val, void* user) -> int {
                if (val->kind == REDFS_KIND_ARRAY) {
                    auto* handle = static_cast<redfs_cr2w*>(user);
                    redfs_cr2w_walk_array(
                        handle, val,
                        [](uint32_t, const redfs_value*, void*) -> int { return 1; }, nullptr);
                }
                return 1;
            },
            f);
    }
    redfs_cr2w_close(f);
}

void fuzz_archive(const std::vector<uint8_t>& data, const std::string& path) {
    {
        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return;
        std::fwrite(data.data(), 1, data.size(), fp);
        std::fclose(fp);
    }

    redfs_depot* depot = nullptr;
    if (redfs_depot_open_empty(&depot) != REDFS_OK || !depot) return;

    if (redfs_depot_mount(depot, path.c_str()) == REDFS_OK) {
        // Mounting a corrupt archive is the dangerous case: the index is mapped,
        // so a bad segment offset reads outside the mapping.
        redfs_enumerate(
            depot,
            [](const redfs_file_info* info, void* user) -> int {
                auto* d = static_cast<redfs_depot*>(user);
                redfs_blob blob{};
                if (redfs_read(d, info->hash, REDFS_PART_ALL, &blob) == REDFS_OK)
                    redfs_blob_free(&blob);
                if (redfs_read(d, info->hash, REDFS_PART_MAIN, &blob) == REDFS_OK)
                    redfs_blob_free(&blob);
                for (uint32_t b = 0; b < info->buffer_count && b < 4; ++b)
                    if (redfs_read(d, info->hash, b, &blob) == REDFS_OK) redfs_blob_free(&blob);

                redfs_texture_desc t{};
                redfs_texture_desc_of(d, info->hash, &t);
                redfs_mesh* m = nullptr;
                if (redfs_mesh_open(d, info->hash, &m) == REDFS_OK) redfs_mesh_close(m);
                return 1;
            },
            depot);
    }
    redfs_depot_close(depot);
    std::remove(path.c_str());
}

std::string temp_path(const char* name) {
    char buf[512];
    const char* tmp = std::getenv("TEMP");
    std::snprintf(buf, sizeof buf, "%s\\redfs_fuzz_%s", tmp ? tmp : ".", name);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t iterations =
        argc >= 2 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 20000;
    const uint64_t seed = argc >= 3 ? std::strtoull(argv[2], nullptr, 10) : 1;

    std::printf("fuzzing parsers: %u iterations, seed %llu\n", iterations,
                (unsigned long long)seed);
#if defined(__SANITIZE_ADDRESS__) || defined(REDFS_ASAN)
    std::printf("ASan is on -- out-of-bounds access will abort\n");
#else
    std::printf("WARNING: ASan is off. Build with -DREDFS_SANITIZE=address for real coverage.\n");
#endif

    // Seed corpus: structurally valid documents, so mutations land on meaningful
    // fields rather than being rejected at the magic check.
    std::vector<std::vector<uint8_t>> cr2w_corpus = {
        fixture::make_texture_cr2w(64, 64, 7, "TCM_QualityColor", "TRF_TrueColor", true),
        fixture::make_mesh_cr2w(4, 8, 10.0f, 0.0f),
    };
    {
        fixture::Cr2wBuilder b;
        b.import("base\\a\\b.mt", "IMaterial");
        b.begin_chunk("Simple");
        b.prop_u32("value", 7);
        b.prop_array_cname("names", {"x", "y", "z"});
        b.end_chunk();
        cr2w_corpus.push_back(b.build());
    }

    std::vector<std::vector<uint8_t>> archive_corpus;
    {
        fixture::ArchiveBuilder ab;
        ab.add(redfs_hash("base\\a.mesh"), fixture::make_mesh_cr2w(3, 6, 5.0f, 1.0f),
               {fixture::make_mesh_geometry(3, 6)});
        ab.add(redfs_hash("base\\b.xbm"),
               fixture::make_texture_cr2w(32, 32, 5, "TCM_DXTAlpha", "TRF_TrueColor", false),
               {std::vector<uint8_t>(1024, 0x22)});
        archive_corpus.push_back(ab.build());
    }

    Rng rng(seed);
    const std::string arc_path = temp_path("mutated.archive");

    uint32_t cr2w_runs = 0, archive_runs = 0;
    for (uint32_t i = 0; i < iterations; ++i) {
        if ((i % 4) == 3) {
            std::vector<uint8_t> data = archive_corpus[rng.below((uint32_t)archive_corpus.size())];
            const uint32_t rounds = rng.below(3) + 1;
            for (uint32_t r = 0; r < rounds; ++r) mutate(data, rng);
            fuzz_archive(data, arc_path);
            ++archive_runs;
        } else {
            std::vector<uint8_t> data = cr2w_corpus[rng.below((uint32_t)cr2w_corpus.size())];
            const uint32_t rounds = rng.below(4) + 1;
            for (uint32_t r = 0; r < rounds; ++r) mutate(data, rng);
            fuzz_cr2w(data);
            ++cr2w_runs;
        }

        if (iterations >= 1000 && (i % (iterations / 10)) == 0 && i)
            std::printf("  %u%%\n", 100 * i / iterations);
    }

    std::printf("\nsurvived %u CR2W and %u archive mutations without crashing\n", cr2w_runs,
                archive_runs);
    return 0;
}
