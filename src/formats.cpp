// Format helpers built on top of the depot + CR2W reader.
//
// These exist because "give me the bytes" is not what a mod actually wants. A
// cooked .xbm is a CR2W document describing a texture plus a separate buffer of
// GPU-ready pixels; nothing can consume that directly. Stitching a DDS header
// onto the payload turns it into something DirectXTex, DirectXTK or a bare
// CreateTexture2D call will accept.

#include "internal.hpp"

#include <algorithm>
#include <vector>

#include <string.h>  // _stricmp

namespace redfs {
namespace {

// --- DXGI ---------------------------------------------------------------------

enum : uint32_t {
    DXGI_UNKNOWN = 0,
    DXGI_R32G32B32A32_FLOAT = 2,
    DXGI_R16G16B16A16_FLOAT = 10,
    DXGI_R16G16B16A16_UNORM = 11,
    DXGI_R8G8B8A8_UNORM = 28,
    DXGI_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_R8G8_UNORM = 49,
    DXGI_R16_FLOAT = 54,
    DXGI_R8_UNORM = 61,
    DXGI_A8_UNORM = 65,
    DXGI_BC1_UNORM = 71,
    DXGI_BC1_UNORM_SRGB = 72,
    DXGI_BC3_UNORM = 77,
    DXGI_BC3_UNORM_SRGB = 78,
    DXGI_BC4_UNORM = 80,
    DXGI_BC5_UNORM = 83,
    DXGI_BC6H_UF16 = 95,
    DXGI_BC7_UNORM = 98,
    DXGI_BC7_UNORM_SRGB = 99
};

// STextureGroupSetup.compression + .rawFormat + .isGamma -> DXGI_FORMAT.
// Mirrors WolvenKit's DDSUtils.GenerateHeader.
uint32_t dxgi_from_setup(const char* compression, const char* raw_format, bool gamma) {
    auto is = [](const char* a, const char* b) { return std::strcmp(a, b) == 0; };

    if (!compression || !*compression || is(compression, "TCM_None")) {
        if (!raw_format || !*raw_format) return DXGI_R8G8B8A8_UNORM;
        if (is(raw_format, "TRF_TrueColor")) return gamma ? DXGI_R8G8B8A8_UNORM_SRGB : DXGI_R8G8B8A8_UNORM;
        if (is(raw_format, "TRF_DeepColor")) return DXGI_R16G16B16A16_UNORM;
        if (is(raw_format, "TRF_Grayscale")) return DXGI_R8_UNORM;
        if (is(raw_format, "TRF_HDRFloat")) return DXGI_R32G32B32A32_FLOAT;
        if (is(raw_format, "TRF_HDRHalf")) return DXGI_R16G16B16A16_FLOAT;
        if (is(raw_format, "TRF_HDRFloatGrayscale")) return DXGI_R16_FLOAT;
        if (is(raw_format, "TRF_R8G8")) return DXGI_R8G8_UNORM;
        if (is(raw_format, "TRF_Grayscale_Font")) return DXGI_A8_UNORM;
        return DXGI_R8G8B8A8_UNORM;  // TRF_Invalid and anything newer
    }
    if (is(compression, "TCM_DXTNoAlpha")) return gamma ? DXGI_BC1_UNORM_SRGB : DXGI_BC1_UNORM;
    if (is(compression, "TCM_DXTAlpha") || is(compression, "TCM_DXTAlphaLinear"))
        return gamma ? DXGI_BC3_UNORM_SRGB : DXGI_BC3_UNORM;
    if (is(compression, "TCM_Normalmap")) return DXGI_BC5_UNORM;
    if (is(compression, "TCM_Normals_DEPRECATED")) return DXGI_BC1_UNORM;
    if (is(compression, "TCM_NormalsHigh_DEPRECATED")) return DXGI_BC3_UNORM;
    if (is(compression, "TCM_QualityR")) return DXGI_BC4_UNORM;
    if (is(compression, "TCM_QualityRG")) return DXGI_BC5_UNORM;
    if (is(compression, "TCM_QualityColor")) return gamma ? DXGI_BC7_UNORM_SRGB : DXGI_BC7_UNORM;
    if (is(compression, "TCM_HalfHDR_Unsigned")) return DXGI_BC6H_UF16;
    return DXGI_UNKNOWN;
}

bool is_block_compressed(uint32_t fmt) { return fmt >= DXGI_BC1_UNORM && fmt <= DXGI_BC7_UNORM_SRGB; }

uint32_t block_bytes(uint32_t fmt) {
    switch (fmt) {
        case DXGI_BC1_UNORM:
        case DXGI_BC1_UNORM_SRGB:
        case DXGI_BC4_UNORM:
            return 8;
        default:
            return 16;
    }
}

uint32_t bits_per_pixel(uint32_t fmt) {
    switch (fmt) {
        case DXGI_R32G32B32A32_FLOAT: return 128;
        case DXGI_R16G16B16A16_FLOAT:
        case DXGI_R16G16B16A16_UNORM: return 64;
        case DXGI_R8G8B8A8_UNORM:
        case DXGI_R8G8B8A8_UNORM_SRGB: return 32;
        case DXGI_R8G8_UNORM:
        case DXGI_R16_FLOAT: return 16;
        case DXGI_R8_UNORM:
        case DXGI_A8_UNORM: return 8;
        default: return 32;
    }
}

// Bytes a full mip chain of this format and extent occupies, per D3D's surface
// rules. Used to sanity-check the blob header against the payload it describes.
uint64_t mip_chain_bytes(uint32_t fmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t mips,
                         uint32_t slices) {
    if (w == 0 || h == 0 || mips == 0) return 0;
    uint64_t total = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = (std::max)(1u, w >> m);
        const uint32_t mh = (std::max)(1u, h >> m);
        const uint32_t md = (std::max)(1u, depth >> m);
        const uint64_t level =
            is_block_compressed(fmt)
                ? static_cast<uint64_t>((mw + 3) / 4) * ((mh + 3) / 4) * block_bytes(fmt)
                : static_cast<uint64_t>(mw) * mh * bits_per_pixel(fmt) / 8;
        total += level * md;
    }
    return total * (std::max)(1u, slices);
}

// --- DDS ----------------------------------------------------------------------

constexpr uint32_t kDdsMagic = 0x20534444;  // 'DDS '
constexpr size_t kDdsHeaderBytes = 4 + 124 + 20;

constexpr uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8,
                   DDSD_PIXELFORMAT = 0x1000, DDSD_MIPMAPCOUNT = 0x20000, DDSD_LINEARSIZE = 0x80000,
                   DDSD_DEPTH = 0x800000;
constexpr uint32_t DDPF_FOURCC = 0x4;
constexpr uint32_t DDSCAPS_COMPLEX = 0x8, DDSCAPS_TEXTURE = 0x1000, DDSCAPS_MIPMAP = 0x400000;
constexpr uint32_t DDSCAPS2_CUBEMAP_ALLFACES = 0xFE00, DDSCAPS2_VOLUME = 0x200000;
constexpr uint32_t D3D10_TEXTURE2D = 3, D3D10_TEXTURE3D = 4;
constexpr uint32_t D3D11_MISC_TEXTURECUBE = 0x4;
constexpr uint32_t DDS_ALPHA_MODE_STRAIGHT = 1;

struct Writer {
    uint8_t* p;
    void u32(uint32_t v) {
        std::memcpy(p, &v, 4);
        p += 4;
    }
    void zeros(size_t n) {
        std::memset(p, 0, n);
        p += n;
    }
};

void write_dds_header(uint8_t* dst, const redfs_texture_desc& d) {
    uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    uint32_t pitch_or_linear;
    if (is_block_compressed(d.dxgi_format)) {
        const uint32_t bw = (std::max)(1u, (d.width + 3) / 4);
        const uint32_t bh = (std::max)(1u, (d.height + 3) / 4);
        pitch_or_linear = bw * bh * block_bytes(d.dxgi_format);
        flags |= DDSD_LINEARSIZE;
    } else {
        pitch_or_linear = (d.width * bits_per_pixel(d.dxgi_format) + 7) / 8;
        flags |= DDSD_PITCH;
    }
    if (d.mip_count > 1) flags |= DDSD_MIPMAPCOUNT;
    if (d.is_3d && d.depth > 1) flags |= DDSD_DEPTH;

    uint32_t caps = DDSCAPS_TEXTURE;
    if (d.mip_count > 1) caps |= DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
    if (d.is_cubemap || d.slice_count > 1 || d.is_3d) caps |= DDSCAPS_COMPLEX;

    uint32_t caps2 = 0;
    if (d.is_cubemap) caps2 |= DDSCAPS2_CUBEMAP_ALLFACES;
    if (d.is_3d && d.depth > 1) caps2 |= DDSCAPS2_VOLUME;

    Writer w{dst};
    w.u32(kDdsMagic);
    w.u32(124);  // DDS_HEADER::dwSize
    w.u32(flags);
    w.u32(d.height);
    w.u32(d.width);
    w.u32(pitch_or_linear);
    w.u32(d.is_3d ? (std::max)(1u, d.depth) : 1u);
    w.u32((std::max)(1u, d.mip_count));
    w.zeros(44);  // dwReserved1[11]
    // DDS_PIXELFORMAT: always DX10, so the real format lives in the DXT10 header.
    w.u32(32);
    w.u32(DDPF_FOURCC);
    w.u32(0x30315844);  // 'DX10'
    w.zeros(20);
    w.u32(caps);
    w.u32(caps2);
    w.zeros(12);  // caps3, caps4, reserved2
    // DDS_HEADER_DXT10
    w.u32(d.dxgi_format);
    w.u32(d.is_3d ? D3D10_TEXTURE3D : D3D10_TEXTURE2D);
    w.u32(d.is_cubemap ? D3D11_MISC_TEXTURECUBE : 0u);
    w.u32((std::max)(1u, d.slice_count));
    w.u32(DDS_ALPHA_MODE_STRAIGHT);
}

// --- shared plumbing ----------------------------------------------------------

// Reads a file's CR2W segment and parses it. `storage` keeps the bytes alive.
redfs_status open_resource(const redfs_depot* depot, uint64_t hash, std::vector<uint8_t>* storage,
                           redfs_cr2w* out) {
    uint64_t size = 0;
    redfs_status st = read_part(depot, hash, REDFS_PART_MAIN, nullptr, 0, &size);
    if (st != REDFS_OK) return st;
    storage->resize(static_cast<size_t>(size));
    st = read_part(depot, hash, REDFS_PART_MAIN, storage->data(), size, &size);
    if (st != REDFS_OK) return st;
    return cr2w_parse(storage->data(), size, out);
}

int32_t find_chunk(const redfs_cr2w& f, const char* type) {
    for (uint32_t i = 0; i < f.chunks.size(); ++i)
        if (std::strcmp(f.name(f.chunks[i].class_name), type) == 0) return static_cast<int32_t>(i);
    return -1;
}

uint64_t uint_or(const redfs_cr2w& f, uint32_t chunk, const char* path, uint64_t fallback) {
    redfs_value v{};
    if (cr2w_find(&f, chunk, path, &v) != REDFS_OK) return fallback;
    if (v.kind == REDFS_KIND_UINT) return v.as.u;
    if (v.kind == REDFS_KIND_INT) return static_cast<uint64_t>(v.as.i);
    if (v.kind == REDFS_KIND_BOOL) return v.as.u;
    return fallback;
}

const char* name_or(const redfs_cr2w& f, uint32_t chunk, const char* path, const char* fallback) {
    redfs_value v{};
    if (cr2w_find(&f, chunk, path, &v) != REDFS_OK) return fallback;
    return v.kind == REDFS_KIND_NAME ? v.as.s : fallback;
}

float float_or(const redfs_cr2w& f, uint32_t chunk, const char* path, float fallback) {
    redfs_value v{};
    if (cr2w_find(&f, chunk, path, &v) != REDFS_OK) return fallback;
    return v.kind == REDFS_KIND_FLOAT ? static_cast<float>(v.as.f) : fallback;
}

redfs_status describe_texture(const redfs_depot* depot, uint64_t hash, const redfs_cr2w& f,
                              redfs_texture_desc* out) {
    // A .mesh embeds its own CBitmapTexture chunks, so "this file contains a
    // texture blob" is not the same as "this file is a texture". Insist on the
    // root chunk actually being a texture resource, otherwise we would happily
    // describe a mesh's first embedded texture and read `setup` off the mesh.
    if (f.chunks.empty()) return fail(REDFS_E_CORRUPT, "CR2W has no chunks");
    const char* root = f.name(f.chunks[0].class_name);
    const bool is_texture = std::strcmp(root, "CBitmapTexture") == 0 ||
                            std::strcmp(root, "CTextureArray") == 0 ||
                            std::strcmp(root, "CCubeTexture") == 0;
    if (!is_texture)
        return fail(REDFS_E_UNSUPPORTED, "root chunk is %s, not a texture resource", root);

    // Follow the resource's own handle rather than searching, so we get *its*
    // blob and not some other chunk that happens to share the class.
    int32_t blob = -1;
    redfs_value handle{};
    if (cr2w_find(&f, 0, "renderTextureResource.renderResourceBlobPC", &handle) == REDFS_OK &&
        handle.kind == REDFS_KIND_HANDLE && handle.as.chunk >= 0)
        blob = handle.as.chunk;
    if (blob < 0) blob = find_chunk(f, "rendRenderTextureBlobPC");
    if (blob < 0)
        return fail(REDFS_E_UNSUPPORTED, "%s has no rendRenderTextureBlobPC (console cook?)", root);

    std::memset(out, 0, sizeof(*out));
    out->width = static_cast<uint32_t>(uint_or(f, blob, "header.sizeInfo.width", 0));
    out->height = static_cast<uint32_t>(uint_or(f, blob, "header.sizeInfo.height", 0));
    out->depth = static_cast<uint32_t>(uint_or(f, blob, "header.sizeInfo.depth", 1));
    out->mip_count = static_cast<uint32_t>(uint_or(f, blob, "header.textureInfo.mipCount", 1));
    out->slice_count = static_cast<uint32_t>(uint_or(f, blob, "header.textureInfo.sliceCount", 1));

    // `setup` on the root carries how the payload was compressed at cook time.
    const char* compression = name_or(f, 0, "setup.compression", "TCM_None");
    const char* raw_format = name_or(f, 0, "setup.rawFormat", "");
    const bool gamma = uint_or(f, 0, "setup.isGamma", 0) != 0;
    out->dxgi_format = dxgi_from_setup(compression, raw_format, gamma);

    const char* type = name_or(f, blob, "header.textureInfo.type", "TEXTYPE_2D");
    out->is_cubemap = std::strcmp(type, "TEXTYPE_CUBE") == 0 ? 1u : 0u;
    out->is_3d = std::strcmp(type, "TEXTYPE_3D") == 0 ? 1u : 0u;

    // textureData is a deferred buffer: the value is an index into this file's
    // attached buffers, which map 1:1 onto the archive segments after the main one.
    redfs_value tex{};
    out->buffer_index = 0;
    if (cr2w_find(&f, blob, "textureData", &tex) == REDFS_OK && tex.kind == REDFS_KIND_BUFFER)
        out->buffer_index = tex.as.buffer;

    if (out->width == 0 || out->height == 0)
        return fail(REDFS_E_CORRUPT, "texture header has zero extent");
    if (out->dxgi_format == DXGI_UNKNOWN)
        return fail(REDFS_E_UNSUPPORTED, "unmapped texture format (compression=%s rawFormat=%s)",
                    compression, raw_format);

    uint64_t data_size = 0;
    const redfs_status st = read_part(depot, hash, out->buffer_index, nullptr, 0, &data_size);
    if (st != REDFS_OK) return st;
    out->data_size = data_size;

    // Reconcile the header against the payload. On a minority of textures CDPR
    // writes the mip-biased extent into sizeInfo while the buffer still holds
    // the unbiased surface, and a DDS whose header disagrees with its payload
    // decodes to garbage. The payload is the actual GPU resource, so when a
    // power-of-two rescale makes the two agree exactly, take that reading.
    if (mip_chain_bytes(out->dxgi_format, out->width, out->height, out->depth, out->mip_count,
                        out->slice_count) != data_size) {
        for (uint32_t shift = 1; shift <= 4; ++shift) {
            const uint32_t w = out->width << shift;
            const uint32_t h = out->height << shift;
            if (mip_chain_bytes(out->dxgi_format, w, h, out->depth, out->mip_count + shift,
                                out->slice_count) != data_size)
                continue;
            log("texture 0x%016llX: header says %ux%u/%u mips but the payload is %ux%u/%u; "
                "using the payload",
                static_cast<unsigned long long>(hash), out->width, out->height, out->mip_count, w,
                h, out->mip_count + shift);
            out->width = w;
            out->height = h;
            out->mip_count += shift;
            break;
        }
    }
    return REDFS_OK;
}

}  // namespace

