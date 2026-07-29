# Open items

**Nothing on this page is implemented.** Everything below is a proposal, a
rejected alternative, or a known gap. Implemented work lives in `done/`.

Ordered by value. Every cost is a **guess**, not a measurement.

---

## Worth doing

### Load once inside the game
**Cost unknown; nothing like it has been tried.** RedFS has never run in the game
process. `CMakeLists.txt` builds two libraries, three tools and three test
binaries — no RED4ext plugin target — and nothing has ever been deployed to
`red4ext/plugins/`. `INTEGRATION.md` carries a `Main`/`Query`/`Supports`
skeleton that nothing in the tree compiles.

A minimal plugin — load, read one texture, log it, shut down on unload — would be
the first exercise of three paths that only exist for the in-game case:

- **`detect_game_dir` succeeding** (`src/api.cpp:195`). It walks up from the
  running executable looking for `archive\pc`; offline it starts in a build
  directory and finds nothing, so every test passes an explicit path.
- **Reusing the game's resident Oodle** (`src/oodle.cpp:46`). The
  `GetModuleHandleW` branch exists so RedFS does not pin a second reference to
  `oo2ext_7_win64.dll`. Offline it always misses and the `LoadLibrary` fallback
  resolves instead, so the branch that matters in the game is never the one taken.
- **Unload under RED4ext.** `lifecycle_test.cpp` drives a LoadLibrary/FreeLibrary
  cycle, but as its own host — not with the game's threads or RED4ext's ordering.

Every claim RedFS makes about in-game behaviour rests on offline evidence. This is
the item that replaces inference with observation.

### LXRS footer parsing
**~2 hours.** WolvenKit-built archives carry a `custom_data` block listing their
own file paths. Parsing it seeds the path dictionary for **modded** content
without needing any file read first, and without shipping a dictionary for mod
archives at all. Today a modded path is learned only when something references it
through a CR2W import table — which works, but lags.

None of the footer is read yet: `Archive::open` maps a 40-byte header, stopping one
field short of `custom_data_length` at 0x28. Layout in `done/archive-format.md`.

### `redfs_mesh_read_indices`
**~half a day** (the guess already in `vertex-streams.md`). Cheap,
self-contained, and enough on its own for custom collision or analysis.

`header.indexBufferOffset` is already on `redfs_mesh_desc`; the per-chunk half is
not read anywhere yet — `chunkIndices.teOffset` is the start relative to it,
`chunkIndices.pe` the width (`IBCT_IndexUShort` in the wild). Design in
`vertex-streams.md`, whose remaining sections are speculative; this piece is not.

### Account for the 2.47 GB peak
A large `redfs_verify` run peaks at **~2.47 GB** working set. It plateaus rather
than climbing, so this is transient, not a leak — but nobody has traced where the
peak comes from. Every read allocates a buffer for a whole segment
(`open_resource`, `audio_probe`, `texture_read_dds`), and the mesh cache is
unbounded by design, though `redfs_verify` never opens it. A heap snapshot at peak
would settle it. Worth knowing before anything ships into a game process.

### Ranged segment reads
**~half a day.** `Archive::read_segment` decompresses a whole segment and
`read_part` is all-or-nothing, so "read the first 16 bytes" does not exist:
`redfs_audio_probe` decodes an entire music `.wem` to look at its magic, which is
what `redfs.h` warns about at the declaration.

A `read_segment_range(seg, offset, length, dst)` **would not remove that decode.**
Kraken has no random access, so it still decompresses the segment internally; the
win is dropping the caller's full-size allocation and copy. Real random access
needs chunk-boundary awareness the format may not expose. That is why this sits
below the items above — it makes the audio work cheaper, not possible.

---

## Do when someone asks

### Voice-over demuxing
**~1 day.** `.opusinfo` is fully mapped, and one of its two open questions has
since been answered out of WolvenKit's own reader. Lower value than it appears:
the caller still needs libopus to hear anything, and `.wem` — which covers SFX —
already works through `redfs_read`. Full design in `audio-opus.md`.

