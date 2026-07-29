# How correctness was established

**Status: implemented** — `tools/redfs_verify.cpp`, `redfs_cli selftest`

## The principle

A format reader can be entirely self-consistent and entirely wrong. It parses
without error, produces plausible numbers, and every internal check agrees —
because all of them derive from the same misreading.

So the checks that count are the ones whose answer comes from **outside the code
being tested**. Each oracle below names where its answer comes from, and how
independent that source really is.

Every finding in this document was produced by these checks. None was found by
reading the code.

## Scope: all of this is offline

Everything here runs in a standalone test process against a real game install on
disk. **Nothing has ever run inside the running game.** There is no RED4ext
plugin in the tree and nothing deployed to `red4ext/plugins/`, so no claim below
should be read as evidence about behaviour in-process alongside the engine. That
gap is listed again at the end because it is the largest one.

## Oracle 1 — DirectXTex parses our DDS headers

RedFS builds DDS headers from RED4 metadata. Rather than trust our reading of the
DDS spec, the result is handed to **DirectXTex** — via the `texconv.dll` that
ships with WolvenKit — and its parse is compared against what RedFS claimed:

```c
TexMetadata m = GetMetadataFromDDSMemory(dds.data, dds.size, 0);
const bool is_cube = (m.miscFlags & 0x4) != 0;   /* TEX_MISC_TEXTURECUBE */
agree = m.width  == t.width      && m.height    == t.height
     && m.mipLevels == t.mip_count && m.format  == t.dxgi_format
     && m.arraySize == t.slice_count && is_cube == (t.is_cubemap != 0);
```

An independent implementation, written by Microsoft, reading bytes we produced.

`arraySize` and `miscFlags` are part of that comparison, and were not always.
They were parsed into the metadata struct and never compared, which is exactly
how a cubemap encoding bug — every emitted cube declaring six times the faces its
payload held — sat behind a clean "0 header mismatches". A surface count the
loader disagrees with is a header mismatch like any other.

**`TexMetadata::arraySize` is already in faces.** The DDS field on disk counts
cubes and the loader multiplies by 6 for `MISC_TEXTURECUBE`, but that happens
during parse, so it is done by the time the verifier sees it. Multiplying again
here fails every cubemap. Measured rather than reasoned: `starmap.cubemap` reads
`arraySize` 1 at byte 140 of the emitted DDS and 6 out of
`GetMetadataFromDDSMemory`, while a non-cube 32-slice array reads 32 both times.

## Oracle 2 — mip-chain arithmetic vs. the archive

The header check validates the header. It says nothing about whether the *payload*
matches, because DirectXTex does not verify size on load.

So: compute how many bytes a mip chain of that exact format and extent **must**
occupy, from the DXGI format definitions, in code deliberately written separately
from the library. Compare against the size of the buffer the archive actually
stores.

Those two numbers come from completely different places — one from CR2W metadata
we decoded, one from the archive segment table. A wrong DXGI format or mip count
cannot make them agree by accident.

Worked example, 1024×1024 BC7 with 11 mips:

```
blocks: 65536+16384+4096+1024+256+64+16+4+1+1+1 = 87,383
87,383 x 16 bytes = 1,398,128
archive segment size = 1,398,128        exact
```

Cubemaps multiply through the same way: `starmap` is 6 × 349,552 = 2,097,312,
which is `data_size` exactly.

## Oracle 3 — `CMesh.boundingBox` vs. computed chunk bounds

Chunk bounds are computed by dequantizing vertex positions. `CMesh` carries its
own whole-mesh box, written by CDPR's cooker and **never read by that code path**.
So the union of computed chunk boxes must fall inside the stored box. A wrong
stride, a wrong quantization scale or a misread offset all push vertices outside
it immediately.

Containment rather than equality: the stored box legitimately includes padding and
covers morph/LOD range the resident chunks do not. On single-chunk meshes the
computed box reproduces CDPR's exactly, which is a stronger signal than
containment and was seen repeatedly.

