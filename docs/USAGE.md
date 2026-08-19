# Using RedFS

From a fresh clone to reading game assets at runtime. `API.md` maps the call
surface; `INTEGRATION.md` covers lifetime inside the game process; `done/`
explains how any of it works internally.

The running example throughout is the query the library exists for: given only a
depot path to a body mesh, work out which of its submeshes are the chest, and
turn that into a `chunkMask` you can hand to a live component.

---

## 1. Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

MSVC 2022 (C++20), CMake ≥ 3.21, Windows x64. Nothing to fetch. Out of `build/`:

| artifact | what it is |
|---|---|
| `redfs_static.lib` | link into your own DLL — **usually what you want** |
| `RedFS.dll` + `redfs_shared.lib` | one shared copy for several mods |
| `redfs_cli.exe` | exploration and diagnostics — §12 |
| `redfs_verify.exe` | format correctness against external oracles |
| `example_chest.exe` | the worked example this guide follows |
| `redfs_test`, `redfs_fuzz`, `redfs_lifecycle` | the test suite; `run-checks.ps1` drives all of it |

From a Bash shell you need the MSVC environment first:

```bash
cmd //c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build'
```

## 2. Link

**CMake** — the target carries its own include path and defines `REDFS_STATIC`
for you:

```cmake
add_subdirectory(third_party/RedFS)
target_link_libraries(my_mod PRIVATE redfs_static)
```

**By hand** — add `include/` to your include path, link `redfs_static.lib`, and
define `REDFS_STATIC` before including the header. Against `RedFS.dll` define
nothing; the header defaults to `dllimport`.

```c
#include "redfs.h"     /* the C ABI -- everything is here        */
#include "redfs.hpp"   /* optional header-only C++ facade; §11   */
```

If you link against the DLL, check the ABI once at startup:

```c
if (redfs_abi_version() != REDFS_ABI_VERSION) { /* refuse to run */ }
```

A mismatch is otherwise silent: `redfs_mesh_chunk` has only ever grown by
appending — ABI 2 added `bounds_valid` — so field reads still land correctly and
only the *stride* is wrong. Code compiled against ABI 1 walks a 48-byte array in
44-byte steps and returns plausible garbage from the second element on.
`redfs::Depot::open` checks this for you; `redfs::abi_ok()` exposes it.

## 3. Open a depot

Once, at startup. Keep it for the process lifetime — reopening re-reads every
index. A depot is immutable once open, which is what makes concurrent reads safe
without any locking of yours.

```c
redfs_depot* depot = NULL;
redfs_status st = redfs_depot_open(NULL, REDFS_SCAN_ALL, &depot);
if (st != REDFS_OK) {
    log("RedFS: %s (%s)", redfs_status_string(st), redfs_last_error());
    return;
}
if (!redfs_oodle_available())
    log("RedFS: no oo2ext_7_win64.dll -- compressed reads will fail");
```

`NULL` auto-detects the install by walking up from the running executable looking
for `archive\pc` — right when you are inside the game, and it makes MO2 and
Vortex work for free. An external tool passes the install root instead:

```c
redfs_depot_open("D:\\SteamLibrary\\steamapps\\common\\Cyberpunk 2077",
                 REDFS_SCAN_ALL, &depot);
```

`REDFS_SCAN_ALL` mounts all four sets in the game's own order — `content`, `ep1`,
REDmod (`mods/<name>/archives`), then legacy mods (`archive/pc/mod`) — with later
mounts overriding earlier ones. Narrow it if you want vanilla data only:
`REDFS_SCAN_CONTENT | REDFS_SCAN_EP1`.

Only indices are read here, never file contents. On the reference install (57
archives, 85 GB, 544,670 distinct paths) that costs ~30 ms, 8.7 MB of heap and
~89 MB of file-backed index mapping the OS is free to evict. Tear down with
`redfs_depot_close(depot)`.

Oodle is resolved here too, from the game's own `bin\x64`. Missing it is not a
mount failure — it surfaces as `REDFS_E_OODLE` on the first compressed read,
which is nearly every read, hence the one-time check above.

### Archives outside the install

