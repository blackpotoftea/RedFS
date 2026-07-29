# Full vertex stream decoding

**Status: NOT implemented.** Nothing below exists in the library. Research is
complete and the design is settled enough to build from; the API sketches are
proposals, not declarations you can call.

RedFS decodes vertex **positions** only, because that is what bounding boxes need.
Normals, tangents, UVs, colours, bone indices and weights all sit in the same
render buffer that the bounding-box sweep already decompresses, and are simply
never interpreted.

## What is already known

Positions work (see `done/mesh-geometry.md`): the geometry buffer is reached, the
chunk table is parsed, and the offset and stride for slot 0 are read correctly.
Everything below is about the *other* slots.

## The layout descriptor

`chunkVertices.vertexLayout` is a `GpuWrapApiVertexLayoutDesc`:

```
elements      static:32,GpuWrapApiVertexPackingPackingElement
slotStrides   static:8,Uint8
slotMask      Uint32
hash          Uint32
```

Each element:

```
type          GpuWrapApiVertexPackingePackingType     -- how it is encoded
usage         GpuWrapApiVertexPackingePackingUsage    -- what it means
usageIndex    Uint8                                   -- UV0 vs UV1
streamIndex   Uint8                                   -- which slot
streamType    GpuWrapApiVertexPackingEStreamType
```

So a decode can be table-driven rather than hardcoded: walk `elements`, and the
stream an element lives in starts at `byteOffsets[element.streamIndex]`. That half
is proven — it is what WolvenKit does, and RedFS's position path is its degenerate
case, `byteOffsets[0]`.

The stride half is **not** established. See the open question below.

Two things about those two arrays. `slotStrides` is `Uint8` while `byteOffsets` is
`Uint32`, so a reader that assumes symmetry gets the wrong stride. And they
disagree about how many slots exist — `slotStrides` holds 8 entries, `byteOffsets`
only 5 — so `streamIndex` has to be bounds-checked against 5, not 8, before it
indexes an offset. WolvenKit indexes `ByteOffsets[StreamIndex]` unchecked and would
throw on a file naming a higher slot.

A real dump, from a terrain mesh chunk:

```
vertexLayout   GpuWrapApiVertexLayoutDesc
  elements     static:32,GpuWrapApiVertexPackingPackingElement  [5 items]
  slotStrides  static:8,Uint8                                   [8 items]
    [0] 8      <- position: 3 x int16 read, the remaining 2 bytes are not
    [1] 0
    ...
  slotMask     129
  hash         508862805
byteOffsets    static:5,Uint32
  [0] 9360     <- this chunk's position stream
  [1..4] 0
```

## What WolvenKit does

`MeshTools.GetMeshesinfo` maps usages to stream offsets:

```csharp
foreach (var element in cv.VertexLayout.Elements) {
    if (element.Usage == PS_Normal)   normalOffsets[i]  = cv.ByteOffsets[element.StreamIndex];
    if (element.Usage == PS_Tangent)  tangentOffsets[i] = cv.ByteOffsets[element.StreamIndex];
    if (element.Usage == PS_Color)    colorOffsets[i]   = cv.ByteOffsets[element.StreamIndex];
    if (element.Usage == PS_TexCoord) /* tex0 then tex1 */
}
```

and `MeshTools.ContainRawMesh` then applies these encodings:

| attribute | encoding | where |
|---|---|---|
| position | 3 × `int16`, `/32767 * quantScale + quantOffset` | slot 0, stride from `slotStrides[0]` |
| UV0 | 2 × half float | its own slot, stride 4 |
| normal | `u32`, 10 bits per axis (`Converters.TenBitShifted`) | shares a slot with tangent |
| tangent | `u32`, same packing; top 2 bits are the sign | same slot, at +4 |
| colour | 4 × `u8` / 255 | shares a slot with UV1 |
| UV1 | 2 × half float | same slot, at +4 |
| bone indices | `u8` each | `posOffset + i*stride + 8` |
| bone weights | `u8` each, `/255`, then renormalised to sum to 1 | `posOffset + i*stride + 8 + weightCount` |
| garment morph | 3 × half float | `posOffset + i*stride + 8 + 2*weightCount` |

Two things in that table are easy to get wrong. **Bone weights are bytes, not
floats** — WolvenKit reads `byte / 255f` and then divides each vertex's weights by
their sum, substituting `(bone 0, weight 1)` when they sum to zero. And **bone data
shares the position stride**: it is not a separate slot but a fixed +8 into the
same vertex, past the position's 6 bytes and the 2 nobody reads.

How many weights a vertex has is not declared directly either. WolvenKit counts
the `PS_SkinIndices` elements in the layout and multiplies by 4.

## The open question: where non-position strides come from

WolvenKit does **not** read `slotStrides` for anything but slot 0. It derives the
others from which attributes are present — stride 4 for a lone normal, 8 when a
tangent shares the slot, likewise for colour and UV1 — and hardcodes the +4 offset
of the second member. That is a layout assumption, not a value read from the file.