// --- public-facing implementations -------------------------------------------

redfs_status texture_desc_of(const redfs_depot* depot, uint64_t hash, redfs_texture_desc* out) {
    std::vector<uint8_t> storage;
    redfs_cr2w f;
    redfs_status st = open_resource(depot, hash, &storage, &f);
    if (st != REDFS_OK) return st;
    return describe_texture(depot, hash, f, out);
}

redfs_status texture_read_raw(const redfs_depot* depot, uint64_t hash, redfs_texture_desc* out_desc,
                              redfs_blob* out_blob) {
    redfs_texture_desc d{};
    redfs_status st = texture_desc_of(depot, hash, &d);
    if (st != REDFS_OK) return st;

    st = blob_alloc(d.data_size, out_blob);
    if (st != REDFS_OK) return st;

    uint64_t written = 0;
    st = read_part(depot, hash, d.buffer_index, out_blob->data, d.data_size, &written);
    if (st != REDFS_OK) return st;

    if (out_desc) *out_desc = d;
    return REDFS_OK;
}

redfs_status texture_read_dds(const redfs_depot* depot, uint64_t hash, redfs_blob* out_blob) {
    redfs_texture_desc d{};
    redfs_status st = texture_desc_of(depot, hash, &d);
    if (st != REDFS_OK) return st;

    st = blob_alloc(kDdsHeaderBytes + d.data_size, out_blob);
    if (st != REDFS_OK) return st;

    write_dds_header(out_blob->data, d);
    uint64_t written = 0;
    return read_part(depot, hash, d.buffer_index, out_blob->data + kDdsHeaderBytes, d.data_size,
                     &written);
}

