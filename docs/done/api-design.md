# API design

**Status: implemented** — `include/redfs.h`, `include/redfs.hpp`

Decisions and the reasoning behind them, including the ones that were reversed.

## C ABI as the contract

The stable surface is C: opaque handles, plain structs, status codes. The C++
header is a header-only convenience layer with no state of its own.

Reason: mods are built by many people with mismatched toolchains. A C++ ABI would
tie consumers to one MSVC version and one STL. C is what survives that, and it
also makes the library reachable from Rust, C#, or anything with an FFI.

`REDFS_ABI_VERSION` is exposed so a consumer can refuse a mismatched DLL.

## No game hooks

The decision that shapes everything else.

An alternative existed: hook the game's own `ResourceDepot` (red-hot-tools does
this) and let the engine do the reading. Rejected, because that approach needs
address-library offsets and **breaks on every game patch**. Reimplementing the
container natively costs more code once and then survives patches indefinitely.

The only game dependency left is `oo2ext_7_win64.dll`, resolved by name rather
than address.

The corollary is that RedFS **does not touch live game state** — no entities, no
components, no RTTI. That is a deliberate boundary, not an omission, and
`entityComponents` sits on the far side of it.

## Segment selection

A file is a run of segments (see `archive-format.md`), so reads are parameterised
by which part you want:

```c
REDFS_PART_MAIN    /* segment 0 -- the CR2W document */
REDFS_PART_ALL     /* everything, concatenated */
0 .. n-1           /* one attached buffer */
```

Sentinel constants rather than three separate functions, because every read
variant (`read`, `read_into`, `part_size`, `read_async`) would otherwise need
tripling. The naming carries the meaning.

This matters for cost, not elegance: metadata queries read `PART_MAIN` and never
touch the multi-megabyte payload. `redfs_texture_desc_of` is cheap for exactly
this reason.

## Three layers

Deliberately, so a caller can stop at the level that suits them:

1. **Raw** — `redfs_read`, `redfs_stat`, `redfs_exists`. Works on anything.
2. **Container** — `redfs_cr2w_*`. Chunk graph, imports, properties by name.
   Works on anything cooked, without RedFS knowing the type.
3. **Typed** — `redfs_texture_*`, `redfs_mesh_*`. Convenient, and necessarily
   incomplete.

Layer 2 is what keeps layer 3's incompleteness from being a wall. When RedFS has
no helper for a format, the generic property walker still answers the question —
you just do the interpreting. That is why `redfs_cr2w_get`/`walk`/`walk_array`
are public rather than internal plumbing.

## Blobs and ownership

```c
typedef struct redfs_blob { uint8_t* data; uint64_t size; void* reserved; } redfs_blob;
```

One owning type, one `redfs_blob_free`. The `reserved` field carries the real
allocation so the layout can change without breaking the ABI.

Buffers are over-allocated by one byte and NUL-terminated, so text payloads
(`.json`, `.xml`) can be handed to string APIs without copying.

`redfs_read_into` exists for callers who want no allocation at all: pass a
buffer, get `REDFS_E_RANGE` and the required size if it is too small.

### The mesh-handle wrinkle

`redfs_mesh_open` returns a handle that the **cache may also hold**. Initially
`redfs_mesh_close` always deleted, which double-freed a cached mesh on the second
open. The first fix added a `caller_owned` flag, set only on a cache miss with
the cache disabled, so that `close` became a no-op on cached meshes.

That was the wrong fix, and it was recorded here as "the sharpest edge in the
API" rather than as the bug it was: the same call returned borrowed or owned
memory depending on configuration, and nothing stopped a caller from holding a
"borrowed" pointer past `redfs_cache_close`. Refcounting was dismissed at the
time as more machinery than the problem deserved.

It is now a `shared_ptr`. Every handle carries its own reference, `close` is
always correct to call, and a handle stays valid across `redfs_cache_close` —
the entry leaves the map, the object survives until the last handle closes. The
lesson worth keeping is that the flag was a way of documenting a lifetime hazard
instead of removing it.

## Errors

Status codes plus a thread-local `redfs_last_error()` string. No exceptions
across the ABI, and no allocation on the error path.

Internally `fail()` records the message and returns the code in one expression,
so error sites stay one line and always carry context:

```cpp
return fail(REDFS_E_CORRUPT, "%s index claims %llu bytes but is %u", path, need, size);
```

An optional `redfs_set_log` callback receives the same messages plus non-fatal
notes — a skipped archive, a texture whose header was reconciled against its
payload.

## Threading

A depot is immutable once open, so reads, stats and texture calls are safe from
any number of threads with no locking. `mount` and `close` are not; open first,
then share. Stated in the header rather than left to be discovered.

Reads take milliseconds — Kraken decode plus page faults — so
`redfs_read_async` exists to keep them off the render thread. The callback runs
on RedFS's worker, never the caller's thread; marshalling back is the caller's
job, because the right place to do that is engine-specific.

### The worker is deliberately leaked

`static Worker* w = new Worker();`, never destroyed.

Joining a `std::thread` from a static destructor runs during
`DLL_PROCESS_DETACH`, **under the loader lock**, which deadlocks. A mod DLL must
never do that. `redfs_drain()` is the supported synchronisation point. The same
reasoning applies to the cache and path-dictionary singletons.

## What was cut

- **A RED4ext plugin exposing RTTI globals.** Designed, then dropped when the
  scope was set to C API only. It would have made the calls reachable from CET
  Lua and redscript, and is the natural home for `entityComponents`.
- **Zero-copy views.** `redfs_read` could return a pointer into the file mapping
  for uncompressed segments, avoiding a memcpy. It complicates ownership for a
  win that only applies to the minority of segments that are stored raw.
- **A synchronous convenience that hides the cost.** Rejected on purpose: reads
  are milliseconds and the API should not make that easy to forget.