Whether `slotStrides` is populated above index 0 in cooked meshes is unverified.
In the one layout dumped so far `slotStrides[1]` is 0 while `slotMask` is 129
(slots 0 and 7 in use), so `slotStrides[7]` was never examined. A table-driven
decode that trusts `slotStrides[element.streamIndex]` would read a stride of 0 if
the field is genuinely unset.

**Resolve this first.** The two candidates are: honour `slotStrides` when non-zero
and otherwise sum element sizes derived from `element.type`; or reproduce
WolvenKit's presence-based rules. The first is more principled, the second is known
to work on shipping content. Dumping the layouts of a few hundred meshes decides
it, and that is cheap.

## Design

Keep it table-driven and let the caller pull only what it wants. Fabricating a fat
vertex struct would force everyone to pay for attributes they do not need.

```c
typedef enum redfs_vertex_usage {
    REDFS_VTX_POSITION = 0, REDFS_VTX_NORMAL, REDFS_VTX_TANGENT,
    REDFS_VTX_TEXCOORD, REDFS_VTX_COLOR, REDFS_VTX_BONE_INDEX,
    REDFS_VTX_BONE_WEIGHT
} redfs_vertex_usage;

typedef struct redfs_vertex_stream {
    redfs_vertex_usage usage;
    uint32_t usage_index;      /* UV0 vs UV1 */
    uint32_t offset;           /* into the render buffer */
    uint32_t stride;
    uint32_t element_size;
    uint32_t packing;          /* raw ePackingType, for callers that care */
} redfs_vertex_stream;

/* Describe, without decoding. */
REDFS_API uint32_t redfs_mesh_stream_count(const redfs_mesh*, uint32_t chunk);
REDFS_API const redfs_vertex_stream* redfs_mesh_stream_at(const redfs_mesh*,
                                                          uint32_t chunk, uint32_t i);

/* Decode one attribute of one chunk into caller memory, normalised to float. */
REDFS_API redfs_status redfs_mesh_read_attribute(const redfs_depot*, const redfs_mesh*,
                                                 uint32_t chunk, redfs_vertex_usage usage,
                                                 uint32_t usage_index,
                                                 float* dst, uint32_t components,
                                                 uint32_t capacity_vertices);

/* Indices, widened to u32 regardless of source width. */
REDFS_API redfs_status redfs_mesh_read_indices(const redfs_depot*, const redfs_mesh*,
                                               uint32_t chunk, uint32_t* dst, uint32_t capacity);
```

### Notes

**Indices are the easier half.** The start is `header.indexBufferOffset` plus
`chunkIndices.teOffset`, and `chunkIndices.pe` gives the width
(`IBCT_IndexUShort` = 1, `IBCT_IndexUInt` = 0). `redfs_mesh_desc_of` already
exposes `index_buffer_offset` and `index_buffer_size`; the per-chunk `teOffset` and
`pe` are not read today, since the box sweep has no use for them. Worth honouring
`pe` rather than following WolvenKit here — its exporter reads `uint16`
unconditionally, which is right for everything sampled but wrong by construction
for a `IBCT_IndexUInt` chunk.

`redfs_mesh_read_indices` could ship independently of the attribute work and would
already enable custom collision and geometry analysis.

**The caller supplies the buffer.** Vertex data is large and callers usually have
somewhere specific for it to go.

**Positions stay where they are.** The existing bbox path must not start depending
on this; it is deliberately minimal so the cache stays cheap.

**Every attribute read costs a fresh decompress.** `mesh_build` frees the geometry
buffer as soon as the sweep is done, and neither `MeshData` nor the on-disk cache
retains it — the boxes are all that is kept. So an attribute read after
`redfs_mesh_open` re-reads and re-decompresses the render buffer, cache hit or not.
The alternatives are to accept that cost per call, or to fold attribute decoding
into `mesh_build` while the buffer is still live and let the caller say up front
what it wants. Retaining the buffer on `MeshData` is not an option worth
considering: it is the largest part of a mesh, and the cache exists precisely to
avoid holding it.

## Effort

Half a day for indices. Two to three days for full attributes, most of it in the
packed normal/tangent formats. `Converters.TenBitShifted` is the reference:
`x = bits 0..9`, `y = 10..19`, `z = 20..29`, each dequantized as
`(v * 2 / 1023) - 1`, with bits 30..31 giving the tangent sign — mapped there as
`0 -> +1`, `3 -> -1`, anything else `0`, over a comment conceding the mapping is
guesswork that only has to hold for normals. That is the part to verify against
rendered output rather than to transcribe.

## Whether it is worth doing

Unclear, and worth saying so. A mod that wants to *render* game geometry usually
wants the game to render it. The concrete uses are custom collision, procedural
placement, and analysis — none of which have been asked for.

`redfs_mesh_read_indices` alone is cheap and plausibly useful. The rest should wait
for a real caller.
