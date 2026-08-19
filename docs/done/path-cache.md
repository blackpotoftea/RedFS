# The path cache

**Status: implemented** — `src/paths.cpp`, `redfs_path_cache_*`

## The problem

The hash → path direction is a dictionary (see `path-hashing.md`), and the only
source that covers paths a *mod* invented is CR2W import tables. Those are free
per file — but only for files you read. Filling the dictionary from them means
reading every file that has them, once, and on a modded install that is
**~180 seconds** of startup.

The result is 652,594 paths at a mean of ~60 bytes. It costs minutes to produce
and nothing to keep. So keep it.

## Why this is not the mesh cache

`caching.md` describes a cache whose whole design problem is *invalidation*: an
archive replaced in place changes the bytes behind a hash, so the cached geometry
becomes wrong, silently, forever. Its fingerprint exists to catch that.

A path dictionary has the opposite property, and the difference decides the
entire design:

> **A hash → name mapping is a fact about the string.**
> `fnv1a64("base\characters\foo.mesh")` is what it is whether that archive is
> mounted, removed, or re-cooked. Nothing a user installs can make a learned name
> wrong.

`redfs.h` already commits to this. Sources 1 and 3 — import learning and
`redfs_path_add` — were never filtered against the depot, and the header states
that a hit tells you what a file is **called**, not that it is readable. A cache
holding names for a mod the user has since removed is therefore the *documented
contract*, not a stale answer: the read still returns `REDFS_E_NOT_FOUND`.

So the file is **never discarded**. Restore is unconditional and only ever adds.

### What the fingerprints are actually for

Not "is this file still valid" but **"which archives have I already read"** — one
digest per archive, the same five fields `depot_fingerprint` mixes (path, entry
count, index size, index CRC, declared file size), scoped to one archive.
`redfs_path_cache_pending` reports the ones absent from that set.

Install a mod and you harvest that mod. On the numbers above, a session that adds
one archive costs a fraction of a second instead of three minutes.

Per-archive granularity was originally deferred for the mesh cache because the
expensive half is eviction bookkeeping — knowing which entries came from which
archive so you can drop them. **For paths there is nothing to evict.** The set is
union-only, so the cheap half is all there is.

### Two things it fixes for free

- **Mount order.** `depot_fingerprint` mixes archives in the order they were
  mounted, so the same archives mounted the other way round give a different
  `uint64_t`. For the mesh cache that costs a re-warm in milliseconds. Here it
  would cost the whole teach, over a depot that did not change. Comparing a *set*
  of per-archive digests is immune by construction.
- **Mounting after opening the cache.** `pending` is computed against the depot
  as it is when you ask, so a late mount needs no wiring at all — no equivalent
  of `cache_invalidate` exists or is needed.

`archive_fingerprint` also lowercases the path before mixing, which
`depot_fingerprint` does not. Windows paths are case-insensitive, so two
spellings of one file are one archive; treating them as two costs a re-harvest,
and a re-harvest is minutes where the mesh cache's equivalent is milliseconds.

## File format

Little-endian throughout.

```
'RFPC' u32 | version u32 | archive_count u32 | path_count u32
archive_count x u64 archive fingerprint
path_count    x { u32 len, bytes }        -- no trailing NUL
```

There is no depot-wide fingerprint field. The per-archive list replaces it.

**Hashes are not stored.** Every route into the dictionary interns the string in
the same form it hashed: `paths_load` interns whichever of the raw line and the
sanitized one produced the hash that resolved, and `paths_add` and
`paths_learn_imports` both sanitize before hashing. So `fnv1a64(stored)` recovers
the key by construction, and storing it as well would only add a way for the file
to contradict itself. At 652,594 paths that is also ~5 MB of file saved.

Written through a sibling `.tmp` plus `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`,
with `fflush` + `_commit` before the rename — the same discipline as the mesh
cache, and not optional at 40 MB. `fwrite`'s return value reports bytes accepted
into the `FILE` buffer, not bytes that reached the disk.

## Four things that are silent when wrong

Each of these fails in a way no error surfaces, which is why each has a test.

