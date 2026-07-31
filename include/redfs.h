/*
 * RedFS - direct read access to Cyberpunk 2077 `.archive` files from native mods.
 *
 * Lets a mod pull textures, audio, meshes or any other resource straight out of
 * the player's own game install at runtime, so the mod does not have to ship
 * extracted + repacked copies of game assets.
 *
 * Stable C ABI. Safe to consume from any compiler / language with C FFI.
 *
 * Threading: a redfs_depot is immutable once opened. redfs_read* / redfs_stat /
 * redfs_enumerate / redfs_find / redfs_path_* / redfs_texture_* / redfs_mesh_*
 * are safe to call concurrently from any number of threads -- the path
 * dictionary carries its own lock, which is also what lets import learning run
 * inside any read. redfs_depot_mount / redfs_depot_mount_dir /
 * redfs_depot_close / redfs_shutdown / redfs_cache_* are NOT (open first, then
 * share) -- mounting rebuilds the depot index in place, and a concurrent read
 * walks it while it is being reallocated.
 *
 * An individual redfs_cr2w handle is SINGLE-THREADED: decoding a CString caches
 * it on the handle, so two threads calling redfs_cr2w_get on the same handle
 * race. Sharing the depot is fine; give each thread its own CR2W handle. The
 * typed helpers above are unaffected -- each builds a private handle per call.
 *
 * All reads are synchronous and can take milliseconds (Oodle decode). Call them
 * from a worker thread, or use redfs_read_async.
 */
#ifndef REDFS_H
#define REDFS_H

#include <stddef.h>
#include <stdint.h>

#if defined(REDFS_STATIC)
#  define REDFS_API
#elif defined(REDFS_BUILD_DLL)
#  define REDFS_API __declspec(dllexport)
#else
#  define REDFS_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on any breaking change to the layout of structs or the meaning of a call.
 *
 * 2: redfs_mesh_chunk gained bounds_valid. Recompile anything built against 1.
 */
#define REDFS_ABI_VERSION 2

/* ------------------------------------------------------------------------- */
/* status                                                                     */
/* ------------------------------------------------------------------------- */

typedef enum redfs_status {
    REDFS_OK = 0,
    REDFS_E_NOT_FOUND    = -1,  /* no such file in the depot / no such property */
    REDFS_E_IO           = -2,  /* the archive could not be opened or mapped */
    REDFS_E_CORRUPT      = -3,  /* archive or CR2W data failed validation */
    REDFS_E_OODLE        = -4,  /* oo2ext_7_win64.dll missing, or decode failed */
    REDFS_E_INVALID_ARG  = -5,
    REDFS_E_OOM          = -6,
    REDFS_E_UNSUPPORTED  = -7,  /* known format, not implemented by this build */
    REDFS_E_RANGE        = -8,  /* index / destination buffer out of range */
    REDFS_E_CANCELLED    = -9,  /* async read dropped by shutdown or depot close */
    /* redfs_find with nothing in the path dictionary to search. Distinct from
     * NOT_FOUND on purpose: that one means "no such file", and reusing it here
     * would read as "nothing matched" -- the exact misreading this status
     * exists to prevent. See redfs_path_load. */
    REDFS_E_NO_DICTIONARY = -10
} redfs_status;

REDFS_API uint32_t    redfs_abi_version(void);
REDFS_API const char* redfs_status_string(redfs_status status);

/* ------------------------------------------------------------------------- */
/* depot                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct redfs_depot redfs_depot;

/*
 * Which archive sets to mount. Mount order decides overrides -- later wins --
 * and REDFS_SCAN_ALL reproduces the game's own order:
 *
 *   content -> ep1 -> REDmod (mods/) -> legacy (archive/pc/mod/)
 *
 * so a legacy mod overrides a REDmod one, which overrides base game. Under
 * archive/pc the archives mount in ordinal filename order, so the alphabetically
 * LAST one wins -- which is why modders prefix with zz_. REDmod is the
 * exception: mod folders are taken in name order, but the archives within one
 * folder mount in reverse, so there the alphabetically FIRST one wins.
 *
 * REDmod is also the only set searched RECURSIVELY -- mods/<name>/archives and
 * everything beneath it, ordered by full path. archive/pc/mod is top level only.
 * Both match the game's own behaviour.
 */
