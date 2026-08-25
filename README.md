# RedFS

Read Cyberpunk 2077 `.archive` files at runtime, from native code.

A mod that needs a game texture, voice line, or mesh normally has to extract it
with WolvenKit, repack it, and ship the copy. RedFS reads it out of the player's
own install instead. Nothing is extracted to disk, nothing is redistributed, and
no game internals are touched.

Ships as `redfs_static.lib` or `RedFS.dll` behind a stable C ABI
(`include/redfs.h`), with an optional header-only C++ wrapper
(`include/redfs.hpp`).

[Usage](docs/USAGE.md) · [Integration & lifecycle](docs/INTEGRATION.md) ·
[API reference](docs/API.md) · [All docs](docs/README.md)

## Quickstart

Build it:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

C++20, MSVC, Windows x64, no external dependencies. Oodle is bound by name at
runtime. The build produces `redfs_static.lib`, `RedFS.dll`, two tools
(`redfs_cli`, `redfs_verify`) and a worked example (`example_chest`).

Link it:

```cmake
add_subdirectory(third_party/RedFS)
target_link_libraries(my_plugin PRIVATE RedFS::static)   # or RedFS::shared
```

Read something:

```c
#include "redfs.h"

redfs_depot* depot;
if (redfs_depot_open(NULL, REDFS_SCAN_ALL, &depot) != REDFS_OK)  /* NULL: auto-detect */
    return;

redfs_blob dds;
if (redfs_texture_read_dds(depot, redfs_hash("base\\icon\\foo.xbm"), &dds) == REDFS_OK) {
    /* dds.data is a complete DDS; hand it to CreateDDSTextureFromMemory */
    redfs_blob_free(&dds);
}
redfs_depot_close(depot);
```

### Handling failures

Every fallible call returns a `redfs_status`. The codes you will actually branch
on:

| Code | Means |
|---|---|
| `REDFS_OK` | Success. |
| `REDFS_E_NOT_FOUND` | No such file in the depot, or no such property. |
| `REDFS_E_UNSUPPORTED` | Format recognised, not handled by this build. |
| `REDFS_E_OODLE` | `oo2ext_7_win64.dll` missing, or a decode failed. |
| `REDFS_E_CORRUPT` | Archive or CR2W data failed validation. |
| `REDFS_E_NO_DICTIONARY` | `redfs_find` with an empty path dictionary. |

`redfs_status_string` names the code. `redfs_last_error` returns a
human-readable reason for the last failure on the calling thread, and is worth
logging:

```c
redfs_status st = redfs_read(depot, hash, REDFS_PART_ALL, &blob);
if (st != REDFS_OK)
    log("read failed: %s (%s)", redfs_status_string(st), redfs_last_error());
```

Read it only after a failure. Every entry point clears it on the way in, so it
never reports some earlier unrelated call, but a call that returned `REDFS_OK`
leaves behind whatever it logged along the way.

`redfs_oodle_available` tells you at startup whether decompression will work at
all, which is a better place to fail than the first read.

## Common tasks

| Goal | Call |
|---|---|
| Hash a path | `redfs_hash` |
| Check a file is present | `redfs_exists` |
| Size, SHA-1, winning archive | `redfs_stat` |
| Read a whole file | `redfs_read` with `REDFS_PART_ALL` |
| Read only the resource header | `redfs_read` with `REDFS_PART_MAIN` |
| Read into your own buffer | `redfs_read_into` |
| Read off the game thread | `redfs_read_async` |
| Texture as a ready-to-upload DDS | `redfs_texture_read_dds` |
| Texture extent, mips, DXGI format | `redfs_texture_desc_of` |
| Per-submesh bounding boxes | `redfs_mesh_open`, `redfs_mesh_chunk_at` |
| Material for a chunk in an appearance | `redfs_mesh_find_appearance`, `redfs_mesh_chunk_material` |
| Path back from a hash | `redfs_path_from_hash` (needs the dictionary) |
| List files matching a glob | `redfs_find` (needs the dictionary) |
| Walk arbitrary CR2W properties | `redfs_cr2w_get`, `redfs_cr2w_walk` |
| `.wem` codec, channels, sample rate | `redfs_audio_info_of` |

## Files