`REDFS_SCAN_ALL` already finds REDmod and legacy mods, so most callers need
nothing here. The exception is a tool run **outside** MO2 or Vortex: their virtual
filesystem only exists inside processes they launch, so from outside those mod
archives are invisible. Mount the staging folder yourself:

```c
uint32_t mounted = 0;
redfs_depot_mount_dir(depot, "D:\\MO2\\mods\\SomeMod\\archive\\pc\\mod", &mounted);
```

Or one archive: `redfs_depot_mount(depot, "...\\thing.archive")`. Both go on top
at highest priority, neither is thread-safe, and both invalidate the mesh cache
(§9) — so mount everything before sharing the depot or opening the cache.
`INTEGRATION.md` has the full story on load order and override resolution.

## 4. Address a file

Every file is addressed by a 64-bit hash of its depot path. `redfs_hash` applies
the engine's normalisation first — trim, collapse separator runs, `/` → `\`,
ASCII lowercase — so `Base/Icon/Foo.XBM` and `base\icon\foo.xbm` are the same
key:

```c
static const char kBody[] =
    "base/characters/common/player_base_bodies/player_female_average"
    "/t0_000_pwa_base__full.mesh";

uint64_t key = redfs_hash(kBody);
if (!redfs_exists(depot, key)) return;   /* not in this install */
```

Forward slashes are worth preferring: in a C literal a backslash has to be
doubled, and `"base\\path"` is easy to get wrong.

Archives store only hashes — there is no path table to enumerate — so a mod has
to know the paths it wants, from WolvenKit, from a shipped list (§8), or from a
live component's resource hash. Guessing does not work; §12 shows how to search
for a path you half-remember.

## 5. Read bytes

A file is stored as segments. Segment 0 is the resource itself, a CR2W document
for anything cooked; the segments after it are its attached buffers, holding the
bulk payload — pixels for a texture, vertex streams for a mesh.

```c
REDFS_PART_MAIN   /* segment 0 only: the CR2W document */
REDFS_PART_ALL    /* every segment, concatenated       */
0, 1, 2, ...      /* one attached buffer               */
```

```c
redfs_blob blob;
if (redfs_read(depot, key, REDFS_PART_MAIN, &blob) == REDFS_OK) {
    /* blob.data, blob.size */
    redfs_blob_free(&blob);
}
```

`REDFS_PART_MAIN` of a 40 MB texture reads kilobytes, not megabytes: the pixels
live in a buffer it never touches. Prefer it for anything you only inspect.

Around that: `redfs_part_size` gives a decompressed size without reading;
`redfs_read_into` fills memory you already own and returns `REDFS_E_RANGE` if it
is too small, writing the required size to `out_written` anyway; `redfs_stat`
reports `buffer_count`, sizes, SHA-1, and which archive won the override.

## 6. The typed helpers

Three resource families have first-class support. Everything else goes through
§7.

### Textures, ready for D3D

```c
redfs_blob dds;
if (redfs_texture_read_dds(depot, key, &dds) == REDFS_OK) {
    DirectX::CreateDDSTextureFromMemory(device, dds.data, (size_t)dds.size,
                                        &resource, &srv);
    redfs_blob_free(&dds);          /* safe: D3D has copied it */
}
```

That blob is a complete DDS — header, DX10 extension, mip chain — built in memory,
so no temp file and no D3D device are needed. `redfs_texture_read_raw` hands back
the raw payload plus the descriptor instead, for callers filling their own
`D3D11_SUBRESOURCE_DATA`.

Metadata alone, without touching pixels:

```c
redfs_texture_desc t;
if (redfs_texture_desc_of(depot, key, &t) == REDFS_OK) {
    /* t.width, t.height, t.mip_count, t.slice_count,
       t.dxgi_format (a DXGI_FORMAT value), t.buffer_index, t.data_size */
}
```

Handles `CBitmapTexture`, `CTextureArray` and `CCubeTexture`; a console cook is
rejected rather than guessed at.

### Mesh chunks, and the chest query

A chunk is one submesh, and **a chunk index is a bit in a component's
`chunkMask`** — which is what makes any of this answerable against a live entity.
The per-chunk bounding box is not stored in the format, so RedFS computes it by
dequantizing that chunk's vertex positions, which means `redfs_mesh_open`
decompresses the geometry buffer. See §9 before you call this in a loop.