redfs_status audio_probe(const redfs_depot* depot, uint64_t hash, redfs_audio_format* out) {
    uint8_t head[16] = {};
    uint64_t total = 0;
    redfs_status st = read_part(depot, hash, REDFS_PART_MAIN, nullptr, 0, &total);
    if (st != REDFS_OK) return st;

    // Decode only enough to see the magic. read_part needs the whole segment, so
    // cap the work by refusing to sniff absurdly large mains.
    std::vector<uint8_t> buf(static_cast<size_t>((std::min<uint64_t>)(total, 1u << 20)));
    if (total <= buf.size()) {
        uint64_t got = 0;
        st = read_part(depot, hash, REDFS_PART_MAIN, buf.data(), buf.size(), &got);
        if (st != REDFS_OK) return st;
        std::memcpy(head, buf.data(), (std::min<size_t>)(sizeof(head), buf.size()));
    }

    *out = REDFS_AUDIO_UNKNOWN;
    const uint32_t magic = rd32(head);
    if (magic == 0x46464952) *out = REDFS_AUDIO_WEM;        // 'RIFF'
    else if (magic == 0x44484B42) *out = REDFS_AUDIO_BNK;   // 'BKHD'
    else if (magic == 0x5367674F) *out = REDFS_AUDIO_OPUSPAK;  // 'OggS'
    else if (magic == kCr2wMagic) *out = REDFS_AUDIO_UNKNOWN;  // a cooked resource, not raw audio
    return REDFS_OK;
}

