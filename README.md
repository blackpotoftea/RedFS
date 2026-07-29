# RedFS

Read Cyberpunk 2077 `.archive` files directly, at runtime, from native code.

A mod that needs a game texture, voice line, or mesh currently has to extract it
with WolvenKit, repack it, and ship the copy. RedFS removes that step: point it
at the player's own install and read what you need, when you need it. Nothing is
extracted to disk, nothing is redistributed, and no game internals are touched.

Static `.lib` or `RedFS.dll`, stable C ABI, optional C++ header.

**[How to use it](docs/USAGE.md)** · [Integration & lifecycle](docs/INTEGRATION.md) ·
[API reference](docs/API.md) · [all docs](docs/README.md)

```c
redfs_depot* depot;
redfs_depot_open(NULL, REDFS_SCAN_ALL, &depot);      /* auto-detects the install */

redfs_blob dds;
redfs_texture_read_dds(depot, redfs_hash("base\\icon\\foo.xbm"), &dds);
/* dds.data is a complete DDS -- hand it to CreateDDSTextureFromMemory */
redfs_blob_free(&dds);
```

## Why this works

The `.archive` container is fully understood: a header, an index of
`(hash -> segment range)`, and a segment table of `(offset, compressed size,
size)`. Compression is Oodle Kraken behind a 8-byte `KARK` header, and
`oo2ext_7_win64.dll` ships with every copy of the game — inside the game process
it is already loaded, so RedFS resolves it with `GetModuleHandle` and never
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
| Bulk decode throughput | 300–780 MB/s |
| Mesh chunk bounds, uncached | ~1–2 ms |
| Mesh chunk bounds, cached | **0.00 ms** |

**Reads are true random access.** Nothing is bulk-extracted. Lookup is a binary
search over the sorted index; then only that file's byte window is mapped and
decoded. Cost is O(size of the file you asked for) and independent of where it
sits:

```
depth      reads   bytes      avg ms   MB/s
0-20%      60      7,883,935  0.352    356
20-40%     60      5,301,367  0.238    354
40-60%     60      3,666,863  0.179    326
60-80%     60      6,585,506  0.245    427
80-100%    60      8,009,581  0.305    418
```

## What it gives you

### Files

```c
uint64_t     redfs_hash(const char* depot_path);   /* normalise + FNV1a64 */
int          redfs_exists(depot, hash);
redfs_status redfs_stat(depot, hash, &info);
redfs_status redfs_read(depot, hash, part, &blob); /* MAIN | ALL | buffer i */
redfs_status redfs_read_async(depot, hash, part, cb, user);
```

A file is stored as segments: segment 0 is the resource (a CR2W document), and
the rest are its attached buffers — pixels for a texture, vertex streams for a
mesh. `REDFS_PART_MAIN` reads just the document, which is what you want when you
only need metadata.

### Paths, both directions

`pathToHash` is a pure function. `hashToPath` cannot be — FNV1a is one-way and
archives keep no path table — so it is a dictionary, filled from three sources:

1. **CR2W import tables**, learned automatically as you read files. Free, and the
   only source that knows paths a mod invented.
2. **A path list**: WolvenKit's `usedhashes.kark`, or plain text, one per line.
3. **`redfs_path_add`**.

Only paths that resolve in the mounted depot are kept, so a hit always names a
readable file. On a stock install WolvenKit's list gives **544,496 of 544,670
files — 99.97 % coverage.**

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
redfs_mesh_open(depot, redfs_hash(path), &m);

for (uint32_t i = 0; i < redfs_mesh_chunk_count(m); ++i) {
    const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
    /* c->index, c->lod, c->vertex_count, c->index_count,
       c->bbox_min[3], c->bbox_max[3],
       c->bounds_valid  -- 0 means no box could be computed */
}
redfs_mesh_close(m);
```

**The bounding box is not stored anywhere in the format.** `rendChunk` carries
vertex/index counts, a `lodMask` and stream offsets; only `CMesh` has a box, and
it covers the whole mesh. So RedFS computes it — dequantizing each chunk's
positions from the geometry buffer:

```
p = int16 / 32767 * header.quantizationScale + header.quantizationOffset
```

Boxes come out in mesh-local **game space (Z up)**, matching component
transforms — not the Y-up flip glTF exporters apply.

What that buys you, on `player_female_average\t0_000_pwa_base__full.mesh`:

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
guess and becomes `z > 1.2` — on any mesh, vanilla or modded, without knowing
anything about the mod.

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

First call for a mesh computes (~1–2 ms); every later call, including after a
game restart, is a lookup (0.00 ms). Warm a known set up front if you prefer:

```c
redfs_cache_warm(depot, hashes, count, &computed);
```

The cache stores a fingerprint of the mounted archive set and silently discards
itself if that set moves, so a patch or a newly installed mod can never serve
stale geometry.

Precomputing *every* mesh in the depot is not viable — there are ~10⁵ of them, so
it would cost minutes of startup and gigabytes. Lazy population gets the same
end state for the meshes you actually touch.

### Textures

```c
redfs_texture_desc d;
redfs_texture_desc_of(depot, hash, &d);   /* w/h/mips/slices/DXGI format */

