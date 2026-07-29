# Textures: `.xbm` to a loadable DDS

**Status: implemented** — `src/formats.cpp`

## The problem

"Give me the bytes" is not what a caller wants. A cooked `.xbm` is a CR2W document
describing a texture, plus a *separate* buffer of GPU-ready pixels with no header
on it. Neither half is loadable by anything.

The job is to reunite them: synthesize a DDS header from the CR2W metadata and
staple the payload to it. Then `CreateDDSTextureFromMemory`, `LoadFromDDSMemory`
or a bare `CreateTexture2D` all work, with no temp files and no D3D device needed
at read time.

## Where the metadata lives

Two chunks, not one:

- **chunk 0** — the resource root: `CBitmapTexture`, `CTextureArray` or
  `CCubeTexture`. Carries `setup` (an `STextureGroupSetup`) with `compression`,
  `rawFormat` and `isGamma`: *how the payload was encoded at cook time*.
- **the blob** — `rendRenderTextureBlobPC`. Carries
  `header.sizeInfo.{width,height,depth}`,
  `header.textureInfo.{mipCount,sliceCount,type}`, and `textureData`, a deferred
  buffer index.

The root chunk's class is checked first, and the blob is reached by following
`renderTextureResource.renderResourceBlobPC` — a handle — rather than searching for
the first chunk of that class. Both of those turned out to matter; see bug 1.

## Format mapping

`compression` + `rawFormat` + `isGamma` → `DXGI_FORMAT`, mirroring WolvenKit's
`DDSUtils.GenerateHeader`. Enums arrive as strings (see `cr2w-format.md`), so this
is string comparison and needs no ordinal tables:

| compression | DXGI |
|---|---|
| `TCM_DXTNoAlpha` | BC1_UNORM(_SRGB) |
| `TCM_DXTAlpha`, `TCM_DXTAlphaLinear` | BC3_UNORM(_SRGB) |
| `TCM_Normalmap`, `TCM_QualityRG` | BC5_UNORM |
| `TCM_QualityR` | BC4_UNORM |
| `TCM_QualityColor` | BC7_UNORM(_SRGB) |
| `TCM_HalfHDR_Unsigned` | BC6H_UF16 |
| `TCM_Normals_DEPRECATED` | BC1_UNORM |
| `TCM_NormalsHigh_DEPRECATED` | BC3_UNORM |
| `TCM_None` | falls through to `rawFormat` |

with `TCM_None` resolving via `rawFormat`: `TRF_TrueColor` → R8G8B8A8_UNORM(_SRGB),
`TRF_DeepColor` → R16G16B16A16_UNORM, `TRF_HDRFloat` → R32G32B32A32_FLOAT,
`TRF_HDRHalf` → R16G16B16A16_FLOAT, `TRF_HDRFloatGrayscale` → R16_FLOAT,
`TRF_Grayscale` → R8_UNORM, `TRF_R8G8` → R8G8_UNORM, `TRF_Grayscale_Font` →
A8_UNORM. An unrecognised `rawFormat` falls back to R8G8B8A8_UNORM; an
unrecognised `compression` is `REDFS_E_UNSUPPORTED` rather than a guess.

## The DDS header

148 bytes: `'DDS '` + a 124-byte `DDS_HEADER` + a 20-byte `DDS_HEADER_DXT10`.
Always the DX10 form, so the real format lives in the DXT10 block and the legacy
`DDS_PIXELFORMAT` is just the `'DX10'` FourCC.

`dwPitchOrLinearSize` is computed properly — block count × block bytes with
`DDSD_LINEARSIZE` for BC formats, row pitch with `DDSD_PITCH` otherwise — even
though DirectXTex and DirectXTK both ignore it on load. Cubemaps set
`DDSCAPS2_CUBEMAP_ALLFACES` and `D3D11_RESOURCE_MISC_TEXTURECUBE`; volumes with
depth > 1 set `DDSCAPS2_VOLUME` and `D3D10_RESOURCE_DIMENSION_TEXTURE3D`.

### `arraySize` counts cubes; `sliceCount` counts faces

This is the one place two conventions have to be bridged. On disk `arraySize` is a
count of **cubes**: every loader multiplies it by 6 when `MISC_TEXTURECUBE` is
set — DirectXTK's `DDSTextureLoader.cpp` does
`if (miscFlag & D3D11_RESOURCE_MISC_TEXTURECUBE) { arraySize *= 6; }`. RED4's
`header.textureInfo.sliceCount` counts **faces**, which is also what the
mip-chain arithmetic wants. So RedFS writes `sliceCount / 6` for a cubemap and
`sliceCount` as-is otherwise.

Writing the face count instead declares 36 faces for a 6-face payload, and every
cubemap RedFS emitted failed to load with `ERROR_HANDLE_EOF`. WolvenKit's
`GenerateHeader` performs the same division (and throws if `arraySize % 6 != 0`),
which is the corroborating reading.

