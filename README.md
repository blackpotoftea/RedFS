# RedFS

Read Cyberpunk 2077 `.archive` files directly, at runtime, from native code.

A mod that needs a game texture, voice line, or mesh currently has to extract it
with WolvenKit, repack it, and ship the copy. RedFS removes that step: point it at
the player's own install and read what you need, when you need it. Nothing is
extracted to disk, nothing is redistributed, and no game internals are touched.

Static `.lib` or `RedFS.dll`, stable C ABI (`include/redfs.h`), optional
header-only C++ facade (`include/redfs.hpp`).

**[How to use it](docs/USAGE.md)** · [Integration & lifecycle](docs/INTEGRATION.md) ·
[API reference](docs/API.md) · [all docs](docs/README.md)

```c
#include "redfs.h"

redfs_depot* depot;
if (redfs_depot_open(NULL, REDFS_SCAN_ALL, &depot) != REDFS_OK)  /* NULL: auto-detect */
    return;

redfs_blob dds;
if (redfs_texture_read_dds(depot, redfs_hash("base\\icon\\foo.xbm"), &dds) == REDFS_OK) {
    /* dds.data is a complete DDS -- hand it to CreateDDSTextureFromMemory */
    redfs_blob_free(&dds);
}
redfs_depot_close(depot);
```

## How it works

The `.archive` container is fully understood: a header, an index of
`(hash -> segment range)`, and a segment table of `(offset, compressed size,
size)`. Compressed segments are Oodle Kraken behind an 8-byte `KARK` header, and
`oo2ext_7_win64.dll` ships with every copy of the game — inside the game process
it is already loaded, so RedFS binds it by name with `GetModuleHandle` and never
redistributes it.

That means **no address-library offsets and no hooks**, so a game patch does not
break RedFS the way it breaks tools that hook `ResourceDepot`.

## Measured on a real install

57 archives, 85 GB, 544,670 files (Steam, 2.3 + Phantom Liberty):

| | |
|---|---|
| Mount the whole depot | **~30 ms** |
| Resident cost | 8.7 MB heap + ~89 MB file-backed index mapping (evictable) |
| Read one 40 KB file from the middle of a 24 GB archive | **0.16 ms** |
| `redfs_mesh_open`, uncached (computes chunk bounds) | median **0.72 ms**, p90 2.27 ms |
| `redfs_mesh_open`, cached | ~0 ms |
| Path dictionary: learn 652,594 paths from import tables | ~180 s |
| Path dictionary: restore the same from cache | **658 ms** |

The mesh figure is 200 meshes sampled by stride across a stock install, with no
RedFS mesh cache and a warm OS page cache: min 0.05, median 0.72, mean 0.98,
p90 2.27, max 7.14 ms. The spread is mesh size, so budget against p90 rather than
the median if you are deciding what fits in a frame.

**Reads are true random access.** Nothing is bulk-extracted. Lookup is a binary
search over the sorted index; then only that file's byte window is mapped and
decoded, so cost is O(size of the file you asked for) and independent of where it
sits: sixty reads from each of five slices of one 24 GB archive give
356 / 354 / 326 / 427 / 418 MB/s. Reproduce with `redfs_cli bench`.

## What it gives you

### Files

```c
uint64_t     redfs_hash(const char* depot_path);   /* normalise + FNV1a64 */
int          redfs_exists(depot, hash);
redfs_status redfs_stat(depot, hash, &info);       /* size, sha1, winning archive */
redfs_status redfs_enumerate(depot, fn, user);     /* every entry, hash-ordered */
redfs_status redfs_read(depot, hash, part, &blob); /* MAIN | ALL | buffer i */
redfs_status redfs_read_into(depot, hash, part, dst, capacity, &written);
redfs_status redfs_read_async(depot, hash, part, cb, user);
```

A file is stored as segments: segment 0 is the resource (a CR2W document), and
the rest are its attached buffers — pixels for a texture, vertex streams for a
mesh. `REDFS_PART_MAIN` reads just the document, which is what you want when you
only need metadata.

### Paths, both directions

`redfs_hash` is a pure function. The reverse cannot be — FNV1a is one-way and
archives keep no path table — so it is a dictionary, filled from three sources:

1. **CR2W import tables**, learned automatically as you read files. Free, and the
   only source that knows paths a mod invented. Off until you call
   `redfs_path_load` or `redfs_path_enable`.
2. **A path list**: WolvenKit's `usedhashes.kark`, or plain text, one per line.
3. **`redfs_path_add`**.

**Only the path list is filtered against the mounted depot.** Import learning and
`redfs_path_add` cannot be — neither has a depot to check against — so a hit tells
you what a file is *called*, not that it is readable. Call `redfs_exists`, or just
handle `REDFS_E_NOT_FOUND` from the read.

The string `redfs_path_from_hash` returns stays valid for the lifetime of the
process; later additions from any source cannot invalidate a pointer you hold.

On the reference install WolvenKit's list resolves **544,496 of the depot's
544,670 files — 99.97 % coverage.**

The dictionary also runs in the forward direction. `redfs_find` globs over it and
**reads nothing**, which is how you get a list of files to work on without
touching a byte of any of them:

```c
redfs_find(depot, "base\\characters\\*.mesh", on_mesh, depot, &total);
```

`*` and `?` both span separators (so `*.mesh` means *any* mesh, anywhere), and a
trailing separator means everything beneath it. The pattern is normalised like a
path, so casing and `/` versus `\` do not matter. With a depot passed, every hit
is a file the index holds — presence, not readability; the read can still fail.

Because this searches the dictionary rather than the depot, it finds only what
the sources above have taught it — archives carry no path table for anything to
enumerate. With nothing in it, it returns `REDFS_E_NO_DICTIONARY` rather than an
empty success, so a missing path list cannot be mistaken for a bad pattern.

For hosts that cannot hold a `uint64` exactly (Lua numbers are doubles and lose
precision above 2⁵³), keys cross as decimal strings:

```c
char key[REDFS_HASH_STRING_MAX];
redfs_hash_string("base\\...\\x.mesh", key, sizeof key);  /* "1234567890123456789" */
uint64_t h = redfs_hash_parse(key);
```

### Mesh chunks

The headline query. A chunk is one submesh, and **a chunk index is a bit in a
component's `chunkMask`** — which is what makes these answers usable against a
live entity.

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

**The bounding box is not stored anywhere in the format.** `rendChunk` carries
vertex/index counts, a `lodMask` and stream offsets; only `CMesh` has a box, and
it covers the whole mesh. So RedFS computes it, dequantizing each chunk's
positions out of the geometry buffer:

```
p = int16 / 32767 * header.quantizationScale + header.quantizationOffset
```

Boxes come out in mesh-local **game space (Z up)**, matching component
transforms — not the Y-up flip glTF exporters apply. A chunk whose geometry is
absent, streamed out, or fails its span check gets `bounds_valid == 0` and a
zeroed box: about 1 stock mesh in 10,000, so test the flag rather than testing
the box against zero.

What that buys you, on the stock `t0_000_pwa_base__full.mesh` (LOD 1 only):

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

A clean vertical stack, feet to head. "Which chunks are the chest" stops being a
guess and becomes a `z` threshold — on any mesh, vanilla or modded, without
knowing anything about the mod. `tools/example_chest.cpp` does exactly this,
taking the top third of whatever the mesh spans and emitting the `chunkMask`.

Chunks repeat per LOD, so filter on `c->lod` to get one copy.

Materials are per *appearance*, since that is what a component selects by CName:

```c
int32_t app = redfs_mesh_find_appearance(m, "01_ca_pale");
const char* mat = redfs_mesh_chunk_material(m, app, chunk_index);
```

### The mesh cache

Chunk bounds cost a geometry decompress to produce and never change, because the
archives they came from never change. So compute once, keep forever:

```c
redfs_cache_open(depot, "redfs_mesh.cache");
/* every redfs_mesh_open from now on is remembered, across restarts */
```

First call for a mesh computes it (median 0.72 ms, up to ~7 ms for a large one);
every later call, including after a game restart, is a lookup. To precompute a
known set up front:

```c
redfs_cache_warm(depot, hashes, count, &computed);
```

`redfs_cache_warm` **requires a cache already open on this depot** and returns
`REDFS_E_INVALID_ARG` without one, rather than paying for decodes it would
immediately discard.

Two things to know:

- **There is one cache per process and it belongs to the depot you passed to
  `redfs_cache_open`.** Entries are keyed by hash alone, and the same hash means
  different bytes in a different depot, so `redfs_mesh_open` on any *other* depot
  bypasses the cache entirely. If you keep two depots, only one benefits.
- The cache fingerprints the mounted archive set — per archive: path, entry
  count, index size, index CRC and declared file size — and silently discards
  itself when that moves. The CRC is what catches an archive **replaced in
  place**, where a re-cook keeps the same file and segment counts and every other
  input stays byte-identical. Mounting after `redfs_cache_open` re-checks.

Precomputing *every* mesh in the depot is not viable — there are ~10⁵ of them, so
it would cost minutes of startup and gigabytes. Lazy population reaches the same
end state for the meshes you actually touch.

A mesh handle is a `shared_ptr` internally, so `redfs_mesh_close` is always
correct to call, cached or not, and a handle stays valid across
`redfs_cache_close`.

### The path cache

Filling the dictionary from import tables means reading every file that has them
— minutes on a modded install. Write it once instead:

```c
redfs_path_cache_open("redfs_paths.cache");

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

