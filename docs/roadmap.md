# Open items

What is not done, why, and roughly what it would cost. Ordered by value.

Implemented work lives in `done/`.

---

## Worth doing

### LXRS footer parsing
**~2 hours.** Archives built by WolvenKit carry a `custom_data` block at
`Header.EXTENDED_SIZE` listing that archive's own file paths. Parsing it would
seed the path dictionary for **modded** content without needing any file to be
read first, and without shipping a dictionary at all for mod archives.

Today modded paths are only learned when something references them through a
CR2W import table — which works, but lags. Offset and length are already read
during mount; only the footer body is skipped.

Notes in `done/archive-format.md`.

### Ranged segment reads
**~half a day.** `Archive::read_segment` decompresses a whole segment. That is
right for almost everything, but wrong for two cases: sniffing the first bytes of
a large file (`redfs_audio_probe` currently refuses to look at anything over
1 MB), and pulling one voice line out of a `.opuspak`.

A `read_segment_range(seg, offset, length, dst)` would fix both. Kraken has no
random access, so this still decompresses the whole segment internally — the win
is avoiding the allocation and copy, not the decode. Real random access would
need chunk-boundary awareness that the format may not expose.

Prerequisite for the audio work below.

### `redfs_mesh_read_indices`
**~half a day.** Index buffers are already located —
`header.indexBufferOffset + chunkIndices.teOffset`, width from `chunkIndices.pe`.
Cheap, self-contained, and enough on its own for custom collision or analysis.

Design in `vertex-streams.md`; the rest of that document is speculative but this
piece is not.

---

## Do when someone asks

### Voice-over demuxing
**~1 day.** `.opusinfo` structure is fully mapped; the work is a ranged read and
two open questions. Lower value than it appears — the caller still needs libopus
to hear anything, and `.wem` (which covers SFX) already works through
`redfs_read`.

Full design in `audio-opus.md`.

### Full vertex attributes
**2–3 days.** Normals, tangents, UVs, colours, skinning. Table-driven off
`vertexLayout.elements`; the packed normal/tangent encodings are the fiddly part.

Held back deliberately: a mod that wants to *render* game geometry usually wants
the game to do it. No concrete caller has appeared.

Design in `vertex-streams.md`.

### Console texture cooks
**~1 day, low value.** `rendRenderTextureBlobPS4`, `XboxOne`, `Prospero`,
`Scarlett` are currently **rejected rather than guessed at**, which is the right
default — a plausible-looking wrong image is worse than an error. Only worth
doing if console archives are actually in scope, and for a PC modding library
they are not.

### Env probes
**~2 hours.** `CReflectionProbeDataResource` holds a texture blob with no
`setup`; WolvenKit handles it by substituting an empty `STextureGroupSetup`.
Rejected today. Trivial to add, rarely wanted.

---

## Considered and rejected

### Hooking the game's `ResourceDepot`
Would mean the engine does the reading. **Rejected**: needs address-library
offsets and breaks on every game patch. Reimplementing the container natively
costs more code once and then survives patches indefinitely. This decision is
what makes the rest of the design coherent — see `done/api-design.md`.

### Precomputing every mesh at load
The archives never change, so caching is right — but there are ~10⁵ `.mesh`
files, so precomputing all of them costs minutes of startup and gigabytes of
result. **Lazy population with a persistent cache** reaches the same end state for
the meshes a mod actually touches. Reasoning in `done/caching.md`.

### Zero-copy read views
`redfs_read` could hand back a pointer into the file mapping for uncompressed
segments. **Rejected**: complicates ownership for a win that only applies to the
minority of segments stored raw.

### Writing archives
Out of scope. RedFS is read-only, and packing is what WolvenKit is for.

---

## Verification gaps

Tracked separately in `done/verification.md`, but the two that matter:

- **No round-trip against WolvenKit's own extraction.** A byte-for-byte
  comparison on a sample would be the strongest available check. Not done because
  building the WolvenKit CLI was judged not worth the time given four independent
  oracles already in place.
- **Pixel content is never decoded.** Header and payload *size* are verified;
  that the bytes form a correct image is inferred. Restoring the DirectXTex PNG
  leg would close this — it needs the `BOOL` marshalling fix noted in
  `done/verification.md` and then debugging why it still crashed.

---

## Out of scope permanently

**Live game state** — entities, components, RTTI. RedFS reads archives and
touches nothing in the running process, which is exactly what keeps it
patch-proof. `entityComponents(entity)` belongs in a RED4ext plugin; RedFS
supplies the other half of that join by turning a component's resource hash into
a path, a chunk table and a set of boxes. See `API.md`.