```c
redfs_mesh* m = NULL;
uint64_t mask = 0;
if (redfs_mesh_open(depot, key, &m) == REDFS_OK) {
    float lo[3], hi[3];
    redfs_mesh_bounds(m, lo, hi);                       /* whole-mesh box */
    const float chest_floor = lo[2] + (hi[2] - lo[2]) * 0.66f;

    for (uint32_t i = 0; i < redfs_mesh_chunk_count(m); ++i) {
        const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
        if (c->lod != 1) continue;          /* one copy, not every LOD    */
        if (!c->bounds_valid) continue;     /* no box computed; see below */
        if (c->bbox_min[2] > chest_floor)
            mask |= 1ull << c->index;       /* index IS the chunkMask bit */
    }
    redfs_mesh_close(m);                    /* always, cached or not      */
}
```

Three things that sample is careful about, each for a reason:

- **`bounds_valid == 0`** means no box could be computed — geometry absent,
  streamed out, or failing its span check. The boxes are then all-zero, which is
  indistinguishable from a real chunk sitting at the origin, so a comparison
  against them silently answers about a chunk you know nothing about. Rare but
  real: about 1 stock mesh in 10,000.
- **`lod`** is the lowest detail level a chunk belongs to, 1-based (`lod_mask` is
  the raw bitfield). Chunks repeat per LOD, so without the filter you count
  geometry twice.
- **Boxes are mesh-local game space, Z up**, matching entity and component
  transforms — not the Y-up flip glTF exporters apply. So they compare directly
  against a component transform.

`redfs_mesh_bounds` is the one box that *is* stored (`CMesh.boundingBox`), and it
is returned as found apart from replacing a non-finite value with 0. Good enough
as the reference frame above; it cannot tell you which chunks are the chest.

Materials are per *appearance*, since an appearance is what a component selects
by CName:

```c
int32_t app = redfs_mesh_find_appearance(m, "01_ca_pale");   /* -1 if absent */
if (app >= 0)
    const char* mat = redfs_mesh_chunk_material(m, (uint32_t)app, c->index);
```

If all you want is the header — buffer layout, submesh count, whole-mesh box —
`redfs_mesh_desc_of` reads the CR2W only and never touches the geometry.

### Audio: exact bytes, honest description

Extraction needs no helper. The archive holds a real `.wem`, so reading it out is
just a read:

```c
redfs_blob wem;
if (redfs_read(depot, key, REDFS_PART_ALL, &wem) == REDFS_OK) {
    fwrite(wem.data, 1, (size_t)wem.size, out);   /* byte-identical .wem */
    redfs_blob_free(&wem);
}
```

To know what you have before handing it to a converter:

```c
redfs_audio_info info;
if (redfs_audio_info_of(depot, key, &info) == REDFS_OK) {
    /* info.codec -- REDFS_CODEC_VORBIS, _PCM, _ADPCM, ... ; name it with
                     redfs_audio_codec_name()
       info.channels, info.sample_rate, info.bits_per_sample
       info.data_offset / info.data_size -- where the payload actually starts
       info.duration_seconds -- 0 when not derivable */
}
```

`redfs_audio_probe` identifies the container (`WEM`, `BNK`, `OPUSPAK`,
`OPUSINFO`) from the first bytes of the file — but it is **not cheap**. Kraken
cannot decode a prefix, so it decompresses the whole main segment to look at 16
bytes, which for music is tens of megabytes. Call it off the game thread, or skip
it when the extension already told you. `redfs_audio_info_of` reads that same
main segment.

For a decoder front-end, `redfs_audio_walk_chunks` enumerates the RIFF chunks of
bytes you already hold — Wwise keeps codec state in non-standard ones (`vorb`,
`seek`) that a front-end has to locate:

```c
redfs_audio_walk_chunks(wem.data, wem.size, on_riff_chunk, user);
```

`redfs_audio_info_parse` does the same for a payload you obtained some other way.

