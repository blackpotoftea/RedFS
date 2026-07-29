# The mesh cache

**Status: implemented** — `src/cache.cpp`

## The problem

Per-chunk bounding boxes are not stored in the format (see `mesh-geometry.md`),
so they have to be computed by dequantizing each chunk's vertex positions. That
means Kraken-decoding the render buffer first, and the decode is the expensive
half — the min/max sweep is one linear pass over positions already in memory.

Measured over 200 meshes sampled by stride across a stock install, in
milliseconds:

```
min 0.05  |  median 0.72  |  mean 0.98  |  p90 2.27  |  max 7.14
```

So ~0.7 ms is typical and the largest meshes reach ~7 ms. **"Cold" throughout
this document means there is no RedFS cache entry, not a cold disk** — the
measurement ran with the OS page cache warm, so what it shows is decode cost, not
I/O.

Fine once. Not fine if the answer is wanted repeatedly, per frame, or per entity.

## Why precomputing everything does not work

The obvious idea — walk the depot at load, compute every mesh, done — was
considered and rejected on arithmetic: the depot holds on the order of **10⁵
`.mesh` files**, and at the measured mean of ~1 ms each that is **a minute and a
half of startup, or more**. A mod touches a handful of them, so nearly all of
that work is thrown away.

Correct instinct, wrong scale. What *is* true is the premise behind it: **the
archives never change**. A mesh's chunk bounds are a pure function of bytes that
are fixed until the game is patched or a mod is installed.

So: compute lazily, but remember the answer permanently.

## Design

An on-disk cache, loaded whole at open, added to on a miss, rewritten whole on
flush.

```c
redfs_cache_open(depot, "redfs_mesh.cache");
/* every redfs_mesh_open from here is remembered, across restarts */
redfs_cache_flush();
```

First call for a given mesh computes; every later call — including after a game
restart — is a hash-map lookup. **0.72 ms cold → 0.00 ms warm**, at the median
above. Flush is a no-op unless something was inserted since the last one, so
calling it on a timer costs nothing when nothing changed.

`redfs_cache_warm(depot, hashes, count, &computed)` precomputes a caller-supplied
list and flushes at the end — the "warm it at load" behaviour applied to the
meshes a mod actually touches rather than to the whole depot. It requires a cache
open on this depot and returns `REDFS_E_INVALID_ARG` if there is none, because
without one every result would be computed at full price and immediately
discarded.

There is **one cache per process**, and it belongs to the depot passed to
`redfs_cache_open`. Entries are keyed by hash alone, and the same hash means
different bytes in a different depot, so `redfs_mesh_open` on any other depot
bypasses the cache in both directions: it neither reads an entry nor records one,
because doing either would hand depot B depot A's geometry and then flush B's
results under A's fingerprint. If you keep two depots, exactly one of them
benefits. The cache holds the owning depot pointer for identity comparison only
and never dereferences it, because it may dangle after `redfs_depot_close`.

## Invalidation

The failure mode that matters is **serving geometry for a mesh that has since
been replaced** — install a body mod, and cached chunk bounds from the vanilla
mesh would be silently wrong. That is worse than no cache, because it looks like
it works.

So every cache file carries a fingerprint of the archive set that produced it.
Per mounted archive, FNV-1a over: **path, entry count, index size, index CRC and
declared file size**. All five come from the header and index that are already
mapped at mount, which is what makes the fingerprint cost no I/O at all — even
the file size is the packer's declared value, not a `stat`. The mix is
order-sensitive, and deliberately: mount order decides which archive wins a
duplicate hash, so a reorder changes which bytes a hash resolves to.

The index CRC is the one that does the real work, and it was missing from the
first version of this design. Path, entry count and index size describe the
*shape* of an archive, not its *contents* — re-cook a mesh and repack, and as
long as the file, segment and dependency counts are unchanged, the index is
exactly as long as before and all three are byte-identical. The cache then
validated and kept serving the old geometry, across restarts, silently.

The packer already stores a CRC-64 over the index body — the file table and the
segment table — so it moves when any entry's content hash or any segment's offset
or size moves. It sits at offset +8 of the 28-byte index header, read once at
mount and kept on the `Archive`. One caveat: the format does not *require* a CRC.
Every archive sampled on a real install has a distinct non-zero value, but a
hand-built archive may report 0, and against those the fingerprint is back to
comparing shapes.

`redfs_cache_open` fingerprints the depot as it stands at that moment, and every
later mount re-fingerprints and drops all entries if it moved — so mount order
relative to `cache_open` does not have to be perfect. Both halves of that are
required: re-fingerprinting alone leaves stale entries live and merely relabels
them on the next flush, and dropping entries alone lets the next flush write
fresh data under the old label.

A mismatch **discards the entire cache** rather than trying to reconcile.
Recomputation is cheap and correctness is not negotiable.

