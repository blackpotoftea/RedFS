// redfs_cli -- exercises the RedFS API against a real Cyberpunk 2077 install.
//
// This is the verification harness for the library: `selftest` mounts the whole
// depot, samples files across every archive, decodes them, and reports.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#include "redfs.h"

namespace {

// The system SHA-1, not one of ours. The whole point of `verify` is to check
// RedFS against a hash RedFS did not compute, so the digest has to come from
// somewhere RedFS has no say in.
bool sha1(const void* data, uint64_t len, uint8_t out[20]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return false;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    if (::BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        // BCryptHashData takes a ULONG, so feed it in chunks: a .opuspak runs to
        // ~1 MB but nothing in the format caps a segment at 4 GB.
        ok = true;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (len && ok) {
            const ULONG chunk = len > 0x10000000u ? 0x10000000u : static_cast<ULONG>(len);
            ok = ::BCryptHashData(h, const_cast<PUCHAR>(p), chunk, 0) == 0;
            p += chunk;
            len -= chunk;
        }
        ok = ok && ::BCryptFinishHash(h, out, 20, 0) == 0;
        ::BCryptDestroyHash(h);
    }
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

void on_log(const char* msg, void*) { std::fprintf(stderr, "[redfs] %s\n", msg); }

int die(const char* what, redfs_status st) {
    std::fprintf(stderr, "%s: %s (%s)\n", what, redfs_status_string(st), redfs_last_error());
    return 1;
}

// Accepts either a depot path or a literal 0x-prefixed hash.
uint64_t parse_key(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return std::strtoull(s + 2, nullptr, 16);
    return redfs_hash(s);
}

const char* kind_name(redfs_kind k) {
    switch (k) {
        case REDFS_KIND_RAW: return "raw";
        case REDFS_KIND_BOOL: return "bool";
        case REDFS_KIND_INT: return "int";
        case REDFS_KIND_UINT: return "uint";
        case REDFS_KIND_FLOAT: return "float";
        case REDFS_KIND_NAME: return "name";
        case REDFS_KIND_STRING: return "string";
        case REDFS_KIND_STRUCT: return "struct";
        case REDFS_KIND_HANDLE: return "handle";
        case REDFS_KIND_BUFFER: return "buffer";
        case REDFS_KIND_ARRAY: return "array";
    }
    return "?";
}

void print_value(const redfs_value& v) {
    switch (v.kind) {
        case REDFS_KIND_BOOL: std::printf("%s", v.as.u ? "true" : "false"); break;
        case REDFS_KIND_INT: std::printf("%" PRId64, v.as.i); break;
        case REDFS_KIND_UINT: std::printf("%" PRIu64, v.as.u); break;
        case REDFS_KIND_FLOAT: std::printf("%g", v.as.f); break;
        case REDFS_KIND_NAME:
        case REDFS_KIND_STRING: std::printf("\"%s\"", v.as.s ? v.as.s : ""); break;
        case REDFS_KIND_HANDLE: std::printf("-> chunk %d", v.as.chunk); break;
        case REDFS_KIND_BUFFER: std::printf("-> buffer %u", v.as.buffer); break;
        case REDFS_KIND_ARRAY: std::printf("[%" PRIu64 " items]", v.as.u); break;
        case REDFS_KIND_STRUCT: std::printf("{...} (%u bytes)", v.size); break;
        default: std::printf("<%u bytes>", v.size); break;
    }
}

int walk_printer(const char* name, const redfs_value* v, void* user) {
    const int indent = *static_cast<int*>(user);
    std::printf("%*s%-32s %-28s %-7s ", indent, "", name, v->type, kind_name(v->kind));
    print_value(*v);
    std::printf("\n");
    return 1;
}

// Recursive variant: descends into nested structs and arrays so a whole subtree
// can be inspected in one go.
struct Dump {
    redfs_cr2w* f;
    int indent;
    int depth;
};

int dump_prop(const char* name, const redfs_value* v, void* user);

int dump_elem(uint32_t index, const redfs_value* v, void* user) {
    char label[32];
    std::snprintf(label, sizeof(label), "[%u]", index);
    // Cap the listing: some arrays hold thousands of elements.
    return dump_prop(label, v, user) && index < 7;
}

int dump_prop(const char* name, const redfs_value* v, void* user) {
    auto* d = static_cast<Dump*>(user);
    std::printf("%*s%-28s %-30s %-7s ", d->indent, "", name, v->type, kind_name(v->kind));
    print_value(*v);
    std::printf("\n");
    if (d->depth > 0 && (v->kind == REDFS_KIND_STRUCT || v->kind == REDFS_KIND_ARRAY)) {
        Dump child{d->f, d->indent + 2, d->depth - 1};
        if (v->kind == REDFS_KIND_STRUCT)
            redfs_cr2w_walk_in(d->f, v, nullptr, dump_prop, &child);
        else
            redfs_cr2w_walk_array(d->f, v, dump_elem, &child);
    }
    return 1;
}

uint32_t parse_part(const char* s) {
    if (!s || std::strcmp(s, "all") == 0) return REDFS_PART_ALL;
    if (std::strcmp(s, "main") == 0) return REDFS_PART_MAIN;
    return static_cast<uint32_t>(std::strtoul(s, nullptr, 10));
}

redfs_depot* mount(const char* game_dir) {
    const auto t0 = Clock::now();
    redfs_depot* d = nullptr;
    const redfs_status st = redfs_depot_open(game_dir, REDFS_SCAN_ALL, &d);
    if (st != REDFS_OK) {
        die("depot_open", st);
        return nullptr;
    }
    std::printf("mounted %u archives, %" PRIu64 " files, %.1f MB index, in %.0f ms\n",
                redfs_depot_archive_count(d), redfs_depot_file_count(d),
                redfs_depot_index_bytes(d) / 1048576.0, ms_since(t0));
    // Nearly every segment is Kraken-compressed, so without Oodle almost nothing
    // reads. Say so once here rather than let every read fail with E_OODLE.
    if (!redfs_oodle_available())
        std::fprintf(stderr, "WARNING: Oodle unresolved -- compressed reads will fail\n");
    return d;
}

// --- commands ----------------------------------------------------------------

int cmd_info(redfs_depot* d) {
    for (uint32_t i = 0; i < redfs_depot_archive_count(d); ++i)
        std::printf("  [%2u] %s\n", i, redfs_depot_archive_path(d, i));
    return 0;
}

int cmd_hash(int argc, char** argv) {
    for (int i = 0; i < argc; ++i)
        std::printf("%20" PRIu64 "  0x%016" PRIX64 "  %s\n", redfs_hash(argv[i]),
                    redfs_hash(argv[i]), argv[i]);
    return 0;
}

// Loads a path dictionary, then resolves hashes back to strings.
int cmd_paths(redfs_depot* d, const char* list_file, int argc, char** argv) {
    uint32_t kept = 0;
    const auto t0 = Clock::now();
    const redfs_status st = redfs_path_load(d, list_file, &kept);
    if (st != REDFS_OK) return die("path_load", st);
    std::printf("dictionary      %u paths resolve in this depot, loaded in %.0f ms\n", kept,
                ms_since(t0));
    std::printf("coverage        %.1f%% of %" PRIu64 " files\n",
                100.0 * kept / (double)redfs_depot_file_count(d), redfs_depot_file_count(d));

    for (int i = 0; i < argc; ++i) {
        const uint64_t h = parse_key(argv[i]);
        const char* path = redfs_path_from_hash(h);
        std::printf("  0x%016" PRIX64 "  %s\n", h, path ? path : "<unknown>");
    }
    return 0;
}

// Finds files by path pattern -- the practical way to locate a mesh when you
// only half-remember where it lives.
struct Finder {
    redfs_depot* depot;
    uint32_t found;
    uint32_t limit;
};

int find_visit(uint64_t hash, const char* path, void* user) {
    auto* f = static_cast<Finder*>(user);
    // redfs_find reads nothing, so the size costs a stat we would not otherwise
    // pay. Worth it here: it is the number that tells you whether the file you
    // half-remember is the one you meant.
    redfs_file_info info{};
    const uint64_t size = redfs_stat(f->depot, hash, &info) == REDFS_OK ? info.size : 0;
    std::printf("  0x%016" PRIX64 "  %9" PRIu64 "  %s\n", hash, size, path);
    return ++f->found < f->limit;
}

int cmd_find(redfs_depot* d, const char* list_file, const char* pattern, uint32_t limit) {
    uint32_t kept = 0;
    // Reported rather than swallowed: the usual failure here is a .kark list with
    // no Oodle to decompress it, and a bare exit code sends you looking at the
    // pattern instead of at the DLL.
    const redfs_status load = redfs_path_load(d, list_file, &kept);
    if (load != REDFS_OK) return die("path_load", load);

    // A bare word stays a substring search, which is what this command has
    // always done and what you want when half-remembering a name. Anything
    // carrying a wildcard is passed through as the glob it plainly is.
    std::string glob = pattern;
    if (glob.find('*') == std::string::npos && glob.find('?') == std::string::npos)
        glob = "*" + glob + "*";

    std::printf("searching %u known paths for \"%s\"\n", kept, glob.c_str());
    Finder f{d, 0, limit};
    uint32_t matched = 0;
    const redfs_status st = redfs_find(d, glob.c_str(), find_visit, &f, &matched);
    if (st != REDFS_OK) return die("find", st);
    // matched is the true total, so truncation is `total > shown`; a run of
    // exactly `limit` matches has dropped nothing.
    if (matched > f.found)
        std::printf("%u matches (showing %u -- raise the limit for the rest)\n", matched, f.found);
    else
        std::printf("%u matches\n", matched);
    return 0;
}

// --- verify ------------------------------------------------------------------
//
// Checks what RedFS decompresses against the SHA-1 the archive index already
// carries for that entry. That hash is an ORACLE: CDPR computed it at cook time,
// it is stored per entry at +0x24 of the file table, and nothing RedFS does can
// influence it. So a match is evidence the Kraken decode produced the original
// bytes -- not merely that it produced the same bytes as last time, which is all
// a checked-in baseline of our own output could ever show.
//
// It only applies to SINGLE-SEGMENT files. Measured against a stock 2.31 install:
// for buffer_count == 0 the index SHA-1 is the hash of the decompressed content
// (2,327 of 2,328 sampled), and for anything with attached buffers it matches
// neither the main segment, nor every segment concatenated, nor buffer 0 -- it
// covers something this reader cannot reconstruct. So multi-segment files are
// counted and skipped rather than reported as failures.
//
// That split is less limiting than it sounds, because it falls almost exactly on
// the bulk audio: 119,857 .wem and 1,878 .opuspak are single-segment, while
// .mesh and .xbm essentially never are. For those, see the WolvenKit round-trip
// in docs/verification.md.

struct Verifier {
    redfs_depot* depot;
    uint32_t checked, matched, skipped_multi, read_failed, limit;
    uint64_t bytes;
};

int verify_visit(uint64_t key, const char* path, void* user) {
    auto* v = static_cast<Verifier*>(user);

    redfs_file_info info{};
    if (redfs_stat(v->depot, key, &info) != REDFS_OK) return 1;
    if (info.buffer_count != 0) {
        ++v->skipped_multi;
        return v->checked + v->skipped_multi < v->limit;
    }

    redfs_blob blob{};
    const redfs_status st = redfs_read(v->depot, key, REDFS_PART_ALL, &blob);
    if (st != REDFS_OK) {
        ++v->read_failed;
        std::printf("  READ  %-58.58s %s\n", path, redfs_status_string(st));
        return v->checked + v->skipped_multi < v->limit;
    }

    ++v->checked;
    v->bytes += blob.size;
    uint8_t got[20];
    if (sha1(blob.data, blob.size, got) && std::memcmp(got, info.sha1, 20) == 0) {
        ++v->matched;
    } else {
        std::printf("  SHA1  %-58.58s\n        want ", path);
        for (int i = 0; i < 20; ++i) std::printf("%02x", info.sha1[i]);
        std::printf("\n        got  ");
        for (int i = 0; i < 20; ++i) std::printf("%02x", got[i]);
        std::printf("\n");
    }
    redfs_blob_free(&blob);
    return v->checked + v->skipped_multi < v->limit;
}

int cmd_verify(redfs_depot* d, const char* list_file, const char* pattern, uint32_t limit) {
    uint32_t kept = 0;
    const redfs_status load = redfs_path_load(d, list_file, &kept);
    if (load != REDFS_OK) return die("path_load", load);

    Verifier v{d, 0, 0, 0, 0, limit, 0};
    uint32_t total = 0;
    const auto t0 = Clock::now();
    const redfs_status st = redfs_find(d, pattern, verify_visit, &v, &total);
    if (st != REDFS_OK) return die("find", st);
    const double took = ms_since(t0);

    const uint32_t examined = v.checked + v.skipped_multi;
    std::printf("%u match \"%s\"\n", total, pattern);
    if (v.checked)
        std::printf("%u of %u matched the index SHA-1 (%.1f MB in %.1f s, %.0f MB/s)\n", v.matched,
                    v.checked, v.bytes / 1048576.0, took / 1000.0,
                    v.bytes / 1048576.0 / (took / 1000.0));
    if (v.read_failed) std::printf("%u failed to read\n", v.read_failed);

    // COVERAGE IS PART OF THE RESULT, not a footnote. The oracle only applies to
    // single-segment files, so a pattern selecting mostly meshes or textures gets
    // almost everything skipped -- and reporting that as success would be a green
    // light for having checked nothing, which is the failure this command exists
    // to catch. Skipping is therefore a non-zero exit: narrow the pattern to what
    // is actually verifiable (.wem, .opuspak, .json) if you want a clean run.
    std::printf("coverage: %u of %u examined were verifiable (%.1f%%)", v.checked, examined,
                examined ? 100.0 * v.checked / examined : 0.0);
    if (v.skipped_multi)
        std::printf("; %u skipped as multi-segment, where the index SHA-1 does not apply",
                    v.skipped_multi);
    std::printf("\n");

    if (!v.checked) {
        std::printf("FAILED: nothing verifiable matched\n");
        return 1;
    }
    if (v.matched != v.checked || v.read_failed) {
        std::printf("FAILED: %u mismatched, %u unreadable\n", v.checked - v.matched,
                    v.read_failed);
        return 1;
    }
    if (v.skipped_multi) {
        std::printf("INCOMPLETE: everything checked passed, but %u of %u could not be checked\n",
                    v.skipped_multi, examined);
        return 2;
    }
    return 0;
}

int cmd_stat(redfs_depot* d, const char* key) {
    const uint64_t h = parse_key(key);
    redfs_file_info fi{};
    const redfs_status st = redfs_stat(d, h, &fi);
    if (st != REDFS_OK) return die("stat", st);

    std::printf("hash            0x%016" PRIX64 "\n", fi.hash);
    std::printf("archive         %s\n", redfs_depot_archive_path(d, fi.archive_index));
    std::printf("size            %" PRIu64 " bytes (%" PRIu64 " on disk, %.1f%%)\n", fi.size,
                fi.compressed_size, fi.size ? 100.0 * fi.compressed_size / fi.size : 0.0);
    std::printf("buffers         %u\n", fi.buffer_count);
    std::printf("sha1            ");
    for (int i = 0; i < 20; ++i) std::printf("%02x", fi.sha1[i]);
    std::printf("\n");
    for (uint32_t b = 0; b < fi.buffer_count; ++b) {
        uint64_t sz = 0;
        if (redfs_part_size(d, h, b, &sz) == REDFS_OK)
            std::printf("  buffer %-2u     %" PRIu64 " bytes\n", b, sz);
    }
    return 0;
}

int cmd_extract(redfs_depot* d, const char* key, const char* out_path, const char* part_str) {
    const uint64_t h = parse_key(key);
    const uint32_t part = parse_part(part_str);

    const auto t0 = Clock::now();
    redfs_blob blob{};
    const redfs_status st = redfs_read(d, h, part, &blob);
    if (st != REDFS_OK) return die("read", st);
    const double took = ms_since(t0);

    FILE* f = std::fopen(out_path, "wb");
    if (!f) {
        redfs_blob_free(&blob);
        std::fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }
    std::fwrite(blob.data, 1, static_cast<size_t>(blob.size), f);
    std::fclose(f);
    std::printf("wrote %" PRIu64 " bytes to %s (decoded in %.2f ms, %.0f MB/s)\n", blob.size,
                out_path, took, blob.size / 1048576.0 / (took / 1000.0));
    redfs_blob_free(&blob);
    return 0;
}

int cmd_cr2w(redfs_depot* d, const char* key, uint32_t chunk, const char* prop_path) {
    const uint64_t h = parse_key(key);
    redfs_blob blob{};
    redfs_status st = redfs_read(d, h, REDFS_PART_MAIN, &blob);
    if (st != REDFS_OK) return die("read", st);

    redfs_cr2w* f = nullptr;
    st = redfs_cr2w_open(blob.data, blob.size, &f);
    if (st != REDFS_OK) {
        redfs_blob_free(&blob);
        return die("cr2w_open", st);
    }

    std::printf("root            %s\n", redfs_cr2w_root_type(f));
    std::printf("chunks          %u\n", redfs_cr2w_chunk_count(f));
    for (uint32_t i = 0; i < redfs_cr2w_chunk_count(f) && i < 24; ++i)
        std::printf("  [%3u] %s\n", i, redfs_cr2w_chunk_type(f, i));
    if (redfs_cr2w_chunk_count(f) > 24) std::printf("  ... %u more\n", redfs_cr2w_chunk_count(f) - 24);

    std::printf("imports         %u\n", redfs_cr2w_import_count(f));
    for (uint32_t i = 0; i < redfs_cr2w_import_count(f) && i < 16; ++i)
        std::printf("  %-24s %s\n", redfs_cr2w_import_type(f, i), redfs_cr2w_import_path(f, i));
    if (redfs_cr2w_import_count(f) > 16) std::printf("  ... %u more\n", redfs_cr2w_import_count(f) - 16);

    std::printf("properties of chunk %u%s%s:\n", chunk, prop_path ? "." : "",
                prop_path ? prop_path : "");
    int indent = 2;
    st = redfs_cr2w_walk(f, chunk, prop_path, walk_printer, &indent);
    if (st != REDFS_OK) std::fprintf(stderr, "walk: %s\n", redfs_status_string(st));

    redfs_cr2w_close(f);
    redfs_blob_free(&blob);
    return 0;
}

void print_tex(const redfs_texture_desc& t) {
    std::printf("dimensions      %ux%ux%u\n", t.width, t.height, t.depth);
    std::printf("mips / slices   %u / %u\n", t.mip_count, t.slice_count);
    std::printf("dxgi format     %u%s%s\n", t.dxgi_format, t.is_cubemap ? "  (cubemap)" : "",
                t.is_3d ? "  (volume)" : "");
    std::printf("pixel buffer    #%u, %" PRIu64 " bytes\n", t.buffer_index, t.data_size);
}

int cmd_tex(redfs_depot* d, const char* key, const char* out_path) {
    const uint64_t h = parse_key(key);
    redfs_texture_desc t{};
    redfs_status st = redfs_texture_desc_of(d, h, &t);
    if (st != REDFS_OK) return die("texture_desc_of", st);
    print_tex(t);

    if (!out_path) return 0;

    const auto t0 = Clock::now();
    redfs_blob dds{};
    st = redfs_texture_read_dds(d, h, &dds);
    if (st != REDFS_OK) return die("texture_read_dds", st);
    const double took = ms_since(t0);

    FILE* f = std::fopen(out_path, "wb");
    if (!f) {
        redfs_blob_free(&dds);
        std::fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }
    std::fwrite(dds.data, 1, static_cast<size_t>(dds.size), f);
    std::fclose(f);
    std::printf("wrote %" PRIu64 "-byte DDS to %s in %.2f ms\n", dds.size, out_path, took);
    redfs_blob_free(&dds);
    return 0;
}

int cmd_mesh(redfs_depot* d, const char* key) {
    redfs_mesh_desc m{};
    const redfs_status st = redfs_mesh_desc_of(d, parse_key(key), &m);
    if (st != REDFS_OK) return die("mesh_desc_of", st);
    std::printf("render buffer   #%u, %" PRIu64 " bytes\n", m.render_buffer_index,
                m.render_buffer_size);
    std::printf("vertex bytes    %u\n", m.vertex_buffer_size);
    std::printf("index bytes     %u at offset %u\n", m.index_buffer_size, m.index_buffer_offset);
    std::printf("submeshes       %u\n", m.submesh_count);
    std::printf("appearances     %u\n", m.appearance_count);
    std::printf("materials       %u\n", m.material_count);
    std::printf("bounds          (%g %g %g) .. (%g %g %g)\n", m.bbox_min[0], m.bbox_min[1],
                m.bbox_min[2], m.bbox_max[0], m.bbox_max[1], m.bbox_max[2]);
    return 0;
}

int print_riff_chunk(const char fourcc[4], uint64_t offset, uint64_t size, void*) {
    std::printf("  %c%c%c%c  at %-10" PRIu64 " %" PRIu64 " bytes\n", fourcc[0], fourcc[1],
                fourcc[2], fourcc[3], offset, size);
    return 1;
}

int cmd_audio(redfs_depot* d, const char* key) {
    const uint64_t h = parse_key(key);

    redfs_audio_format fmt = REDFS_AUDIO_UNKNOWN;
    const redfs_status st = redfs_audio_probe(d, h, &fmt);
    if (st != REDFS_OK) return die("audio_probe", st);
    static const char* names[] = {"unknown", "wem", "bnk", "opuspak", "opusinfo"};
    std::printf("container       %s\n", names[fmt]);

    if (fmt != REDFS_AUDIO_WEM) return 0;

    redfs_audio_info info{};
    if (redfs_audio_info_of(d, h, &info) != REDFS_OK) {
        std::fprintf(stderr, "audio_info_of: %s\n", redfs_last_error());
        return 1;
    }
    std::printf("codec           %s (tag 0x%04X)\n", redfs_audio_codec_name(info.codec),
                info.format_tag);
    std::printf("channels        %u\n", info.channels);
    std::printf("sample rate     %u Hz\n", info.sample_rate);
    std::printf("bits            %u\n", info.bits_per_sample);
    std::printf("payload         %" PRIu64 " bytes at offset %" PRIu64 "\n", info.data_size,
                info.data_offset);
    if (info.duration_seconds > 0)
        std::printf("duration        %.2f s\n", info.duration_seconds);

    // Wwise keeps codec state in non-standard chunks; a decoder front-end needs them.
    redfs_blob blob{};
    if (redfs_read(d, h, REDFS_PART_MAIN, &blob) == REDFS_OK) {
        std::printf("riff chunks:\n");
        redfs_audio_walk_chunks(blob.data, blob.size, print_riff_chunk, nullptr);
        redfs_blob_free(&blob);
    }
    return 0;
}

// --- selftest ----------------------------------------------------------------

struct Sample {
    std::vector<redfs_file_info> picked;
    uint64_t seen = 0;
    uint64_t stride = 1;
};

int sampler(const redfs_file_info* info, void* user) {
    auto* s = static_cast<Sample*>(user);
    if (s->seen++ % s->stride == 0 && s->picked.size() < 400) s->picked.push_back(*info);
    return 1;
}

int cmd_selftest(redfs_depot* d) {
    int failures = 0;

    // 1. hash vectors, straight from the FNV-1a spec
    struct {
        const char* in;
        uint64_t want;
    } vectors[] = {
        {"a", 0xaf63dc4c8601ec8cull},
        {"foobar", 0x85944171f73967e8ull},
        {"hello", 0xa430d84680aabd0bull},
        {"127.0.0.1", 0xaabafe7104d914beull},
    };
    for (const auto& v : vectors) {
        const uint64_t got = redfs_hash(v.in);
        if (got != v.want) {
            std::printf("FAIL  fnv1a64(\"%s\") = 0x%016" PRIX64 ", want 0x%016" PRIX64 "\n", v.in,
                        got, v.want);
            ++failures;
        }
    }
    // normalisation must fold case and separators
    if (redfs_hash("Base/Icon/Foo.XBM") != redfs_hash("base\\icon\\foo.xbm")) {
        std::printf("FAIL  path normalisation\n");
        ++failures;
    }
    std::printf("PASS  hashing (%zu vectors + normalisation)\n", sizeof(vectors) / sizeof(*vectors));

    // 2. sample files across the whole depot and decode them
    Sample s;
    const uint64_t total = redfs_depot_file_count(d);
    s.stride = total > 400 ? total / 400 : 1;
    redfs_enumerate(d, sampler, &s);

    uint64_t bytes = 0, cr2w_ok = 0, read_ok = 0;
    const auto t0 = Clock::now();
    for (const auto& fi : s.picked) {
        redfs_blob blob{};
        const redfs_status st = redfs_read(d, fi.hash, REDFS_PART_ALL, &blob);
        if (st != REDFS_OK) {
            std::printf("FAIL  read 0x%016" PRIX64 ": %s (%s)\n", fi.hash,
                        redfs_status_string(st), redfs_last_error());
            ++failures;
            continue;
        }
        if (blob.size != fi.size) {
            std::printf("FAIL  0x%016" PRIX64 " read %" PRIu64 " bytes, index said %" PRIu64 "\n",
                        fi.hash, blob.size, fi.size);
            ++failures;
        }
        ++read_ok;
        bytes += blob.size;

        redfs_cr2w* f = nullptr;
        if (redfs_cr2w_open(blob.data, blob.size, &f) == REDFS_OK) {
            if (redfs_cr2w_root_type(f)[0] != '\0') ++cr2w_ok;
            redfs_cr2w_close(f);
        }
        redfs_blob_free(&blob);
    }
    const double took = ms_since(t0);
    std::printf("PASS  decoded %" PRIu64 "/%zu sampled files, %.1f MB in %.0f ms (%.0f MB/s)\n",
                read_ok, s.picked.size(), bytes / 1048576.0, took,
                bytes / 1048576.0 / (took / 1000.0));
    std::printf("      %" PRIu64 " parsed as CR2W resources\n", cr2w_ok);

    // 3. find real textures and meshes by sniffing the sampled resources
    int tex_ok = 0, mesh_ok = 0;
    uint64_t first_tex = 0;
    for (const auto& fi : s.picked) {
        if (tex_ok >= 5 && mesh_ok >= 5) break;
        redfs_blob main{};
        if (redfs_read(d, fi.hash, REDFS_PART_MAIN, &main) != REDFS_OK) continue;
        redfs_cr2w* f = nullptr;
        if (redfs_cr2w_open(main.data, main.size, &f) != REDFS_OK) {
            redfs_blob_free(&main);
            continue;
        }
        const std::string root = redfs_cr2w_root_type(f);
        redfs_cr2w_close(f);
        redfs_blob_free(&main);

        if (tex_ok < 5 && (root == "CBitmapTexture" || root == "CCubeTexture" ||
                           root == "CTextureArray")) {
            redfs_texture_desc t{};
            const redfs_status st = redfs_texture_desc_of(d, fi.hash, &t);
            if (st != REDFS_OK) {
                std::printf("FAIL  texture 0x%016" PRIX64 " (%s): %s\n", fi.hash, root.c_str(),
                            redfs_last_error());
                ++failures;
                continue;
            }
            redfs_blob dds{};
            if (redfs_texture_read_dds(d, fi.hash, &dds) != REDFS_OK) {
                std::printf("FAIL  dds 0x%016" PRIX64 ": %s\n", fi.hash, redfs_last_error());
                ++failures;
                continue;
            }
            const bool magic_ok = dds.size > 148 && std::memcmp(dds.data, "DDS ", 4) == 0;
            std::printf("%s  %-14s 0x%016" PRIX64 "  %4ux%-4u mips=%-2u dxgi=%-3u  %" PRIu64
                        " byte DDS\n",
                        magic_ok ? "PASS " : "FAIL ", root.c_str(), fi.hash, t.width, t.height,
                        t.mip_count, t.dxgi_format, dds.size);
            if (!magic_ok) ++failures;
            if (!first_tex) first_tex = fi.hash;
            redfs_blob_free(&dds);
            ++tex_ok;
        } else if (mesh_ok < 5 && root == "CMesh") {
            redfs_mesh_desc m{};
            const redfs_status st = redfs_mesh_desc_of(d, fi.hash, &m);
            if (st != REDFS_OK) {
                std::printf("FAIL  mesh 0x%016" PRIX64 ": %s\n", fi.hash, redfs_last_error());
                ++failures;
                continue;
            }
            std::printf("PASS  CMesh          0x%016" PRIX64 "  submeshes=%-3u vtx=%-8u idx=%-8u "
                        "buffer=%" PRIu64 "\n",
                        fi.hash, m.submesh_count, m.vertex_buffer_size, m.index_buffer_size,
                        m.render_buffer_size);
            ++mesh_ok;
        }
    }
    if (tex_ok == 0) std::printf("WARN  no textures in the sample\n");
    if (mesh_ok == 0) std::printf("WARN  no meshes in the sample\n");

    // 4. async path
    if (first_tex) {
        struct Ctx {
            redfs_status st = REDFS_E_IO;
            uint64_t size = 0;
        } ctx;
        redfs_read_async(d, first_tex, REDFS_PART_MAIN,
                         [](redfs_status st, redfs_blob b, void* u) {
                             auto* c = static_cast<Ctx*>(u);
                             c->st = st;
                             c->size = b.size;
                             redfs_blob_free(&b);
                         },
                         &ctx);
        redfs_drain();
        if (ctx.st == REDFS_OK && ctx.size > 0) {
            std::printf("PASS  async read returned %" PRIu64 " bytes\n", ctx.size);
        } else {
            std::printf("FAIL  async read: %s\n", redfs_status_string(ctx.st));
            ++failures;
        }
    }

    std::printf("\n%s (%d failures)\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED", failures);
    return failures ? 1 : 0;
}

// Dumps the elements of an array property. This is how the chunk and appearance
// tables inside a real mesh were inspected while building redfs_mesh_*.
struct ArrCtx {
    redfs_cr2w* f;
    uint32_t limit;
    int depth;
};

int arr_printer(uint32_t index, const redfs_value* v, void* user) {
    auto* ctx = static_cast<ArrCtx*>(user);
    std::printf("[%u] %-24s ", index, v->type);
    print_value(*v);
    std::printf("\n");
    if (v->kind == REDFS_KIND_STRUCT) {
        Dump dump{ctx->f, 4, ctx->depth};
        redfs_cr2w_walk_in(ctx->f, v, nullptr, dump_prop, &dump);
    }
    return index + 1 < ctx->limit;
}

int cmd_arr(redfs_depot* d, const char* key, uint32_t chunk, const char* prop_path, uint32_t limit) {
    redfs_blob blob{};
    if (redfs_read(d, parse_key(key), REDFS_PART_MAIN, &blob) != REDFS_OK) return 1;
    redfs_cr2w* f = nullptr;
    if (redfs_cr2w_open(blob.data, blob.size, &f) != REDFS_OK) {
        redfs_blob_free(&blob);
        return 1;
    }

    redfs_value arr{};
    if (redfs_cr2w_get(f, chunk, prop_path, &arr) != REDFS_OK || arr.kind != REDFS_KIND_ARRAY) {
        std::fprintf(stderr, "%s is not an array\n", prop_path);
        redfs_cr2w_close(f);
        redfs_blob_free(&blob);
        return 1;
    }
    std::printf("%s: %" PRIu64 " elements of %s\n", prop_path, arr.as.u, arr.type);

    ArrCtx ctx{f, limit, 3};
    const redfs_status st = redfs_cr2w_walk_array(f, &arr, arr_printer, &ctx);
    if (st != REDFS_OK) std::fprintf(stderr, "walk_array: %s\n", redfs_status_string(st));

    redfs_cr2w_close(f);
    redfs_blob_free(&blob);
    return 0;
}

// The headline query: per-chunk LOD, material and bounding box, which is what
// turns "which chunks are the chest" into something you can compute.
int cmd_chunks(redfs_depot* d, const char* key, const char* appearance_name, uint32_t lod_filter) {
    const auto t0 = Clock::now();
    redfs_mesh* m = nullptr;
    const redfs_status st = redfs_mesh_open(d, parse_key(key), &m);
    if (st != REDFS_OK) return die("mesh_open", st);
    const double took = ms_since(t0);

    float lo[3], hi[3];
    redfs_mesh_bounds(m, lo, hi);
    std::printf("chunks          %u\n", redfs_mesh_chunk_count(m));
    std::printf("lods            %u\n", redfs_mesh_lod_count(m));
    std::printf("mesh bounds     (%.3f %.3f %.3f) .. (%.3f %.3f %.3f)\n", lo[0], lo[1], lo[2],
                hi[0], hi[1], hi[2]);

    std::printf("appearances     %u\n", redfs_mesh_appearance_count(m));
    for (uint32_t a = 0; a < redfs_mesh_appearance_count(m) && a < 8; ++a)
        std::printf("  [%2u] %s\n", a, redfs_mesh_appearance_name(m, a));

    int32_t app = 0;
    if (appearance_name) {
        app = redfs_mesh_find_appearance(m, appearance_name);
        if (app < 0) {
            std::printf("no appearance named \"%s\"; using index 0\n", appearance_name);
            app = 0;
        }
    }
    std::printf("\nmaterials shown for appearance \"%s\"\n",
                redfs_mesh_appearance_name(m, static_cast<uint32_t>(app)));
    std::printf("%-4s %-4s %-7s %-7s %-26s %-23s %s\n", "idx", "lod", "verts", "tris", "material",
                "bbox min (x y z)", "bbox max (x y z)");

    for (uint32_t i = 0; i < redfs_mesh_chunk_count(m); ++i) {
        const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
        if (!c) continue;
        if (lod_filter && c->lod != lod_filter) continue;
        std::printf("%-4u %-4u %-7u %-7u %-26s %7.3f %7.3f %7.3f  %7.3f %7.3f %7.3f\n", c->index,
                    c->lod, c->vertex_count, c->index_count / 3,
                    redfs_mesh_chunk_material(m, static_cast<uint32_t>(app), c->index),
                    c->bbox_min[0], c->bbox_min[1], c->bbox_min[2], c->bbox_max[0], c->bbox_max[1],
                    c->bbox_max[2]);
    }
    std::printf("\ndecoded in %.2f ms (cache holds %u meshes)\n", took, redfs_cache_entry_count());
    redfs_mesh_close(m);
    return 0;
}

// --- bench -------------------------------------------------------------------
// Answers "does reading a file in the middle of a 13 GB archive cost more than
// one at the front?" by timing reads bucketed by how deep into the archive the
// data sits.

struct BenchPick {
    uint64_t hash;
    uint64_t size;
    uint32_t archive;
};

int bench_collect(const redfs_file_info* info, void* user) {
    auto* v = static_cast<std::vector<BenchPick>*>(user);
    if (info->size > 16 * 1024 && info->size < 4 * 1024 * 1024)
        v->push_back(BenchPick{info->hash, info->size, info->archive_index});
    return 1;
}

// What does fetching one voice line out of the archives cost, and what is in it?
//
// The question this answers is a consumer's: a mod that plays game dialogue at
// runtime needs to know whether a fetch fits in a frame. It measures only the part
// RedFS owns -- index lookup, read with Kraken decode, RIFF parse, payload located
// -- because RedFS deliberately does not decode audio. Every voice line in the
// shipped game is Wwise Vorbis, whose setup headers Wwise strips, so turning one
// into PCM needs a codebook rebuild plus a Vorbis decoder that lives elsewhere.
// The codec column exists so that stops being a surprise.
// Writes `bytes` to a temp file and returns its path, or an empty string.
std::string write_temp(const char* stem, const void* bytes, size_t n) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = ".";
    std::string p = std::string(tmp) + "\\redfs_" + stem;
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return {};
    const bool ok = std::fwrite(bytes, 1, n, f) == n;
    std::fclose(f);
    if (!ok) {
        std::remove(p.c_str());
        return {};
    }
    return p;
}

// Validates a WAV rather than trusting an exit code: magic, PCM tag, and the
// internal consistency of byteRate/blockAlign against rate/channels/bits. A
// decoder that half-failed usually still writes a file.
struct WavCheck {
    bool ok = false;
    uint32_t rate = 0, frames = 0, data_bytes = 0;
    uint16_t channels = 0, bits = 0;
    double seconds = 0;
};

WavCheck check_wav(const std::string& path) {
    WavCheck w;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return w;
    uint8_t h[44];
    const bool got = std::fread(h, 1, sizeof h, f) == sizeof h;
    std::fseek(f, 0, SEEK_END);
    const long total = std::ftell(f);
    std::fclose(f);
    if (!got || total < 44) return w;

    auto u16 = [&h](int o) { return static_cast<uint16_t>(h[o] | (h[o + 1] << 8)); };
    auto u32 = [&h](int o) {
        return static_cast<uint32_t>(h[o] | (h[o + 1] << 8) | (h[o + 2] << 16) |
                                    (static_cast<uint32_t>(h[o + 3]) << 24));
    };
    if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return w;
    if (std::memcmp(h + 12, "fmt ", 4) != 0 || std::memcmp(h + 36, "data", 4) != 0) return w;
    if (u16(20) != 1) return w;  // not PCM

    w.channels = u16(22);
    w.rate = u32(24);
    w.bits = u16(34);
    w.data_bytes = u32(40);
    if (!w.channels || !w.rate || !w.bits) return w;

    const uint32_t block = w.channels * w.bits / 8u;
    if (u16(32) != block) return w;                 // blockAlign disagrees
    if (u32(28) != w.rate * block) return w;        // byteRate disagrees
    if (u32(4) != static_cast<uint32_t>(total) - 8) return w;  // riff size disagrees
    if (w.data_bytes % block) return w;             // partial frame

    w.frames = w.data_bytes / block;
    w.seconds = static_cast<double>(w.frames) / w.rate;
    w.ok = w.frames > 0;
    return w;
}

int cmd_voice(redfs_depot* d, const char* list, int want, const char* vgmstream) {
    uint32_t kept = 0;
    if (redfs_path_load(d, list, &kept) != REDFS_OK) {
        std::printf("path list: %s\n", redfs_last_error());
        return 1;
    }

    std::vector<uint64_t> voices;
    redfs_enumerate(
        d,
        [](const redfs_file_info* i, void* u) {
            const char* p = redfs_path_from_hash(i->hash);
            if (!p) return 1;
            const size_t n = std::strlen(p);
            // Voice-over only: \vo\ excludes sfx and music, which have very
            // different sizes and would blur the distribution.
            if (n > 4 && std::strcmp(p + n - 4, ".wem") == 0 && std::strstr(p, "\\vo\\"))
                static_cast<std::vector<uint64_t>*>(u)->push_back(i->hash);
            return 1;
        },
        &voices);

    if (voices.empty()) {
        std::printf("no voice-over .wem resolved; is this the right path list?\n");
        return 1;
    }
    std::printf("voice lines in depot   %zu\n", voices.size());

    std::map<std::string, int> codecs;
    std::vector<double> read_ms, parse_ms, decode_ms;
    std::vector<uint64_t> sizes;
    double secs = 0, audio_secs = 0;
    uint64_t bytes = 0, pcm_bytes = 0;
    int decoded = 0, decode_failed = 0, wav_bad = 0;
    // Stride rather than take a prefix: enumeration is hash-ordered, so a prefix
    // would sample one corner of the hash space and possibly one archive.
    const size_t stride = voices.size() / static_cast<size_t>(want > 0 ? want : 1) + 1;

    for (size_t k = 0; k < voices.size() && static_cast<int>(read_ms.size()) < want; k += stride) {
        const auto t0 = std::chrono::steady_clock::now();
        redfs_blob b{};
        if (redfs_read(d, voices[k], REDFS_PART_MAIN, &b) != REDFS_OK) continue;
        const auto t1 = std::chrono::steady_clock::now();

        redfs_audio_info ai{};
        const redfs_status st = redfs_audio_info_parse(b.data, b.size, &ai);
        const auto t2 = std::chrono::steady_clock::now();

        if (st == REDFS_OK && ai.data_size > 0) {
            codecs[redfs_audio_codec_name(ai.codec)]++;
            read_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            parse_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
            sizes.push_back(b.size);
            bytes += b.size;
            secs += ai.duration_seconds;

            // Optional second half: hand the bytes to an external decoder and
            // verify what comes back is a real WAV. RedFS supplies the .wem; the
            // codec is somebody else's, deliberately.
            if (vgmstream) {
                const std::string wem = write_temp("voice.wem", b.data, (size_t)b.size);
                if (!wem.empty()) {
                    const std::string wav = std::string(wem) + ".wav";
                    // Quoted twice: cmd.exe strips the outer pair, so a path with
                    // spaces (Program Files, and TEMP under a named user) survives.
                    std::string cmd = "\"\"" + std::string(vgmstream) + "\" -o \"" + wav +
                                      "\" \"" + wem + "\"\" >nul 2>&1";
                    const auto d0 = std::chrono::steady_clock::now();
                    const int rc = std::system(cmd.c_str());
                    const auto d1 = std::chrono::steady_clock::now();

                    if (rc != 0) {
                        ++decode_failed;
                    } else {
                        const WavCheck w = check_wav(wav);
                        if (!w.ok) {
                            ++wav_bad;
                        } else {
                            ++decoded;
                            decode_ms.push_back(
                                std::chrono::duration<double, std::milli>(d1 - d0).count());
                            audio_secs += w.seconds;
                            pcm_bytes += w.data_bytes;
                        }
                    }
                    // Nothing is kept: this measures and verifies, it does not export.
                    std::remove(wav.c_str());
                    std::remove(wem.c_str());
                }
            }
        }
        redfs_blob_free(&b);
    }
    if (read_ms.empty()) {
        std::printf("nothing parsed\n");
        return 1;
    }

    // Sort up front rather than inside the accessor. Function arguments are
    // unsequenced, so `at(v, 0.5)` and `v.back()` in one printf could evaluate
    // back() before the sort -- which printed a max below the p90.
    std::sort(read_ms.begin(), read_ms.end());
    std::sort(parse_ms.begin(), parse_ms.end());
    std::sort(sizes.begin(), sizes.end());
    auto at = [](const std::vector<double>& v, double p) {
        return v[static_cast<size_t>(v.size() * p)];
    };

    std::printf("sampled                %zu lines, %.1f MB\n\n", read_ms.size(),
                bytes / 1048576.0);
    for (const auto& [name, n] : codecs)
        std::printf("codec                  %s (%d)\n", name.c_str(), n);
    std::printf("file size              median %llu bytes, max %llu\n",
                static_cast<unsigned long long>(sizes[sizes.size() / 2]),
                static_cast<unsigned long long>(sizes.back()));
    if (secs > 0)
        std::printf("audio duration         %.1f s across the sample\n", secs);
    std::printf("\n  %-22s %9s %9s %9s\n", "stage", "median", "p90", "max");
    std::printf("  %-22s %8.3f  %8.3f  %8.3f\n", "read (incl. Kraken)", at(read_ms, 0.5),
                at(read_ms, 0.9), read_ms.back());
    std::printf("  %-22s %8.4f  %8.4f  %8.4f\n", "parse header", at(parse_ms, 0.5),
                at(parse_ms, 0.9), parse_ms.back());
    if (!decode_ms.empty()) {
        std::sort(decode_ms.begin(), decode_ms.end());
        std::printf("  %-22s %8.1f  %8.1f  %8.1f\n", "decode to WAV (ext)", at(decode_ms, 0.5),
                    at(decode_ms, 0.9), decode_ms.back());
    }
    std::printf("\nmilliseconds. Payload offset and size are resolved by the parse, so the first\n");
    std::printf("two stages are the whole cost of getting playable bytes addressable.\n");

    if (vgmstream) {
        std::printf("\nWAV verification (%s)\n", vgmstream);
        std::printf("  decoded and valid    %d\n", decoded);
        std::printf("  decoder failed       %d\n", decode_failed);
        std::printf("  WAV rejected         %d\n", wav_bad);
        if (decoded) {
            std::printf("  audio produced       %.1f s, %.1f MB of PCM\n", audio_secs,
                        pcm_bytes / 1048576.0);
            std::printf("  expansion            %.1fx over the .wem bytes\n",
                        static_cast<double>(pcm_bytes) / static_cast<double>(bytes));
        }
        std::printf("\nEvery WAV is checked field by field -- RIFF/WAVE/fmt/data present, PCM tag,\n");
        std::printf("and byteRate and blockAlign consistent with rate, channels and bit depth --\n");
        std::printf("because a half-failed decode still writes a file. Nothing is kept on disk.\n");
        std::printf("The decoder is external on purpose: every voice line is Wwise Vorbis, whose\n");
        std::printf("setup headers Wwise strips, and RedFS does not bundle codecs.\n");
    } else {
        std::printf("Decoding is not included. Pass a vgmstream-cli path as the third argument\n");
        std::printf("to decode each line to WAV and verify it.\n");
    }
    return 0;
}

int cmd_bench(redfs_depot* d) {
    std::vector<BenchPick> all;
    redfs_enumerate(d, bench_collect, &all);
    if (all.empty()) {
        std::printf("nothing to benchmark\n");
        return 1;
    }

    // Work inside the single archive that holds the most files, so position is
    // the only variable.
    std::vector<uint32_t> per_archive(redfs_depot_archive_count(d), 0);
    for (const auto& p : all) per_archive[p.archive]++;
    const uint32_t target = static_cast<uint32_t>(
        std::max_element(per_archive.begin(), per_archive.end()) - per_archive.begin());

    std::vector<BenchPick> picks;
    for (const auto& p : all)
        if (p.archive == target) picks.push_back(p);

    std::printf("archive         %s\n", redfs_depot_archive_path(d, target));
    std::printf("candidates      %zu files\n\n", picks.size());
    std::printf("  %-8s %-12s %-12s %-10s %s\n", "depth", "reads", "bytes", "avg ms", "MB/s");

    // Enumeration is hash-ordered, i.e. effectively random with respect to
    // physical offset, so slicing it into deciles samples the whole file.
    constexpr int kBuckets = 5;
    constexpr int kPerBucket = 60;
    for (int b = 0; b < kBuckets; ++b) {
        const size_t start = picks.size() * b / kBuckets;
        uint64_t bytes = 0;
        double total_ms = 0;
        int reads = 0;
        for (int i = 0; i < kPerBucket && start + i < picks.size(); ++i) {
            const auto& p = picks[start + i];
            const auto t0 = Clock::now();
            redfs_blob blob{};
            if (redfs_read(d, p.hash, REDFS_PART_ALL, &blob) != REDFS_OK) continue;
            total_ms += ms_since(t0);
            bytes += blob.size;
            redfs_blob_free(&blob);
            ++reads;
        }
        if (!reads) continue;
        std::printf("  %d-%d%%    %-12d %-12" PRIu64 " %-10.3f %.0f\n", b * 100 / kBuckets,
                    (b + 1) * 100 / kBuckets, reads, bytes, total_ms / reads,
                    bytes / 1048576.0 / (total_ms / 1000.0));
    }

    // Single-file latency, cold-ish: one random file, nothing else touched.
    const auto& one = picks[picks.size() / 2];
    const auto t0 = Clock::now();
    redfs_blob blob{};
    redfs_read(d, one.hash, REDFS_PART_ALL, &blob);
    const double single = ms_since(t0);
    std::printf("\none file from the middle: 0x%016" PRIX64 ", %" PRIu64 " bytes in %.3f ms\n",
                one.hash, blob.size, single);
    redfs_blob_free(&blob);
    return 0;
}

void usage() {
    std::printf(
        "redfs_cli [--game DIR] [--cache FILE] <command>\n"
        "\n"
        "  --game DIR                    install root; omit to auto-detect\n"
        "  --cache FILE                  persist decoded meshes between runs\n"
        "\n"
        "<key> is a depot path or a literal 0x-prefixed hash.\n"
        "<list> is a path dictionary: usedhashes.kark, or one path per line.\n"
        "\n"
        "  info                          list mounted archives\n"
        "  hash <path>...                depot path -> 64-bit key\n"
        "  paths <list> [key...]         load a dictionary, resolve hashes to paths\n"
        "  find <list> <pattern> [n]     files matching a glob (* ?); a bare word\n"
        "                                is taken as a substring\n"
        "  verify <list> <pattern> [n]   decode matches and check them against the\n"
        "                                SHA-1 in the archive index (single-segment\n"
        "                                files only -- .wem, .opuspak, .json)\n"
        "  stat <key>                    where a file lives and how big it is\n"
        "  extract <key> <out> [part]    part = main | all | <buffer index>\n"
        "  cr2w <key> [chunk] [path]     chunks, imports and properties\n"
        "  arr <key> <chunk> <path> [n]  elements of an array property\n"
        "  chunks <key> [appear] [lod]   per-chunk lod, material and bounding box\n"
        "  tex <key> [out.dds]           texture descriptor, optionally as DDS\n"
        "  mesh <key>                    mesh geometry layout\n"
        "  audio <key>                   sniff the audio container\n"
        "  voice <list> [n] [vgmstream]  cost of fetching n random voice lines;\n"
        "                                with vgmstream-cli.exe, also decode each to\n"
        "                                WAV and verify it (nothing is kept)\n"
        "  bench                         read cost vs position inside an archive\n"
        "  selftest                      verify the library against the install\n"
        "\n"
        "Set REDFS_VERBOSE=1 for internal logging.\n");
}

}  // namespace