**RedFS does not decode audio, and will not** — that would mean bundling Vorbis
and Opus, and Wwise Vorbis additionally needs its stripped codebooks rebuilt.
Hand the bytes to a tool built for it: `ww2ogg` or `vgmstream` for wem → ogg,
`ffmpeg` or `lame` for ogg → mp3, or `ffmpeg -i in.wem out.mp3` directly if your
build has the codec. What RedFS supplies is the exact bytes and an accurate
description of them, which is the part that needed the archive.

## 7. Everything else: open the resource

There is no format-specific helper for most resource types, and mostly there does
not need to be. Any cooked resource is a CR2W document, and the generic reader
handles the container with no knowledge of the RED4 type system — every value
carries its own RED type name, so you can act on types this header does not
model.

```c
redfs_resource* r = NULL;
if (redfs_open(depot, key, &r) != REDFS_OK) return;

printf("%s, %u buffers\n", redfs_resource_type(r), redfs_resource_buffer_count(r));

const redfs_cr2w* f = redfs_resource_cr2w(r);
for (uint32_t i = 0; i < redfs_cr2w_import_count(f); ++i)   /* dependencies */
    printf("  %-24s %s\n", redfs_cr2w_import_type(f, i),
                           redfs_cr2w_import_path(f, i));

redfs_close(r);   /* one close; the handle owns the bytes and the document */
```

**`redfs_open` works on any file, not only CR2W documents.** For a `.wem` or a
`.bnk`, `redfs_resource_type` returns `""` and `redfs_resource_cr2w` returns
`NULL`, while `redfs_resource_data`/`_size` still give you the bytes. So "what is
this?" is one call rather than a guess about the extension:

```
base\...\ro_road_a.mesh        CMesh                         doc  6,685 B, 1 buffer
base\...\hanako_no_coat_r.xbm  CBitmapTexture                doc  2,116 B, 1 buffer
base\...\q005_arasaka.ent      entEntityTemplate             doc 14,688 B, 1 buffer
base\...\int_food_001.app      appearanceAppearanceResource  doc  1,158 B, 2 buffers
base\...\elizabeth_peralez.wem                               doc 58,693 B, 0 buffers
```

### Why not `redfs_read` plus `redfs_cr2w_open`

That still works and the pieces are still public, but it has two traps the handle
removes.

**The free order is load-bearing.** Every `redfs_value` points into the blob or
into the handle, so the blob has to outlive the document — `redfs_cr2w_close`
first, then `redfs_blob_free`. Getting it backwards reads freed memory.

**And `part` has a wrong value that does not look wrong.** It is one `uint32_t`
carrying three meanings, and `0` — the value most people reach for to mean "the
first part" — means *attached buffer 0*. On the mesh above:

```c
redfs_read(depot, key, 0, &blob);   /* -> REDFS_OK, 6200 bytes of vertex data */
redfs_cr2w_open(blob.data, blob.size, &f);   /* -> REDFS_E_CORRUPT */
```

Neither error names the cause. On a file with **no** buffers the same call
returns `REDFS_E_RANGE` instead, which reads as "the file isn't there". One
mistake, two unrelated symptoms, and a sweep across a whole install produced
288,302 of the first and 209,228 of the second.

`redfs_resource_buffer(r, i)` has no such value: `i` is bounds-checked against
the count on the handle, and the main segment is not reachable through it at all.
The index numbering matches `redfs_value.as.buffer`,
`redfs_texture_desc.buffer_index` and `redfs_mesh_desc.render_buffer_index`, so
an index read out of a document goes straight in.

Properties are addressed by a dotted path that descends through nested structs —
`"header.sizeInfo.width"`, `"setup.rawFormat"` — and every value arrives tagged
with its kind and its RED type name:

```c
redfs_value v;
if (redfs_cr2w_get(f, chunk, "someStruct.someField", &v) == REDFS_OK) {
    switch (v.kind) {
        case REDFS_KIND_UINT:   use(v.as.u); break;
        case REDFS_KIND_FLOAT:  use(v.as.f); break;
        case REDFS_KIND_NAME:   use(v.as.s); break;    /* CName, or an enum   */
        case REDFS_KIND_STRING: use(v.as.s); break;    /* CString, or an rRef */
        case REDFS_KIND_HANDLE: use(v.as.chunk); break;   /* chunk index, -1 if null */
        case REDFS_KIND_BUFFER: redfs_read(depot, key, v.as.buffer, &buf); break;
        case REDFS_KIND_ARRAY:  redfs_cr2w_walk_array(f, &v, visit, user); break;
        default: break;                                /* v.type, v.data, v.size */
    }
}
```

