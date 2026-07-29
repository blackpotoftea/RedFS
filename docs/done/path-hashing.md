# Paths, hashes, and the direction that does not exist

**Status: implemented** — `src/archive.cpp` (forward), `src/paths.cpp` (reverse)

## Forward: path → hash

Pure function. Normalise, then FNV-1a 64.

### Normalisation

Exactly what the engine does, ported from `ResourcePath.SanitizePath`:

1. Trim `' " / \ space \n \r` from **both** ends
2. Collapse runs of separators to one
3. `/` → `\`
4. ASCII lowercase

So `Base/Icon/Foo.XBM` and `base\icon\foo.xbm` produce the same key. This is
tested — the selftest asserts they agree.

### FNV-1a 64

Standard, no CDPR modifications:

```
offset basis  0xCBF29CE484222325
prime         0x00000100000001B3
h = basis; for each byte b: h = (h ^ b) * prime
```

Over the ASCII bytes, **no trailing NUL**. Verified against the canonical FNV
test vectors, which the selftest runs on every invocation:

```
"a"          0xaf63dc4c8601ec8c
"foobar"     0x85944171f73967e8
"hello"      0xa430d84680aabd0b
"127.0.0.1"  0xaabafe7104d914be
```

### Decimal strings

Lua numbers are doubles and lose precision above 2⁵³, so a 64-bit key cannot
cross that boundary as a number. `redfs_hash_string` / `redfs_hash_parse` move it
as text. 21 bytes is always enough (`REDFS_HASH_STRING_MAX`).

## Reverse: hash → path

**This is not computable.** FNV-1a is one-way, and archives store no path table —
the string is discarded at cook time. Anything that claims otherwise is doing a
dictionary lookup, and so does RedFS.

Three sources, cheapest first.

### 1. CR2W import tables (free, automatic)

Every cooked resource names its dependencies as **real path strings** in its
import table. So reading any file teaches us paths at zero marginal cost, and
`cr2w_parse` feeds them in automatically once the dictionary is switched on.

This is the only source that can know a path a **mod** invented, because no
shipped dictionary was built when that mod existed. It is also self-reinforcing:
the more of the depot you touch, the better coverage gets.

### 2. A path list on disk (the bulk source)

WolvenKit ships `WolvenKit.Common/Resources/usedhashes.kark` — a KARK-compressed,
newline-separated list. 3.4 MB on disk, ~135 MB decompressed.

RedFS reads either form: if the first four bytes are `KARK` it decompresses with
the same Oodle path used for archive segments, otherwise it treats the file as
plain text. So any list of one-path-per-line works.

### 3. `redfs_path_add`

For anything the caller knows itself.

## The filtering decision

135 MB of path strings resident is a lot for an in-game plugin. The mitigation:
**only keep paths whose hash resolves in the mounted depot.**

The reasoning is that an unresolvable path is useless — you cannot read the file
— so retaining it costs memory for nothing. And a hit then carries a guarantee:
if `redfs_path_from_hash` returns a string, that file is readable right now, in
this install, with these mods.

Filtering also makes coverage measurable rather than notional. On the reference
install:

```
544,496 of 544,670 files resolve  --  99.97 %
```

Storage is one flat `std::vector<char>` of NUL-terminated strings plus a sorted
`{u64 hash, u32 offset}` index — 12 bytes per entry, no per-path allocation,
binary-searched. Additions land in a small pending list and merge in batches so
that learning from imports during a read burst does not re-sort on every file.

Hashing the list uses the raw line first and falls back to the sanitising hash if
that misses, since the shipped list is already normalised and re-normalising
every line would be wasted work.

## Practical consequence

Reverse lookup is what makes a live component's `mesh` field legible. On its own
it is an opaque `u64`; with the dictionary it is a path, and from there a chunk
table with bounding boxes. That join is the reason the feature exists — see
`../API.md`.

It also makes the CLI usable for exploration, which is how the character meshes
in `mesh-geometry.md` were found:

```
$ redfs_cli --game <dir> find usedhashes.kark "tshirt" 6
  0x014991A3FFB92B8D   50808  base\characters\garment\light_crowd\torso\...\t1_004_ma_shirt__mexico_under_lc.mesh
```

## Known gap

The LXRS footer in WolvenKit-built mod archives lists that archive's own paths
and is not yet parsed. It would seed the dictionary for modded content without
needing any file to be read first. See `../roadmap.md`.