Chunks with no usable geometry are excluded on the API's own
`redfs_mesh_chunk->bounds_valid` flag. The sweep used to infer "no bounds" from
"all six floats are zero", which also discards any real chunk centred on the
origin with zero extent.

The tolerance is per axis and scales with the mesh:

```
eps = max(1e-4, extent/32767, |coord| * 1e-6)
```

Both terms are justified below, under *When the check was the problem*.

## Oracle 4 — the two mesh entry points must agree

`redfs_mesh_desc_of` reads the CR2W only; `redfs_mesh_open` decodes the geometry.
They reach chunk and appearance counts by different routes, so a disagreement
means one of them is wrong — and for a long time one was, reporting the array
count a file *declared* rather than the number that actually walked. A caller
looping to `desc.submesh_count` and indexing with `redfs_mesh_chunk_at` was the
documented pattern.

This is a differential check between two implementations rather than an external
oracle, so it is weaker than 1–3: both could be wrong the same way. It is included
because it is the only check that catches a class of bug — two exports describing
one file inconsistently — that no single-entry-point test can see. The verifier
now fails the run on a disagreement rather than counting it silently.

## Oracle 5 — canonical FNV vectors

Path hashing is checked against the published FNV-1a test vectors, not against our
own expectations:

```
"a"          0xAF63DC4C8601EC8C
"foobar"     0x85944171F73967E8
"hello"      0xA430D84680AABD0B
"127.0.0.1"  0xAABAFE7104D914BE
```

Plus a normalisation assertion that `Base/Icon/Foo.XBM` and `base\icon\foo.xbm`
hash identically.

## Oracle 6 — the format's own redundancy

`redfs_cli selftest` samples ~400 files strided across the whole depot (all 57
archives of the reference install, `content` plus `ep1`), reads each with
`REDFS_PART_ALL`, and checks the produced size against what the index declared.
Oodle's decoder is internally checksummed and fails on corrupt input, so exact
sizes across hundreds of compressed files means offsets and lengths are right.

A weaker but useful signal: CR2W parses yield **coherent depot paths** in import
tables. Random bytes do not produce
`base\materials\multilayered_terrain.mt`.

## Current results

Run as part of `run-checks.ps1 -GameDir ...` at `VerifyCount` 12000:

```
11255 textures checked
  0 header mismatches vs DirectXTex
  0 payload size mismatches
  0 skipped (format not in the reference table)

12000 meshes checked
  0 chunk unions escaping the stored CMesh box
  0 where desc_of and mesh_open disagree about counts

selftest: sampled files decoded, sizes match the index
```

Whole-install context for the texture figure: 55,855 textures, of which 5 are
cubemaps — 0.009%. That ratio is why the sample size matters, below.

## What the sweep found

Four real defects, all of which had shipped as "working":

1. **`texture_desc_of` describing a mesh's embedded texture.** A `.mesh` contains
   its own `CBitmapTexture` chunks, so searching by class found the wrong one and
   read `setup` off `CMesh`, which has no such field. 153 of 400 sampled files
   failed. Every failure reported the fallback format, which is what made the
   pattern legible.

2. **`serializationDeferredDataBuffer`** — lowercase `s`, in texture resources
   only. The exact-match comparison missed and `textureData` decoded as an enum
   name. Benign *by accident*: the fallback buffer index was 0 and textures have
   one buffer, so the wrong decode gave the right answer. Precisely the class of
   bug internal consistency cannot catch.

3. **Mip-biased headers.** A minority of textures record the biased extent while
   the buffer holds the unbiased surface, producing a DDS that decodes to garbage.
   Detected because header-implied size and payload size disagreed.

4. **Every emitted cubemap declaring 6× its faces.** `arraySize` counts cubes on
   disk, so writing the face count there declared 36 faces for a 6-face payload
   and no loader could open the result. Found only once oracle 1 compared
   `arraySize` at all.

