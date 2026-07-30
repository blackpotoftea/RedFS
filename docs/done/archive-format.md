# The `.archive` container

**Status: implemented** — `src/archive.cpp`, `src/oodle.cpp`

Reverse-engineering notes and the resulting implementation. The layout was derived
from WolvenKit's C# reader (`WolvenKit.RED4/Archive/`, principally
`Base/Header.cs`, `Base/Index.cs`, `Base/FileEntry.cs`, `IO/ArchiveReader.cs` and
`IO/ArchiveWriter.cs`) and then confirmed against the 57 archives of a live
2.3 + Phantom Liberty install.

This is a reference. Every field is listed with its offset, its width, and whether
RedFS reads it — because "RedFS ignores this" and "nobody knows what this is" are
different statements and the tables below keep them apart.

## Conventions

- **Little-endian** throughout. No field in this container is big-endian.
- All offsets and sizes are in **bytes**. `0x..` is an offset from the start of
  the file; `+0x..` is relative to the start of the structure being described.
- **Nothing may be assumed aligned.** `index_position` is arbitrary, the entry
  table starts 28 bytes into the index (4-aligned, not 8), and `debug_position`
  sits at `0x14`. Every read in `archive.cpp` goes through the `memcpy`-based
  `rd16` / `rd32` / `rd64` helpers in `internal.hpp`.
- **Version 12** is what the shipping game and WolvenKit both write. RedFS reads
  the field's position but **does not check the value** — `Archive::open`
  validates only the magic. A version 13 archive would be parsed as if it were 12.

## File header

`0x00` – `0x27`, 40 bytes (`Header.SIZE`).

| offset | type | field | RedFS |
|---|---|---|---|
| `0x00` | `u32` | `magic` — bytes `52 44 41 52` (`'RDAR'`), `0x52414452` read as a LE u32 | validated |
| `0x04` | `u32` | `version` — 12 | not read |
| `0x08` | `u64` | `index_position` — absolute file offset of the index | read |
| `0x10` | `u32` | `index_size` — total length of the index in bytes | read |
| `0x14` | `u64` | `debug_position` — 0 in every archive sampled | not read |
| `0x1C` | `u32` | `debug_size` — 0 in every archive sampled | not read |
| `0x20` | `u64` | `file_size` — declared total length of the archive | read, fingerprint only |
| `0x28` | `u32` | `custom_data_length` — length of the LXRS footer, measured **from `0xAC`** | not read |
| `0x2C` | 128 B | zero padding | — |
| `0xAC` | | LXRS footer when `custom_data_length != 0`, then the file payload | — |

`0xAC` is `Header.EXTENDED_SIZE`; `ArchiveWriter.WriteArchive` reaches it by
writing the 40-byte header followed by 132 zero bytes, then overwriting `0x28`
with the footer length on its second pass.

Two quirks in the reference implementation, both harmless but worth knowing if you
are diffing bytes: `ArchiveWriter` writes a C# `long` at `0x28` (8 bytes) while
`ArchiveReader` reads a `uint` (4 bytes) — the high half is always zero, so
`0x2C` – `0x2F` is guaranteed zero for WolvenKit output. RedFS reads only the
first 40 bytes of the file and never looks at `0x28` at all.

## Index

At `index_position`: a **28-byte** header (`kIndexHeaderSize`) followed by three
tables, back to back with no padding.

| offset | type | field | RedFS |
|---|---|---|---|
| `+0x00` | `u32` | `file_table_offset` — WolvenKit writes `8` | not read |
| `+0x04` | `u32` | `file_table_size` — WolvenKit writes `index_size - 8` | not read |
| `+0x08` | `u64` | `crc` — CRC-64 over `+0x10` to the end of the index | read, never validated |
| `+0x10` | `u32` | `entry_count` | read |
| `+0x14` | `u32` | `segment_count` | read |
| `+0x18` | `u32` | `dependency_count` | read, bounds check only |
| `+0x1C` | | file entry table begins | |