Two kinds deserve attention. `REDFS_KIND_BUFFER` carries an attached-buffer index
you pass straight to `redfs_read` — that is how bulk payloads are reached.
`REDFS_KIND_ARRAY` puts the *declared* element count in `as.u`; prefer
`redfs_cr2w_walk_array`, which stops early when an element does not decode.

`redfs_cr2w_walk` enumerates a chunk or a nested struct; `redfs_cr2w_get_in` and
`redfs_cr2w_walk_in` do the same rooted at a struct value you already resolved,
which is how you descend into an array element.

### Worked example: a facial rig

A `.facialsetup` has no helper and needs none. Its root chunk is
`animFacialSetup`; `rig` is a resource reference, which decodes to the referenced
depot path as a string; `bakedData`, `mainPosesData` and `correctivePosesData`
are `DataBuffer` properties, so each yields a buffer index:

```c
redfs_value rig, baked;
if (redfs_cr2w_get(f, 0, "rig", &rig) == REDFS_OK        /* rRef -> KIND_STRING */
    && rig.kind == REDFS_KIND_STRING)
    printf("drives %s\n", rig.as.s);

if (redfs_cr2w_get(f, 0, "bakedData", &baked) == REDFS_OK /* -> KIND_BUFFER */
    && baked.kind == REDFS_KIND_BUFFER)
    redfs_read(depot, key, baked.as.buffer, &blob);
```

`faceCorrectiveNames` is an array of CNames, reachable with
`redfs_cr2w_walk_array`. So the whole rig is reachable without RedFS knowing
anything about facial animation. Explore any resource this way with
`redfs_cli cr2w` and `redfs_cli arr` before writing code against it.

## 8. Hash → path

The reverse direction is not computable — FNV1a is one-way and archives keep no
path table — so it is a dictionary lookup. It matters when a live component hands
you a resource hash and you want to know what it is.

```c
uint32_t kept = 0;   /* receives how many of the list's paths resolve here */
redfs_path_load(depot, "usedhashes.kark", &kept);   /* once, at startup */

const char* path = redfs_path_from_hash(key);       /* NULL if unknown  */
```

The dictionary is filled from three sources:

1. **CR2W import tables**, learned automatically as you read files. Free, and the
   only source that knows paths a mod invented. It is off until the dictionary is
   switched on by `redfs_path_load` or `redfs_path_enable`.
2. **A path list**: `WolvenKit.Common/Resources/usedhashes.kark`, or any plain
   text file with one path per line. Ship a copy alongside your mod.
3. **`redfs_path_add`**, for anything you know yourself.

**Only source 2 is filtered against the mounted depot.** The other two cannot be
— import learning happens inside CR2W parsing, which has no depot, and
`redfs_path_add` receives only a string. So a hit tells you what a file is
*called*, not that it is readable: check `redfs_exists`, or just handle
`REDFS_E_NOT_FOUND` from the read.

The pointer it returns is valid for the lifetime of the process — later additions
from any source cannot invalidate one you already hold — so it is safe to keep.

Cost, on the reference install: `usedhashes.kark` is 3.4 MB on disk and
decompresses to ~135 MB, held only for the duration of the call. Roughly a
quarter of its lines resolve in a stock depot, and what stays resident is those
~40 MB of interned strings — 544,496 paths, which is 99.97 % of the depot's
544,670 files. Load it only if you call `redfs_path_from_hash` or `redfs_find`.

### Finding files

The dictionary is also what makes "which of these are meshes?" answerable.
`redfs_find` globs over it and **reads nothing** — no segment is decoded, so the
cost is one pattern match per known path:

```c
static int on_mesh(uint64_t key, const char* path, void* user) {
    redfs_depot* depot = (redfs_depot*)user;   /* that is what `user` is for */
    redfs_mesh* mesh = NULL;
    if (redfs_mesh_open(depot, key, &mesh) == REDFS_OK) {
        ...
        redfs_mesh_close(mesh);
    }
    return 1;   /* 0 stops delivery -- see below */
}

uint32_t total = 0;
redfs_find(depot, "base\\characters\\*.mesh", on_mesh, depot, &total);
```

`*` matches any run of characters **including separators**, and `?` matches
exactly one character, **which may also be a separator**. Spanning separators is
deliberately unlike a shell glob: the common query is "every mesh anywhere",
which under shell rules would match nothing. Narrow with a prefix instead. A
pattern ending in a separator means everything beneath it, so
`base\characters\` is shorthand for `base\characters\*`.

The pattern is normalised exactly like a path, so `Base/Characters/*.MESH` and
`base\characters\*.mesh` are the same query, and a path pasted out of WolvenKit
works with a component replaced by `*`.

Passing the depot restricts hits to files **the depot index holds** — worth it
because sources 1 and 3 above are unfiltered. That is presence, not readability:
a read can still fail `REDFS_E_OODLE` or `REDFS_E_CORRUPT`. Pass `NULL` to search
the dictionary as-is.

**It searches the dictionary, not the depot.** The archives carry no path table,
so `redfs_find` can only see what the three sources above have taught it. With
nothing in it at all you get `REDFS_E_NO_DICTIONARY`, not an empty success —
"there was nothing to search" and "nothing matched" are different answers, and
conflating them is how you end up inspecting a glob that was never the problem.
That status is distinct from `REDFS_E_NOT_FOUND` for the same reason: that one
means "no such file", which reads here as exactly the wrong thing. Note also
that the dictionary is **process-global** while the depot filter is not, so
loading a list against one depot and searching another reports their
intersection.

`total` receives the **total** match count, not the number delivered. Returning 0
from the callback stops delivery, not the search: the scan and its allocation
have both finished before your callback runs at all. Count deliveries yourself if
you need them.

The callback runs with no lock held, so reading a file from inside it is fine and
is the intended shape. Paths those reads learn do not join a walk already running.
The strings it hands you are interned for the life of the process, so they are
safe to keep.

### Crossing into Lua

Lua numbers are doubles and lose precision above 2⁵³, so a key cannot cross that
boundary as a number. Move it as decimal text:

```c
char key_str[REDFS_HASH_STRING_MAX];        /* 21 bytes is always enough */
redfs_hash_string(kBody, key_str, sizeof key_str);
uint64_t back = redfs_hash_parse(key_str);
```

## 9. Turn on the mesh cache

Chunk bounds cost a geometry decompress to produce and never change, because the
archives they came from never change. So pay once, ever:

```c
redfs_cache_open(depot, "redfs_mesh.cache");   /* after everything is mounted */
...
redfs_cache_flush();                            /* periodically */
redfs_cache_close();                            /* or let redfs_shutdown do it */
```

Every `redfs_mesh_open` from then on is remembered, and the file survives process
restarts. On the reference body mesh: 0.72 ms cold, 0.00 ms warm; a cold open of
a typical body mesh is 1–2 ms. `redfs_cache_entry_count()` reports how many
meshes are held.

### And the path cache

Same shape, different guarantees. Learning the dictionary from import tables
means reading every file that has them — minutes on a modded install — so persist
it:

```c
redfs_path_cache_open("redfs_paths.cache");   /* after everything is mounted */

uint32_t n = 0;
redfs_path_cache_pending(depot, NULL, 0, &n);        /* archives not yet read */
uint32_t* todo = malloc(n * sizeof *todo);
redfs_path_cache_pending(depot, todo, n, &n);
for (uint32_t i = 0; i < n; ++i) {
    /* read the files whose redfs_file_info.archive_index == todo[i] */
    redfs_path_cache_mark(depot, todo[i]);           /* per archive, as it finishes */
}

