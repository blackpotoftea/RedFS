# Full vertex stream decoding

**Status: NOT implemented.** Research complete, design below.

RedFS decodes vertex **positions** only, because that is what bounding boxes
need. Normals, tangents, UVs, colours, bone indices and weights are all sitting in
the same buffer, already read and decompressed, and simply not interpreted.

## What is already known

Positions work (see `done/mesh-geometry.md`): the geometry buffer is reached, the
chunk table is parsed, and the stride and offset for slot 0 are read correctly.
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

So the decode is table-driven, not hardcoded: walk `elements`, and for each one
the stream it lives in is `byteOffsets[element.streamIndex]` with stride
`slotStrides[element.streamIndex]`.

That is the piece already proven in the position path — RedFS just always uses
slot 0 today.

A real dump, from a terrain mesh chunk:

```
vertexLayout   GpuWrapApiVertexLayoutDesc
  elements     static:32,GpuWrapApiVertexPackingPackingElement  [5 items]
  slotStrides  static:8,Uint8                                   [8 items]
    [0] 8      <- positions: 4 x int16
    [1] 0
    ...
  slotMask     129
  hash         508862805
byteOffsets    static:5,Uint32
  [0] 9360     <- this chunk's position stream
  [1..4] 0
```

Note `slotStrides` is `Uint8` while the adjacent `byteOffsets` is `Uint32`.

## What WolvenKit does

`MeshTools.GetMeshesinfo` maps usages to offsets:

```csharp
foreach (var element in cv.VertexLayout.Elements) {
    if (element.Usage == PS_Normal)   normalOffsets[i]  = cv.ByteOffsets[element.StreamIndex];
    if (element.Usage == PS_Tangent)  tangentOffsets[i] = cv.ByteOffsets[element.StreamIndex];
    if (element.Usage == PS_Color)    colorOffsets[i]   = cv.ByteOffsets[element.StreamIndex];
    if (element.Usage == PS_TexCoord) /* tex0 then tex1 */
}
```

and the encodings it then applies:

| attribute | encoding |
|---|---|
| position | 3 × `int16`, `/32767 * quantScale + quantOffset`, stride from slot 0 |
| UV0 / UV1 | 2 × half float, stride 4 |
| normal / tangent | packed into `u32` (`Converters.TenBitShifted` style) |
| colour | 4 × `u8` / 255 |
| bone indices | `u8` each, at `posOffset + i*stride + 8` |
| bone weights | `f32` each, at `posOffset + i*stride + 8 + weightCount` |

Bone data sharing the position stride is worth noting — it is not a separate
slot; it sits after the position within the same vertex.

## Design

Keep it table-driven and let the caller pull only what it wants. Fabricating a
fat vertex struct would force everyone to pay for attributes they do not need.

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

**Indices are the easier half and are already located.**
`chunkIndices.teOffset` plus `header.indexBufferOffset` gives the start;
`chunkIndices.pe` gives the width (`IBCT_IndexUShort` seen in the wild).
`redfs_mesh_read_indices` could ship independently of the attribute work and
would already enable custom rendering and collision.

**The caller supplies the buffer.** Vertex data is large and callers usually have
somewhere specific for it to go.

**Positions stay where they are.** The existing bbox path must not start
depending on this; it is deliberately minimal so the cache stays cheap.

**Nothing new is read from disk** when the mesh is already open — the geometry
buffer has been decompressed. Though note the cache stores only *derived* results,
so a cached mesh would need a re-read to serve attributes. Either accept that or
let attribute reads bypass the cache.

## Effort

Half a day for indices. Two to three days for full attributes, most of it in the
packed normal/tangent formats, which are the fiddly part and where WolvenKit's
`Converters` is the reference.

## Whether it is worth doing

Unclear, and worth saying so. A mod that wants to *render* game geometry usually
wants the game to render it. The concrete uses are custom collision, procedural
placement, and analysis — none of which have been asked for.

`redfs_mesh_read_indices` alone is cheap and plausibly useful. The rest should
wait for a real caller.
