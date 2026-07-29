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

## Conventions

- **Little-endian** throughout, same as the container.
- All offsets and sizes are in **bytes**. `0x..` is from the start of the CR2W
  blob; `+0x..` is from the start of the structure being described.
- Table offsets in the header are **absolute within the blob**. String offsets in
  the name and import tables are **relative to the string table's start**.
- Nothing may be assumed aligned; every read goes through `rd16` / `rd32` /
  `rd64`.
- Versions **163 – 195** are accepted (`cr2w_parse`, matching
  `CR2WReader.ReadFileInfo`); anything outside returns `REDFS_E_UNSUPPORTED`.

## Header

Magic, a 36-byte header (`CR2WFileHeader`), then ten 12-byte table descriptors —
`0xA0` bytes in total before any payload.

| offset | type | field | RedFS |
|---|---|---|---|
| `0x00` | `u32` | `magic` — bytes `43 52 32 57` (`'CR2W'`), `0x57325243` as a LE u32 | validated |
| `0x04` | `u32` | `version` — 163 – 195 | validated |
| `0x08` | `u32` | `flags` | not read |
| `0x0C` | `u64` | `timestamp` | not read |
| `0x14` | `u32` | `build_version` | not read |
| `0x18` | `u32` | `objects_end` — end of the document proper; attached buffers start here | not read |
| `0x1C` | `u32` | `buffers_end` — end of the appended buffers | not read |
| `0x20` | `u32` | `crc32` | not read |
| `0x24` | `u32` | `num_chunks` | not read; the chunk table's own `item_count` is used |
| `0x28` | 10 × 12 B | table descriptors, `{u32 offset; u32 item_count; u32 crc32}` | offsets and counts read; the per-table CRC-32s are not |
| `0xA0` | | string blob, in every file seen | — |

`objects_end` is worth knowing even though RedFS ignores it: it is the length of
the CR2W document without its buffers, and it is exactly what
`ArchiveWriter.WriteArchive` uses to decide how many bytes of a loose file go into
the archive's first segment. That is the seam where a loose `.xbm`'s `KARK` blob
begins.

## The ten tables

Slots 7 – 9 are unused — "not used in cr2w so far", per `CR2WReader.File.cs`.

| # | contents | stride | entry layout | RedFS |
|---|---|---|---|---|
| 0 | string blob | — | `item_count` is the blob's **length in bytes**, not a count | read |
| 1 | names | 8 | `{u32 str_offset; u32 hash}` | `str_offset` read, `hash` ignored |
| 2 | imports | 8 | `{u32 str_offset; u16 class_name; u16 flags}` | all four read, `flags` not interpreted |
| 3 | properties | 16 | `{u16 class_name; u16 class_flags; u16 prop_name; u16 prop_flags; u64 hash}` | **never read** |
| 4 | chunks (exports) | 24 | `{u16 class; u16 obj_flags; u32 parent; u32 data_size; u32 data_offset; u32 template; u32 crc32}` | `class`, `data_size`, `data_offset` read |
| 5 | buffers | 24 | `{u32 flags; u32 index; u32 offset; u32 disk_size; u32 mem_size; u32 crc32}` | `index`, `offset`, `disk_size`, `mem_size` read |
| 6 | embedded | 16 | `{u32 import_index; u32 chunk_index; u64 path_hash}` | **never read** |
| 7–9 | — | — | absent from every file examined | not read |

`cr2w_parse` bounds-checks tables 1, 2, 4 and 5 against the blob length before
reading any of them, and requires the string blob's last byte to be a NUL — which
is what turns "this offset is in range" into "this string is safe to `strcmp`".

Note the chunk table's field order: **`data_size` at `+0x08` comes before
`data_offset` at `+0x0C`**. Easy to transpose, and a transposition still parses.

Table 3 deserves its own note. It is not "unused in practice" in the loose sense —
WolvenKit reads it, and then throws `TodoException` if it holds more than one
entry, so in every shipping file it is empty or nearly so and its contents have
never been needed. RedFS does not read it at all.

Table 1's `hash` is a short RED hash of the name string. WolvenKit uses exactly
one of them — `NameInfo[1].hash` — to decide whether a file predates patch 1.2
(`CR2WReader.IdentifyHash`). RedFS resolves names positionally and ignores it.

### Strings and names

The string blob is a run of NUL-terminated strings. A name-table entry's
`str_offset` is relative to the blob's start, so offset 0 is the empty string.
Both indirections are bounds-checked in `redfs_cr2w::name` / `::str`, which return
`""` rather than null because every caller hands the result to a `strcmp`-family
function.

Table 0's `offset` is `0xA0` in every file examined, but RedFS reads the field
rather than assuming it.