redfs_path_cache_close();                            /* or let redfs_shutdown do it */
```

Unlike the mesh cache it is **never discarded** — a hash → name mapping cannot go
stale — so it merges rather than invalidating, and installing a mod costs
harvesting that mod instead of the whole depot. It also switches the dictionary
on, so you do not need a separate `redfs_path_enable()`.

Four things to know:

- **The cache belongs to the depot you pass here.** There is one per process,
  entries are keyed by hash alone, and the same hash means different bytes in a
  different depot — so `redfs_mesh_open` on any *other* depot bypasses the cache
  entirely rather than risk a cross-depot answer. Keep two depots and only one of
  them benefits.
- **It fingerprints the mounted archive set** and silently discards itself when
  that moves, so a game patch, a new mod, or an archive replaced in place cannot
  serve stale geometry. Mounting after `redfs_cache_open` re-fingerprints and
  drops what it holds — correct, but it costs you the warm cache, hence mounting
  first.
- **A handle outlives the cache.** Entries are reference-counted, so
  `redfs_mesh_close` is always right to call and a handle stays valid across
  `redfs_cache_close`.
- **Precompute deliberately.** `redfs_cache_warm` needs a cache opened on this
  depot and returns `REDFS_E_INVALID_ARG` without one, rather than paying for
  results it would then discard:

```c
uint32_t computed = 0;
redfs_cache_warm(depot, hashes, count, &computed);   /* skips what is cached */
```

It flushes when it finishes. Do **not** try to warm the whole depot — on the
order of 10⁵ meshes would cost minutes of startup and gigabytes of memory. Warm
what you actually touch.

## 10. Keep it off the game thread

A read is a fraction of a millisecond for a small file and milliseconds for a
large one — 0.16 ms for 40 KB on the reference install, 1–2 ms for a cold mesh.
Either call from your own worker, or queue it:

```c
static void on_loaded(redfs_status st, redfs_blob blob, void* user) {
    if (st == REDFS_OK) queue_for_main_thread(blob);   /* you own it now */
    /* Chaining the next read from here is fine and is the normal pattern.
       Do NOT call redfs_drain, redfs_shutdown or redfs_depot_close from a
       callback: the first two would wait on the job you are completing (they
       detect it and return without acting), and the third cannot cancel from
       this thread. */
}

if (redfs_read_async(depot, key, REDFS_PART_ALL, on_loaded, user) != REDFS_OK)
    handle_it_here();
```

**The return value decides whether the callback fires at all.** `REDFS_OK` means
it will fire exactly once, on RedFS's worker thread, with either a result or
`REDFS_E_CANCELLED`. Anything else — `REDFS_E_CANCELLED` because a shutdown is in
progress, `REDFS_E_INVALID_ARG` for a null depot or callback — means it will
**not** fire, so that branch has to handle the failure itself. On `REDFS_OK` the
callback owns the blob and must free it, or hand ownership on as above.
Marshalling the result back to the game thread is yours to do, because the right
way is engine-specific.

`redfs_drain()` blocks until every queued read has completed.
`redfs_depot_close` cancels anything still queued against that depot — those
callbacks fire with `REDFS_E_CANCELLED` — and waits out a read already in flight,
bounded by one segment decode, so closing with work outstanding is safe on its
own.

Finally, **`redfs_shutdown()` before your DLL can be unloaded**, from a RED4ext
plugin's `Main(EMainReason::Unload)` or your CET `onShutdown`. It stops and joins
the worker and closes the cache; unloading with the worker alive unmaps code that
thread is running. Never call it from `DllMain` — joining a thread under the
loader lock deadlocks. `INTEGRATION.md` covers this properly, including what
happens if you skip it.

## 11. The C++ facade

Same library, less typing: header-only, RAII, `std::optional` instead of status
codes. Mixing layers is fine — `handle()` on `Depot`, `Mesh` and `Cr2w` gives the
C pointer.

```cpp
#include "redfs.hpp"

auto depot = redfs::Depot::open();       // nullopt on failure or ABI mismatch
if (!depot) return;
depot->enable_cache("redfs_mesh.cache");
depot->load_paths("usedhashes.kark");    // returns how many resolve here

uint64_t mask = 0;
if (auto mesh = depot->mesh(kBody)) {    // named, not a temporary -- see below
    const auto [lo, hi] = mesh->bounds();
    const float chest_floor = lo[2] + (hi[2] - lo[2]) * 0.66f;
    for (const auto& c : mesh->chunks())
        if (c.lod == 1 && c.bounds_valid && c.bbox_min[2] > chest_floor)
            mask |= 1ull << c.index;
}

