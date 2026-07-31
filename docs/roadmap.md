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

### `redfs_path_learn_all(depot)`
**Cost dominated by one full sweep; unmeasured.** `redfs_find` can only match what
the dictionary knows, and today that means shipping a path list. Walking every
CR2W and harvesting its imports is the other way to fill it — but it decodes the
main segment of every file in the depot, 544,670 of them on the reference install.

So it is only worth having if it is paid **once**: cancellable, progress-reporting,
off the calling thread, and persisted with the same archive-set fingerprint the
mesh cache uses (`redfs_cache_open`), so a restart does not repeat it. Without the
persistence it is a worse deal than shipping WolvenKit's list.

**It cannot be complete, and that caps its value.** Import harvesting only ever
learns a path some *other* file references, so every unreferenced root — the
entry-point `.ent` files most of all — stays invisible no matter how thorough the
sweep. For modded content specifically, LXRS footer parsing (below) is cheaper at
~2 hours, reads no file contents at all, and gets the archive's own path list
rather than an inferred one. **Do that first.** This item is for the case where
neither a shipped list nor a footer covers what you need.

### A resource handle that owns its bytes
**~half a day.** `redfs_read(hash, part)` is the only way to reach a resource that
has no typed helper — `.ent`, `.app`, `.rig`, `.anims` — and it makes the caller
name a segment. `part` is a `uint32_t` overloading three meanings, and `0` is
valid-but-wrong: it selects attached buffer 0 where most callers mean the document.
Textures, meshes and audio are unaffected; they already have one-call paths that
take no part number.

Be exact about how `0` fails, because it splits by file shape (`resolve_part`,
`src/archive.cpp:252-264`, computes `seg = start + 1 + part`):

- **Single-segment files — including the `.ent`/`.app`/`.rig` above — return
  `REDFS_E_RANGE`.** They fail loudly. The reported incident bears this out: 288,302
  of 662,485 reads came back "out of range".
- **Files WITH attached buffers return buffer 0's raw payload and `REDFS_OK`.**
  That is the silent case, and it is what `cr2w_open` then reports as corrupt data
  — 209,228 of the same run.

So the argument for the handle is not "it always fails silently"; it is that one
integer produces two unrelated failures, neither naming the real cause.

`redfs_open` / `redfs_resource_type` / `redfs_resource_buffer(i)` removes the
argument rather than renaming it, and folds in the blob/CR2W lifetime pairing that
`USAGE.md` currently warns about in prose. `Depot::open_resource` in `redfs.hpp` is
the same shape already, minus the ownership.

Renumbering `part` so 0 means the main segment was considered and **rejected** —
but not for being a silent ABI break. `REDFS_ABI_VERSION` exists for exactly this
("the meaning of a call", `redfs.h:44`) and `abi_ok()` makes a mismatch a hard
refusal, so it would be the loudest break available. The real objection is that it
desynchronises `part` from the three public 0-based buffer fields
(`redfs_value.as.buffer`, `redfs_texture_desc.buffer_index`,
`redfs_mesh_desc.render_buffer_index`): shift those too and that is three more
silent breaks, leave them and the two numbering schemes move out of a comment and
into the structs.

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
