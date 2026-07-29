# The `.archive` container

**Status: implemented** — `src/archive.cpp`

Research notes and the resulting implementation. Everything here was derived from
WolvenKit's C# reader (`WolvenKit.RED4/Archive/`) and then confirmed against the
57 archives of a live 2.3 + Phantom Liberty install.

## Layout

All little-endian. Version 12 is what the shipping game uses.

```
offset  size  field
0x00    4     magic          'RDAR' (0x52414452)
0x04    4     version        12
0x08    8     index_position
0x10    4     index_size
0x14    8     debug_position
0x1C    4     debug_size
0x20    8     file_size
0x28    4     custom_data_length     -- LXRS path footer, if present
0xAC          custom data begins (Header.EXTENDED_SIZE)
```

At `index_position`, a 28-byte header followed by three tables:

```
+0x00   4     file_table_offset
+0x04   4     file_table_size
+0x08   8     crc
+0x10   4     entry_count
+0x14   4     segment_count
+0x18   4     dependency_count
+0x1C         tables begin
```

**File entries**, `entry_count` × 56 bytes:

```
+0x00   8     name_hash64        FNV1a64 of the normalised depot path
+0x08   8     timestamp          Windows FILETIME
+0x10   4     num_inline_buffer_segments
+0x14   4     segments_start     index into the segment table
+0x18   4     segments_end       exclusive
+0x1C   4     resource_deps_start
+0x20   4     resource_deps_end
+0x24   20    sha1
```

**Segments**, `segment_count` × 16 bytes:

```
+0x00   8     offset      absolute, into the archive file
+0x08   4     zsize       bytes on disk
+0x0C   4     size        bytes once decoded
```

**Dependencies**, `dependency_count` × `u64`.

### The 28-byte trap

The index header is 28 bytes, not 24. Getting this wrong shifts the entire file
table by one field and produces garbage that still *parses* — entry counts come
out plausible, hashes do not resolve, and nothing obviously fails. It cost a
debugging round; the constant is now named (`kIndexHeaderSize`) rather than
inline.

## Segments and files

A file is a **contiguous run of segments**, `[segments_start, segments_end)`:

- segment 0 is the resource itself — a CR2W document for anything cooked
- segments 1..n are its **attached buffers**: pixel data for a texture, vertex
  and index streams for a mesh, and so on

This is the single most useful structural fact in the format. It means a caller
that only wants metadata reads segment 0 and never touches the multi-megabyte
payload, and a caller that wants the payload reads exactly one buffer. Both are
exposed directly:

```c
REDFS_PART_MAIN    /* segment 0 only  */
REDFS_PART_ALL     /* all, concatenated -- what WolvenKit would export */
0 .. n-1           /* one attached buffer */
```

WolvenKit's own loose-file export is `PART_MAIN` decompressed plus the buffers
left *still compressed*, which is why an extracted `.xbm` on disk has a `KARK`
blob hanging off the end of its CR2W.

## Compression

A segment is stored raw when `zsize == size`. Otherwise:

```
'KARK' (0x4B52414B) | u32 raw_size | Kraken bitstream
```

Decode with `OodleLZ_Decompress(src+8, zsize-8, dst, raw_size)`.

The `raw_size` in the KARK header occasionally disagrees with `size` in the
segment table. WolvenKit treats the KARK header as authoritative and so do we,
clamping to the table size so a bad header cannot overrun the destination.

Not every `zsize != size` segment is actually compressed — a few are not, and the
magic check catches them.

### Where Oodle comes from

`oo2ext_7_win64.dll` ships in `bin/x64` of every install, and **inside the game
process it is already loaded**. So:

1. `GetModuleHandleW(L"oo2ext_7_win64.dll")` — the in-game path, no load at all
2. `LoadLibraryA(<game_dir>/bin/x64/oo2ext_7_win64.dll)` — for external tools
3. bare `LoadLibraryW` on the search path — last resort

Nothing about Oodle is redistributed. On x64 Windows there is only one calling
convention, so the `__stdcall` in the signature is decorative; the parameter list
matters and is taken from WolvenKit's P/Invoke, with `fuzzSafe=1`, `checkCRC=0`,
`verbosity=0`, `threadPhase=3` (unthreaded).

## Reading strategy

Two decisions worth recording, both about not paying for what you do not use.

**The index is mapped, not copied.** A naive reader deserializes 544,670 entries
into heap structs — at ~48 bytes each that is 26 MB of entries plus 32 MB of
segments plus the lookup table, all of it committed and none of it evictable.
Instead the index region of each archive stays as a read-only file mapping and
entries are read in place through unaligned accessors. Cost becomes address space
plus page cache, and the OS can drop pages under memory pressure.

Consequence: nothing may assume natural alignment, because `index_position` is
arbitrary. All reads go through `memcpy`-based `rd16/rd32/rd64`.

**Data is windowed per read, not mapped whole.** Mapping all 85 GB would work on
x64 — it is only address space — but per-read windowing is simpler and keeps the
process's VAD list small. `MapViewOfFile` demands an allocation-granularity
(64 KB) aligned offset, so the segment offset is rounded down and the remainder
carried as a delta.

This is what makes reads position-independent. Measured across depth deciles of a
24 GB archive: 356 / 354 / 326 / 427 / 418 MB/s. A 40 KB file from the middle
comes back in 0.16 ms because only 40 KB is touched.

## Mount order and overrides

`redfs_depot_open` scans `archive/pc/content`, then `ep1`, then `mod`, each in
filename order — matching the game. The depot builds one global
`(hash, archive, entry)` table sorted by hash, 16 bytes per file, deduplicated so
**the last mount wins**. That is the override rule, and it falls out of using a
`stable_sort` and keeping the final duplicate.

Lookup is a binary search: ~20 comparisons over 544,670 entries, sub-microsecond,
no I/O.

Measured on the reference install: 57 archives, 85 GB, 544,670 files, mounted in
**~30 ms**, for 8.7 MB of heap and ~89 MB of file-backed mapping.

## What was left out

- **The LXRS footer.** WolvenKit-built mod archives carry a `custom_data` block
  listing their own file paths, which would seed the path dictionary for free.
  Not parsed; CR2W import learning covers most of the same ground. See
  `../roadmap.md`.
- **The dependency table.** Read for bounds validation, not exposed. CR2W import
  tables give the same information with actual path strings attached.
- **Writing.** RedFS is read-only by design.