```
index_position + 0x00                                index header, 28 bytes
index_position + 0x1C                                entry_count      x 56
index_position + 0x1C + 56*E                         segment_count    x 16
index_position + 0x1C + 56*E + 16*S                  dependency_count x  8
```

`Archive::open` requires `28 + 56*E + 16*S + 8*D <= index_size` and returns
`REDFS_E_CORRUPT` otherwise, so a truncated or over-claiming index is rejected
before any table pointer is published.

**What the offset/size pair means is inferred, not confirmed.** WolvenKit writes
`file_table_offset = 8` (with a `//TODO` beside it) and
`file_table_size = index_size - 8`, which together describe the span from the CRC
field to the end of the index — the index minus its own first two fields. Whether
the game reads them that way, or at all, is unknown. RedFS derives every table
position from `kIndexHeaderSize` and the three counts instead.

### File entries — stride 56 (`kEntryStride`)

| offset | type | field |
|---|---|---|
| `+0x00` | `u64` | `name_hash64` — FNV-1a 64 of the sanitised depot path (see `path-hashing.md`) |
| `+0x08` | `i64` | `timestamp` — Windows `FILETIME` (`DateTime.FromFileTime` in `ArchiveReader.ReadFileEntry`) |
| `+0x10` | `u32` | `num_inline_buffer_segments` — **meaning unknown**, see below |
| `+0x14` | `u32` | `segments_start` — index into the segment table |
| `+0x18` | `u32` | `segments_end` — **exclusive** |
| `+0x1C` | `u32` | `resource_deps_start` — index into the dependency table |
| `+0x20` | `u32` | `resource_deps_end` — **exclusive** |
| `+0x24` | `u8[20]` | `sha1` |

RedFS exposes `name_hash64`, `timestamp`, the segment range and `sha1`
(`Archive::entry_*` in `internal.hpp`). It never reads the dependency range or
`num_inline_buffer_segments`.

`num_inline_buffer_segments` is the one entry field whose meaning is not
established. WolvenKit names it that but its writer stores
`max(buffer_count - 1, 0)` there (`ArchiveWriter.cs`, the `flags` local), which
does not obviously match the name, and its reader never consults the value. Do
not build anything on it.

### Segments — stride 16 (`kSegmentStride`)

| offset | type | field |
|---|---|---|
| `+0x00` | `u64` | `offset` — **absolute** offset into the archive file, not relative to anything |
| `+0x08` | `u32` | `zsize` — bytes on disk |
| `+0x0C` | `u32` | `size` — bytes after decoding |

### Dependencies — stride 8

`dependency_count` × `u64`, each a path hash of the same flavour as
`name_hash64`. RedFS reads the count to bounds-check the index length and never
touches the table. WolvenKit's writer has its import-registration loop commented
out, so archives it builds carry `dependency_count == 0`; shipping game archives
do not.

## The index CRC

`+0x08` is a real, computed **CRC-64/XZ**: reflected polynomial
`0xC96C5795D7870F42`, init and xorout both all-ones. `ArchiveWriter.WriteIndex`
computes it over the index body it has just serialised — the three counts, then
the entry, segment and dependency tables — that is, everything from `+0x10` to
the end of the index. The CRC field itself and the two fields before it are
outside the covered range. `tests/fixtures.cpp` reproduces the same computation
for synthesised archives.

Every archive sampled on a real install has a distinct non-zero value here. The
format does not appear to require one, though: a hand-built archive that leaves
it zero parses fine, and RedFS **never validates it**.

What RedFS does with it is mix it into the mesh cache fingerprint
(`depot_fingerprint` in `cache.cpp`). That is the one input that notices an
archive whose files were replaced *in place*: path, entry count and index length
are all byte-identical after a repack that changes no counts, so without the CRC
the cache would keep serving pre-edit geometry. See `caching.md`.

## The 28-byte trap

The index header is 28 bytes, not 24. Getting it wrong shifts the entire file
table by one field and produces garbage that still *parses* — entry counts come
out plausible, hashes do not resolve, and nothing obviously fails. It cost a
debugging round; the constant is now named (`kIndexHeaderSize`) rather than
inline.

## Segments and files