typedef enum redfs_scan_flags {
    REDFS_SCAN_CONTENT = 1u << 0,  /* archive/pc/content   (base game)          */
    REDFS_SCAN_EP1     = 1u << 1,  /* archive/pc/ep1       (Phantom Liberty)    */
    REDFS_SCAN_MODS    = 1u << 2,  /* archive/pc/mod       (legacy .archive mods) */
    REDFS_SCAN_REDMOD  = 1u << 3,  /* mods/<name>/archives (REDmod)             */
    REDFS_SCAN_ALL     = 0xFu
} redfs_scan_flags;

/*
 * Open a depot over a Cyberpunk 2077 install.
 *
 * game_dir  root of the install (the folder holding bin/, archive/, r6/).
 *           Pass NULL to auto-detect from the running process (works when
 *           called from inside the game, e.g. a RED4ext plugin).
 * flags     which archive folders to mount; REDFS_SCAN_ALL is the usual choice.
 *
 * Index-only: file *contents* are never read here.
 */
REDFS_API redfs_status redfs_depot_open(const char* game_dir, uint32_t flags, redfs_depot** out_depot);

/*
 * An empty depot, to be filled with redfs_depot_mount / redfs_depot_mount_dir.
 * For callers assembling a set of archives by hand -- a mod-manager staging
 * tree, a test, a single archive you were handed -- rather than scanning an
 * install. Never fails for want of a game directory.
 */
REDFS_API redfs_status redfs_depot_open_empty(redfs_depot** out_depot);

/* Mount one extra .archive on top of everything already mounted (highest priority). */
REDFS_API redfs_status redfs_depot_mount(redfs_depot* depot, const char* archive_path);

/*
 * Mount every .archive in a folder, in name order, on top of what is already
 * mounted -- for mod trees outside the game folder, such as an MO2 or Vortex
 * staging directory. Not needed when running inside a game MO2 launched: its
 * VFS already makes those archives appear under archive/pc/mod, where
 * REDFS_SCAN_MODS finds them.
 *
 * Returns REDFS_E_NOT_FOUND if the folder holds no archives; out_mounted, when
 * given, receives how many were added.
 */
REDFS_API redfs_status redfs_depot_mount_dir(redfs_depot* depot, const char* dir,
                                             uint32_t* out_mounted);

/* Cancels anything still queued by redfs_read_async against this depot -- those
 * callbacks fire with REDFS_E_CANCELLED -- and waits out a read already in
 * flight, bounded by one segment decode. So closing a depot with work
 * outstanding is safe; it is not a reason to call redfs_drain first. */
REDFS_API void redfs_depot_close(redfs_depot* depot);

REDFS_API uint32_t    redfs_depot_archive_count(const redfs_depot* depot);
REDFS_API const char* redfs_depot_archive_path(const redfs_depot* depot, uint32_t index);
REDFS_API uint64_t    redfs_depot_file_count(const redfs_depot* depot);
/* Bytes of process memory held by the mounted indices. */
REDFS_API uint64_t    redfs_depot_index_bytes(const redfs_depot* depot);

/* ------------------------------------------------------------------------- */
/* paths                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Depot path -> 64-bit key, the only way to address a file.
 *
 * Applies the engine's normalisation first: trims quotes/slashes/space at both
 * ends, collapses runs of separators, '/' -> '\', ASCII lowercase. So
 * "Base/Icon/Foo.xbm" and "base\icon\foo.xbm" hash identically.
 *
 * Archives store only these hashes -- there is no path table to enumerate, so a
 * mod must know the paths it wants (from WolvenKit, or a shipped list).
 */
REDFS_API uint64_t redfs_hash(const char* depot_path);

/* Same, for a non NUL-terminated path. */
REDFS_API uint64_t redfs_hash_n(const char* depot_path, size_t length);

/*
 * Decimal-string forms of the same key, for hosts that cannot hold a uint64
 * exactly -- Lua numbers are doubles and lose precision above 2^53, so a hash
 * has to cross that boundary as text.
 *
 * redfs_hash_string writes the decimal key and returns its length, or 0 if the
 * buffer is too small. 21 bytes is always enough.
 */
#define REDFS_HASH_STRING_MAX 21
REDFS_API size_t   redfs_hash_string(const char* depot_path, char* out, size_t capacity);
REDFS_API uint64_t redfs_hash_parse(const char* decimal);

/* ------------------------------------------------------------------------- */
/* hash -> path                                                               */
/* ------------------------------------------------------------------------- */

