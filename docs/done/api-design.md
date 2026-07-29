# API design

**Status: implemented** — `include/redfs.h`, `include/redfs.hpp`

The decisions that shape the surface, and the alternatives each one rejected.

## C ABI as the contract

The stable surface is C: opaque handles, plain structs, status codes.

Mods are built by many people with mismatched toolchains. A C++ ABI would tie
every consumer to one MSVC version and one STL. C survives that, and it also
makes the library reachable from Rust, C#, or anything else with an FFI.

`redfs.hpp` sits on top, header-only and optional. It holds nothing but the C
handles — `Blob`, `Cr2w`, `Mesh` and `Depot` are move-only RAII wrappers and
every method forwards — so mixing the two layers in one program is fine.

### A version check, because a mismatch is silent

`REDFS_ABI_VERSION` is a compile-time constant (currently 2);
`redfs_abi_version()` reports what the loaded DLL was built with. `abi_ok()`
compares them and `Depot::open` does it for you.

That check earns its place because the failure it catches announces itself in no
other way. `redfs_mesh_chunk` has only ever grown by appending fields, so under a
mismatch individual field reads still land correctly and only the array *stride*
is wrong: `chunks()` walks a 48-byte array in 44-byte steps and returns plausible
garbage from the second element on. No crash, no status code. Comparing two
integers is the only thing that turns that into a diagnosable failure.

## No game hooks

The decision everything else follows from.

The alternative existed and works: hook the game's own `ResourceDepot`, as
red-hot-tools does, and let the engine do the reading. Rejected because it needs
address-library offsets and **breaks on every game patch**. Reimplementing the
container natively costs more code once, then survives patches indefinitely.

The only game dependency left is `oo2ext_7_win64.dll`, resolved by name — already
loaded in the game process, else `<game_dir>/bin/x64`, else the DLL search path.
Never by address.

The corollary is that RedFS **does not touch live game state**: no entities, no
components, no RTTI. A deliberate boundary rather than an omission, and
`entityComponents` sits on the far side of it.

## Segment selection by sentinel

A file is a run of segments (see `archive-format.md`), so every read is
parameterised by which part the caller wants:

```c
#define REDFS_PART_MAIN 0xFFFFFFFFu  /* segment 0 -- the CR2W document */
#define REDFS_PART_ALL  0xFFFFFFFEu  /* every segment, concatenated    */
/* 0 .. n-1                             one attached buffer            */
```

Sentinels rather than three functions each: `read`, `read_into`, `part_size` and
`read_async` would all have to triple. The naming carries the meaning.

The reason this is in the API at all is cost, not elegance. Metadata queries read
`PART_MAIN` and never touch the payload — `redfs_texture_desc_of` and
`redfs_mesh_desc_of` both do, which is why they are cheap on a file whose pixels
run to megabytes.

Because `part` is a caller-supplied `uint32_t` that reaches segment arithmetic,
it is range-checked rather than trusted; `out_of_range_part_is_rejected_not_wrapped`
in the test suite exists for the version that wrapped.

## Three layers, and the middle one is load-bearing

A caller should be able to stop at the level that suits them:

1. **Raw** — `redfs_read`, `redfs_stat`, `redfs_exists`. Works on anything.
2. **Container** — `redfs_cr2w_*`. Chunk graph, imports, properties by name.
   Works on anything cooked, without RedFS knowing the type.
3. **Typed** — `redfs_texture_*`, `redfs_mesh_*`, `redfs_audio_*`. Convenient,
   and necessarily incomplete.

Layer 2 is what keeps layer 3's incompleteness from being a wall. Where RedFS has
no helper, the generic property walker still answers the question and the caller
does the interpreting. That is why `redfs_cr2w_get` / `walk` / `walk_array` are
public rather than internal plumbing — the alternative was a typed API that grew
a new function every time someone met a new resource.

## One owning type for bytes

```c
typedef struct redfs_blob { uint8_t* data; uint64_t size; void* reserved; } redfs_blob;
```

One owning type, one `redfs_blob_free`. `reserved` carries the real allocation, so
the layout behind `data` can change without breaking the ABI.

Two details are contract rather than implementation:

- Buffers are over-allocated by one byte and NUL-terminated, so text payloads
  (`.json`, `.xml`) can go straight to a string API without copying.
- A zero-length read still returns non-null `data`. There is deliberately no
  early-out for `size == 0`, because handing back null is exactly the case the
  padding exists to make safe.

`redfs_read_into` is the escape hatch for callers who want no allocation at all:
pass a buffer and get `REDFS_E_RANGE` plus the required size if it is too small.

## Mesh handles are shared-owned

