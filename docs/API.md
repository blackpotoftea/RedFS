# Your five calls, mapped

The spec you gave, against what RedFS actually provides. Terms are yours.

| # | you asked for | status |
|---|---|---|
| 1 | `hashToPath(hashDecimalStr) -> pathStr` | done, dictionary-backed |
| 2 | `pathToHash(pathStr) -> hashDecimalStr` | done, pure function |
| 3 | `resourceExists(pathStr) -> bool` | done |
| 4 | `meshInfo(pathStr) -> per-mesh + per-chunk` | done, bbox computed |
| 5 | `entityComponents(entityHandle) -> rows` | **out of scope — see below** |

---

## 1. `hashToPath`

```c
redfs_path_load(depot, "usedhashes.kark", &kept);   /* once, at startup */
const char* path = redfs_path_from_hash(redfs_hash_parse(hash_decimal_str));
```

You were right that the string is gone. FNV1a is one-way and archives carry no
path table, so this cannot be computed — only looked up. RedFS fills the
dictionary from three places:

- **CR2W import tables**, learned automatically as files are read. Free, and the
  only source that knows paths a mod invented rather than shipped.
- **A path list on disk** — WolvenKit's `usedhashes.kark` (Kraken-compressed) or
  plain text, one path per line. This is the bulk source.
- **`redfs_path_add`** for anything you know yourself.

The **path list** is filtered: only entries that resolve in the mounted depot are
retained. Measured on the reference install: **544,496 of 544,670 files —
99.97 % coverage**, loaded in well under a second.

The other two sources are not filtered and cannot be — import learning happens
inside CR2W parsing, which has no depot, and `redfs_path_add` takes only a
string. So a hit tells you what a file is *called*, not that it is readable.
Check `redfs_exists` if you need that, or just handle `REDFS_E_NOT_FOUND`.

Without a list loaded, this still works for anything reachable through an import
table, which grows as you read. It just will not be exhaustive.

## 2. `pathToHash`

```c
char key[REDFS_HASH_STRING_MAX];              /* 21 bytes is always enough */
redfs_hash_string("Base/Icon/Foo.XBM", key, sizeof key);   /* decimal string */
uint64_t h = redfs_hash_parse(key);
```

Normalisation happens first, exactly as the engine does it: trim quotes, slashes
and space at both ends, collapse separator runs, `/` → `\`, ASCII lowercase. So
`Base/Icon/Foo.XBM` and `base\icon\foo.xbm` produce the same key.

The decimal-string forms exist for your Lua constraint — doubles lose precision
above 2⁵³, so a 64-bit key has to cross that boundary as text.

## 3. `resourceExists`

```c
int ok = redfs_exists(depot, redfs_hash(path));
```

A binary search over the sorted index; no I/O. Answers for the depot as actually
mounted, mod overrides included, which is the point — a path that resolves in one
setup and not another stops being a silent nothing.

`redfs_stat` gives you the rest: size, compressed size, buffer count, which
archive won, SHA1, timestamp.

## 4. `meshInfo`

```c
redfs_mesh* m;
redfs_mesh_open(depot, redfs_hash(path), &m);

redfs_mesh_chunk_count(m);        /* chunkCount   */
redfs_mesh_lod_count(m);          /* lodCount     */
redfs_mesh_appearance_name(m, i); /* appearanceNames[] */

const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
/*  c->index          the bit in chunkMask
    c->lod            which detail level
    c->vertex_count   (you said take it if free -- it was)
    c->index_count
    c->bbox_min[3]    the one you actually need
    c->bbox_max[3]
    c->bounds_valid   0 when no box could be computed -- check it, because
                      the boxes are then all-zero, which reads exactly like a
                      real chunk at the origin (~1 stock mesh in 10,000)  */

redfs_mesh_chunk_material(m, appearance, c->index);   /* materialName */
```

### The bounding box

**Nothing in the format stores one.** `rendChunk` carries vertex and index
counts, a `lodMask`, a `materialId` and stream offsets — no bounds. Only `CMesh`
has a box and it covers the whole mesh. So RedFS computes it, per chunk, by
dequantizing positions out of the geometry buffer:

```
positions at renderBuffer[ chunkVertices.byteOffsets[0]
                           + i * vertexLayout.slotStrides[0] ]
as three int16, then
    p = i16 / 32767 * header.quantizationScale + header.quantizationOffset
```

Boxes are in mesh-local **game space (Z up)** — not the Y-up flip glTF exporters
apply — so they line up with the component transforms you are comparing against.

Verified against `CMesh.boundingBox`, which CDPR's cooker wrote and this code
path never reads: across 1500 meshes, **zero chunk unions escaped the stored
box.** On single-chunk meshes the computed box reproduces CDPR's exactly.

### `materialName`

Materials are per *appearance*, because that is what a component selects by
CName. `meshMeshAppearance.chunkMaterials` is an array parallel to the chunk
list, so `chunk_material(appearance, chunk_index)` is a direct index — the same
shape as your component model.

### Cost

`redfs_mesh_open` decompresses the geometry buffer: ~1–2 ms for a body mesh.
Archives never change, so turn on the cache and pay it once, ever:

```c
redfs_cache_open(depot, "redfs_mesh.cache");   /* survives restarts */
redfs_cache_warm(depot, hashes, count, &n);    /* or precompute a known set */
```

Measured: 0.72 ms cold → **0.00 ms warm**, including across a process restart.
The cache fingerprints the mounted archive set and discards itself if that set
moves, so a patch or a new mod cannot serve stale geometry.

Precomputing the entire depot at load is not viable — ~10⁵ meshes would cost
minutes of startup and gigabytes. Lazy population reaches the same end state for
the meshes you actually touch.

### LODs

Chunks repeat per detail level, as you noted: index 3 and index 14 can be the
same geometry. `c->lod` is the lowest level that chunk belongs to (1-based);
`c->lod_mask` is the raw bitfield. Filter on `lod == 1` for one copy.

## 5. `entityComponents` — not this library

This one needs live game state: an entity handle, its component list, each
component's `chunkMask` and `meshAppearance`. That is RTTI on a running process,
not archive data, and RedFS deliberately touches neither — which is what keeps it
immune to game patches.

It belongs in your own RED4ext plugin. RedFS supplies the other half: take the
resource hash off a live component, and

```c
redfs_path_from_hash(component_mesh_hash)   /* -> a readable path       */
redfs_mesh_open(depot, component_mesh_hash) /* -> chunk table and boxes */
```

turns an opaque number into the chunk geometry you can actually query. Pair that
with the component's `chunkMask` and the join is complete.

---

## Worked end to end

`tools/example_chest.cpp`, run against the stock player body — no mod knowledge,
no hardcoded region numbers:

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