RedFS's corrected cubemap output was then confirmed by decoding pixels through
`ConvertFromDds`, with a negative control that patches `arraySize` 1 → 6 to
recreate the original bug: all five cubemaps in the install decode as shipped, and
all five fail with `ERROR_HANDLE_EOF` under the control. That was a one-off
confirmation, not part of the automated sweep.

## When the check was the problem

Worth recording separately, because the fix in each case is principled rather than
"loosen until green".

**A fixed epsilon flags large meshes for being large.** A mesh reported its chunk
union escaping the stored box by 0.006 — on a mesh **394 units tall**. One int16
quantization step there is 394/32767 ≈ 0.012, so the gap was half a step.

**And far-from-origin meshes for being far away.** After scaling by extent, two
more failed at world coordinates ~2800, where float32 has ~2.4e-4 between
representable values. The printed computed and stored boxes were *identical to
three decimals*; the difference was a few ULPs.

Both were tolerance bugs, identified as such because the printed values agreed.
Hence `max(1e-4, extent/32767, |coord| * 1e-6)` per axis — two terms for two
physical error sources, with the reasoning in the code so nobody later reads it as
an arbitrary fudge.

**A new check producing a false alarm.** The `arraySize` comparison added to catch
a false-clean result multiplied by 6 for cubemaps, which the loader had already
done. It failed every cubemap in the sample. Same confusion as the bug it was
added to find, pointed the other way.

**A clean result from never reaching the case.** `VerifyCount` defaulted to 4000.
The sampled depot contains exactly one cubemap and it sits at about index 11,000,
so every run at the old default skipped the cubemap encoding path entirely — and
reported "0 header mismatches" while doing so. True, and meaningless for cubemaps.
The default is now 12000, which costs about 18 s.

## Running it

```
redfs_cli --game "<install>" selftest

redfs_verify <path-to-texconv.dll> "<install>" [count]
```

`count` bounds both the texture and mesh sweeps. Non-zero exit on any failure,
including an oracle-4 disagreement.

`texconv.dll` comes from `WolvenKit.Common/lib/`. Without it `redfs_verify` cannot
run oracle 1; `run-checks.ps1` skips the leg with a warning rather than failing,
and the remaining checks are self-contained.

## Gaps in the verification itself

- **Nothing has run inside the game.** Restated because it bounds every result
  above: these are offline checks on archive bytes, not evidence about a plugin
  loaded into Cyberpunk's process. Loader-lock behaviour, RED4ext teardown and
  contention with the engine's own I/O are covered only by the lifecycle tests'
  approximation of them (see `testing.md`).
- **No round-trip against WolvenKit's own extraction.** Building the WolvenKit CLI
  was judged not worth the time given the oracles above, but a byte-for-byte
  comparison on a sample would be the strongest check available.
- **Pixel content is not decoded by the sweep.** Sizes and headers are verified
  automatically; that the bytes form a *correct image* was demonstrated only for
  the five cubemaps, by hand. Wiring the `ConvertFromDds` leg into `redfs_verify`
  would close this. It was dropped earlier because .NET marshals `System.Boolean`
  as a 4-byte `BOOL`, so `vflip`/`hflip` need `int32_t` rather than C++ `bool`,
  and even corrected it crashed inside the DLL — the cubemap confirmation shows
  the call can be made to work, so the obstacle is effort, not feasibility.
- **Audio is unverified against anything external** beyond container sniffing.
  The unit tests cover WEM header parsing, PCM duration and RIFF chunk walking,
  but no third party has been asked to agree.
- **Kraken decode has no unit coverage.** It is exercised here and only here —
  every synthetic fixture is stored uncompressed. See `testing.md`.
- **Meshes reporting no computable bounds** are counted by the sweep but not
  investigated.