/*
 * The reverse direction is not computable -- FNV1a is one-way and archives keep
 * no path table -- so it is a dictionary lookup. RedFS fills the dictionary
 * from, cheapest first:
 *
 *   1. CR2W import tables, learned automatically as you read files. Free, and
 *      the only source that knows paths a mod invented.
 *   2. A path list you load: WolvenKit's usedhashes.kark, or plain text, one
 *      path per line.
 *   3. redfs_path_add.
 *
 * Only source 2 is filtered against the mounted depot -- on a stock install that
 * keeps roughly a quarter of WolvenKit's shipped list. Sources 1 and 3 are not,
 * so a hit tells you what a file is CALLED, not that it is readable: check
 * redfs_exists, or just handle REDFS_E_NOT_FOUND from the read.
 *
 * Import learning is off until the dictionary is switched on by either
 * redfs_path_load or redfs_path_enable.
 */
REDFS_API redfs_status redfs_path_load(const redfs_depot* depot, const char* list_file,
                                       uint32_t* out_kept);
REDFS_API void         redfs_path_enable(void);
REDFS_API void         redfs_path_add(const char* depot_path);
REDFS_API uint32_t     redfs_path_count(void);

/* NULL when the hash is not in the dictionary. The returned string stays valid
 * for the lifetime of the process: later additions from any source cannot
 * invalidate a pointer you already hold. */
REDFS_API const char* redfs_path_from_hash(uint64_t hash);

/* ------------------------------------------------------------------------- */
/* find                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Files whose path matches a glob -- the way to answer "which of these are
 * meshes?" without reading anything.
 *
 * `pattern` is matched against the NORMALISED path, so it is case-insensitive
 * and '/' and '\' are the same separator: "Base/Characters/*.MESH" and
 * "base\characters\*.mesh" behave identically. Two wildcards:
 *
 *   *   any run of characters, INCLUDING separators
 *   ?   exactly one character, which MAY be a separator
 *
 * Both crossing separators is deliberate and differs from a shell glob. The
 * common query is "every mesh anywhere", and under shell rules "*.mesh" would
 * match nothing at all. Narrow with a prefix instead: "base\characters\*.mesh".
 * A pattern ending in a separator means everything beneath it, so
 * "base\characters\" is shorthand for "base\characters\*".
 *
 * With `depot` given, only files the depot INDEX HOLDS are reported. That is
 * presence, not readability -- a read can still fail REDFS_E_OODLE or
 * REDFS_E_CORRUPT. Pass NULL to search the dictionary unfiltered, which also
 * reports paths learned from imports or redfs_path_add that no mounted archive
 * holds at all.
 *
 * THIS SEARCHES THE DICTIONARY, NOT THE DEPOT. Archives carry no path table, so
 * it can only find what redfs_path_load, redfs_path_add or import learning have
 * taught it. The dictionary is PROCESS-GLOBAL while this filter is not, so
 * searching one depot with a list loaded against another reports their
 * intersection. With nothing in it at all you get REDFS_E_NO_DICTIONARY rather
 * than an empty success, because "there was nothing to search" and "nothing
 * matched" are different answers.
 *
 * out_matched, when given, receives the TOTAL number of matches -- not the
 * number delivered. Returning 0 from `fn` stops delivery, not the search: the
 * scan and its allocation have both completed before `fn` is called at all, so
 * stopping early saves callback work and nothing else. Count deliveries
 * yourself if you need them.
 *
 * The strings handed to `fn` are interned exactly as redfs_path_from_hash's are.
 *
 * `fn` is called with no lock held, so it may call back into RedFS -- including
 * reads, which is the point. Paths those reads learn do not join a walk already
 * in progress.
 */
typedef int (*redfs_find_fn)(uint64_t hash, const char* path, void* user);
REDFS_API redfs_status redfs_find(const redfs_depot* depot, const char* pattern,
                                  redfs_find_fn fn, void* user, uint32_t* out_matched);

/* ------------------------------------------------------------------------- */
/* lookup                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct redfs_file_info {
    uint64_t hash;
    uint64_t size;             /* total bytes once decompressed (all segments) */
    uint64_t compressed_size;  /* bytes on disk                                */
    uint32_t buffer_count;     /* attached buffers, i.e. segments after main   */
    uint32_t archive_index;    /* index into redfs_depot_archive_path()        */
    int64_t  timestamp;        /* Windows FILETIME                             */
    uint8_t  sha1[20];
} redfs_file_info;