Deliberately *not* used: file modification times (unreliable across installs and
copies), or content hashes computed by RedFS over the archives themselves (that
would mean reading 85 GB to open a cache). Note the second objection never
applied to data already sitting in the mapped index — the CRC above, or the
per-entry SHA-1s. The CRC wins over sweeping the SHA-1s because it is O(1) per
archive rather than O(entries), and the fingerprint is recomputed on every mount.

## File format

Little-endian, one record per mesh, self-contained.

```
'RFMC' | u32 version | u64 fingerprint | u32 entry_count | u32 reserved

per entry:
  u64 hash | u32 lod_count | u32 chunk_count | u32 appearance_count
  f32 bbox_min[3] | f32 bbox_max[3]
  chunk_count      x { u32 index, lod_mask, lod, vertex_count, index_count,
                       u32 bounds_valid, f32 min[3], f32 max[3] }
  appearance_count x { u32 name_len, bytes,
                       u32 material_count,
                       material_count x { u32 len, bytes } }
```

The reader is bounds-checked throughout and gives up on the first inconsistency,
keeping whatever it read up to that point — a partial record leaves it mid-field
with no way to resynchronise, so stopping is the only option. A truncated cache
degrades to a partial cache, never to a crash or bad data. When that happens the
log says `loaded N of M entries`, because reporting only the survivors makes a
corrupt file look like a small one.

Every declared count is bounded by the bytes actually remaining in the file
rather than by a fixed limit, derived from the smallest on-disk footprint of the
element it counts. A fixed cap has two problems: it lets a 68-byte file ask for
tens of megabytes of chunks before anything is read, and — because the writer
applies no caps at all — it can reject a record the writer legitimately produced,
which then truncates every later record too. A bound derived from bytes remaining
is satisfied by construction on the write side, so the two halves cannot
disagree.

Invariants the writer guarantees but the file cannot are repaired rather than
rejected, on the same reasoning: rejecting discards every later record over a
trivially fixable field. `lod_count` and each chunk's `lod` are clamped to at
least 1, since `redfs.h` documents them as 1-based; `chunk_materials` is
truncated to the chunk count, since `internal.hpp` documents it as parallel to
`chunks`. The truncation happens *after* the strings are read, never by skipping
them — the count is part of the stream, so reading fewer would desynchronise
every following record.

Writes go to `<file>.tmp` and are moved over the target with
`MoveFileExA(MOVEFILE_REPLACE_EXISTING)` — one atomic step. Deleting the
destination first, as an earlier version did, opens a window where neither file
exists, and a move that then fails (a scanner holding the path is enough)
destroys the good cache instead of preserving it. Success is judged on `fflush` +
`_commit` + `fclose`, not on `fwrite`: `fwrite` reports bytes accepted into the
`FILE` buffer, so on a network share or a synced folder the failure surfaces only
at flush or close, and trusting `fwrite` would promote a truncated file over a
good one. On any failure the temporary is removed and the old cache stands.

Version is checked on open; a bump discards rather than migrates.

## Threading

One mutex around the map. The expensive part — `mesh_build` — happens **outside**
the lock, so concurrent misses on different meshes proceed in parallel. The mesh
is built privately and published only once complete, so nothing ever observes a
half-decoded object and nothing mutates one after it is shared.

Two threads racing on the *same* mesh both compute it; the second finds the
first's entry when it goes to insert and drops its own copy, so every caller
still observes one object per hash. Wasteful in a rare case, and much simpler
than per-key locking. Chosen deliberately.

The singleton is heap-allocated and never destroyed, for the same reason as the
async worker: running destructors during `DLL_PROCESS_DETACH` under the loader
lock is how mod DLLs deadlock. `redfs_cache_close` is the explicit teardown.

The log sink is called with the mutex **released** — messages are built inside the
lock and emitted after it. The sink is host code running on this thread and is
free to call back in; annotating a line with `redfs_cache_entry_count()` is an
obvious thing to write, and on the MSVC STL re-locking a `std::mutex` the same
thread already holds throws rather than blocks, so it would have crossed the C
ABI as an exception. `fail()` emits too, so the error paths needed this as much
as the success ones.

## Ownership consequence

A cached mesh is held by `shared_ptr`, and every open handle carries its own
reference. So `redfs_mesh_close` is always correct to call, on cached and
uncached meshes alike, and a handle stays valid across `redfs_cache_close` — the
entry leaves the map, the object survives until the last handle closes.

This replaced an earlier `caller_owned` flag, where the same call returned
borrowed or owned memory depending on configuration and `redfs_mesh_close` was a
no-op on cached meshes. That was the sharpest edge in the API and it was a
use-after-free waiting to happen: nothing stopped a caller from holding a
"borrowed" pointer past `redfs_cache_close`. Refcounting was rejected at the time
as more machinery than the problem deserved. It was not.

## What is not cached

Only meshes. Textures are not, because `texture_read_dds` is already dominated by
the unavoidable decompress of pixel data that the caller wants anyway — there is
no expensive *derived* result to keep, unlike bounding boxes.

The path dictionary is held in memory but not persisted; loading it from the
compressed list already costs well under a second.