This is **not** the mesh cache's kind of cache, and the difference is the whole
design:

- **It is never discarded.** A hash → name mapping is a fact about the *string*,
  so removing a mod, patching the game or re-cooking an archive cannot make a
  restored entry wrong. Restore only ever adds. A name for a file no longer
  installed keeps resolving — that is the existing dictionary contract, not a
  stale answer, and the read still returns `REDFS_E_NOT_FOUND`.
- What the file **does** track is which archives it already read, one fingerprint
  each. So installing a mod costs harvesting that mod, not the whole depot.
  Mount order does not matter, and neither does mounting more archives after
  opening the cache — `redfs_path_cache_pending` is computed against the depot as
  it is when you ask.
- Opening it also switches the dictionary on, exactly as `redfs_path_enable`
  does. Restoring names while import learning stayed off would hand you a full
  dictionary that never grew again.

`redfs_path_cache_mark` is per archive on purpose: call it as each one finishes,
so a teach interrupted halfway loses only what it had not got to.

Measured on 652,594 paths: **restore 658 ms** against the ~180 s teach it
replaces, a 41.1 MB file written in 88 ms, and 0.006 ms at shutdown when nothing
new was learned.

See [docs/done/path-cache.md](docs/done/path-cache.md) for the file format and
the four ways this fails silently if built the obvious way.

### Textures

```c
redfs_texture_desc d;
redfs_texture_desc_of(depot, hash, &d);  /* extent, mips, slices, DXGI format */

redfs_blob dds;
redfs_texture_read_dds(depot, hash, &dds);   /* complete in-memory DDS */

redfs_blob raw;
redfs_texture_read_raw(depot, hash, &d, &raw);  /* just the pixels, plus desc */
```

Handles `CBitmapTexture`, `CTextureArray`, `CCubeTexture`; anything else is
rejected with `REDFS_E_UNSUPPORTED` rather than described from the wrong chunk.

### Audio