if (auto dds = depot->texture_dds(icon_path))
    upload(dds->data(), dds->size());    // freed on scope exit
```

**`chunks()` returns a span that borrows the `Mesh`.** So
`depot->mesh(path)->chunks()` dangles: the optional temporary dies at the end of
the full expression, taking the handle with it. Bind the mesh to a named variable
first, as above. With a cache open the data survives on the cache's own reference
and the mistake appears to work, which is worse than a clean crash.

`Depot::open_resource` handles the same class of problem on the CR2W side: it
returns the `Blob` *and* the `Cr2w` as a pair, so the bytes cannot be dropped
while the document still points at them. Also worth knowing: `redfs::path_of`
yields an empty view rather than null for an unknown hash, and `read_async` takes
any callable — moved to the heap and destroyed by the callback, so it may be a
temporary, but whatever it captures by reference must outlive the read.

## 12. Diagnose with the CLI

```
redfs_cli [--game DIR] [--cache FILE] <command>

  --game DIR                    install root; omit to auto-detect
  --cache FILE                  persist decoded meshes between runs

  info                          list mounted archives
  hash <path>...                depot path -> 64-bit key
  paths <list> [key...]         load a dictionary, resolve hashes to paths
  find <list> <pattern> [n]     files matching a glob (* ?); a bare word
                                is taken as a substring
  stat <key>                    where a file lives and how big it is
  extract <key> <out> [part]    part = main | all | <buffer index>
  cr2w <key> [chunk] [path]     chunks, imports and properties
  arr <key> <chunk> <path> [n]  elements of an array property
  chunks <key> [appear] [lod]   per-chunk lod, material and bounding box
  tex <key> [out.dds]           texture descriptor, optionally as DDS
  mesh <key>                    mesh geometry layout
  audio <key>                   sniff the audio container
  bench                         read cost vs position inside an archive
  selftest                      verify the library against the install
```

`<key>` is a depot path or a literal `0x`-prefixed hash; `<list>` is
`usedhashes.kark` or one path per line. Every command but `hash` mounts a depot
first.

Finding something when you only half-remember the path:

```
redfs_cli --game "D:\...\Cyberpunk 2077" find usedhashes.kark "tshirt" 10
```

And the chest query, straight from the command line, before writing any code:

```
redfs_cli --cache redfs_mesh.cache chunks "base\...\t0_000_pwa_base__full.mesh"
```

That prints the appearances, then a chunk table of LOD, material and bounding box,
then the decode time and the cache size. `chunks <key> <appearance> 1` narrows it
to one appearance's materials and to LOD 1.

Set `REDFS_VERBOSE=1` for internal logging on any of them.

## Pitfalls, in one place

- **`redfs_cr2w` borrows its bytes.** Free the blob only after
  `redfs_cr2w_close`, and copy anything you keep past it.
- **A `redfs_cr2w` handle is single-threaded.** Decoding a `CString` caches it on
  the handle, so two threads calling `redfs_cr2w_get` on the *same* handle mutate
  the same containers with no lock — heap corruption, not a stale read. One handle
  per thread; the depot underneath is still shared, and the typed helpers are
  unaffected because each builds a private handle per call.
- **`mount`, `close`, `shutdown`, the cache calls and the path calls are not
  thread-safe.** Reads, stats, enumeration and the texture/mesh helpers are — so
  open and mount first, then share the depot freely.
- **Check `bounds_valid` before believing a chunk box** (§6), and filter on `lod`
  or you count geometry twice.
- **`redfs_mesh_close` every handle**, cached or not.
- **`redfs_audio_probe` decodes the whole main segment** to read 16 bytes. Never
  on the game thread.
- **A `redfs_path_from_hash` hit is not proof the file exists** (§8).
- **Missing `oo2ext_7_win64.dll` is not a mount failure**; it surfaces on the
  first compressed read, so check `redfs_oodle_available()` at startup.
- **Reading from a path you guessed** usually fails. Confirm with
  `redfs_cli find` first.
- **`redfs_shutdown()` before your DLL can be unloaded**, and never from
  `DllMain`.