A file is a **contiguous run of segments**, `[segments_start, segments_end)`:

- the first segment is the resource itself — a CR2W document for anything cooked
- the rest are its **attached buffers**: pixel data for a texture, vertex and
  index streams for a mesh, and so on

This is the single most useful structural fact in the format. A caller that only
wants metadata reads the first segment and never touches the multi-megabyte
payload; a caller that wants the payload reads exactly one buffer. Both are
reachable through the `part` argument of `redfs_read` (`resolve_part` in
`archive.cpp`):

| `part` | value | segments read |
|---|---|---|
| `REDFS_PART_MAIN` | `0xFFFFFFFF` | `segments_start` only |
| `REDFS_PART_ALL` | `0xFFFFFFFE` | `[segments_start, segments_end)`, all decoded, concatenated |
| buffer index `i` | `0 ..` | `segments_start + 1 + i`, rejected with `REDFS_E_RANGE` past `segments_end` |

`REDFS_PART_ALL` is **not** what WolvenKit's loose-file export produces.
`Archive.CopyFileToStream` decompresses the first segment and appends the buffers
**still Kraken-compressed** unless its `decompressBuffers` flag is set, which the
export path leaves false. That is why an extracted `.xbm` on disk has a `KARK`
blob hanging off the end of its CR2W. The same asymmetry shows up in
`ArchiveReader.ReadIndex`, which computes `FileEntry.Size` as the first segment's
`Size` plus every later segment's **`ZSize`** — so WolvenKit's reported file size
is not the fully decoded size that `REDFS_PART_ALL` returns.

## Compression

A segment is stored raw when `zsize == size`. Otherwise it opens with a KARK
block:

| offset | type | field |
|---|---|---|
| `+0x00` | `u32` | magic — bytes `4B 41 52 4B` (`'KARK'`), `0x4B52414B` as a LE u32 |
| `+0x04` | `u32` | `raw_size` — decoded length claimed by the block |
| `+0x08` | | Kraken bitstream, `zsize - 8` bytes |

Decoded with `OodleLZ_Decompress(src + 8, zsize - 8, dst, raw)`, plus a
caller-supplied `decoderMemory` — sized once from
`OodleLZDecoder_MemorySizeNeeded(Invalid, -1)` (462288 bytes against 2.31) and
pooled across decodes. Leaving it null makes Oodle allocate through the
process-global plugin allocator, which inside the game is CDPR's and asserts on
sight; see `src/oodle.cpp`.

`raw_size` occasionally disagrees with the segment table's `size`, and the two
implementations resolve it in **opposite directions**:

- **WolvenKit** treats the KARK header as authoritative — `size = headerSize`,
  then it allocates an output buffer to match (`Oodle.DecompressAndCopySegment`).
- **RedFS clamps `raw_size` down to `size`.** The destination buffer was sized
  from the index, so the index is the only bound that is safe to trust; a KARK
  header claiming more would overrun the caller. When `raw_size < size` the tail
  of the destination is zero-filled so the caller never sees uninitialised bytes.

Not every `zsize != size` segment is actually compressed. Both readers fall back
to a raw copy when the magic does not match — RedFS copies `min(zsize, size)`
bytes and zero-fills any remainder. A `zsize == 0` segment is special-cased
before any mapping happens: `MapViewOfFile` reads a length of 0 as "to the end of
the mapping", which would reserve the rest of a multi-gigabyte archive to copy
nothing.

### Where Oodle comes from

`oo2ext_7_win64.dll` ships in `bin/x64` of every install, and **inside the game
process it is already loaded**. So, in order (`oodle.cpp`):

1. `GetModuleHandleW(L"oo2ext_7_win64.dll")` — the in-game path, no load at all
2. `LoadLibraryA(<game_dir>/bin/x64/oo2ext_7_win64.dll)` — for external tools
3. bare `LoadLibraryW` on the search path — last resort