redfs_blob dds;
redfs_texture_read_dds(depot, hash, &dds);  /* complete in-memory DDS */
redfs_texture_read_raw(depot, hash, &d, &blob);  /* just the pixels */
```

Handles `CBitmapTexture`, `CTextureArray`, `CCubeTexture`.

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

## Mod managers

`redfs_depot_open` scans `archive/pc/content`, `ep1`, `mods/` (REDmod) and
`archive/pc/mod` automatically, in the game's own order. Lookup is one flat
namespace; later mounts override earlier ones, so a legacy `.archive` beats a
REDmod one which beats the base game, and `zz_` prefixes win exactly as modders
expect. Files a mod *adds* read like any other. `redfs_stat` reports which
archive actually won.

**MO2 / Vortex:** their VFS only exists inside processes they launch. Running
*inside* a game launched by MO2, the merged mod archives already appear under
`archive/pc/mod` and `REDFS_SCAN_ALL` finds them. Running a tool *outside* MO2,
they are invisible — mount the staging folder explicitly:

```c
redfs_depot_mount_dir(depot, "D:\\mods\\SomeMod", &mounted);
```

## Threading

A depot is immutable once open, so reads, stats and texture calls are safe from
any number of threads. `redfs_depot_mount*` and `redfs_depot_close` are not —
open first, then share.

Reads take milliseconds. Call them off the render thread, or use
`redfs_read_async` (the callback runs on RedFS's worker; marshal back yourself).

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Produces `redfs_static.lib` (link into your own DLL), `RedFS.dll`, and two tools.
C++20, MSVC, Windows x64. No dependencies.

## Verifying

```
.\run-checks.ps1                                    # needs no game install
.\run-checks.ps1 -GameDir "D:\...\Cyberpunk 2077"   # + integration sweep
```

Four configurations, because each catches what the others cannot: **release**
(logic), **debug** (leaks, via the CRT heap), **asan** (out-of-bounds and
use-after-free in every parser), **install** (format correctness against external
oracles).

Unit tests need no game: `tests/fixtures.cpp` builds `.archive` and CR2W
containers byte-exactly from nothing, which also makes it possible to test
malformed input that no real install would ever produce. 31 cases, 243 checks,
under a second.

A deterministic mutation fuzzer hammers both parsers — corrupt archives must
produce errors, never crashes. Under ASan it caught a real out-of-bounds read:
`name()` bounds-checked the string-table *index* but not the *offset*, so a
corrupt name table sent `strcmp` off the end of the buffer. Details and the rest
of the tooling story in [docs/done/testing.md](docs/done/testing.md).

`redfs_cli selftest` mounts the depot, samples 400 files across all 57 archives,
decodes them, and checks sizes against the index.

`redfs_verify` is the real check — it validates against oracles outside RedFS:

- **Texture headers** go to DirectXTex (WolvenKit's `texconv.dll`), which parses
  them independently; its answer must match what RedFS claimed.
- **Texture payloads** are checked by arithmetic: how many bytes a mip chain of
  that exact format and extent *must* occupy, versus the size the archive
  actually stores. Those numbers come from different places, so a wrong DXGI
  format or mip count cannot match by accident.
- **Mesh bounds** are checked against `CMesh.boundingBox` — written by CDPR's
  cooker, never read by the code being tested. The union of computed chunk boxes
  must fall inside it; a wrong stride or quantization scale escapes immediately.

Current results on the reference install:

```
11255 textures checked
  0 header mismatches vs DirectXTex
  0 payload size mismatches
  0 skipped (format not in the reference table)

20000 meshes checked
  0 chunk unions escaping the stored CMesh box
  2 with no computable bounds (geometry absent)
```

That sweep found three real bugs during development: `texture_desc_of` happily
describing a mesh's *embedded* texture instead of refusing; `textureData` being
misdecoded because the type is spelled `serializationDeferredDataBuffer` in
texture resources and `SerializationDeferredDataBuffer` everywhere else; and a
minority of textures whose blob header records the mip-biased extent while the
buffer holds the unbiased surface, which produced a DDS that decoded to garbage.

The bounds check needs a tolerance that scales two ways — one int16 quantization
step is `extent/32767`, and a mesh authored at world coordinate 2800 has ~2.4e-4
between representable floats there. A fixed epsilon flags large meshes for being
large and distant ones for being distant; both were test bugs, not library bugs,
and the code comments say so.

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
- **[docs/API.md](docs/API.md)** — the requested call surface, mapped
- **[docs/done/](docs/done/)** — research and design for what is built: the two
  container formats, path hashing, the texture and mesh pipelines, API
  rationale, caching, and how correctness was established
- **[docs/roadmap.md](docs/roadmap.md)** — what is not built