**1. Restore must not filter.** Sending restored entries through `paths_load`'s
route would run them past `depot->locate`. The dictionary then comes back
*smaller* on the second run than the first: a name learned yesterday from a mod's
import table quietly stops resolving, and nothing logs it, because dropping
unresolvable lines is exactly what `redfs_path_load` is supposed to do. Restore
goes through `add_locked` — the same entry point `paths_add` uses — which also
supplies dedupe and the `kMaxPathLength` bound.

**2. Opening the cache must enable the dictionary.** Import learning is off until
something switches it on. A restore that filled the dictionary without enabling
it hands back a full dictionary that then never grows — every mod installed after
the file was written stays unlearned for the life of the process. It has to
happen in `path_cache_open` itself, *not* on the restore path: the very first run
has no file to restore and returns early, which is precisely the run that had
everything to learn. (This was a real bug; the cross-process test caught it.)

**3. A partial restore must not keep its coverage.** The digests are at the
front of the file and the paths at the back, so a tail truncation restores
*every* digest and only *some* paths. Keeping them publishes full coverage over a
partial dictionary: `pending` answers "nothing to do", the host reads nothing,
and the lost paths never come back — and because the truncation also forces a
rewrite, that lie gets written down and survives every later run. So any restore
that is not a faithful image of the file drops `harvested` entirely. Coverage is
cheap to re-derive; the paths are not.

"Faithful" is stricter than `loaded == declared`, because a file with duplicate
or empty records can satisfy that while still not being what a flush would write.
When the dictionary started empty — the only case where the arithmetic is exact —
the restore also checks that each record actually produced an entry.

**4. Restore must not mark the cache dirty.** Restore goes through the ordinary
insert path, so a plain dirty *flag* would be set by the restore itself and
rewrite a byte-identical 40 MB file at every start-up. Instead the cache records
what the last successful write contained and compares sizes — both the dictionary
and the harvested set only ever grow, so matching counts mean the file is
current. The mesh cache does not have this problem: its loader writes `entries`
directly and bypasses the insert path entirely.

## Deliberately left out

- **A refcount or owner for the cache itself.** There is one per process and the
  second `open` flushes the first and takes over. Two plugins sharing one
  `RedFS.dll` therefore share it — but they already share the *dictionary*, so
  two cache files would hold the same paths regardless. One of them should own
  it, and `redfs.h` says so.
- **An `owner` depot.** The mesh cache tracks one and bypasses for any other,
  because a hash means different bytes in a different depot. The dictionary is
  process-global and already merges across depots, so an owner here would be new
  semantics, not a mirror. This is why `redfs_path_cache_open` takes no depot at
  all while `pending` and `mark` do: those genuinely need one, and a parameter
  that went unused would imply a scoping that does not exist. It works out
  correctly too — an archive fingerprint identifies a physical file, so an
  archive harvested through one depot is harvested for any depot that mounts it.
- **Automatic marking at flush.** `redfs_path_cache_mark` is per archive, called
  as the host finishes each one. Stamping the whole mounted set at flush would
  record coverage the host does not have whenever a teach dies halfway — and
  those paths would then never be learned.
- **Periodic flushing.** The file is rewritten whole. Fine at close, wasteful on
  a timer.
- **Removing names for archives that are gone.** They stay resolvable. That is
  the existing dictionary contract, and the read returns `REDFS_E_NOT_FOUND` as
  it always did.

## Measured

652,594 paths averaging ~60 bytes, on the reference machine, in two processes so
the restore starts from a dictionary that has never held anything:

| | |
|---|---|
| **Restore** | **658 ms** — against the ~180 s teach it replaces, **~270×** |
| Flush (`fwrite` + `_commit` + rename) | 88 ms, **41.1 MB** |
| Shutdown, nothing new learned | **0.006 ms** |
| Filling the same dictionary in memory, for reference | 971 ms |

Restore is not free, but it is the same work `redfs_path_load` already does for a
list of that size: it goes through `add_locked` like every other source, so it
pays the same intern and dedupe, and the arena it fills is the one the teach
would have produced anyway.

The last row is the one that matters for `redfs_shutdown`, which flushes and runs
in RED4ext's `Main(Unload)`. The 88 ms only lands on a session that actually
learned something — a first run, or one where a mod was installed — and that
session has just spent minutes harvesting, so it is not the cost anyone notices.
Every other shutdown short-circuits on the dirty check and writes nothing. On a
mechanical disk or a synced folder the 88 ms will grow; it is still one-time.