int main(int argc, char** argv) {
    const char* game_dir = nullptr;
    const char* cache_file = nullptr;
    int i = 1;
    for (;;) {
        if (i + 1 < argc && std::strcmp(argv[i], "--game") == 0) {
            game_dir = argv[i + 1];
            i += 2;
        } else if (i + 1 < argc && std::strcmp(argv[i], "--cache") == 0) {
            cache_file = argv[i + 1];
            i += 2;
        } else {
            break;
        }
    }
    if (i >= argc) {
        usage();
        return 2;
    }
    if (std::getenv("REDFS_VERBOSE")) redfs_set_log(on_log, nullptr);

    const char* cmd = argv[i++];
    const int rest = argc - i;
    char** args = argv + i;

    if (std::strcmp(cmd, "hash") == 0) return cmd_hash(rest, args);

    redfs_depot* d = mount(game_dir);
    if (!d) return 1;

    if (cache_file) {
        const auto tc = Clock::now();
        if (redfs_cache_open(d, cache_file) == REDFS_OK)
            std::printf("mesh cache      %u entries from %s in %.0f ms\n",
                        redfs_cache_entry_count(), cache_file, ms_since(tc));
    }

    int rc = 2;
    if (std::strcmp(cmd, "info") == 0) rc = cmd_info(d);
    else if (std::strcmp(cmd, "selftest") == 0) rc = cmd_selftest(d);
    else if (std::strcmp(cmd, "bench") == 0) rc = cmd_bench(d);
    else if (std::strcmp(cmd, "paths") == 0 && rest >= 1)
        rc = cmd_paths(d, args[0], rest - 1, args + 1);
    else if (std::strcmp(cmd, "find") == 0 && rest >= 2)
        rc = cmd_find(d, args[0], args[1], rest >= 3 ? std::strtoul(args[2], nullptr, 10) : 40);
    else if (std::strcmp(cmd, "verify") == 0 && rest >= 2)
        rc = cmd_verify(d, args[0], args[1], rest >= 3 ? std::strtoul(args[2], nullptr, 10) : 500);
    else if (std::strcmp(cmd, "chunks") == 0 && rest >= 1)
        rc = cmd_chunks(d, args[0], rest >= 2 ? args[1] : nullptr,
                        rest >= 3 ? std::strtoul(args[2], nullptr, 10) : 0);
    else if (std::strcmp(cmd, "arr") == 0 && rest >= 3)
        rc = cmd_arr(d, args[0], std::strtoul(args[1], nullptr, 10), args[2],
                     rest >= 4 ? std::strtoul(args[3], nullptr, 10) : 3);
    else if (std::strcmp(cmd, "stat") == 0 && rest >= 1) rc = cmd_stat(d, args[0]);
    else if (std::strcmp(cmd, "extract") == 0 && rest >= 2)
        rc = cmd_extract(d, args[0], args[1], rest >= 3 ? args[2] : nullptr);
    else if (std::strcmp(cmd, "cr2w") == 0 && rest >= 1)
        rc = cmd_cr2w(d, args[0], rest >= 2 ? std::strtoul(args[1], nullptr, 10) : 0,
                      rest >= 3 ? args[2] : nullptr);
    else if (std::strcmp(cmd, "tex") == 0 && rest >= 1)
        rc = cmd_tex(d, args[0], rest >= 2 ? args[1] : nullptr);
    else if (std::strcmp(cmd, "mesh") == 0 && rest >= 1) rc = cmd_mesh(d, args[0]);
    else if (std::strcmp(cmd, "audio") == 0 && rest >= 1) rc = cmd_audio(d, args[0]);
    else if (std::strcmp(cmd, "voice") == 0 && rest >= 1)
        rc = cmd_voice(d, args[0], rest >= 2 ? std::atoi(args[1]) : 300,
                       rest >= 3 ? args[2] : nullptr);
    else usage();

    if (cache_file) redfs_cache_close();
    redfs_depot_close(d);
    return rc;
}