```c
uint64_t     redfs_hash(const char* depot_path);   /* normalise + FNV1a64 */
int          redfs_exists(depot, hash);
redfs_status redfs_stat(depot, hash, &info);       /* size, sha1, winning archive */
redfs_status redfs_enumerate(depot, fn, user);     /* every entry, hash-ordered */
redfs_status redfs_read(depot, hash, part, &blob); /* MAIN | ALL | buffer i */
redfs_status redfs_read_into(depot, hash, part, dst, capacity, &written);
redfs_status redfs_read_async(depot, hash, part, cb, user);
```

A file is stored as segments. Segment 0 is the resource, a CR2W document; the
rest are its attached buffers, such as pixels for a texture or vertex streams for
a mesh. `REDFS_PART_MAIN` reads just the document, which is what you want when
you only need metadata. `REDFS_PART_ALL` concatenates everything, which is what
you want for containers like `.bnk` and `.opuspak`.

Use `redfs_part_size` to size a destination buffer before `redfs_read_into`.

## Paths, both directions

`redfs_hash` is a pure function. The reverse cannot be, since FNV1a is one-way
and archives keep no path table, so RedFS keeps a dictionary filled from three
sources:

1. **CR2W import tables**, learned automatically as you read files. Free, and the
   only source that knows paths a mod invented. Off until you call
   `redfs_path_load` or `redfs_path_enable`.
2. **A path list**: WolvenKit's `usedhashes.kark`, or plain text, one per line.
3. **`redfs_path_add`**.

Only the path list is filtered against the mounted depot. Import learning and
`redfs_path_add` have no depot to check against, so a hit there tells you what a
file is *called*, not that it is readable. Call `redfs_exists`, or just handle
`REDFS_E_NOT_FOUND` from the read.

Strings from `redfs_path_from_hash` stay valid for the lifetime of the process.
Later additions from any source cannot invalidate a pointer you hold.

On the reference install WolvenKit's list resolves 544,496 of the depot's
544,670 files, or 99.97% coverage.

### Searching without reading

`redfs_find` globs over the dictionary and reads no file data, which is how you
build a work list without touching a byte of any of it:

```c
redfs_find(depot, "base\\characters\\*.mesh", on_mesh, depot, &total);
```

`*` and `?` both span separators, so `*.mesh` means any mesh anywhere, and a
trailing separator means everything beneath it. The pattern is normalised like a
path, so casing and `/` versus `\` do not matter. Pass a depot and every hit is a
file the index holds; that is presence, not readability, and the read can still
fail.

This searches the dictionary, not the depot, so it finds only what the sources
above have taught it. Archives carry no path table for anything to enumerate.
With an empty dictionary it returns `REDFS_E_NO_DICTIONARY`, so a missing path
list cannot be mistaken for a bad pattern.

### Hashes in scripting hosts

Hosts that cannot hold a `uint64` exactly (Lua numbers are doubles and lose
precision above 2⁵³) can move keys across as decimal strings:

```c
char key[REDFS_HASH_STRING_MAX];
redfs_hash_string("base\\...\\x.mesh", key, sizeof key);  /* "1234567890123456789" */
uint64_t h = redfs_hash_parse(key);
```

## Mesh chunks

A chunk is one submesh, and a chunk index is a bit in a component's `chunkMask`,
which is what makes these answers usable against a live entity.

```c
redfs_mesh* m;
if (redfs_mesh_open(depot, redfs_hash(path), &m) != REDFS_OK) return;

