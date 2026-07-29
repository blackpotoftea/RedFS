# CR2W, and reading it without the type system

**Status: implemented** — `src/cr2w.cpp`

CR2W is the container every cooked RED4 resource lives in. The obvious reading of
the problem is that you need the game's RTTI to parse it — thousands of generated
classes, which is exactly what WolvenKit carries and what makes it a 200 MB tool
rather than a library.

That reading is wrong, and this is the key insight the whole library rests on.

## The insight

**Every property in a CR2W carries its own name and its own RED type name**, as
indices into the file's own string table. The serialization is self-describing.
So a reader that knows *zero* classes can still:

- walk the entire object graph
- pull out any field by name
- report what type that field is, and hand back its bytes

You lose the ability to *construct* typed objects. You keep the ability to
*query*, which is all a mod needs to find a texture's dimensions or a mesh's
geometry buffer.

This is why RedFS is ~5,000 lines instead of a port of WolvenKit.

## Header

```
0x00   4    magic 'CR2W' (0x57325243)
0x04   4    version        163..195 supported
0x08   4    flags
0x0C   8    timestamp
0x14   4    build_version
0x18   4    objects_end
0x1C   4    buffers_end
0x20   4    crc32
0x24   4    num_chunks
0x28  120   10 x { u32 offset; u32 item_count; u32 crc32 }
0xA0        string table begins
```

The ten tables, of which seven are used:

| # | contents | stride | notes |
|---|---|---|---|
| 0 | string blob | — | `item_count` is **bytes**, not a count |
| 1 | names | 8 | `{u32 str_offset; u32 hash}` |
| 2 | imports | 8 | `{u32 str_offset; u16 class_name; u16 flags}` |
| 3 | properties | 16 | unused in practice |
| 4 | chunks (exports) | 24 | `{u16 class; u16 flags; u32 parent; u32 data_size; u32 data_offset; u32 template; u32 crc}` |
| 5 | buffers | 24 | `{u32 flags; u32 index; u32 offset; u32 disk_size; u32 mem_size; u32 crc}` |
| 6 | embedded | 16 | not exposed |

String-table offsets in tables 1 and 2 are **relative to the string table start**,
not absolute. The root class name sits at relative offset 1 (offset 0 is the
empty string), which is why WolvenKit's file-type guesser seeks to absolute
`0xA1`.

Note the chunk table's field order: `data_size` at +8 comes *before*
`data_offset` at +12. Easy to transpose.

## The property stream

Chunk data at `data_offset`, `data_size` bytes:

```
u8  0                       -- always zero
repeat {
    u16 name_index          -- 0 terminates
    u16 type_index
    u32 size                -- INCLUDES these 4 bytes
    u8  value[size - 4]
}
```

That is the whole thing. Both indices point into the name table, which points
into the string table.

**Nested structs use the identical encoding.** A struct-valued property's bytes
are just another `0`-then-TLV stream. That single fact is what makes dotted paths
work:

```c
redfs_cr2w_get(f, chunk, "header.sizeInfo.width", &v);
```

Each segment of the path is one TLV scan of the previous segment's value bytes.
No schema needed at any level.

## Decoding values

Type names are matched literally. The fixed-width ones:

| RED type | encoding |
|---|---|
| `Bool` `Int8` `Uint8` | 1 byte |
| `Int16` `Uint16` | 2 |
| `CName` | 2 — **index into the name table** |
| `Int32` `Uint32` `Float` | 4 |
| `Int64` `Uint64` `Double` `TweakDBID` `NodeRef` | 8 |
| `handle:X` `whandle:X` | 4 — `i32 - 1`, chunk index, −1 = null |
| `rRef:X` `raRef:X` | 2 — `u16 - 1`, **index into the import table** |
| `DataBuffer` | 4 — if `> 0x80000000`, `(v ^ 0x80000000) - 1` is a buffer index |
| `SerializationDeferredDataBuffer` | 2 — `u16 - 1`, buffer index |
| `CString` | VLQ length prefix, then UTF-8 or UTF-16 |
| `array:X` `static:N,X` | `u32` count, then elements |

Buffer indices matter: they map 1:1 onto the archive segments after the main one,
so a `DataBuffer` value tells you which `redfs_read` part to ask for.

### Enums are strings

`ETextureCompression`, `GpuWrapApieTextureType` and friends serialize as a
**2-byte name-table index**, not an ordinal. So format mapping compares strings —
`"TCM_QualityColor"` — instead of carrying enum value tables that would rot with
every patch. Cheap and version-stable.

The detection rule is a heuristic: after all known type names are excluded,
anything exactly 2 bytes wide is an enum. Nothing else in the format is 2 bytes
once `Uint16`, `Int16`, `CName` and `SerializationDeferredDataBuffer` are handled
by name. A struct cannot be 2 bytes — the minimum is 3 (leading zero plus the
`u16` terminator).

### A real bug this hid

`textureData` on `rendRenderTextureBlobPC` is typed
**`serializationDeferredDataBuffer`** — lowercase `s`. Everywhere else the same
concept is spelled with a capital. The exact-match comparison failed, the value
fell through to the 2-byte enum branch, and decoded as the *name* `"CBitmapTexture"`.

It went unnoticed because the buffer index defaulted to 0 and almost every
texture has exactly one buffer, so the wrong decode produced the right answer by
luck. Found by dumping a texture's property tree by hand. The comparison for that
one type is now case-insensitive, with a comment saying why.

## VLQ

CDPR's LEB128 variant, used only for `CString` lengths. The first octet is
special: bit 7 is the sign, bit 6 is the continuation flag, bits 0–5 are the low
value bits. Later octets are ordinary LEB128 (bit 7 continues, bits 0–6 value).
A positive prefix means UTF-16 and `|n|` *characters*; negative means UTF-8 and
`|n|` bytes.

## Arrays

`redfs_cr2w_walk_array` iterates elements. Fixed-width element types step by a
known stride; struct elements are measured by scanning their TLV to the
terminator. That makes struct-array iteration O(n) overall rather than O(n²),
which matters for a 44-chunk mesh.

`static:N,X` arrays still carry a leading `u32` count, confirmed empirically —
`static:5,Uint32` and `static:8,Uint8` both round-trip correctly.

Element type extraction handles both spellings: strip `array:`, or take
everything after the comma in `static:N,X`.

## Lifetime

`redfs_cr2w` **borrows** the bytes it was opened over; the caller keeps the blob
alive. Values returned by `get`/`walk` point directly into that blob — no copies,
no allocation on the query path.

The one exception is `CString`, which needs decoding into a real buffer. Those
are owned by the `redfs_cr2w` handle so `redfs_value::as.s` stays valid as long
as the handle does.

## What this gives you for free

Because imports are real path strings, every CR2W parse yields a batch of
hash → path pairs at no cost. RedFS feeds them into the path dictionary
automatically. This is the only source that knows paths a *mod* invented, since
no shipped dictionary can. See `path-hashing.md`.

Dumped from a terrain mesh, with no class definitions involved:

```
root            CMesh
chunks          13   [CMesh, meshMeshParamTerrain, CMaterialInstance, ...]
imports         6
  base\materials\multilayered_terrain.mt
  base\worlds\03_night_city\multilayer_terrain.mlsetup
  ...
properties of chunk 0:
  cookingPlatform     ECookingPlatform     name    "PLATFORM_PC"
  boundingBox         Box                  struct  {...} (121 bytes)
  materialEntries     array:CMeshMaterialEntry  array  [1 items]
  renderResourceBlob  handle:IRenderResourceBlob  handle -> chunk 4
```