// --- .wem (Wwise RIFF) --------------------------------------------------------
//
// A .wem is a RIFF/WAVE file with Wwise's own extensions: a normal 'fmt ' chunk
// whose wFormatTag may be a Wwise-private value, a 'data' chunk, and codec state
// in non-standard chunks such as 'vorb' and 'seek'.
//
// RedFS parses the header and reports where the payload is. It deliberately does
// not decode: Vorbis and Opus would mean bundling codecs, and Wwise Vorbis needs
// its stripped codebooks rebuilt anyway (that is what ww2ogg and vgmstream do).
// Knowing the codec, the layout and the payload offset is what a caller actually
// needs to hand it to a decoder it already has.

namespace {

constexpr uint32_t kRiff = 0x46464952;  // 'RIFF'
constexpr uint32_t kWave = 0x45564157;  // 'WAVE'
constexpr uint32_t kFmt  = 0x20746D66;  // 'fmt '
constexpr uint32_t kData = 0x61746164;  // 'data'

// Wwise wFormatTag values seen in shipping content.
redfs_audio_codec codec_from_tag(uint32_t tag) {
    switch (tag) {
        case 0x0001: return REDFS_CODEC_PCM;
        case 0x0002: return REDFS_CODEC_ADPCM;
        case 0x0166: return REDFS_CODEC_XMA2;
        case 0x3039:
        case 0x3040:
        case 0x3041: return REDFS_CODEC_OPUS;
        case 0xFFFF: return REDFS_CODEC_VORBIS;
        // WAVE_FORMAT_EXTENSIBLE: the real codec lives in the GUID, which Wwise
        // does not populate usefully. Report unknown rather than guess.
        case 0xFFFE: return REDFS_CODEC_UNKNOWN;
        default: return REDFS_CODEC_UNKNOWN;
    }
}

// Walks RIFF chunks, calling `visit(fourcc, payload_offset, payload_size)`.
template <typename Visit>
redfs_status riff_walk(const uint8_t* b, uint64_t size, Visit&& visit) {
    if (!b || size < 12) return fail(REDFS_E_CORRUPT, "not a RIFF file (too small)");
    if (rd32(b) != kRiff) return fail(REDFS_E_CORRUPT, "missing RIFF magic");
    if (rd32(b + 8) != kWave) return fail(REDFS_E_UNSUPPORTED, "RIFF form is not WAVE");

    // The declared RIFF size excludes the first 8 bytes; clamp to what we have so
    // a truncated or over-declared file cannot walk past the buffer.
    const uint64_t declared = static_cast<uint64_t>(rd32(b + 4)) + 8;
    const uint64_t limit = (std::min)(declared, size);

    uint64_t at = 12;
    while (at + 8 <= limit) {
        const uint32_t id = rd32(b + at);
        const uint64_t chunk_size = rd32(b + at + 4);
        const uint64_t payload = at + 8;
        if (payload + chunk_size > size) break;  // truncated; stop rather than read past

        if (!visit(id, b + payload, payload, chunk_size)) break;

        at = payload + chunk_size;
        if (chunk_size & 1) ++at;  // RIFF chunks are word-aligned
    }
    return REDFS_OK;
}

}  // namespace

