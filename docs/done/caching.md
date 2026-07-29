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
for each mounted archive, its path, entry count and index size, mixed with FNV-1a.
That moves whenever archives are added, removed, patched or reordered.

On open, a fingerprint mismatch **discards the entire cache** rather than trying
to reconcile. Recomputation is cheap and correctness is not negotiable.

Deliberately *not* used: file modification times (unreliable across installs and
copies), or content hashes of the archives themselves (would mean reading 85 GB
to open a cache).

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
partial cache, never to a crash or bad data.

Writes go to `<file>.tmp` and are renamed over the target, so a crash mid-write
cannot corrupt a cache that was previously good.

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

## Ownership consequence

A cached mesh is owned by the cache, so `redfs_mesh_close` on it must **not**
delete. The handle carries a `caller_owned` flag, true only on a cache miss with
the cache disabled.

This is the sharpest edge in the API — the same call returns borrowed or owned
memory depending on configuration. Refcounting was the alternative and was more
machinery than the problem deserved, but it is noted in `api-design.md` as the
most likely place a future change goes wrong.

## What is not cached

Only meshes. Textures are not, because `texture_read_dds` is already dominated by
the unavoidable decompress of pixel data that the caller wants anyway — there is
no expensive *derived* result to keep, unlike bounding boxes.

The path dictionary is held in memory but not persisted; loading it from the
compressed list is already fast enough.