REDFS_API int          redfs_exists(const redfs_depot* depot, uint64_t hash);
REDFS_API redfs_status redfs_stat(const redfs_depot* depot, uint64_t hash, redfs_file_info* out_info);

/* Return 0 to stop the walk. Do not mutate the depot from the callback -- the
 * walk holds no lock and iterates the index in place. */
typedef int (*redfs_enum_fn)(const redfs_file_info* info, void* user);
REDFS_API redfs_status redfs_enumerate(const redfs_depot* depot, redfs_enum_fn fn, void* user);

/* ------------------------------------------------------------------------- */
/* reading                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * A file is stored as N segments. Segment 0 is the resource itself (a CR2W
 * document for cooked resources); segments 1..N-1 are its attached buffers,
 * which hold the bulk payload -- pixel data for a texture, vertex streams for a
 * mesh, and so on.
 */
#define REDFS_PART_MAIN      0xFFFFFFFFu  /* segment 0 only: the CR2W document  */
#define REDFS_PART_ALL       0xFFFFFFFEu  /* every segment, concatenated        */
/* 0..buffer_count-1 selects one attached buffer. */

typedef struct redfs_blob {
    uint8_t* data;
    uint64_t size;
    void*    reserved;  /* owned by RedFS; do not touch */
} redfs_blob;

/* Allocates; release with redfs_blob_free. */
REDFS_API redfs_status redfs_read(const redfs_depot* depot, uint64_t hash, uint32_t part, redfs_blob* out_blob);

/* Decompressed byte count of `part`, without reading it. */
REDFS_API redfs_status redfs_part_size(const redfs_depot* depot, uint64_t hash, uint32_t part, uint64_t* out_size);

/* Zero-allocation variant. REDFS_E_RANGE if capacity is too small; out_written
 * still receives the required size in that case. */
REDFS_API redfs_status redfs_read_into(const redfs_depot* depot, uint64_t hash, uint32_t part,
                                       void* dst, uint64_t capacity, uint64_t* out_written);

REDFS_API void redfs_blob_free(redfs_blob* blob);

/* --- async ---------------------------------------------------------------- */

/*
 * Queue a read on RedFS's worker thread. `cb` runs on that worker, never on the
 * caller's thread -- marshal back to the game thread yourself. When the callback
 * receives REDFS_OK it owns `blob` and must redfs_blob_free it.
 *
 * The return value decides whether the callback fires at all:
 *   REDFS_OK             -- queued; the callback WILL fire exactly once, with
 *                           either a result or REDFS_E_CANCELLED
 *   REDFS_E_CANCELLED    -- refused because redfs_shutdown is in progress; the
 *                           callback will NOT fire, handle it here
 *   REDFS_E_INVALID_ARG  -- likewise not queued, callback will not fire
 *
 * Chaining the next read from inside a callback is fine and is the normal
 * pattern; during shutdown that chained call simply returns REDFS_E_CANCELLED.
 *
 * Do NOT call redfs_drain or redfs_shutdown from a callback -- both would wait
 * on the very job you are completing. They detect it and return without acting.
 * Do not close a depot from a callback either: redfs_depot_close cannot cancel
 * from the worker thread, so anything still queued against that depot outlives
 * the free.
 */
typedef void (*redfs_read_fn)(redfs_status status, redfs_blob blob, void* user);
REDFS_API redfs_status redfs_read_async(const redfs_depot* depot, uint64_t hash, uint32_t part,
                                        redfs_read_fn cb, void* user);
/* Block until every queued read has completed. */
REDFS_API void redfs_drain(void);

/*
 * Quiesce RedFS: cancel queued reads, stop and JOIN the worker thread, flush and
 * close the mesh cache.
 *
 * Call this before your DLL can be unloaded -- from a RED4ext plugin's
 * Main(EMainReason::Unload), or your CET onShutdown. Unloading with the worker
 * alive unmaps the code that thread is running, which crashes the game.
 *
 * NEVER call it from DllMain: joining a thread there runs under the loader lock
 * and deadlocks. DllMain is exactly what this call exists to avoid.
 *
 * The wait is bounded by a single segment decode, not by how much was queued or
 * how large the target was: unstarted reads are dropped and the running one
 * gives up at its next segment boundary. There is deliberately no timeout --
 * abandoning the thread would reintroduce the unmapped-code crash this prevents.
 *
 * Dropped reads get their callback with REDFS_E_CANCELLED, so nothing is left
 * waiting on a callback that never arrives. Call redfs_drain() first if you
 * actually want queued work to finish.
 *
 * Idempotent, and it quiesces rather than disables: a later redfs_read_async
 * starts a fresh worker, so one plugin unloading does not permanently break
 * async for others sharing RedFS.dll. Synchronous reads are unaffected
 * throughout.
 */
