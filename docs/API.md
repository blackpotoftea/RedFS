# Your five calls, mapped

The spec you gave, against what RedFS actually provides. Terms are yours.
Signatures, error codes and threading rules live in
[`include/redfs.h`](../include/redfs.h) and are not repeated here; this page says
which call answers which question, and what the answer is worth.

| # | you asked for | status |
|---|---|---|
| 1 | `hashToPath(hashDecimalStr) -> pathStr` | done, dictionary-backed |
| 2 | `pathToHash(pathStr) -> hashDecimalStr` | done, pure function |
| 3 | `resourceExists(pathStr) -> bool` | done |
| 4 | `meshInfo(pathStr) -> per-mesh + per-chunk` | done; chunk boxes computed |
| 5 | `entityComponents(entityHandle) -> rows` | **out of scope — see below** |

Check `redfs_abi_version() == REDFS_ABI_VERSION` once at startup. `redfs_mesh_chunk`
gained a field at ABI 2 and a mismatch fails silently rather than loudly: the
fields still read correctly, only the array *stride* is wrong, so everything from
chunk 1 on is plausible garbage. `redfs::Depot::open` checks it for you.

---

## 1. `hashToPath`

```c
redfs_path_load(depot, "usedhashes.kark", &kept);   /* once, at startup */
const char* path = redfs_path_from_hash(redfs_hash_parse(hash_decimal_str));
```

You were right that the string is gone. FNV1a is one-way and archives carry no
path table, so this cannot be computed — only looked up. Three sources fill the
dictionary:

- **CR2W import tables**, learned as files are read. Free, and the only source
  that knows paths a mod invented rather than shipped.
- **A path list on disk** — WolvenKit's `usedhashes.kark` (Kraken-compressed) or
  plain text, one path per line. This is the bulk source.
- **`redfs_path_add`**, for anything you know yourself.

**Only the list is filtered** against the mounted depot: a line whose hash does
not resolve is dropped, and `out_kept` counts the survivors. On the reference
install that leaves **544,496 of 544,670 files — 99.97 % coverage.**

The other two sources cannot be filtered. Import learning runs inside CR2W
parsing, which has no depot to check against; `redfs_path_add` receives only a
string. So a hit tells you what a file is *called*, not that it is readable —
call `redfs_exists` if you need that, or just handle `REDFS_E_NOT_FOUND` from the
read.

**Import learning stays off until the dictionary is switched on**, by
`redfs_path_load`, `redfs_path_enable` or `redfs_path_cache_open`. Ship no list
and you must call one of them yourself, or nothing is ever learned. With it,
coverage grows as you read rather than being exhaustive up front.

Growing it that way costs minutes on a modded install, because it means reading
every file with an import table. `redfs_path_cache_open(file)` persists
the result and restores it next run, and tracks which archives it already read so
that installing a mod costs harvesting *that mod*. Unlike the mesh cache it is
never discarded — a hash → name mapping cannot go stale — so restored entries
keep the unfiltered semantics above. See
[done/path-cache.md](done/path-cache.md).

The full list costs ~135 MB transient while it decompresses and **~40 MB
resident** afterwards. Strings are interned, so a pointer from
`redfs_path_from_hash` is valid for the lifetime of the process and no later
addition can invalidate it; NULL means "not in the dictionary".

## 2. `pathToHash`

```c
char key[REDFS_HASH_STRING_MAX];              /* 21 bytes is always enough */
redfs_hash_string("Base/Icon/Foo.XBM", key, sizeof key);   /* decimal string */
uint64_t h = redfs_hash_parse(key);
```

Normalisation happens first, exactly as the engine does it: trim quotes, slashes
and space at both ends, collapse separator runs, `/` → `\`, ASCII lowercase. So
`Base/Icon/Foo.XBM` and `base\icon\foo.xbm` produce the same key. Use
`redfs_hash` for the `uint64_t` directly when nothing forces you through a string.

The decimal forms exist for your Lua constraint: doubles lose precision above
2⁵³, so a 64-bit key has to cross that boundary as text.

## 3. `resourceExists`

```c
int ok = redfs_exists(depot, redfs_hash(path));
```

A binary search over the sorted index; no I/O. It answers for the depot *as
mounted*, mod overrides included, which is the point — a path that resolves in
one setup and not another stops being a silent nothing.

`redfs_stat` gives you the rest at the same cost: sizes, buffer count, which
archive won, SHA1, timestamp.

## 4. `meshInfo`

```c
redfs_mesh* m;
redfs_mesh_open(depot, redfs_hash(path), &m);

redfs_mesh_chunk_count(m);
redfs_mesh_lod_count(m);
redfs_mesh_appearance_name(m, i);

const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
/* c->index is the bit in a component's chunkMask; plus lod, lod_mask,
   vertex_count, index_count, bbox_min/max and bounds_valid. Field notes are in
   redfs.h. */

redfs_mesh_chunk_material(m, appearance, c->index);
redfs_mesh_close(m);
```

### Two boxes, and only one of them is computed

**Per chunk: computed.** Nothing in the format stores one — `rendChunk` carries
vertex and index counts, a `lodMask`, a `materialId` and stream offsets, no
bounds. So RedFS dequantizes positions out of the geometry buffer:

```
positions at renderBuffer[ chunkVertices.byteOffsets[0]
                           + i * vertexLayout.slotStrides[0] ]
