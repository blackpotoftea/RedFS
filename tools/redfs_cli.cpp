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
#include <string>
#include <vector>

#include "redfs.h"

namespace {

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

// Finds files whose resolved path contains a substring -- the practical way to
// locate a mesh when you only half-remember where it lives.
struct Finder {
    const char* needle;
    uint32_t found;
    uint32_t limit;
};

int find_visit(const redfs_file_info* info, void* user) {
    auto* f = static_cast<Finder*>(user);
    const char* path = redfs_path_from_hash(info->hash);
    if (!path || !std::strstr(path, f->needle)) return 1;
    std::printf("  0x%016" PRIX64 "  %9" PRIu64 "  %s\n", info->hash, info->size, path);
    return ++f->found < f->limit;
}

int cmd_find(redfs_depot* d, const char* list_file, const char* needle, uint32_t limit) {
    uint32_t kept = 0;
    if (redfs_path_load(d, list_file, &kept) != REDFS_OK) return 1;
    std::printf("searching %u known paths for \"%s\"\n", kept, needle);
    Finder f{needle, 0, limit};
    redfs_enumerate(d, find_visit, &f);
    std::printf("%u matches\n", f.found);
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
        "  find <list> <substr> [n]      files whose path contains substr\n"
        "  stat <key>                    where a file lives and how big it is\n"
        "  extract <key> <out> [part]    part = main | all | <buffer index>\n"
        "  cr2w <key> [chunk] [path]     chunks, imports and properties\n"
        "  arr <key> <chunk> <path> [n]  elements of an array property\n"
        "  chunks <key> [appear] [lod]   per-chunk lod, material and bounding box\n"
        "  tex <key> [out.dds]           texture descriptor, optionally as DDS\n"
        "  mesh <key>                    mesh geometry layout\n"
        "  audio <key>                   sniff the audio container\n"
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
    else usage();

    if (cache_file) redfs_cache_close();
    redfs_depot_close(d);
    return rc;
}