REDFS_API void redfs_shutdown(void);

/* ------------------------------------------------------------------------- */
/* CR2W introspection                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Reads the CR2W container without any knowledge of the RED4 type system: the
 * chunk graph, the import (dependency) list, and properties addressed by name.
 * Property values carry their RED type name, so a caller can decide what to do
 * with anything this header does not model.
 */
typedef struct redfs_cr2w redfs_cr2w;

/* Borrows `data`; it must outlive the handle. Every redfs_value the calls below
 * hand back points into `data` or into the handle, so those pointers die with
 * the handle too -- copy anything you keep past redfs_cr2w_close. */
REDFS_API redfs_status redfs_cr2w_open(const void* data, uint64_t size, redfs_cr2w** out_cr2w);
REDFS_API void         redfs_cr2w_close(redfs_cr2w* cr2w);

REDFS_API const char* redfs_cr2w_root_type(const redfs_cr2w* cr2w);
REDFS_API uint32_t    redfs_cr2w_chunk_count(const redfs_cr2w* cr2w);
REDFS_API const char* redfs_cr2w_chunk_type(const redfs_cr2w* cr2w, uint32_t chunk);
/* First chunk of the given RED class, or -1. */
REDFS_API int32_t     redfs_cr2w_find_chunk(const redfs_cr2w* cr2w, const char* type_name);

/* Every resource this file references -- the dependency list, as depot paths. */
REDFS_API uint32_t    redfs_cr2w_import_count(const redfs_cr2w* cr2w);
REDFS_API const char* redfs_cr2w_import_path(const redfs_cr2w* cr2w, uint32_t index);
REDFS_API const char* redfs_cr2w_import_type(const redfs_cr2w* cr2w, uint32_t index);

typedef enum redfs_kind {
    REDFS_KIND_RAW = 0,  /* not decoded; use .data/.size                      */
    REDFS_KIND_BOOL,     /* as.u                                              */
    REDFS_KIND_INT,      /* as.i                                              */
    REDFS_KIND_UINT,     /* as.u                                              */
    REDFS_KIND_FLOAT,    /* as.f                                              */
    REDFS_KIND_NAME,     /* as.s -- CName, or an enum's symbolic value        */
    REDFS_KIND_STRING,   /* as.s -- CString                                   */
    REDFS_KIND_STRUCT,   /* nested; address through it with a dotted path     */
    REDFS_KIND_HANDLE,   /* as.chunk -- index of the pointed-to chunk, or -1  */
    REDFS_KIND_BUFFER,   /* as.buffer -- attached buffer index for redfs_read */
    REDFS_KIND_ARRAY     /* as.u -- element count; elements are in .data + 4  */
} redfs_kind;

typedef struct redfs_value {
    const char*    type;  /* RED type name, e.g. "Uint16", "ETextureCompression" */
    const uint8_t* data;  /* serialized bytes, inside the CR2W blob              */
    uint32_t       size;
    redfs_kind     kind;
    union {
        int64_t     i;
        uint64_t    u;
        double      f;
        const char* s;
        int32_t     chunk;
        uint32_t    buffer;
    } as;
} redfs_value;

/* prop_path is dot separated and descends into nested structs,
 * e.g. "setup.rawFormat" or "header.sizeInfo.width". */
REDFS_API redfs_status redfs_cr2w_get(const redfs_cr2w* cr2w, uint32_t chunk,
                                      const char* prop_path, redfs_value* out_value);

/* Walk one chunk's (or nested struct's) properties. prop_path may be NULL for the chunk root. */
typedef int (*redfs_prop_fn)(const char* name, const redfs_value* value, void* user);
REDFS_API redfs_status redfs_cr2w_walk(const redfs_cr2w* cr2w, uint32_t chunk,
                                       const char* prop_path, redfs_prop_fn fn, void* user);

/*
 * Walk the elements of a REDFS_KIND_ARRAY value. Elements arrive decoded the
 * same way properties do, so an array of structs yields REDFS_KIND_STRUCT
 * values you can address further with redfs_cr2w_get_in.
 *
 * Prefer this to looping on as.u: that count is only what the file declares,
 * and the walk stops early when an element does not decode.
 */