**The root class name is a convention, not a guarantee.** In practice it is
name index 1, whose string sits at relative offset 1 — immediately after the empty
string — which is why `ArchiveReader.GuessFileType` can seek to absolute `0xA1`
and read a NUL-terminated string to identify a file type. RedFS does not rely on
that: `redfs_cr2w_root_type` resolves chunk 0's `class` index through the name
table.

## The property stream

A chunk's body is `data_size` bytes at `data_offset`:

```
u8  0                       -- always zero
repeat {
    u16 name_index          -- 0 terminates the stream
    u16 type_index
    u32 size                -- INCLUDES these 4 bytes
    u8  value[size - 4]
}
```

That is the whole thing. Both indices point into the name table, which points
into the string blob. `size` counting itself is the detail to get right: the
payload is `size - 4`, confirmed against `CR2WReader.ReadVariable`
(`var size = _reader.ReadUInt32() - 4;`). `PropWalker` treats `size < 4` as
end-of-stream rather than wrapping.

The leading zero byte is not decorative — WolvenKit's `ReadClass` throws if it is
anything else — but nothing is known about what it would mean if it were not zero.

**Nested structs use the identical encoding.** A struct-valued property's bytes
are just another `0`-then-TLV stream. That single fact is what makes dotted paths
work:

```c
redfs_cr2w_get(f, chunk, "header.sizeInfo.width", &v);
```

Each segment of the path is one TLV scan of the previous segment's value bytes.
No schema needed at any level.

## Decoding values

Type names are matched as literal strings. `size` below is the payload width;
where it says "var" the value is self-delimiting.

| RED type | size | RedFS kind | notes |
|---|---|---|---|
| `Bool` | 1 | bool | nonzero is true |
| `Int8` `Uint8` | 1 | int / uint | |
| `Int16` `Uint16` | 2 | int / uint | |
| `CName` | 2 | name | `u16` **index into the name table**, resolved to a string |
| `Int32` `Uint32` | 4 | int / uint | |
| `Float` | 4 | float | |
| `Int64` `Uint64` | 8 | int / uint | |
| `Double` | 8 | float | |
| `TweakDBID` | 8 | uint | raw `u64`, not resolved to a name |
| `NodeRef` | 8 | uint | **wrong — see below** |
| `handle:X` `whandle:X` | 4 | handle | `i32 - 1` is a chunk index; `-1` is null |
| `rRef:X` `raRef:X` | 2 | string | `u16 - 1` indexes the **import table**; RedFS returns that import's depot path, or `""` for 0 / out of range |
| `DataBuffer` | 4 | buffer / raw | `v > 0x80000000` → buffer index `(v ^ 0x80000000) - 1`; `v == 0x80000000` → null; `v < 0x80000000` → `v` **inline** bytes follow and the value is left raw |
| `SerializationDeferredDataBuffer` | 2 | buffer | `u16 - 1` is a buffer index; 0 is null; matched **case-insensitively** |
| `CString` | var | string | VLQ length prefix, then the characters |
| `array:X` `static:N,X` `[N]X` | var | array | `u32` count, then the elements |
| *unrecognised, exactly 2 bytes* | 2 | name | an enum, stored as a name-table index |
| *unrecognised, ≥ 3 bytes starting `00`* | var | struct | a nested TLV body |
| *anything else* | var | raw | bytes handed back undecoded |

Buffer indices matter: index `i` is the CR2W buffer table's `i`-th entry, and
those map 1:1 onto the archive segments after the first one — segment
`segments_start + 1 + i`. So a `DataBuffer` value tells you which `part` to pass
to `redfs_read`. (`ArchiveWriter` appends one segment per `BufferInfo`, in order,
which is what pins the correspondence.)

### `NodeRef` is decoded incorrectly

In CR2W, `NodeRef` is a **VLQ length-prefixed string** — the same encoding as
`CString` — which WolvenKit hashes into `NodeRefPool` after reading
(`Red4Reader.ReadNodeRef`). `CR2WReader` does not override that method; the only
overrides are in `RedPackageReader` and the save-file parser, neither of which
handles CR2W chunk data.

RedFS treats `NodeRef` as a raw 8-byte integer, in both `fixed_width` (array
striding) and `cr2w_decode`. A `NodeRef` property therefore decodes to whatever
the first 8 bytes of its length prefix and characters happen to be, and a
`NodeRef` array strides by 8 instead of walking the strings. Nothing in the
shipped feature set — textures, meshes — reads a `NodeRef`, which is why this has
not surfaced. It is recorded here rather than quietly documented as correct.

### Bit fields are not handled

`CBitField` values are a **NUL-terminated run of `u16` name indices**
(`Red4Reader.ReadCBitField` loops until it reads 0), so a bitfield property is
`2 × (flags_set + 1)` bytes. RedFS has no case for it: an empty one is 2 bytes and
falls into the enum branch, decoding to the empty name; a populated one falls
through to raw, or is misread as a struct if its first byte happens to be zero.
Fixable by matching the type name, but no shipped feature needs it.

### Enums are strings