Nothing about Oodle is redistributed. On x64 Windows there is only one calling
convention, so the `__stdcall` in the signature is decorative; the parameter list
matters and is taken from WolvenKit's P/Invoke, with `fuzzSafe=1`, `checkCRC=0`,
`verbosity=0`, `threadPhase=3` (unthreaded). `fuzzSafe` is the load-bearing one:
segment bytes come from whatever mod is installed, and it is what obliges the
decoder to stay inside `raw_len`.

## Reading strategy

Two decisions worth recording, both about not paying for what you do not use.

**The index is mapped, not copied.** The entry table alone is
56 × 544,670 = 30.5 MB on a stock install, and the full index across all 57
archives is ~89 MB; a reader that deserialises it into heap structs commits all of
that and none of it is evictable. Instead the index region of each archive stays a
read-only file mapping and entries are read in place through the unaligned
accessors. The cost becomes address space plus page cache, which the OS can drop
under memory pressure.

This is what forces the alignment rule above: `index_position` is arbitrary, so
nothing may assume natural alignment.

**Data is windowed per read, not mapped whole.** Mapping all 85 GB would work on
x64 — it is only address space — but per-read windowing is simpler and keeps the
process's VAD list small. `MapViewOfFile` demands an allocation-granularity
(64 KB) aligned offset, so the segment offset is rounded down and the remainder
carried as a delta.

That is what makes reads position-independent. Measured across depth quintiles of
a 24 GB archive: 356 / 354 / 326 / 427 / 418 MB/s. A 40 KB file from the middle
comes back in 0.16 ms because only 40 KB is touched.

## Mount order and overrides

`redfs_depot_open` scans, in this order (each gated on its `REDFS_SCAN_*` flag):

| # | location | recursion | within-location order |
|---|---|---|---|
| 1 | `archive/pc/content` | flat | path order, ascending |
| 2 | `archive/pc/ep1` | flat | path order, ascending |
| 3 | `mods/<name>/archives` (REDmod) | **recursive** | path order **descending** |
| 4 | `archive/pc/mod` | flat | path order, ascending |

The depot then builds one global `(hash, archive, entry)` table sorted by hash,
16 bytes per file, deduplicated so **the last mount wins**. That is the override
rule, and it falls out of using a `stable_sort` and keeping the final duplicate
(`reindex` in `api.cpp`).

The descending sort inside a REDmod is not a typo — it mirrors
`ArchiveManager.LoadModsArchives`, which sorts full paths ordinally and then
reverses, so within one REDmod the ordinally-*first* path mounts last and wins.
A `zz_` filename prefix therefore **loses** inside a REDmod and wins in
`archive/pc/mod`. The REDmod folders themselves are visited in ascending name
order, so across mods the later name still wins; the reversal applies only within
one mod's `archives` tree. Only REDmod recurses — the legacy mod folder is
top-level only in the reference implementation, so RedFS keeps it flat too.

Lookup is a binary search: ~20 comparisons over 544,670 entries, sub-microsecond,
no I/O.

Measured on the reference install: 57 archives, 85 GB, 544,670 files, mounted in
**~30 ms**, for 8.7 MB of heap and ~89 MB of file-backed mapping.

## What was left out

- **The LXRS footer.** WolvenKit-built mod archives carry a `custom_data` block
  at `0xAC` listing their own file paths, which would seed the path dictionary for
  free. Its layout is known (`Base/LxrsFooter.cs`): magic `53 52 58 4C` (`'SRXL'`
  on disk, `0x4C585253` as a LE u32), `u32 version = 1`, `i32 size`, `i32 zsize`,
  `i32 count`, then `zsize` bytes holding NUL-terminated ISO-8859-1 paths — raw
  when `size == zsize`, Kraken otherwise. Not parsed; CR2W import learning covers
  most of the same ground. See `../roadmap.md`.
- **The dependency table**, and the per-entry range that indexes it. The count is
  read for bounds validation, nothing else. CR2W import tables give the same
  information with actual path strings attached.
- **`file_table_offset` / `file_table_size`**, `debug_position` / `debug_size`,
  `version`, `num_inline_buffer_segments`, and the index CRC as a *check*. Read
  positions are documented above; none of these gate a parse.
- **Writing.** RedFS is read-only by design.