typedef int (*redfs_elem_fn)(uint32_t index, const redfs_value* value, void* user);
REDFS_API redfs_status redfs_cr2w_walk_array(const redfs_cr2w* cr2w, const redfs_value* array,
                                             redfs_elem_fn fn, void* user);

/* Like redfs_cr2w_get / redfs_cr2w_walk, but rooted at an already-resolved
 * struct value rather than at a chunk -- for descending into an array element.
 * prop_path may be NULL to mean the struct itself. */
REDFS_API redfs_status redfs_cr2w_get_in(const redfs_cr2w* cr2w, const redfs_value* parent,
                                         const char* prop_path, redfs_value* out_value);
REDFS_API redfs_status redfs_cr2w_walk_in(const redfs_cr2w* cr2w, const redfs_value* parent,
                                          const char* prop_path, redfs_prop_fn fn, void* user);

/* ------------------------------------------------------------------------- */
/* textures                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct redfs_texture_desc {
    uint32_t width, height, depth;
    uint32_t mip_count;
    uint32_t slice_count;
    uint32_t dxgi_format;   /* DXGI_FORMAT value                            */
    uint32_t is_cubemap;
    uint32_t is_3d;
    uint32_t buffer_index;  /* attached buffer holding the pixel data       */
    uint64_t data_size;     /* decompressed size of that buffer             */
} redfs_texture_desc;

/* Works for CBitmapTexture, CTextureArray, CCubeTexture. */
REDFS_API redfs_status redfs_texture_desc_of(const redfs_depot* depot, uint64_t hash,
                                             redfs_texture_desc* out_desc);

/*
 * Read an .xbm and hand back a complete in-memory DDS (DDS_HEADER + DXT10 +
 * pixels), ready for DirectX::CreateDDSTextureFromMemory or
 * DirectX::LoadFromDDSMemory. No temp files, no D3D device needed.
 */
REDFS_API redfs_status redfs_texture_read_dds(const redfs_depot* depot, uint64_t hash,
                                              redfs_blob* out_blob);

/* Just the raw pixel payload, plus the descriptor -- for callers building their
 * own D3D11_SUBRESOURCE_DATA. */
REDFS_API redfs_status redfs_texture_read_raw(const redfs_depot* depot, uint64_t hash,
                                              redfs_texture_desc* out_desc, redfs_blob* out_blob);

/* ------------------------------------------------------------------------- */
/* audio                                                                      */
/* ------------------------------------------------------------------------- */

typedef enum redfs_audio_format {
    REDFS_AUDIO_UNKNOWN = 0,
    REDFS_AUDIO_WEM,      /* Wwise RIFF; feed to vgmstream/ww2ogg or a Wwise SFX source */
    REDFS_AUDIO_BNK,      /* Wwise SoundBank                                            */
    REDFS_AUDIO_OPUSPAK,  /* voice-over pack; needs the matching .opusinfo to index      */
    REDFS_AUDIO_OPUSINFO
} redfs_audio_format;

/* Identifies the container from the first bytes of the file.
 *
 * NOT cheap: it decodes the whole main segment to look at 16 bytes, because
 * Kraken cannot decode a prefix. For music that is tens of MB. Call it off the
 * game thread. */
REDFS_API redfs_status redfs_audio_probe(const redfs_depot* depot, uint64_t hash,
                                         redfs_audio_format* out_format);

typedef enum redfs_audio_codec {
    REDFS_CODEC_UNKNOWN = 0,
    REDFS_CODEC_PCM,
    REDFS_CODEC_ADPCM,
    REDFS_CODEC_VORBIS,  /* Wwise Vorbis -- a rebuilt header is needed to decode */
    REDFS_CODEC_XMA2,
    REDFS_CODEC_OPUS
} redfs_audio_codec;

typedef struct redfs_audio_info {
    redfs_audio_format container;
    redfs_audio_codec  codec;
    uint32_t format_tag;       /* raw wFormatTag, authoritative if codec is UNKNOWN */
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t bits_per_sample;
    uint32_t avg_bytes_per_sec;
    uint64_t data_offset;      /* payload start, as a byte offset into the file  */
    uint64_t data_size;
    uint64_t total_samples;    /* 0 when the container does not state it         */
    double   duration_seconds; /* 0 when not derivable                           */
} redfs_audio_info;

