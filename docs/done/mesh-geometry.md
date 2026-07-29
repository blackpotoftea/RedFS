# Mesh chunks, and the bounding boxes the format does not store

**Status: implemented** — `src/mesh.cpp`

## The requirement

Given a depot path, answer "which submeshes are the chest" — on any mesh, vanilla
or modded, without knowing anything about the mod. That has to be a **query**, not
a hardcoded region table, or it breaks on the first mesh nobody anticipated.

A geometric query needs geometry: a bounding box per chunk.

## No chunk carries a box

`rendChunk` in full, confirmed against WolvenKit's class definition and against
real data:

```
chunkVertices     rendVertexBufferChunk   { vertexLayout, byteOffsets[5] }
chunkIndices      rendIndexBufferChunk    { pe, teOffset }
numVertices       Uint16
numIndices        Uint32
materialId        array:CName
vertexFactory     Uint8
baseRenderMask    Uint16
mergedRenderMask  Uint16
renderMask        EMeshChunkFlags
lodMask           Uint8
```

Counts, masks, stream offsets. No bounds. The only box anywhere in the format is
`CMesh.boundingBox`, and it covers the whole mesh.

So each box has to be computed from the vertices — which costs one Kraken
decompress of the geometry buffer per mesh, which is why `caching.md` exists.

## Reaching the positions

Every step below was confirmed by dumping a real mesh rather than assumed:

```
blob         = CMesh.renderResourceBlob            -> handle, chunk index
geometry     = blob.renderBuffer                   -> DataBuffer, archive segment
chunks       = blob.header.renderChunkInfos        -> array:rendChunk
scale        = blob.header.quantizationScale       -> Vector4
offset       = blob.header.quantizationOffset      -> Vector4

per chunk:
  pos_offset = chunkVertices.byteOffsets[0]                 static:5,Uint32
  stride     = chunkVertices.vertexLayout.slotStrides[0]    static:8,Uint8  (!)
  count      = numVertices                                  Uint16
```

`slotStrides` is `Uint8`, not `Uint32` — worth stating because the sibling
`byteOffsets` array right next to it *is* `Uint32`, and assuming symmetry silently
reads the wrong stride.

Positions are three `int16` at the start of each vertex, dequantized per axis:

```
p = (i16 / 32767.0f) * scale + offset
```

Checked arithmetically on a live mesh: 1169 vertices at stride 8 from offset 0
occupy 9,352 bytes, and the next chunk's position stream starts at 9,360 —
eight-byte aligned, consistent.

## Coordinate space: game space, Z up

Boxes come out in mesh-local **game space, Z up**. No axis swap.

WolvenKit's exporter emits `(x, z, -y)` because glTF is Y-up
(`MeshTools.ContainRawMesh`). Copying that would have been the easy mistake: these
boxes exist to be compared against **entity and component transforms**, which are
in game space. A Y-up flip would silently misalign every query — "upper front of
the body" would point somewhere else — and the failure would look like bad data
rather than a convention error.

## When there is no box: `bounds_valid`

`compute_bounds` publishes a box only when it can prove one, and
`redfs_mesh_chunk.bounds_valid` reports which happened. It refuses on:

- **A zero vertex count, or a stride below 6.** Three `int16` do not fit.
- **A span running past the geometry buffer** —
  `position_offset + (vertex_count - 1) * stride + 6 > size`. That is what
  truncated or streamed-out geometry looks like.
- **A non-finite quantization scale or offset.** Every dequantized value would be
  NaN, and both `std::min` and `std::max` return their *first* argument when the
  comparison is false — so the `±FLT_MAX` sentinels would survive the sweep
  untouched and the chunk would publish an inverted box marked valid.
- **A non-finite computed result.** The input check above is not sufficient:
  `scale = offset = FLT_MAX` are both finite, yet a vertex at `q = 32767`
  dequantizes to `FLT_MAX + FLT_MAX`, which overflows to `+inf` — and
  `min(FLT_MAX, +inf)` returns `FLT_MAX`, so the sentinel survives again. Only
  validating what was actually computed catches this one.

A geometry buffer that cannot be read at all is handled a level up in
`mesh_build`: the header facts stay, every chunk keeps `bounds_valid = 0`, and the
reason is logged.

When `bounds_valid` is 0 all six floats are zero, which is indistinguishable from
a real chunk of zero extent at the origin — so callers have to test the flag
rather than infer it from the values. About 1 stock mesh in 10,000 is affected.