as three int16, then
    p = i16 / 32767 * header.quantizationScale + header.quantizationOffset
```

Boxes come out in mesh-local **game space (Z up)** — not the Y-up flip glTF
exporters apply — so they line up with the component transforms you are comparing
against.

`bounds_valid == 0` means no box could be computed: the geometry buffer was
absent, streamed out, or failed its span check. The boxes are all-zero in that
case, which reads exactly like a real chunk sitting at the origin, so test the
flag before treating a box as a fact. Rare but real — 2 of the 20,000 meshes in
the verification sweep.

**Whole mesh: stored.** `redfs_mesh_bounds` hands back `CMesh.boundingBox` as the
cooker wrote it, with a non-finite value replaced by 0 and nothing else checked.
It covers the entire mesh including padding and morph/LOD range, so it cannot
answer "which chunks are the chest" — use the chunk boxes for that. It is useful
as a frame of reference: the example below derives its own threshold from it.

That difference is what makes the stored box a genuine oracle — this code path
never reads it. Across 20,000 meshes, **zero chunk unions escaped it**, and on
single-chunk meshes the computed box reproduces CDPR's exactly.

### Counts are walked, not declared

`redfs_mesh_chunk_count` reports the chunks actually decoded out of
`rendRenderMeshBlob.header.renderChunkInfos` — not the length the array header
claims, so a truncated file cannot hand you a four-billion loop bound.
`redfs_mesh_desc_of`'s `submesh_count` is that same walked count, and it costs
only the CR2W read: use it when you want header facts without the geometry pass.
`redfs_mesh_lod_count` is derived the same way, from `header.renderLODs` and the
chunks' own LOD levels.

### `materialName`

Materials are per *appearance*, because that is what a component selects by
CName. Appearance names come from `CMesh.appearances`, and each appearance's
`chunkMaterials` is an array parallel to the chunk list — so
`redfs_mesh_chunk_material(m, appearance, chunk)` is a direct index, the same
shape as your component model. `""` when that appearance names no material for
that chunk. `redfs_mesh_find_appearance` turns the CName into the index.

### Handles and cost

`redfs_mesh_open` has to decompress the geometry buffer to compute the boxes.
Archives never change, so turn on the cache and pay it once, ever:

```c
redfs_cache_open(depot, "redfs_mesh.cache");   /* survives restarts */
redfs_cache_warm(depot, hashes, count, &n);    /* or precompute a known set */
```

Measured **0.72 ms cold → 0.00 ms warm**, and the warm path holds across a
process restart. The cache fingerprints the mounted archive set and discards
itself if that set moves, so a patch or a new mod cannot serve stale geometry.

There is one cache per process and it belongs to the depot passed to
`redfs_cache_open`. `redfs_mesh_open` on any other depot bypasses it rather than
risk a cross-depot answer, and `redfs_cache_warm` without a cache on that depot
returns `REDFS_E_INVALID_ARG` instead of computing results it would immediately
discard.

The pointer from `redfs_mesh_chunk_at` is owned by the mesh and valid until you
call `redfs_mesh_close` on that handle. Closing is always correct and never
premature: the handle holds its own reference, so it survives
`redfs_cache_close`.

Precomputing the whole depot at load is not viable — on the order of 10⁵ `.mesh`
files would cost minutes of startup and gigabytes of results. Lazy population
reaches the same end state for the meshes you actually touch.

### LODs

Chunks repeat per detail level, as you noted: index 3 and index 14 can be the
same geometry. `c->lod` is the lowest level that chunk belongs to (1-based);
`c->lod_mask` is the raw bitfield. Filter on `lod == 1` for one copy.

## 5. `entityComponents` — not this library

This one needs live game state: an entity handle, its component list, each
component's `chunkMask` and `meshAppearance`. That is RTTI on a running process
rather than archive data, and RedFS reads neither RTTI nor process memory — no
hooks, no address-library offsets, which is exactly what stops a game patch from
breaking it.

It belongs in your own RED4ext plugin. RedFS supplies the other half: take the
resource hash off a live component, and

```c
redfs_path_from_hash(component_mesh_hash);       /* -> what it is called    */
redfs_mesh_open(depot, component_mesh_hash, &m); /* -> chunk table + boxes  */
```

turn an opaque number into geometry you can query. Pair that with the component's
own `chunkMask` and the join is complete.

---

## Worked end to end

`tools/example_chest.cpp`, run against the stock player body — no mod knowledge,
no hardcoded region numbers. "Chest" is posed as a query: LOD 1 only, and the top
third of whatever z range this particular mesh reports. Abridged output:

```
mesh spans z -0.117..1.653; treating z > 1.051 as upper body
chunk  lod   verts     material                 z range
0      1     1011      01_ca_pale               1.226 .. 1.509   <-- chest
1      1     257       01_ca_pale               1.421 .. 1.531   <-- chest
2      1     367       01_ca_pale               1.045 .. 1.270
3      1     514       01_ca_pale               0.917 .. 1.109
4      1     988       01_ca_pale               0.510 .. 0.980
5      1     260       01_ca_pale               0.277 .. 0.528
6      1     396       01_ca_pale               0.074 .. 0.286
7      1     848       01_ca_pale               0.005 .. 0.106

chunkMask selecting those chunks: 3
```

The chunks come out as a clean vertical stack, feet to head, and the answer is a
`chunkMask` value you can hand straight to a component.