/*
 * Parse a .wem (Wwise RIFF) header: codec, channel layout, sample rate, and
 * where the payload actually starts.
 *
 * RedFS does NOT decode audio. It tells you what you have and where it is, so
 * you can hand the payload to whatever decoder you already use. For PCM and
 * ADPCM that is enough to play directly; Wwise Vorbis needs its codebooks
 * rebuilt (ww2ogg/vgmstream) first.
 *
 * Reads only the main segment.
 */
REDFS_API redfs_status redfs_audio_info_of(const redfs_depot* depot, uint64_t hash,
                                           redfs_audio_info* out_info);

/* Parse a .wem already in memory -- for payloads you extracted some other way. */
REDFS_API redfs_status redfs_audio_info_parse(const void* data, uint64_t size,
                                              redfs_audio_info* out_info);

/* Enumerate the RIFF chunks of a .wem. Wwise carries codec state in non-standard
 * chunks ('vorb', 'seek'), and a decoder front-end usually needs to find them.
 * `fourcc` is exactly 4 bytes and is NOT NUL-terminated. */
typedef int (*redfs_riff_chunk_fn)(const char fourcc[4], uint64_t offset, uint64_t size,
                                   void* user);
REDFS_API redfs_status redfs_audio_walk_chunks(const void* data, uint64_t size,
                                               redfs_riff_chunk_fn fn, void* user);

REDFS_API const char* redfs_audio_codec_name(redfs_audio_codec codec);

/* ------------------------------------------------------------------------- */
/* meshes                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct redfs_mesh_desc {
    uint32_t render_buffer_index;  /* attached buffer with the vertex/index streams */
    uint64_t render_buffer_size;
    uint32_t vertex_buffer_size;   /* bytes, at offset 0 of the render buffer       */
    uint32_t index_buffer_size;
    uint32_t index_buffer_offset;  /* bytes into the render buffer                  */
    uint32_t submesh_count;        /* rendChunks                                    */
    uint32_t appearance_count;
    uint32_t material_count;
    float    bbox_min[3];
    float    bbox_max[3];
} redfs_mesh_desc;

/* Header-level facts about a .mesh plus where its geometry lives inside the
 * render buffer. Cheap: reads the CR2W only, never the geometry. */
REDFS_API redfs_status redfs_mesh_desc_of(const redfs_depot* depot, uint64_t hash,
                                          redfs_mesh_desc* out_desc);

/* --- chunks -----------------------------------------------------------------
 *
 * A chunk is one submesh. The renderer draws chunk i of a mesh component when
 * bit i of that component's chunkMask is set, so a chunk index *is* a chunkMask
 * bit -- which is what makes these queryable against a live entity.
 *
 * Chunks repeat per LOD: two entries can be the same geometry at different
 * detail. Filter on `lod` to get one copy.
 *
 * The bounding box is NOT stored in the format, so RedFS computes it by
 * dequantizing that chunk's vertex positions -- which means redfs_mesh_open has
 * to decompress the geometry buffer. See the cache below: pay that once per
 * mesh, ever.
 *
 * Boxes are in mesh-local GAME space (Z up), matching entity/component
 * transforms -- not the Y-up convention glTF exporters use.
 */

typedef struct redfs_mesh redfs_mesh;

typedef struct redfs_mesh_chunk {
    uint32_t index;         /* the bit in a component's chunkMask           */
    uint32_t lod_mask;      /* raw lodMask bitfield                         */
    uint32_t lod;           /* lowest LOD level it belongs to, 1-based      */
    uint32_t vertex_count;
    uint32_t index_count;
    float    bbox_min[3];
    float    bbox_max[3];
    /* 0 when no box could be computed -- the geometry buffer was absent,
     * streamed out, or failed its span check. The boxes are then all-zero, which
     * is otherwise indistinguishable from a real chunk sitting at the origin, so
     * test this before treating a box as a fact. Rare but real: about 1 stock
     * mesh in 10,000. */
    uint32_t bounds_valid;
} redfs_mesh_chunk;

REDFS_API redfs_status redfs_mesh_open(const redfs_depot* depot, uint64_t hash,
                                       redfs_mesh** out_mesh);
REDFS_API void         redfs_mesh_close(redfs_mesh* mesh);

REDFS_API uint32_t                redfs_mesh_chunk_count(const redfs_mesh* mesh);
/* The returned pointer is owned by `mesh` and stays valid until you call
 * redfs_mesh_close on it. Do not hold it past that. */