`ETextureCompression`, `ETextureRawFormat` and friends serialize as a **2-byte
name-table index**, not an ordinal (`Red4Reader.ReadCEnum`). So format mapping
compares strings — `"TCM_QualityColor"` — instead of carrying enum value tables
that would rot with every patch. Cheap and version-stable.

The detection rule is a heuristic: after all known type names are excluded,
anything exactly 2 bytes wide is treated as an enum. Nothing else in the format is
2 bytes once `Uint16`, `Int16`, `CName` and `SerializationDeferredDataBuffer` are
handled by name. A struct cannot be 2 bytes — the minimum is 3, a leading zero
plus the `u16` terminator.

### A real bug this hid

`textureData` on `rendRenderTextureBlobPC` is typed
**`serializationDeferredDataBuffer`** — lowercase `s`. Everywhere else the same
concept is spelled with a capital. The exact-match comparison failed, the value
fell through to the 2-byte enum branch, and decoded as the *name*
`"CBitmapTexture"`.

It went unnoticed because the buffer index defaulted to 0 and almost every
texture has exactly one buffer, so the wrong decode produced the right answer by
luck. Found by dumping a texture's property tree by hand. The comparison for that
one type is now `_stricmp`, with a comment saying why.

A second trap in the same branch: index 0 means null, and it has to be rejected
*before* the `- 1`. `0 - 1` is `0xFFFFFFFF`, which is `REDFS_PART_MAIN`, so a null
buffer would otherwise resolve to the first segment and hand back the CR2W
document as if it were payload.

## VLQ

CDPR's LEB128 variant, used for `CString` and `NodeRef` lengths. The first octet
is special: bit 7 is the **sign**, bit 6 is the **continuation** flag, bits 0–5
are the low value bits. Later octets are ordinary LEB128 (bit 7 continues, bits
0–6 carry value). At most five octets — WolvenKit throws if the fifth still has
its continuation bit set.

The sign selects the encoding, not the length:

| prefix | encoding | length means |
|---|---|---|
| positive | UTF-16 | `n` code units, `2n` bytes |
| negative | UTF-8 | `n` bytes |

WolvenKit's writer always emits the negative form, noting that every string seen
in CP77 so far has been UTF-8.

RedFS's decoder is lossy on the UTF-16 path: code units below `0x80` are kept and
everything else becomes `'?'`. UTF-8 payloads are copied verbatim, so the common
case is exact.

## Arrays

All three array spellings carry a leading `u32` element count, `static:N,X`
included. That is confirmed on both sides now: `Red4Reader.ReadCArrayFixedSize`
reads an `Int32` count before the elements, and `static:5,Uint32` /
`static:8,Uint8` round-trip correctly against real files.

`element_type` handles all three spellings — strip `array:`, take everything after
the comma in `static:N,X`, or everything after `]` in `[N]X`. Missing one would
mean the whole array type name gets returned as its own element type, and every
element sized and decoded as if it were that array.

`classify_elements` then decides how to step, in this order:

1. **Known fixed width** (`fixed_width`): step by that many bytes.
2. **`CString`**: self-delimiting, step past the VLQ prefix and its characters.
3. **Divides evenly**: for an element type name RedFS does not recognise — in
   practice an enum — if the payload divides exactly by the count and the quotient
   is 1 – 8 bytes, step by that quotient.
4. **Otherwise a struct**: measure each element by walking its TLV to the
   terminator.

Step 3 has to come before step 4. Guessing "struct" first misfires on any enum
whose name index is a multiple of 256, because its low byte is zero and therefore
indistinguishable from a struct's leading zero. The 1 – 8 bound is deliberately
tight so that a struct array which happens to divide evenly is not mistaken for a
uniform one.

Because struct elements are measured forward from where the previous one ended,
iteration is O(total bytes) rather than O(n²) — which is what makes walking a
mesh's per-chunk arrays cheap.

## Lifetime

`redfs_cr2w` **borrows** the bytes it was opened over; the caller keeps the blob
alive. Values returned by `get` / `walk` point directly into that blob — no
copies, no allocation on the query path.

The one exception is `CString`, which needs decoding into a real buffer. Those are
owned by the handle so `redfs_value::as.s` stays valid as long as the handle does,
and they are cached by source pointer so a property queried every frame allocates
once instead of growing the handle.

That cache is written through a `const_cast`, which is what makes an individual
`redfs_cr2w*` **single-threaded**. The depot stays shareable; it is one CR2W
handle that two threads must not touch at once.

## What this gives you for free

Because imports are real path strings, every CR2W parse yields a batch of
hash → path pairs at no cost, and `cr2w_parse` feeds them into the path
dictionary automatically. This is the only source that knows paths a *mod*
invented, since no shipped dictionary can. See `path-hashing.md`.

Abridged from `redfs_cli cr2w` on a terrain mesh, with no class definitions
involved:

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
