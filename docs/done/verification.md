# How correctness was established

**Status: implemented** — `tools/redfs_verify.cpp`, `redfs_cli selftest`

## The principle

A format reader can be entirely self-consistent and entirely wrong. It parses
without error, produces plausible numbers, and every internal check agrees —
because all of them derive from the same misreading.

So the only checks that count here are ones whose answer comes from **outside the
code being tested**. Each one below names its oracle.

Every finding in this document was produced by these checks. None was found by
reading the code.

## Oracle 1 — DirectXTex parses our DDS headers

RedFS builds DDS headers from RED4 metadata. Rather than trust our reading of the
DDS spec, the result is handed to **DirectXTex** — via the `texconv.dll` that
ships with WolvenKit — and its parse is compared to what RedFS claimed:

```c
TexMetadata m = GetMetadataFromDDSMemory(dds.data, dds.size, 0);
agree = m.width == t.width && m.height == t.height
     && m.mipLevels == t.mip_count && m.format == t.dxgi_format;
```

An independent implementation, written by Microsoft, reading bytes we produced.

*(The `ConvertFromDds` leg — full decode to PNG — was dropped. .NET marshals
`System.Boolean` as a 4-byte `BOOL`, so the `vflip`/`hflip` parameters need
`int32_t` rather than C++ `bool`; even corrected it crashed inside the DLL. Not
worth chasing once the payload check below proved stronger.)*

## Oracle 2 — mip-chain arithmetic vs. the archive

The header check validates the header. It says nothing about whether the *payload*
matches — DirectXTex does not verify size on load.

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

## Oracle 3 — `CMesh.boundingBox` vs. computed chunk bounds

Chunk bounds are computed by dequantizing vertex positions. `CMesh` carries its
own whole-mesh box, written by CDPR's cooker and **never read by that code path**.

So the union of computed chunk boxes must fall inside the stored box. A wrong
stride, a wrong quantization scale, a misread offset — all push vertices outside
it immediately.

Containment rather than equality: the stored box legitimately includes padding
and covers morph/LOD range the resident chunks do not. On single-chunk meshes the
computed box reproduces CDPR's exactly, which is a stronger signal than
containment and was seen repeatedly.

## Oracle 4 — canonical FNV vectors

Path hashing is checked against the published FNV-1a test vectors, not against
our own expectations. Plus a normalisation assertion that
`Base/Icon/Foo.XBM == base\icon\foo.xbm`.

## Oracle 5 — the format's own redundancy

`redfs_cli selftest` samples 400 files spread across all 57 archives, decodes
every segment, and checks the produced size against the index. Oodle's decoder
is internally checksummed and fails on corrupt input, so exact sizes across
hundreds of files means offsets and lengths are right.

A weaker but useful signal: CR2W parses yield **coherent depot paths** in import
tables. Random bytes do not produce
`base\materials\multilayered_terrain.mt`.

## Current results

```
11255 textures checked
  0 header mismatches vs DirectXTex
  0 payload size mismatches
  0 skipped (format not in the reference table)

20000 meshes checked
  0 chunk unions escaping the stored CMesh box
  2 with no computable bounds (geometry absent)

selftest: 400/400 files decoded, 79.6 MB, sizes match the index
```

## What the sweep found

Three real bugs, all of which had shipped as "working":

1. **`texture_desc_of` describing a mesh's embedded texture.** A `.mesh` contains
   its own `CBitmapTexture` chunks, so searching by class found the wrong one and
   read `setup` off `CMesh`, which has no such field. 153 of 400 sampled files
   failed. Every failure reported the fallback format, which is what made the
   pattern legible.

2. **`serializationDeferredDataBuffer`** — lowercase `s` in texture resources
   only. The exact-match comparison missed and `textureData` decoded as an enum
   name. Benign *by accident*: the fallback buffer index was 0 and textures have
   one buffer, so the wrong decode gave the right answer. This is precisely the
   class of bug internal consistency cannot catch.

3. **Mip-biased headers.** A minority of textures record the biased extent while
   the buffer holds the unbiased surface, producing a DDS that decodes to
   garbage. Detected because header-implied size and payload size disagreed.

## Two failures that were the test's fault

Worth recording, because the fix is principled rather than "loosen until green".

A mesh reported its chunk union escaping the stored box by 0.006 — on a mesh
**394 units tall**. One int16 quantization step there is 394/32767 ≈ 0.012, so the
gap was half a step. The epsilon was a fixed `1e-3`, which flags large meshes for
being large.

After scaling by extent, two more failed — at world coordinates ~2800, where
float32 has ~2.4e-4 between representable values. Printed computed and stored
boxes were *identical to three decimals*; the difference was a few ULPs.

Both were tolerance bugs, identified as such because the printed values agreed.
The epsilon is now `max(1e-4, extent/32767, |coord| * 1e-6)` per axis, justified
by the two physical error sources, with the reasoning in the code so nobody later
reads it as an arbitrary fudge.

## Running it

```
redfs_cli --game "<install>" selftest

redfs_verify <path-to-texconv.dll> "<install>" [count]
```

`count` bounds both the texture and mesh sweeps; 20000 takes a few minutes.
Non-zero exit on any failure.

`texconv.dll` comes from `WolvenKit.Common/lib/`. Without it, `redfs_verify`
cannot run oracle 1; the rest of the checks are self-contained.

## Gaps in the verification itself

- **No round-trip against WolvenKit's own extraction.** Building the WolvenKit
  CLI was judged not worth the time given the four independent oracles above, but
  a byte-for-byte comparison on a sample would be the strongest check available.
- **Pixel content is never decoded.** Sizes and headers are verified; that the
  bytes form a *correct image* is inferred, not demonstrated. Restoring the
  DirectXTex PNG leg would close this.
- **Audio is unverified** beyond container sniffing.
- **The 2 meshes with no computable bounds** are reported but not investigated.