REDFS_API const redfs_mesh_chunk* redfs_mesh_chunk_at(const redfs_mesh* mesh, uint32_t index);
REDFS_API uint32_t                redfs_mesh_lod_count(const redfs_mesh* mesh);
/* Whole-mesh box. Unlike the per-chunk boxes above, this one IS stored in the
 * file (CMesh.boundingBox) and is returned as found -- RedFS does not verify it
 * beyond replacing a non-finite value with 0. It cannot answer "which chunks are
 * the chest"; use the chunk boxes for that. */
REDFS_API void redfs_mesh_bounds(const redfs_mesh* mesh, float out_min[3], float out_max[3]);

/* Appearances are the named material sets a component selects by CName. */
REDFS_API uint32_t    redfs_mesh_appearance_count(const redfs_mesh* mesh);
REDFS_API const char* redfs_mesh_appearance_name(const redfs_mesh* mesh, uint32_t appearance);
REDFS_API int32_t     redfs_mesh_find_appearance(const redfs_mesh* mesh, const char* name);

/* Material used by `chunk` under `appearance` -- "" when the appearance does not
 * name one for that chunk. */
REDFS_API const char* redfs_mesh_chunk_material(const redfs_mesh* mesh, uint32_t appearance,
                                                uint32_t chunk);

/* --- mesh cache -------------------------------------------------------------
 *
 * Computing chunk bounds costs a geometry decompress. Measured over 200 meshes
 * sampled across a stock install: median 0.7 ms, mean 1.0 ms, p90 2.3 ms, worst
 * 7.1 ms. Point RedFS at a file and every redfs_mesh_open result is remembered:
 * the first call for a mesh computes, every later call -- including after a
 * restart -- is a lookup.
 *
 * The cache fingerprints the mounted archive set and silently discards itself
 * when that moves, so a game patch, a new mod, or an archive REPLACED IN PLACE
 * (a re-cook that keeps the same file and segment counts) cannot serve stale
 * geometry. Mounting after redfs_cache_open re-checks, so mount order does not
 * have to be perfect.
 *
 * There is ONE cache per process, and it belongs to the depot you pass here.
 * Entries are keyed by hash alone, and the same hash means different bytes in a
 * different depot -- so redfs_mesh_open on any other depot bypasses the cache
 * entirely rather than risking a cross-depot answer. If you keep two depots,
 * only one of them benefits.
 */
REDFS_API redfs_status redfs_cache_open(const redfs_depot* depot, const char* cache_file);
/* Write pending entries to disk. Also happens on redfs_cache_close. */
REDFS_API redfs_status redfs_cache_flush(void);
REDFS_API void         redfs_cache_close(void);
REDFS_API uint32_t     redfs_cache_entry_count(void);

/* Precompute a list of meshes up front -- the "warm it at load" pattern.
 * Skips anything already cached. Returns how many were newly computed.
 *
 * Requires a cache opened on this depot; without one every result would be
 * computed and immediately discarded, so it returns REDFS_E_INVALID_ARG rather
 * than burning the time silently. */
REDFS_API redfs_status redfs_cache_warm(const redfs_depot* depot, const uint64_t* hashes,
                                        uint32_t count, uint32_t* out_computed);

/* ------------------------------------------------------------------------- */
/* diagnostics                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Diagnostics sink. Three things your callback has to cope with:
 *
 *   - It may be invoked from RedFS's WORKER thread, not only from yours. Any
 *     failure inside redfs_read_async reports from there.
 *   - It may be invoked CONCURRENTLY. Synchronize your own logger; RedFS does
 *     not serialize calls into it.
 *   - `message` is valid only for the duration of the call. Copy it if you keep
 *     it.
 *
 * It is never invoked with a RedFS lock held, so calling back into RedFS from
 * the sink is safe.
 */
typedef void (*redfs_log_fn)(const char* message, void* user);
REDFS_API void redfs_set_log(redfs_log_fn fn, void* user);

/*
 * Whether Oodle resolved. Compressed segments -- which is nearly everything --
 * fail with REDFS_E_OODLE when this is 0, so check it once after opening a depot
 * and warn loudly rather than letting every read fail.
 *
 * Inside the game it is effectively always true: the DLL is already resident.
 * Standalone tools, and installs missing bin/x64/oo2ext_7_win64.dll, see 0.
 */
REDFS_API int redfs_oodle_available(void);

/* Human-readable reason for the last failure on this thread. */
REDFS_API const char* redfs_last_error(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* REDFS_H */
