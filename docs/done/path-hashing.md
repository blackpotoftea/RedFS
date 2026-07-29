# Paths, hashes, and the direction that does not exist

**Status: implemented** — `src/archive.cpp` (forward), `src/paths.cpp` (reverse)

## Forward: path → hash

Pure function. Normalise, then FNV-1a 64.

### Normalisation

Four steps, matching WolvenKit's `ResourcePath.SanitizePath`:

1. Trim `' " / \ space \n \r` from **both** ends
2. Collapse runs of separators to one
3. `/` → `\`
4. ASCII lowercase

So `Base/Icon/Foo.XBM` and `base\icon\foo.xbm` produce the same key. The unit
tests pin all four steps plus a leading separator, and the selftest re-checks the
case-and-separator fold against the live install.

A path that normalises to nothing — `""`, `"///"`, a null pointer — hashes to **0**
rather than to the FNV offset basis, so a caller can tell a rejected path from a
real key instead of getting a plausible-looking one. The tests pin that too.

### FNV-1a 64

Standard, no CDPR modifications:

```
offset basis  0xCBF29CE484222325
prime         0x00000100000001B3
h = basis; for each byte b: h = (h ^ b) * prime
```

Over the ASCII bytes, **no trailing NUL**. The canonical FNV test vectors are an
external oracle rather than a record of our own output, and both the unit tests
and the selftest run them:

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

### 1. CR2W import tables (free, but opt-in)

Every cooked resource names its dependencies as **real path strings** in its
import table. So reading any file teaches us paths at zero marginal cost, and
`cr2w_parse` feeds them in automatically.

With one condition: the dictionary has to be switched on first, by
`redfs_path_load` or by `redfs_path_enable`. Until one of those is called
`paths_learn_imports` early-returns on the enabled flag and **nothing is learned
at all** — a host that loads no list and never calls `redfs_path_enable` ends up
with an empty dictionary no matter how much of the depot it reads. Learning into a
dictionary nobody will query is pure overhead, so off is the right default; it
just means "free and automatic" only holds after the switch.

This is the only source that can know a path a **mod** invented, because no
shipped dictionary was built when that mod existed. It is also self-reinforcing:
the more of the depot you touch, the better coverage gets.

### 2. A path list on disk (the bulk source)

WolvenKit ships `WolvenKit.Common/Resources/usedhashes.kark` — a KARK-compressed,
newline-separated list, 3.4 MB on disk.

RedFS reads either form: if the first four bytes are `KARK` it decompresses with
the same Oodle path used for archive segments, otherwise it treats the file as
plain text. So any list of one-path-per-line works. The declared decoded size is
capped at 256 MB before that allocation is made, because the buffer is committed
and zero-filled before Oodle is ever asked whether the remaining bytes could
plausibly decode to it — under a loose bound a nine-byte file makes RedFS
allocate gigabytes.

### 3. `redfs_path_add`

For anything the caller knows itself.

## The filtering decision

The shipped list decompresses to **~135 MB** of text, which is a lot to hold
resident in a game process. The mitigation: **keep only the paths whose hash
resolves in the mounted depot**, and let the decompressed text go at the end of
the call. An unresolvable path is useless — you cannot read the file — so
retaining it costs memory for nothing.

That is a real cut, not a rounding: the list spans more than any single install
ships, and roughly a quarter of its lines survive on a stock depot, leaving
**~40 MB** resident as interned strings plus the index. These are the two figures
that get confused with each other; 135 MB is transient, 40 MB is what stays.

Filtering also makes coverage measurable rather than notional, because what
survives is exactly "depot files RedFS can name". `redfs_cli paths` reports it
against the depot's file count, and on the reference install it is **544,496 of
544,670 files — 99.97 %**.

**Filtering applies only to the loaded list.** Import learning and
`redfs_path_add` are unfiltered, and structurally cannot be otherwise:
`paths_learn_imports` runs inside `cr2w_parse`, which has no depot and cannot be
given one because `redfs_cr2w_open` takes bytes rather than a depot, and
`redfs_path_add` receives only a string. So a hit means "this is what the file is
called", not "this file is readable" — check `redfs_exists`, or just handle
`REDFS_E_NOT_FOUND` from the read.

That claim has now been wrong twice, in both directions: first the header
promised filtering on all three sources, then this page fixed it by exempting
import learning and left `redfs_path_add` on the wrong side of the line. Two
sources, one rule — the depot filter exists only where a depot does.

## Storage

An arena of fixed 1 MiB blocks holding NUL-terminated strings, plus a sorted
`{u64 hash, const char* str}` index — 16 bytes per entry, binary-searched. A path
longer than a block gets an exact block of its own, which is not adopted as the
current one since it has no room left.

The arena matters more than it looks. This was originally one flat
`std::vector<char>` with `{u64 hash, u32 offset}` entries, which looks smaller —
except it is not, because the entry struct pads to 16 bytes either way, so the
pointer is free. And the vector could not keep the promise the API makes:
`redfs_path_from_hash` hands back an interior pointer, and any later insert may
reallocate the buffer under pointers already returned. Small tests miss that; the
documented usage pattern hits it, because resolving a hash and then opening that
mesh parses a CR2W and learns its imports. Arena blocks are never moved and never
freed, so an interned pointer is valid for the lifetime of the process and never
has to be freed by the caller.

Additions land in a pending list, which is merged into the sorted index once it
passes 4096 entries — and on any lookup or count, which needs the index whole to
answer. So learning from imports during a read burst does not re-sort. The merge
sorts only the new run and `inplace_merge`s it, because re-sorting the whole
dictionary each time made a full load quadratic — introsort cannot exploit the
fact that all but the trailing few thousand elements are already in order. This
lowers the constant, **not** the complexity class: there are still O(N/B) merges,
each linear, and the duplicate check still scans `pending` linearly. At the
dictionary sizes involved neither is worth more structure.

## Which string gets interned

For the list, the raw line is hashed first and only falls back to the sanitising
hash if the raw hash does not resolve in the depot — the shipped list is already
normalised, so normalising every line again would be wasted work on the common
path. Whichever form produced the winning hash is the form that gets interned. A
line that takes the fallback typically has a leading quote or space, which the
line trim leaves and `sanitize_path` removes, so interning the raw line would
hand callers back text that is not the canonical path.

Import strings get no such benefit of the doubt: they are archive content with no
guarantee of being canonical, so they are always sanitised before hashing.
Hashing `Base/Foo.mesh` raw filed it under a key `redfs_hash` can never produce,
leaving the entry permanently dead and the real path unresolvable.

## Practical consequence

Reverse lookup is what makes a live component's `mesh` field legible. On its own
it is an opaque `u64`; with the dictionary it is a path, and from there a chunk
table with bounding boxes. That join is the reason the feature exists — see
`../API.md`.

It also makes the CLI usable for exploration, which is how the character meshes
in `mesh-geometry.md` were found:

```
$ redfs_cli --game <dir> find usedhashes.kark "tshirt" 6
  0x014991A3FFB92B8D      50808  base\characters\garment\light_crowd\torso\...\t1_004_ma_shirt__mexico_under_lc.mesh
```

## Known gap

The LXRS footer in WolvenKit-built mod archives lists that archive's own paths
and is not yet parsed. It would seed the dictionary for modded content without
needing any file to be read first, closing the lag in source 1. See
`../roadmap.md`.