`redfs_mesh_open` returns a handle to an object the cache may also hold, so
ownership had to be decided rather than assumed. It is a `shared_ptr`: every
handle carries its own reference, `redfs_mesh_close` is always correct to call,
and a handle stays valid across `redfs_cache_close` — the entry leaves the map,
the object survives until the last handle closes.

Two cheaper designs were tried first and both were wrong:

- **`close` always deletes.** Double-frees a cached mesh on the second open.
- **A `caller_owned` flag**, set only on a cache miss with the cache disabled, so
  `close` became a no-op on cached meshes. This made the same call return borrowed
  or owned memory depending on configuration, and nothing stopped a caller holding
  the "borrowed" pointer past `redfs_cache_close`. Refcounting was dismissed at
  the time as more machinery than the problem deserved; the flag was a way of
  documenting a lifetime hazard instead of removing it.

Shared ownership also buys the concurrency the header promises: eight threads
opening and closing the same cached mesh is a supported pattern, and
`concurrent_mesh_open_is_safe` holds it to that.

## Errors: status codes plus a thread-local string

No exceptions cross the ABI, and nothing allocates on the error path. `fail()`
formats into a `thread_local` buffer and returns the code in one expression, so
error sites stay one line and still carry context:

```cpp
return fail(REDFS_E_CORRUPT, "%s index claims %llu bytes but is %u", path.c_str(),
            need, size);
```

`redfs_last_error()` returns that buffer. An optional `redfs_set_log` callback
receives the same messages plus non-fatal notes — a skipped archive, a texture
whose header was reconciled against its payload — which are the cases where the
call succeeds and the caller still wants to know.

Every allocating C export sits behind an exception barrier, because internal
`bad_alloc` had a clear path out of the ABI otherwise: `reindex()` reserves one
ref per entry across every archive, 544k on a stock install.

## Threading: immutable after open

A depot is immutable once opened, so `redfs_read*`, `redfs_stat`,
`redfs_enumerate`, `redfs_texture_*` and `redfs_mesh_*` are safe from any number
of threads with no locking on the caller's side.

The mutating calls are not: `redfs_depot_mount`, `redfs_depot_mount_dir`,
`redfs_depot_close`, `redfs_shutdown`, `redfs_cache_*` and `redfs_path_*`. Mount
rebuilds the depot index in place and a concurrent read would walk it mid
reallocation. Open first, then share.

One exception is worth stating separately because it cuts across that rule: an
individual `redfs_cr2w` handle is **single-threaded**. Decoding a `CString` caches
it on the handle, so two threads calling `redfs_cr2w_get` on one handle race.
Share the depot, give each thread its own CR2W handle. The typed helpers are
unaffected — each builds a private handle per call.

All of this is stated in the header rather than left to be discovered.

## Async exists because reads are milliseconds

Kraken decode plus page faults puts a read in the millisecond range, which is a
frame. `redfs_read_async` keeps that off the render thread.

The callback runs on RedFS's worker, never the caller's thread. Marshalling back
is the caller's job, because the right way to do it is engine-specific and a
wrong guess here would be worse than no help. What the API does guarantee is
exactly-once: a return of `REDFS_OK` means the callback will fire with either a
result or `REDFS_E_CANCELLED`, and a return of `REDFS_E_CANCELLED` or
`REDFS_E_INVALID_ARG` means it will not fire at all, so the caller handles the
failure inline.

### The worker is deliberately leaked, and `redfs_shutdown` is the answer

`static Worker* w = new Worker();`, never destroyed. Same for the mesh cache and
the path dictionary.

Joining a `std::thread` from a static destructor runs during
`DLL_PROCESS_DETACH`, **under the loader lock**, which deadlocks. A mod DLL must
never do that.

So teardown is explicit instead. `redfs_shutdown()` cancels queued reads, stops
and joins the worker, and flushes the cache; it is what a plugin calls before its
DLL can be unloaded, and the lifecycle tests show `FreeLibrary` crashing without
it. `redfs_drain()` is the lighter call for "wait for my queued reads" without
stopping the worker. Neither may be called from a read callback — both would wait
on the very job completing — and both detect that and return without acting.

## What was cut

- **A RED4ext plugin exposing RTTI globals.** Designed, then dropped when the
  scope was set to C API only. It would have made these calls reachable from CET
  Lua and redscript, and is the natural home for `entityComponents`. Nothing in
  the tree occupies that slot today, which is also why nothing here has yet run
  inside the game — see `testing.md`.
- **Zero-copy views.** `redfs_read` could return a pointer into the file mapping
  for uncompressed segments and skip a memcpy. It complicates ownership for a win
  that only applies to the minority of segments stored raw.
- **A synchronous convenience that hides the cost.** Rejected on purpose: reads
  are milliseconds and the API should not make that easy to forget.
