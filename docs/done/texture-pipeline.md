# Textures: `.xbm` to a usable DDS

**Status: implemented** — `src/formats.cpp`

## The problem

"Give me the bytes" is not what a caller wants. A cooked `.xbm` is a CR2W
document describing a texture, plus a *separate* buffer of GPU-ready pixels with
no header on it. Neither half is loadable by anything.

The job is to reunite them: synthesize a DDS header from the CR2W metadata and
staple the payload to it. Then `CreateDDSTextureFromMemory`,
`LoadFromDDSMemory` or a bare `CreateTexture2D` all work, with no temp files and
no D3D device needed at read time.

## Where the metadata lives

Two chunks, not one:

- **chunk 0** — the resource root: `CBitmapTexture`, `CTextureArray` or
  `CCubeTexture`. Carries `setup` (an `STextureGroupSetup`) with `compression`,
  `rawFormat` and `isGamma`: *how the payload was encoded at cook time*.
- **the blob** — `rendRenderTextureBlobPC`. Carries
  `header.sizeInfo.{width,height,depth}`,
  `header.textureInfo.{mipCount,sliceCount,type}`, and `textureData`, a deferred
  buffer index.

Reached by following `renderTextureResource.renderResourceBlobPC` — a handle —
rather than searching for the first chunk of that class. That distinction turned
out to matter; see the bug below.

## Format mapping

`compression` + `rawFormat` + `isGamma` → `DXGI_FORMAT`, mirroring WolvenKit's
`DDSUtils.GenerateHeader`. Enums arrive as strings (see `cr2w-format.md`), so
this is string comparison and needs no ordinal tables:

| compression | DXGI |
|---|---|
| `TCM_DXTNoAlpha` | BC1_UNORM(_SRGB) |
| `TCM_DXTAlpha`, `TCM_DXTAlphaLinear` | BC3_UNORM(_SRGB) |
| `TCM_Normalmap`, `TCM_QualityRG` | BC5_UNORM |
| `TCM_QualityR` | BC4_UNORM |
| `TCM_QualityColor` | BC7_UNORM(_SRGB) |
| `TCM_HalfHDR_Unsigned` | BC6H_UF16 |
| `TCM_None` | falls through to `rawFormat` |

with `TCM_None` resolving via `rawFormat`: `TRF_TrueColor` → R8G8B8A8_UNORM(_SRGB),
`TRF_HDRHalf` → R16G16B16A16_FLOAT, `TRF_Grayscale` → R8_UNORM,
`TRF_Grayscale_Font` → A8_UNORM, and so on.

## The DDS header

148 bytes: `'DDS '` + a 124-byte `DDS_HEADER` + a 20-byte `DDS_HEADER_DXT10`.
Always DX10, so the real format lives in the DXT10 block and the legacy
`DDS_PIXELFORMAT` is just the `'DX10'` FourCC.

`dwPitchOrLinearSize` is computed properly — block count × block bytes for BC
formats with `DDSD_LINEARSIZE`, row pitch otherwise with `DDSD_PITCH` — even
though DirectXTex and DirectXTK both ignore it on load. Cubemaps set
`DDSCAPS2_CUBEMAP_ALLFACES` and `D3D11_RESOURCE_MISC_TEXTURECUBE`; volumes set
`DDSCAPS2_VOLUME` and `D3D10_RESOURCE_DIMENSION_TEXTURE3D`.

## Three bugs the verifier found

This is the part worth recording, because all three shipped as "working" and were
only caught by checking against something outside the library.

### 1. Describing a mesh's embedded texture

`texture_desc_of` originally located the blob with
`find_chunk("rendRenderTextureBlobPC")` and read `setup` from chunk 0.

But **a `.mesh` embeds its own `CBitmapTexture` chunks** — the terrain mesh
dumped in `cr2w-format.md` has three. So calling the texture path on a mesh found
the mesh's first embedded texture blob, then read `setup` from `CMesh`, which has
no such field, fell back to defaults, and confidently reported
`R8G8B8A8_UNORM` for a BC7 payload.

153 of 400 sampled files failed this way. Every failure was the fallback format,
which is what made the pattern legible.

Fix: require the **root chunk** to be a texture resource, and follow the
resource's own handle to its blob instead of searching by class.

### 2. `serializationDeferredDataBuffer`

Covered in `cr2w-format.md`. Lowercase `s` in texture resources only; the
exact-match comparison missed and `textureData` decoded as an enum name. Benign
by accident because the fallback buffer index was 0 and textures have one buffer.

### 3. Mip-biased headers

A minority of textures record the **mip-biased** extent in `sizeInfo` while the
buffer holds the **unbiased** surface. One real case: header says 128×128 with 8
mips (10,936 bytes for BC1), buffer holds 174,776 bytes — which is exactly
512×512 BC1 with 10 mips. `setup.platformMipBiasPC` is 3 and `CBitmapTexture`
declares 1024×1024.

WolvenKit has commented-out exploratory code around precisely this and ships the
header values as-is, so its output has the same issue.

A DDS whose header disagrees with its payload decodes to garbage, so guessing is
not neutral here. RedFS reconciles: compute the mip-chain size the header implies,
and if it does not match the payload, try power-of-two rescales. If exactly one
makes them agree — and 512×512/10 mips does, to the byte — take that reading and
log it. The payload is the actual GPU resource; it wins.

## Verification

Two oracles, both outside RedFS. Details in `verification.md`.

- **Header**: DirectXTex (WolvenKit's `texconv.dll`) parses the DDS and must
  report the same extent, mip count and format we claimed.
- **Payload**: independent arithmetic — how many bytes a mip chain of that exact
  format and extent *must* occupy, versus what the archive actually stores. These
  numbers come from different places, so a wrong format cannot match by accident.

```
11255 textures checked
  0 header mismatches vs DirectXTex
  0 payload size mismatches
```

A worked example of why the payload check is decisive: a 1024×1024 BC7 texture
with 11 mips. Block counts 65536+16384+4096+1024+256+64+16+4+1+1+1 = 87,383
blocks × 16 bytes = **1,398,128** — the exact byte count in the archive. A wrong
DXGI format or mip count cannot produce that.

## Not implemented

Console cooks (`rendRenderTextureBlobPS4`, `...XboxOne`, `...Prospero`,
`...Scarlett`) are **rejected rather than guessed at** — their tiling and layout
differ and producing a plausible-looking wrong image would be worse than an
error. `CReflectionProbeDataResource` (env probes) is likewise not handled;
WolvenKit supports it via a synthetic empty `setup`.