## The whole-mesh box is stored, not computed

`redfs_mesh_bounds` hands back `CMesh.boundingBox` as found. Being archive content
it can be NaN or infinity, and every comparison a caller writes against NaN is
false in both directions — which turns a chunk filter into a silent no-op rather
than an error. Non-finite components are replaced with 0.

That clamp deliberately lives at the call site rather than in the shared float
helper: the quantization terms are read through the same helper, and
`compute_bounds` *relies* on seeing a non-finite value there so it can skip the
sweep.

## `renderChunkInfos`, not `renderChunks`

`rendRenderMeshBlobHeader` declares both. Cooked meshes populate
**`renderChunkInfos`**; `renderChunks` is absent, so a reader looking only at the
latter silently reports zero chunks. The selftest showed `submeshes=0` on every
mesh, which is what surfaced it.

`mesh_build` reads `renderChunkInfos` and fails with `REDFS_E_UNSUPPORTED` if it
is missing — nothing in a cooked archive lacks it. The cheap header-only path,
`redfs_mesh_desc_of`, accepts either and tries `renderChunkInfos` first, because
`renderChunks` does appear on some uncooked variants.

## LODs

Chunks repeat per detail level: two entries can be the same geometry at different
LODs. `lodMask` is a bitfield; `lod` is reported as the lowest set level, 1-based,
which is the level artists mean. Filter `lod == 1` for one copy of each piece of
geometry — the same filter WolvenKit's exporter applies.

## Materials belong to appearances

`rendChunk.materialId` is an `array:CName`, not the single index the name
suggests. The useful mapping is elsewhere: `meshMeshAppearance.chunkMaterials` is
an `array:CName` **parallel to the chunk list**, so the material for chunk *i*
under appearance *a* is `appearances[a].chunkMaterials[i]`.

That is exactly the shape of the component model — a component picks an appearance
by CName — so the API mirrors it:

```c
int32_t app = redfs_mesh_find_appearance(m, "01_ca_pale");
const char* mat = redfs_mesh_chunk_material(m, app, chunk_index);
```

## Result

`redfs_cli chunks` on the stock player female body, decoded with no mod knowledge:

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

A clean vertical stack, head at index 0 down to feet at index 7. Because a chunk
index *is* a `chunkMask` bit, a z threshold turns directly into a mask value:
`z > 1.2` selects chunks 0 and 1, which is `chunkMask = 3`.

`tools/example_chest.cpp` does the same thing without the hardcoded threshold —
it takes the upper third of whatever the mesh happens to span.

## Verification

`CMesh.boundingBox` is written by CDPR's cooker and **never read by this code
path**, which makes it a genuine oracle: the union of the computed chunk boxes
must fall inside it. A wrong stride, a wrong quantization scale or a misread
offset all push vertices outside it immediately.

```
12000 meshes checked
  0 chunk unions escaping the stored CMesh box
  0 disagreements between redfs_mesh_desc_of and redfs_mesh_open
```

Containment rather than equality, because the stored box legitimately includes
padding and covers morph/LOD range the resident chunks do not. On single-chunk
meshes the computed box reproduces CDPR's **exactly** — the t-shirt used during
development came out `(-0.182 -0.005 0.960) .. (0.183 0.130 1.422)` from both
sources. Full setup in `verification.md`.

### Tolerance is not a fudge factor

Two apparent failures during verification were both *test* bugs, and both printed
computed and stored boxes that were identical to three decimals — which is what
identified them as tolerance rather than correctness:

- **Quantization.** Positions are `int16` over the axis extent, so one step is
  `extent/32767` — 12 mm on a 394-unit mesh. A computed bound can legitimately sit
  half a step outside the cooker's rounding. A fixed epsilon flags large meshes
  for being large.
- **Float resolution.** A mesh authored at world coordinate 2800 has ~2.4e-4
  between representable floats there, so dequantize-then-add lands a few ULPs from
  whatever the cooker computed. A fixed epsilon flags distant meshes for being
  distant.

The epsilon is now `max(1e-4, extent/32767, |coord| * 1e-6)` per axis, with the
reasoning in the code so nobody later reads it as an arbitrary constant.

## Not implemented

Only positions are decoded. Normals, tangents, UVs, colours, bone indices and
weights are all present in the render buffer and reachable through
`vertexLayout.elements` — see `../vertex-streams.md` for the design.
