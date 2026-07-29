# Mesh chunks and the bounding boxes that do not exist

**Status: implemented** — `src/mesh.cpp`

The headline feature, and the one that required real work rather than
transcription.

## The requirement

Given a depot path, answer "which submeshes are the chest" — on any mesh, vanilla
or modded, without knowing anything about the mod. That has to be a **query**,
not a hardcoded region table, or it breaks on the first mesh nobody anticipated.

A geometric query needs geometry. Specifically, a bounding box per chunk.

## The finding

**No bounding box is stored per chunk. Anywhere.**

`rendChunk` was inspected field by field on real data:

```
chunkVertices   rendVertexBufferChunk    { vertexLayout, byteOffsets[5] }
chunkIndices    rendIndexBufferChunk     { pe, teOffset }
numVertices     Uint16
numIndices      Uint32
vertexFactory   Uint8
renderMask      EMeshChunkFlags
lodMask         Uint8
```

Counts, a LOD mask, stream offsets. No bounds. The only box in the format is
`CMesh.boundingBox`, and it covers the whole mesh.

So the box has to be computed from the vertices. Which is why this costs a
geometry decompress, which is why `caching.md` exists.

## Reaching the positions

Piece by piece, all confirmed by dumping a real mesh rather than assumed:

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
`byteOffsets` array right next to it *is* `Uint32`, and assuming symmetry would
silently read the wrong stride.

Positions are three `int16` at the start of each vertex, dequantized:

```
p.x = i16_x / 32767.0f * scale.X + offset.X
p.y = i16_y / 32767.0f * scale.Y + offset.Y
p.z = i16_z / 32767.0f * scale.Z + offset.Z
```

Verified arithmetically on a live mesh: chunk 0 has 1169 vertices at stride 8
starting at offset 0 → 9,352 bytes, and chunk 1 starts at 9,360. Eight-byte
aligned, consistent.

## Coordinate space

Boxes come out in mesh-local **game space, Z up**. No axis swap.

WolvenKit's exporter emits `(x, z, -y)` because glTF is Y-up. Copying that would
have been the easy mistake: these boxes exist to be compared against **entity and
component transforms**, which are in game space. A Y-up flip would silently
misalign every query — "upper front of the body" would point somewhere else — and
the failure would look like bad data rather than a convention error.

## `renderChunkInfos`, not `renderChunks`

`rendRenderMeshBlobHeader` declares both. Cooked meshes populate
**`renderChunkInfos`**; `renderChunks` is absent, so it silently returned zero.
The selftest showed `submeshes=0` on every mesh, which is what surfaced it.
Both are now tried, `renderChunkInfos` first.

## LODs

Chunks repeat per detail level — index 3 and index 14 can be the same geometry at
different LODs, as you noted. `lodMask` is a bitfield; `lod` is reported as the
lowest set level, 1-based, which is the level artists mean. Filter `lod == 1` for
one copy of each piece of geometry.

## Materials belong to appearances

`rendChunk.materialId` is an `array:CName`, not the simple index the name
suggests. The useful mapping is elsewhere:

`meshMeshAppearance.chunkMaterials` is an `array:CName` **parallel to the chunk
list**. So the material for chunk *i* under appearance *a* is
`appearances[a].chunkMaterials[i]`.

That is exactly the shape of the component model — a component picks an
appearance by CName — so the API mirrors it:

```c
int32_t app = redfs_mesh_find_appearance(m, "01_ca_pale");
const char* mat = redfs_mesh_chunk_material(m, app, chunk_index);
```

## Result

The stock player female body, decoded with no mod knowledge:

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

A clean vertical stack, feet to head. Chest is `z > 1.2`, and since a chunk index
*is* a `chunkMask` bit, the answer is directly a mask value:

```
chunkMask selecting the chest chunks: 3
```

## Verification

`CMesh.boundingBox` is written by CDPR's cooker and **never read by this code
path**, which makes it a genuine oracle: the union of computed chunk boxes must
fall inside it.

```
20000 meshes checked
  0 chunk unions escaping the stored CMesh box
  2 with no computable bounds (geometry absent)
```

On single-chunk meshes the computed box reproduces CDPR's **exactly** — the
t-shirt used during development came out
`(-0.182 -0.005 0.960) .. (0.183 0.130 1.422)` from both sources.

Containment rather than equality, because the stored box legitimately includes
padding and covers morph/LOD range the resident chunks do not.

### Tolerance is not a fudge factor

Two apparent failures during verification were both *test* bugs, and the fix is
principled rather than "loosen until green":

- **Quantization.** Positions are `int16` over the axis extent, so one step is
  `extent/32767` — 12 mm on a 394-unit mesh. A computed bound can legitimately
  sit half a step outside the cooker's rounding. A fixed epsilon flags large
  meshes for being large.
- **Float resolution.** A mesh authored at world coordinate 2800 has ~2.4e-4
  between representable floats there. The dequantize-then-add lands a few ULPs
  from whatever the cooker computed. A fixed epsilon flags distant meshes for
  being distant.

Both cases printed computed and stored boxes that were *identical to three
decimals*, which is what identified them as tolerance rather than correctness.
The epsilon is now `max(1e-4, extent/32767, |coord| * 1e-6)` per axis, with the
reasoning in the code.

## Not implemented

Only positions are decoded. Normals, tangents, UVs, colours, bone indices and
weights are all present in the buffer and reachable through
`vertexLayout.elements` — see `../vertex-streams.md` for the design.