### Full vertex attributes
**2–3 days.** Normals, tangents, UVs, colours, skinning. Table-driven off
`vertexLayout.elements`; the packed normal/tangent encodings are the fiddly part.

Held back deliberately: a mod that wants to *render* game geometry usually wants
the game to do it. No concrete caller has appeared. Design in `vertex-streams.md`.

### Console texture cooks
**~1 day, low value.** `rendRenderTextureBlobPS4`, `XboxOne`, `Prospero` and
`Scarlett` are **rejected rather than guessed at**, which is the right default — a
plausible-looking wrong image is worse than an error. `describe_texture` looks
only for `rendRenderTextureBlobPC` and fails with `REDFS_E_UNSUPPORTED` naming the
console cook as the likely cause. Only worth doing if console archives are in
scope, and for a PC modding library they are not.

### Env probes
**~2 hours.** `CReflectionProbeDataResource` holds a texture blob with no `setup`;
WolvenKit substitutes a default `STextureGroupSetup` (`RedImage.FromEnvProbe`).
Rejected today at `describe_texture`'s root-chunk check, before `setup` is
consulted at all. Three small changes: allow the class, follow
`textureData.renderResourceBlobPC` rather than
`renderTextureResource.renderResourceBlobPC`, and default the setup.

But note what defaulting yields: `TCM_None` + `TRF_TrueColor` maps to
`R8G8B8A8_UNORM`, a guess for an HDR probe, and the payload-size check that would
catch it currently logs rather than fails. Rarely wanted; do not ship it without
checking one probe's format against its payload size.

---

## Considered and rejected

### Hooking the game's `ResourceDepot`
Would mean the engine does the reading. **Rejected**: needs address-library
offsets and breaks on every game patch. Reimplementing the container natively
costs more code once and then survives patches indefinitely. This decision is what
makes the rest of the design coherent — see `done/api-design.md`.

### Precomputing every mesh at load
The archives never change, so caching is right — but there are ~10⁵ `.mesh` files,
so precomputing all of them costs minutes of startup and gigabytes of result.
**Lazy population with a persistent cache** reaches the same end state for the
meshes a mod actually touches. Reasoning in `done/caching.md`.

### Zero-copy read views
`redfs_read` could hand back a pointer into the file mapping for uncompressed
segments. **Rejected**: complicates ownership for a win that applies only to the
minority of segments stored raw.

### Writing archives
Out of scope. RedFS is read-only, and packing is what WolvenKit is for.

---

## Verification gaps

Tracked in `done/verification.md`. Three that matter; the first is not listed
there yet.

- **Kraken decode has no unit coverage.** Every fixture stores its segments
  uncompressed (`zsize == size`), because emitting a compressed one needs
  `OodleLZ_Compress` out of the game's DLL and `src/oodle.cpp` binds only
  `OodleLZ_Decompress`. So `read_segment`'s KARK branch — and the
  `zsize != size` with no KARK magic fallback beside it — is reached only by the
  integration sweeps against a real install: nothing in `redfs_test` covers
  either, and on a machine without the game nothing does at all. Fix is binding
  the compressor when it happens to be present, or checking in one compressed
  segment as test data.
- **No round-trip against WolvenKit's own extraction.** A byte-for-byte
  comparison on a sample would be the strongest available check. Not done because
  building the WolvenKit CLI was judged not worth the time given the independent
  oracles already in place.
- **Pixel content is never decoded.** Headers and payload *size* are verified —
  including `arraySize` and the cubemap `miscFlags` bit against DirectXTex — but
  that the bytes form a correct *image* is inferred. Restoring the DirectXTex PNG
  leg would close it: it needs the `BOOL` marshalling fix noted in
  `done/verification.md`, and then debugging why it still crashed.

---

## Out of scope permanently

**Live game state** — entities, components, RTTI. RedFS reads archives and touches
nothing in the running process, which is exactly what keeps it patch-proof.
`entityComponents(entity)` belongs in a RED4ext plugin; RedFS supplies the other
half of that join by turning a component's resource hash into a path, a chunk
table and a set of boxes. See `API.md`.