### Mip counts are capped at 32

`mip_chain_bytes` loops on a count read straight out of the file. 32 is where the
arithmetic stops meaning anything: level *m* has extent `max(1, w >> m)`, so for a
32-bit extent level 31 is the last that can differ from its predecessor, and
`w >> m` is undefined beyond that. Real content tops out at 15 (16384×16384).
`describe_texture` rejects a declared `mip_count > 32` outright rather than
clamping, so no caller is handed a descriptor built from a nonsense count. Before
the cap, a crafted `.xbm` spun for about 35 seconds on the caller's thread, spread
across the five `mip_chain_bytes` calls `describe_texture` can make.

## Three bugs the verifier found

All three shipped as "working" and were caught only by checking against something
outside the library.

### 1. Describing a mesh's embedded texture

`texture_desc_of` originally located the blob with
`find_chunk("rendRenderTextureBlobPC")` and read `setup` from chunk 0.

But **a `.mesh` embeds its own `CBitmapTexture` chunks** — the terrain mesh dumped
in `cr2w-format.md` has three. So calling the texture path on a mesh found the
mesh's first embedded texture blob, then read `setup` from `CMesh`, which has no
such field, fell back to defaults, and confidently reported `R8G8B8A8_UNORM` for a
BC7 payload.

153 of 400 sampled files failed this way. Every failure reported the fallback
format, which is what made the pattern legible.

Fix: require the **root chunk** to be a texture resource, and follow the
resource's own handle to its blob instead of searching by class.

### 2. `serializationDeferredDataBuffer`

Covered in `cr2w-format.md`. Lowercase `s` in texture resources only; the
exact-match comparison missed and `textureData` decoded as an enum name. Benign by
accident, because the fallback buffer index was 0 and textures have one buffer.

### 3. Mip-biased headers

A minority of textures record the **mip-biased** extent in `sizeInfo` while the
buffer holds the **unbiased** surface. One real case: the header says 128×128 with
8 mips — 10,936 bytes for BC1 — while the buffer holds 174,776 bytes, which is
exactly 512×512 BC1 with 10 mips. `setup.platformMipBiasPC` is 3 and
`CBitmapTexture` declares 1024×1024.

A DDS whose header disagrees with its payload decodes to garbage, so guessing is
not neutral here. `describe_texture` reconciles: compute the byte count the header
implies, and when it does not match the payload, try up to four power-of-two
rescales — `width << s`, `height << s`, `mip_count + s` — in 64-bit arithmetic, so
a large extent cannot wrap to zero and "match" a zero-size chain. The first
rescale that agrees to the byte wins and is logged; the payload is the actual GPU
resource, so it decides.

When no rescale fits, the header is returned as-is with both byte counts logged.
Rejecting would break stock content the four-shift search does not cover, and a
caller that wants to be strict has the numbers in the log.

WolvenKit has commented-out exploratory code around precisely this and ships the
header values unchanged, so its output has the same issue.

## Verification

Two oracles, both outside RedFS. Details in `verification.md`.

- **Header**: DirectXTex (WolvenKit's `texconv.dll`) parses the DDS and must
  report the same width, height, mip count, format, `arraySize` and `miscFlags`.
  The last two were being parsed and never compared, and that gap is exactly how
  the cubemap bug above sat behind a clean "0 header mismatches" result.
- **Payload**: independent arithmetic — how many bytes a mip chain of that exact
  format and extent *must* occupy, versus what the archive actually stores. These
  numbers come from different places, so a wrong format cannot match by accident.

```
11255 textures checked
  0 header mismatches vs DirectXTex
  0 payload size mismatches
```

The same run covers 12,000 meshes, also with no mismatches; that half is in
`mesh-geometry.md`.

A worked example of why the payload check is decisive: a 1024×1024 BC7 texture
with 11 mips. Block counts 65536+16384+4096+1024+256+64+16+4+1+1+1 = 87,383
blocks × 16 bytes = **1,398,128** — the exact byte count in the archive. A wrong
DXGI format or mip count cannot produce that.

### Cubemaps are rare enough to miss

Only 5 of the 55,855 textures on a stock install are cubemaps — 0.009% — and in
the sampled depot the single cubemap sits at roughly index 11,000. The sweep's
default sample had to be raised from 4,000 textures to 12,000 to reach it at all.
Until it was, the header check reported zero mismatches while never once
exercising the cubemap path: a true number that meant nothing for cubemaps.

## Not implemented

Console cooks (`rendRenderTextureBlobPS4`, `...XboxOne`, `...Prospero`,
`...Scarlett`) are **rejected rather than guessed at** — their tiling and layout
differ, and producing a plausible-looking wrong image would be worse than an
error. `CReflectionProbeDataResource` (env probes) is likewise not handled;
WolvenKit reads them by pairing the blob with a default-constructed
`STextureGroupSetup` (`RedImage.FromEnvProbe`).