redfs_status audio_info_parse(const void* data, uint64_t size, redfs_audio_info* out) {
    std::memset(out, 0, sizeof(*out));
    out->container = REDFS_AUDIO_WEM;

    const uint8_t* b = static_cast<const uint8_t*>(data);
    bool saw_fmt = false;

    const redfs_status st = riff_walk(
        b, size,
        [&](uint32_t id, const uint8_t* p, uint64_t offset, uint64_t chunk_size) {
            if (id == kFmt && chunk_size >= 16) {
                saw_fmt = true;
                out->format_tag = rd16(p);
                out->channels = rd16(p + 2);
                out->sample_rate = rd32(p + 4);
                out->avg_bytes_per_sec = rd32(p + 8);
                out->bits_per_sample = rd16(p + 14);
                out->codec = codec_from_tag(out->format_tag);
            } else if (id == kData) {
                out->data_offset = offset;
                out->data_size = chunk_size;
            }
            return true;  // keep walking; 'data' can precede other chunks
        });
    if (st != REDFS_OK) return st;
    if (!saw_fmt) return fail(REDFS_E_CORRUPT, "wem has no fmt chunk");

    // Duration is only honest where the layout makes it so. For PCM the sample
    // count follows from the block size; for compressed codecs the byte rate is
    // an average and would give a misleading answer, so leave it at zero rather
    // than report something a caller might trust.
    if (out->codec == REDFS_CODEC_PCM && out->channels && out->bits_per_sample) {
        const uint64_t frame = static_cast<uint64_t>(out->channels) * (out->bits_per_sample / 8);
        if (frame) {
            out->total_samples = out->data_size / frame;
            if (out->sample_rate)
                out->duration_seconds =
                    static_cast<double>(out->total_samples) / out->sample_rate;
        }
    }
    return REDFS_OK;
}

