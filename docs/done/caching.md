# The mesh cache

**Status: implemented** — `src/cache.cpp`

## The problem

Per-chunk bounding boxes are not stored in the format (see `mesh-geometry.md`),
so they must be computed from vertex positions. That means decompressing the
geometry buffer: ~1–2 ms for a body mesh, dominated by Kraken decode rather than
the min/max sweep.

Fine once. Not fine if the answer is wanted repeatedly, per frame, or per entity.

## Why precomputing everything does not work

The obvious idea — walk the depot at load, compute every mesh, done — was
considered and rejected on arithmetic:

- the depot holds on the order of **10⁵ `.mesh` files**
- at ~1–2 ms each that is **minutes** of startup
- the decoded results would run to gigabytes

Correct instinct, wrong scale. What *is* true is the premise behind it: **the
archives never change**. A mesh's chunk bounds are a pure function of bytes that
are fixed until the game is patched or a mod is installed.

So: compute lazily, but remember the answer permanently.

## Design

An on-disk cache, loaded whole at open, appended to on a miss, written back on
flush.

```c
redfs_cache_open(depot, "redfs_mesh.cache");
/* every redfs_mesh_open from here is remembered, across restarts */
redfs_cache_flush();
```

First call for a given mesh computes; every later call — including after a game
restart — is a hash-map lookup. Measured: **0.72 ms cold → 0.00 ms warm.**

`redfs_cache_warm(depot, hashes, count, &computed)` precomputes a caller-supplied
list, which is the "warm it at load" behaviour applied to the meshes a mod
actually touches rather than the whole depot.

## Invalidation

The failure mode that matters is **serving geometry for a mesh that has since
been replaced** — install a body mod, and cached chunk bounds from the vanilla
mesh would be silently wrong. That is worse than no cache, because it looks like
it works.

So every cache file carries a fingerprint of the archive set that produced it:
for each mounted archive, its **path, entry count, index size, index CRC and
declared file size**, mixed with FNV-1a.

The index CRC is the one that does the real work, and it was missing from the
first version of this design. Path, entry count and index size describe the
*shape* of an archive, not its *contents* — re-cook a mesh and repack, and as
long as the file count, segment count and dependency count are unchanged, the
index is exactly as long as before and all three are byte-identical. The cache
then validated and kept serving the old geometry, across restarts, silently.
That is the failure this section opens by calling worse than no cache, and the
fingerprint was blind to it.

The packer already stores a CRC-64 over the index body — the file table and the
segment table, so it covers every entry's content hash and every segment's
offset and size. It sits in the 28-byte index header at offset +8, which RedFS
maps anyway, so mixing it costs one `rd64` per archive and no I/O at all.

On open, a fingerprint mismatch **discards the entire cache** rather than trying
to reconcile. Recomputation is cheap and correctness is not negotiable.

Deliberately *not* used: file modification times (unreliable across installs and
copies), or content hashes computed by RedFS over the archives themselves (that
would mean reading 85 GB to open a cache). Note this objection never applied to
data already sitting in the mapped index — the CRC above, or the per-entry
SHA-1s. The CRC is preferred over sweeping the SHA-1s because it is O(1) per
archive rather than O(entries), and `depot_fingerprint` runs on every mount.

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
keeping whatever it read up to that point. A truncated cache degrades to a
partial cache, never to a crash or bad data. When that happens the log says
`loaded N of M entries`, because reporting only the survivors makes a corrupt
file look like a small one.

Every declared count is bounded by the bytes actually remaining in the file
rather than by a fixed limit. A fixed limit has two problems: it lets a 68-byte
file ask for tens of megabytes before anything is read, and — because the writer
applies no limits at all — it can reject a record the writer legitimately
produced, which then truncates the rest of the file on every load. A bound
derived from bytes remaining is satisfied by construction on the write side, so
the two halves cannot disagree.

Values the writer guarantees but the file cannot are repaired rather than
rejected: `lod_count` and per-chunk `lod` are clamped to at least 1, since
`redfs.h` documents them as 1-based and rejecting would discard every later
record over a trivially fixable field.

Writes go to `<file>.tmp`, are flushed to disk, and are then moved over the
target with `MoveFileExA(MOVEFILE_REPLACE_EXISTING)` — one atomic step. Deleting
the destination first, as an earlier version did, opens a window where neither
file exists, and a move that then fails destroys the good cache instead of
preserving it. The write is judged on `fflush` + `_commit` + `fclose`, not on
`fwrite` alone: `fwrite` reports bytes accepted into the FILE buffer, so on a
network share or synced folder a failure surfaces only at close, and checking
`fwrite` alone would promote a truncated file over a good one.

Version is checked on open; a bump discards rather than migrates.

## Threading

One mutex around the map. The expensive part — `mesh_build` — happens **outside**
the lock, so concurrent misses on different meshes proceed in parallel.

Two threads racing on the *same* mesh both compute it; the second discovers the
first's entry on insert and drops its own copy. Wasteful in a rare case, and much
simpler than per-key locking. Chosen deliberately.

The singleton is heap-allocated and never destroyed, for the same reason as the
async worker: running destructors during `DLL_PROCESS_DETACH` under the loader
lock is how mod DLLs deadlock. `redfs_cache_close` is the explicit teardown.

The log sink is called with the mutex **released**. Messages are built inside the
lock and emitted after it, because the sink is host code running on this thread
and is free to call back in — annotating a line with `redfs_cache_entry_count()`
is an obvious thing to write, and on the MSVC STL re-locking a `std::mutex` the
same thread already holds throws rather than blocks, so it would have crossed the
C ABI as an exception. `fail()` emits too, so the error paths needed this as much
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
compressed list is already fast enough.