for (uint32_t i = 0; i < redfs_mesh_chunk_count(m); ++i) {
    const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
    if (!c->bounds_valid) continue;  /* boxes are all-zero; do not read them */
    /* c->index, c->lod, c->lod_mask, c->vertex_count, c->index_count,
       c->bbox_min[3], c->bbox_max[3] */
}
redfs_mesh_close(m);
```

Chunks repeat per LOD, so filter on `c->lod` to get one copy of each.

### Where the bounding boxes come from

The format stores no per-chunk box. `rendChunk` carries vertex and index counts,
a `lodMask` and stream offsets; only `CMesh` has a box, and it covers the whole
mesh. RedFS computes the per-chunk boxes by dequantizing each chunk's positions
out of the geometry buffer:

```
p = int16 / 32767 * header.quantizationScale + header.quantizationOffset
```

Boxes come out in mesh-local game space, Z up, matching component transforms
rather than the Y-up flip glTF exporters apply.

A chunk whose geometry is absent, streamed out, or failing its span check gets
`bounds_valid == 0` and a zeroed box. That is about 1 stock mesh in 10,000.
**Test the flag, not the box against zero.**

### What that gives you

The stock `t0_000_pwa_base__full.mesh`, LOD 1 only:

```
idx  lod  verts   tris   material     bbox min (x y z)        bbox max (x y z)
0    1    1011    1832   01_ca_pale   -0.156 -0.139  1.226    0.156  0.090  1.509
1    1    257     380    01_ca_pale   -0.121 -0.138  1.421    0.121  0.050  1.531
2    1    367     628    01_ca_pale   -0.157 -0.120  1.045    0.157  0.082  1.270
3    1    514     792    01_ca_pale   -0.180 -0.159  0.917    0.180  0.059  1.109
4    1    988     1820   01_ca_pale   -0.189 -0.124  0.510    0.189  0.056  0.980
5    1    260     424    01_ca_pale   -0.201 -0.139  0.277    0.201 -0.022  0.528
6    1    396     612    01_ca_pale   -0.193 -0.130  0.074    0.193 -0.021  0.286
7    1    848     1408   01_ca_pale   -0.205 -0.139  0.005    0.205  0.077  0.106
```

The chunks stack vertically, feet to head, so "which chunks are the chest"
answers to a `z` threshold on any mesh, vanilla or modded, with no prior
knowledge of the mod. `tools/example_chest.cpp` does exactly this: it takes the
top third of whatever the mesh spans and emits the `chunkMask`.

### Materials

Materials are per *appearance*, since that is what a component selects by CName:

```c
int32_t app = redfs_mesh_find_appearance(m, "01_ca_pale");
const char* mat = redfs_mesh_chunk_material(m, app, chunk_index);
```

`redfs_mesh_desc_of` is the cheap header-only path when you need chunk and
appearance counts without the geometry pass.

## Textures

```c
redfs_texture_desc d;
redfs_texture_desc_of(depot, hash, &d);  /* extent, mips, slices, DXGI format */

redfs_blob dds;
redfs_texture_read_dds(depot, hash, &dds);   /* complete in-memory DDS */

redfs_blob raw;
redfs_texture_read_raw(depot, hash, &d, &raw);  /* just the pixels, plus desc */
```

Handles `CBitmapTexture`, `CTextureArray` and `CCubeTexture`. Anything else
returns `REDFS_E_UNSUPPORTED` instead of being described from the wrong chunk.

## Audio

`redfs_audio_info_of` parses a `.wem` header for codec, channels, sample rate and
payload offset. `redfs_audio_walk_chunks` enumerates its RIFF chunks, including
Wwise's private `vorb` and `seek`. RedFS does not decode audio; see
[Limits](#limits).

## CR2W introspection

RedFS reads the container with no knowledge of the RED4 type system. Every
property carries its own name and type name, so a reader that knows zero classes
can still walk the whole graph:

```c
redfs_cr2w_get(f, chunk, "header.sizeInfo.width", &v);   /* dotted paths */
redfs_cr2w_walk(f, chunk, NULL, print_prop, NULL);       /* enumerate */
redfs_cr2w_walk_array(f, &array_value, visit_elem, NULL);
redfs_cr2w_import_path(f, i);                            /* dependency list */
```

## Caching

Two caches, both optional and both opt-in.

**Mesh bounds** cost a geometry decompress to produce and never change, because
the archives they came from never change. Compute once and keep:

```c
redfs_cache_open(depot, "redfs_mesh.cache");
/* every redfs_mesh_open from now on is remembered, across restarts */
```

The first call for a mesh computes it (median 0.72 ms); every later one is a
lookup. `redfs_cache_warm(depot, hashes, count, &computed)` precomputes a known
set, and requires a cache already open on this depot. The cache belongs to that
one depot, since entries are keyed by hash alone, so `redfs_mesh_open` on any
other depot bypasses it. It fingerprints the mounted archive set and silently
discards itself when that moves.

**The path dictionary** is expensive to fill from import tables, minutes on a
modded install. Persist it instead:

```c
redfs_path_cache_open("redfs_paths.cache");   /* also enables the dictionary */