redfs_status audio_info_of(const redfs_depot* depot, uint64_t hash, redfs_audio_info* out) {
    uint64_t size = 0;
    redfs_status st = read_part(depot, hash, REDFS_PART_MAIN, nullptr, 0, &size);
    if (st != REDFS_OK) return st;
    if (size < 12) return fail(REDFS_E_CORRUPT, "file is too small to be a wem");

    std::vector<uint8_t> buf(static_cast<size_t>(size));
    st = read_part(depot, hash, REDFS_PART_MAIN, buf.data(), size, &size);
    if (st != REDFS_OK) return st;
    return audio_info_parse(buf.data(), size, out);
}

redfs_status audio_walk_chunks(const void* data, uint64_t size, redfs_riff_chunk_fn fn,
                               void* user) {
    if (!fn) return REDFS_E_INVALID_ARG;
    return riff_walk(static_cast<const uint8_t*>(data), size,
                     [&](uint32_t id, const uint8_t*, uint64_t offset, uint64_t chunk_size) {
                         char fourcc[4];
                         std::memcpy(fourcc, &id, 4);
                         return fn(fourcc, offset, chunk_size, user) != 0;
                     });
}

redfs_status mesh_desc_of(const redfs_depot* depot, uint64_t hash, redfs_mesh_desc* out) {
    std::vector<uint8_t> storage;
    redfs_cr2w f;
    redfs_status st = open_resource(depot, hash, &storage, &f);
    if (st != REDFS_OK) return st;

    const int32_t blob = find_chunk(f, "rendRenderMeshBlob");
    if (blob < 0) return fail(REDFS_E_UNSUPPORTED, "no rendRenderMeshBlob chunk (not a .mesh?)");

    std::memset(out, 0, sizeof(*out));

    redfs_value rb{};
    if (cr2w_find(&f, blob, "renderBuffer", &rb) == REDFS_OK && rb.kind == REDFS_KIND_BUFFER)
        out->render_buffer_index = rb.as.buffer;

    out->vertex_buffer_size = static_cast<uint32_t>(uint_or(f, blob, "header.vertexBufferSize", 0));
    out->index_buffer_size = static_cast<uint32_t>(uint_or(f, blob, "header.indexBufferSize", 0));
    out->index_buffer_offset = static_cast<uint32_t>(uint_or(f, blob, "header.indexBufferOffset", 0));

    // Cooked meshes carry renderChunkInfos; renderChunks only appears on some
    // uncooked variants.
    redfs_value v{};
    if ((cr2w_find(&f, blob, "header.renderChunkInfos", &v) == REDFS_OK ||
         cr2w_find(&f, blob, "header.renderChunks", &v) == REDFS_OK) &&
        v.kind == REDFS_KIND_ARRAY)
        out->submesh_count = static_cast<uint32_t>(v.as.u);
    if (cr2w_find(&f, 0, "appearances", &v) == REDFS_OK && v.kind == REDFS_KIND_ARRAY)
        out->appearance_count = static_cast<uint32_t>(v.as.u);
    if (cr2w_find(&f, 0, "materialEntries", &v) == REDFS_OK && v.kind == REDFS_KIND_ARRAY)
        out->material_count = static_cast<uint32_t>(v.as.u);

    out->bbox_min[0] = float_or(f, 0, "boundingBox.Min.X", 0.f);
    out->bbox_min[1] = float_or(f, 0, "boundingBox.Min.Y", 0.f);
    out->bbox_min[2] = float_or(f, 0, "boundingBox.Min.Z", 0.f);
    out->bbox_max[0] = float_or(f, 0, "boundingBox.Max.X", 0.f);
    out->bbox_max[1] = float_or(f, 0, "boundingBox.Max.Y", 0.f);
    out->bbox_max[2] = float_or(f, 0, "boundingBox.Max.Z", 0.f);

    uint64_t sz = 0;
    if (read_part(depot, hash, out->render_buffer_index, nullptr, 0, &sz) == REDFS_OK)
        out->render_buffer_size = sz;
    return REDFS_OK;
}

}  // namespace redfs