`redfs_audio_info_of` parses a `.wem` header — codec, channels, sample rate,
payload offset — and `redfs_audio_walk_chunks` enumerates its RIFF chunks,
including Wwise's private `vorb` and `seek`. RedFS does not decode audio; see
[Known gaps](#known-gaps).

### CR2W introspection

Reads the container with no knowledge of the RED4 type system — every property
carries its own name and type name, so a reader that knows zero classes can still
walk the whole graph:

```c
redfs_cr2w_get(f, chunk, "header.sizeInfo.width", &v);   /* dotted paths */
redfs_cr2w_walk(f, chunk, NULL, print_prop, NULL);       /* enumerate */
redfs_cr2w_walk_array(f, &array_value, visit_elem, NULL);
redfs_cr2w_import_path(f, i);                            /* dependency list */
```

## Mount order, mods and mod managers

`redfs_depot_open` scans four archive sets in the game's own order, and **later
mounts win**:

```
archive/pc/content  ->  archive/pc/ep1  ->  mods/<name>/archives  ->  archive/pc/mod
```

So a legacy `.archive` beats a REDmod one, which beats the base game. Lookup is
one flat namespace, files a mod *adds* read like any other, and `redfs_stat`
reports which archive actually won.

Ordering *within* a set is not uniform, and this trips people up:

- **Under `archive/pc`** (`content`, `ep1`, `mod`) archives mount in filename
  order and the alphabetically **last** one wins — which is why modders prefix
  with `zz_`. These folders are scanned top level only.
- **REDmod is searched recursively** — `mods/<name>/archives` and everything
  beneath it — ordered by full path and then **reversed**. So inside a single
  REDmod the ordinally **first** path wins and a `zz_` prefix **loses**. Between
  REDmod folders the later folder name still wins.

Both match WolvenKit's `ArchiveManager`, which is the reference for what the game
does.

**MO2 / Vortex:** their VFS only exists inside processes they launch. Running
*inside* a game launched by MO2, the merged mod archives already appear under
`archive/pc/mod` and `REDFS_SCAN_ALL` finds them. Running a tool *outside* MO2
they are invisible — mount the staging folder explicitly:

```c
redfs_depot_mount_dir(depot, "D:\\mods\\SomeMod", &mounted);
```

## Threading and lifecycle

A depot is immutable once open, so `redfs_read*`, `redfs_stat`,
`redfs_enumerate`, `redfs_texture_*` and `redfs_mesh_*` are safe from any number
of threads. **Not** safe: `redfs_depot_mount*`, `redfs_depot_close`,
`redfs_shutdown`, `redfs_cache_*` and `redfs_path_*` — open first, then share.

A single `redfs_cr2w` handle is single-threaded: decoding a `CString` caches it
on the handle. Share the depot, give each thread its own handle. The typed
helpers above are unaffected; each builds a private handle per call.

Reads take milliseconds. Call them off the render thread, or use
`redfs_read_async` — the callback runs on RedFS's worker, so marshal back
yourself.

`redfs_depot_close` cancels anything still queued against that depot; those
callbacks fire with `REDFS_E_CANCELLED`, so nothing is left waiting. Call
`redfs_shutdown` before your DLL can be unloaded — from RED4ext's
`Main(EMainReason::Unload)` or CET's `onShutdown`, and **never from `DllMain`**.
[docs/INTEGRATION.md](docs/INTEGRATION.md) covers the whole lifecycle.

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Produces `redfs_static.lib` (link into your own DLL), `RedFS.dll`, two tools
(`redfs_cli`, `redfs_verify`) and a worked example (`example_chest`). C++20, MSVC,
Windows x64. No external dependencies — Oodle is bound by name at runtime.

`REDFS_ABI_VERSION` is **2**. Check `redfs_abi_version()` against it at startup if
you link `RedFS.dll`; version 2 added `redfs_mesh_chunk::bounds_valid`, so
anything built against 1 needs a recompile.

Know what that check does *not* cover. It bumps for struct layout and for the
meaning of a call, not for additions — `redfs_find` was added without a bump,
correctly. So a new header against an older DLL reports 2 on both sides and passes,
then fails at load with *entry point redfs_find could not be found*. That is loud
and immediate rather than silent, but it is the loader telling you, not the check:
one integer compared with `==` cannot express "has `redfs_find`".

## Verifying

```
.\run-checks.ps1                                    # needs no game install
.\run-checks.ps1 -GameDir "D:\...\Cyberpunk 2077"   # + integration sweep
```

Four configurations, because each catches what the others cannot: **release**
(logic), **debug** (leaks, via the CRT heap, measured as steady state across two
full passes), **asan** (out-of-bounds and use-after-free in every parser),
**install** (format correctness against external oracles).

Unit tests need no game: `tests/fixtures.cpp` builds `.archive` and CR2W
containers byte-exactly from nothing, which also makes it possible to test
malformed input no real install would ever produce. **70 cases, 1175 checks, ~1.6
seconds.** A separate `redfs_lifecycle` binary covers teardown — abrupt exit,
shutdown under load, and a real `LoadLibrary`/`FreeLibrary` cycle.

A deterministic mutation fuzzer hammers both parsers — corrupt archives must
produce errors, never crashes. Under ASan it caught a real out-of-bounds read:
`name()` bounds-checked the string-table *index* but not the *offset*, so a
corrupt name table sent `strcmp` off the end of the buffer. Details and the rest
of the tooling story in [docs/done/testing.md](docs/done/testing.md).

`redfs_cli selftest` mounts the depot, samples 400 files spread across the
archives, decodes them, and checks sizes against the index.

`redfs_verify` is the real check — it validates against oracles outside RedFS:

- **Texture headers** go to DirectXTex (WolvenKit's `texconv.dll`), which parses
  them independently; extent, mips, DXGI format, slice count and the cubemap flag
  must all match what RedFS claimed.
- **Texture payloads** are checked by arithmetic: how many bytes a mip chain of
  that exact format and extent *must* occupy, versus the size the archive
  actually stores. Those numbers come from different places, so a wrong DXGI
  format or mip count cannot match by accident.
- **Mesh bounds** are checked against `CMesh.boundingBox` — written by CDPR's
  cooker, never read by the code being tested. The union of computed chunk boxes
  must fall inside it; a wrong stride or quantization scale escapes immediately.
- **`redfs_mesh_desc_of` against `redfs_mesh_open`**, which reach chunk and
  appearance counts by different routes and must agree.

The last full sweep, on the reference install: **11,255 textures — 0 header
mismatches, 0 payload size mismatches; 12,000 meshes — 0 chunk unions escaping the
stored `CMesh` box, 0 `desc_of`/`mesh_open` disagreements.** A small number of
meshes have no computable bounds and are reported as such rather than counted
clean; see `bounds_valid` above.

**What the sweep does not cover:** the reference install has no
`archive/pc/mod` folder and no REDmod folders under `mods/`, so both mod scan
paths — including the recursive REDmod discovery and its reversed ordering — are
exercised only by synthesized fixtures, never against real mod content. The
ordering rules above are derived from WolvenKit's `ArchiveManager` and covered by
unit tests, not confirmed against a modded install.

That sweep found three real bugs during development: `texture_desc_of` happily
describing a mesh's *embedded* texture instead of refusing; `textureData` being
misdecoded because the type is spelled `serializationDeferredDataBuffer` in
texture resources and `SerializationDeferredDataBuffer` everywhere else; and a
minority of textures whose blob header records the mip-biased extent while the
buffer holds the unbiased surface, which produced a DDS that decoded to garbage.

The bounds check needs a tolerance that scales with both mesh extent and distance
from the origin; a fixed epsilon flags large meshes for being large and distant
ones for being distant. That and the rest of the oracle design is in
[docs/done/verification.md](docs/done/verification.md).

## Scope

RedFS reads archives. It does **not** touch live game state — no entities, no
components, no RTTI. `entityComponents(entity)` and anything else needing the
running game belongs in your own RED4ext plugin; RedFS gives you the other half,
turning the resource hash on a live component into a path, a chunk table and a
set of boxes.

## Known gaps

- **Audio is never decoded.** `.wem` extraction and header parsing work (codec,
  channels, rate, payload offset, RIFF chunk walk), but converting to ogg/mp3
  means bundling Vorbis and Opus, and Wwise Vorbis needs its codebooks rebuilt —
  that is ww2ogg/vgmstream/ffmpeg's job, not an archive reader's.
- **`redfs_audio_probe` is not cheap.** Kraken cannot decode a prefix, so
  identifying a container from its first 16 bytes decodes the whole main
  segment — tens of MB for music. Call it off the game thread, or skip it when
  the extension already tells you what you have.
- **Voice-over `.opuspak`** needs its paired `.opusinfo` to index; RedFS reports
  the container type and hands back raw bytes but does not demux it. Format
  mapped, design in [docs/audio-opus.md](docs/audio-opus.md).
- **Vertex streams** beyond positions are not decoded. Chunk bounds are computed;
  normals, UVs and weights are left in the raw buffer.
  See [docs/vertex-streams.md](docs/vertex-streams.md).
- **Console texture cooks** (`rendRenderTextureBlobPS4` and friends) are rejected
  rather than guessed at — a plausible-looking wrong image is worse than an error.
- `redfs_mesh_desc_of` and `redfs_mesh_open` overlap; the former is the cheap
  header-only path, the latter does the geometry pass.

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