uint32_t n = 0;
redfs_path_cache_pending(depot, NULL, 0, &n);       /* how many need reading */
uint32_t* todo = malloc(n * sizeof *todo);
redfs_path_cache_pending(depot, todo, n, &n);

for (uint32_t i = 0; i < n; ++i) {
    /* read the files whose redfs_file_info.archive_index == todo[i];
       parsing them is what teaches the dictionary their imports */
    redfs_path_cache_mark(depot, todo[i]);
}
redfs_path_cache_close();                            /* flushes */
```

Unlike the mesh cache, this one is never discarded. A hash-to-name mapping is a
fact about the string, so no mod install or game patch can make a restored entry
wrong. It tracks which archives it has already read, so installing a mod costs
harvesting that mod and not the whole depot. Restore takes 658 ms for 652,594
paths, against the ~180 s teach it replaces.

Design and file formats: [docs/done/caching.md](docs/done/caching.md) and
[docs/done/path-cache.md](docs/done/path-cache.md).

## Mount order and mods

`redfs_depot_open` scans four archive sets in the game's own order, and later
mounts win:

```
archive/pc/content  ->  archive/pc/ep1  ->  mods/<name>/archives  ->  archive/pc/mod
```

So a legacy `.archive` beats a REDmod one, which beats the base game. Lookup is
one flat namespace, files a mod adds read like any other, and `redfs_stat`
reports which archive actually won.

Ordering *within* a set is not uniform, and that is the part that catches people.
Under `archive/pc` the alphabetically **last** archive wins, which is why modders
prefix with `zz_`. REDmod is searched recursively and ordered in reverse, so
there the **first** path wins and a `zz_` prefix loses. Both match WolvenKit's
`ArchiveManager`.

Mod managers, the MO2 and Vortex VFS problem, and the full override rules are in
[docs/INTEGRATION.md](docs/INTEGRATION.md#mods-layering-and-overrides).

## Threading

A depot is immutable once open, so `redfs_read*`, `redfs_stat`,
`redfs_enumerate`, `redfs_texture_*` and `redfs_mesh_*` are safe from any number
of threads. Not safe: `redfs_depot_mount*`, `redfs_depot_close`,
`redfs_shutdown`, `redfs_cache_*` and `redfs_path_*`. Open first, then share.

A single `redfs_cr2w` handle is single-threaded, since decoding a `CString`
caches it on the handle. Give each thread its own.

Reads take milliseconds, so keep them off the render thread or use
`redfs_read_async`. Call `redfs_shutdown` before your DLL can be unloaded, and
**never from `DllMain`**. [docs/INTEGRATION.md](docs/INTEGRATION.md) has the full
startup and shutdown order.

## How it works

The `.archive` container is fully understood: a header, an index of
`(hash -> segment range)`, and a segment table of `(offset, compressed size,
size)`. Compressed segments are Oodle Kraken behind an 8-byte `KARK` header, and
`oo2ext_7_win64.dll` ships with every copy of the game. Inside the game process
it is already loaded, so RedFS binds it by name with `GetModuleHandle` and never
redistributes it.

There are no address-library offsets and no hooks, so a game patch does not break
RedFS the way it breaks tools that hook `ResourceDepot`.

Reads are true random access. Nothing is bulk-extracted. Lookup is a binary
search over the sorted index, then only that file's byte window is mapped and
decoded, so cost is O(size of the file you asked for) and independent of where it
sits. Sixty reads from each of five slices of one 24 GB archive give
356 / 354 / 326 / 427 / 418 MB/s. Reproduce with `redfs_cli bench`.

### Measured on a real install

57 archives, 85 GB, 544,670 files (Steam, 2.3 plus Phantom Liberty):

| | |
|---|---|
| Mount the whole depot | ~30 ms |
| Resident cost | 8.7 MB heap + ~89 MB file-backed index mapping (evictable) |
| Read one 40 KB file from the middle of a 24 GB archive | 0.16 ms |
| `redfs_mesh_open`, uncached (computes chunk bounds) | median 0.72 ms, p90 2.27 ms |
| `redfs_mesh_open`, cached | ~0 ms |
| Path dictionary: learn 652,594 paths from import tables | ~180 s |
| Path dictionary: restore the same from cache | 658 ms |

The mesh figure is 200 meshes sampled by stride across a stock install, with no
RedFS mesh cache and a warm OS page cache: min 0.05, median 0.72, mean 0.98, p90
2.27, max 7.14 ms. The spread is mesh size, so budget against p90 if you are
deciding what fits in a frame.

## ABI versioning

`REDFS_ABI_VERSION` is **2**. If you link `RedFS.dll`, check `redfs_abi_version()`
against it at startup. Version 2 added `redfs_mesh_chunk::bounds_valid`, so
anything built against 1 needs a recompile. A mismatch is silent in the worst
way, and the check has a real blind spot for added functions;
[docs/INTEGRATION.md](docs/INTEGRATION.md#check-the-abi-when-you-link-the-dll)
explains both.

## Verifying

```
.\run-checks.ps1                                    # needs no game install
.\run-checks.ps1 -GameDir "D:\...\Cyberpunk 2077"   # + integration sweep
```

Four configurations, because each catches what the others cannot: release
(logic), debug (leaks, via the CRT heap), asan (out-of-bounds and use-after-free
in every parser), and install (format correctness against external oracles).

Unit tests need no game install. `tests/fixtures.cpp` builds `.archive` and CR2W
containers byte-exactly from nothing, which also makes it possible to test
malformed input no real install would ever produce. 70 cases, 1175 checks, ~1.6
seconds, plus a deterministic mutation fuzzer over both parsers and a separate
`redfs_lifecycle` binary covering teardown.

`redfs_verify` is the stronger check, validating against oracles outside RedFS:
DirectXTex independently parsing the DDS headers RedFS emits, mip-chain
arithmetic against the payload sizes the archive stores, and `CMesh.boundingBox`
containing the union of the computed chunk boxes. The last full sweep on the
reference install: 11,255 textures with 0 header and 0 payload size mismatches,
and 12,000 meshes with 0 chunk unions escaping the stored box.

Neither mod scan path is exercised against real mod content, only against
synthesized fixtures. That and the rest of the coverage gaps are listed in
[docs/done/verification.md](docs/done/verification.md#gaps-in-the-verification-itself).

How the oracles were designed:
[docs/done/verification.md](docs/done/verification.md). Fuzzing, leak detection,
and the bugs the tooling found: [docs/done/testing.md](docs/done/testing.md).

## Scope

RedFS reads archives. It does not touch live game state: no entities, no
components, no RTTI. `entityComponents(entity)` and anything else needing the
running game belongs in your own RED4ext plugin. RedFS gives you the other half,
turning the resource hash on a live component into a path, a chunk table and a
set of boxes.

## Limits

- **Audio is never decoded.** `.wem` extraction and header parsing work (codec,
  channels, rate, payload offset, RIFF chunk walk), but converting to ogg or mp3
  means bundling Vorbis and Opus, and Wwise Vorbis needs its codebooks rebuilt.
  That is ww2ogg, vgmstream and ffmpeg's job, not an archive reader's.
- **`redfs_audio_probe` is not cheap.** Kraken cannot decode a prefix, so
  identifying a container from its first 16 bytes decodes the whole main segment,
  which is tens of MB for music. Call it off the game thread, or skip it when the
  extension already tells you what you have.
- **Voice-over `.opuspak`** needs its paired `.opusinfo` to index. RedFS reports
  the container type and hands back raw bytes without demuxing it. Format mapped,
  design in [docs/audio-opus.md](docs/audio-opus.md).
- **Vertex streams beyond positions are not decoded.** Chunk bounds are computed;
  normals, UVs and weights are left in the raw buffer. See
  [docs/vertex-streams.md](docs/vertex-streams.md).
- **Console texture cooks** (`rendRenderTextureBlobPS4` and friends) are rejected
  instead of guessed at, since a plausible-looking wrong image is worse than an
  error.

Everything open, with cost estimates and the ideas that were rejected and why, is
in [docs/roadmap.md](docs/roadmap.md).

## Documentation

- **[docs/USAGE.md](docs/USAGE.md)** — build, link, recipes, pitfalls
- **[docs/INTEGRATION.md](docs/INTEGRATION.md)** — lifecycle inside the game
- **[docs/API.md](docs/API.md)** — the requested call surface, mapped
- **[docs/done/](docs/done/)** — research and design for what is built: the two
  container formats, path hashing, the texture and mesh pipelines, API
  rationale, caching, and how correctness was established
- **[docs/roadmap.md](docs/roadmap.md)** — what is not built
