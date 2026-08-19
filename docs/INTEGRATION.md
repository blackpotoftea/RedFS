# Integrating RedFS into a mod

`USAGE.md` covers the calls. This covers the *lifecycle*: loading, shutdown,
threading, mod load order and cost inside a game process — and the ways each goes
wrong.

[linking](#static-or-shared) · [startup](#startup-in-order) ·
[shutdown](#shutdown) · [threading](#threading-rules) ·
[load order](#mods-layering-and-overrides) · [mod managers](#mod-managers) ·
[cost](#cost) · [failure modes](#failure-modes)

## Read this first: none of it has run inside the game

There is now a plugin — `examples/red4ext_plugin` — and it **compiles and links
against the real RED4ext SDK**, exporting `Main` / `Query` / `Supports`. That is
where the evidence stops. It has never been loaded by the game: RED4ext is not
installed on the machine this was developed on, so no part of RedFS has run inside
the Cyberpunk process.

Building it did earn one fact immediately. The sample below previously used
`RED4ext::PluginHandle` and `RED4EXT_SEMVER`, neither of which the SDK defines —
it had been written to a remembered shape and never compiled. Apply the same
suspicion to the rest of this document until someone runs it.

Every figure below comes from offline harnesses — `redfs_test`, `redfs_lifecycle`,
`redfs_verify`, `redfs_cli selftest` — against synthesized archives, or against a
real install read from *outside* the game process. The lifecycle rules are
therefore constraints derived from the code and from teardown tests that reproduce
what RED4ext does to a plugin. Claims about the game's own behaviour are cited to
WolvenKit as the reference implementation, not observed.

### What loading it would actually establish

Three things no offline harness can reach:

1. **Install auto-detection.** `redfs_depot_open(nullptr, ...)` walks up from the
   running executable. Outside the game that branch is never taken.
2. **Oodle against the resident DLL.** The game already has `oo2ext_7_win64.dll`
   loaded, so `GetModuleHandleW` should find it without touching disk; standalone
   tools take a fallback path instead.

   Sharing that module means sharing its *plugin allocator*, which is
   process-global and belongs to whoever installed it last — in the game, to
   CDPR. Theirs asserts the moment it is called (`OodleMallocAligned called
   unexpectedly`, `wrapperKraken.cpp:85`), because the engine only ever calls
   Oodle through APIs it can hand preallocated memory to. So RedFS has to supply
   its own decoder scratch on every decode; passing null asks Oodle to allocate
   and takes the game down on the first compressed read. This is the one item
   here that had actually shipped broken — binding the function was exercised,
   calling it in-process was not.
3. **Unload ordering for real.** RED4ext calls `Main(Unload)` and then
   `FreeLibrary`. `redfs_lifecycle` reproduces that shape, but with a test harness
   driving it rather than RED4ext's plugin manager.

To try it: install RED4ext, build with `-DREDFS_RED4EXT_SDK=<path>`, drop
`RedFS.SmokeTest.dll` in `red4ext/plugins/RedFS.SmokeTest/`, launch, and read
`red4ext/logs/`.

---

## The short version

```
game start
  └─ RED4ext loads your plugin
       └─ Main(Load)     -> redfs_depot_open, redfs_cache_open
                            ~30 ms, 8.7 MB heap on a stock install

gameplay
  └─ your code           -> redfs_read / redfs_texture_read_dds / redfs_mesh_open
                            milliseconds each; not on the game thread

game close
  └─ Main(Unload)        -> redfs_shutdown()    <-- REQUIRED
                            redfs_depot_close()
       └─ RED4ext FreeLibrary
  └─ process exit
```

**`redfs_shutdown()` in `Unload` is not optional.** Skip it and a live worker
thread is still executing code `FreeLibrary` is about to unmap.

RedFS does not hook the game, register RTTI, or install itself anywhere — it opens
files and parses bytes. So it does not care whether it sits inside RED4ext, CET or
a standalone tool, and it has no idea when the game is shutting down. Nothing tells
it. That is why shutdown is your call to make.

## Static or shared?

```cmake
target_link_libraries(my_plugin PRIVATE redfs_static)   # or RedFS::static
```

| | `redfs_static.lib` | `RedFS.dll` |
|---|---|---|
| ship | one self-contained plugin DLL | plugin + the DLL |
| two plugins | two of everything: depots, indices, mesh caches, path dictionaries, worker threads | one of each |
| ABI mismatch | impossible — compiled together | possible, and silent unless you check |
| plugins interfering | no | yes, see below |

For a single plugin, static. Always. Sharing is worth it only when several plugins
genuinely want one depot, and it buys three problems:

- **One mesh cache, one owner.** The cache is process-wide and belongs to the depot
  passed to `redfs_cache_open`. `redfs_mesh_open` on any *other* depot bypasses it
  entirely rather than risk serving depot A's geometry for depot B's hash, so a
  second plugin with its own depot gets no caching at all.
- **`redfs_shutdown()` is global.** It cancels the whole async queue, so the first
  plugin to unload resolves everyone's queued reads with `REDFS_E_CANCELLED`. It
  quiesces rather than disables — a later `redfs_read_async` starts a fresh worker —
  but in-flight work is lost.
- **Agree who calls it.** The DLL outlives any single plugin.

Two statically-linked plugins cannot interfere, with one exception: **give each its
own mesh cache file.** Two caches on one path overwrite each other on every flush.

### Check the ABI when you link the DLL

`REDFS_ABI_VERSION` is **2** (`redfs_mesh_chunk` gained `bounds_valid`; anything
built against 1 must be recompiled). A mismatch is silent in the worst way: the
struct has only ever grown by appending, so field reads still land and only the
*stride* is wrong — walking the chunk array returns plausible garbage from the
second element on.

`redfs::Depot::open()` checks it and returns `nullopt`. That is the one facade
failure that never reaches the DLL, so `redfs_last_error()` is empty for it; report
the two version numbers yourself.

---

## RED4ext plugin

RED4ext invokes your exported `Main` from its own plugin manager rather than from
`DllMain`, which is what makes `Load` and `Unload` safe places to start and stop a
thread. That is RED4ext's design, taken on trust; nothing here verifies it.

```cpp
#include <RED4ext/RED4ext.hpp>
#include "redfs.h"

namespace {
redfs_depot* g_depot = nullptr;

// May arrive on RedFS's worker thread, and concurrently. `msg` dies with the call.
void on_redfs_log(const char* msg, void*) { /* copy it into your own logger */ }
}  // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle handle,
                                        RED4ext::v1::EMainReason reason,
                                        const RED4ext::v1::Sdk* sdk) {
    switch (reason) {
    case RED4ext::v1::EMainReason::Load: {
        if (redfs_abi_version() != REDFS_ABI_VERSION) {
            sdk->logger->ErrorF(handle, "RedFS ABI %u, expected %u",
                                redfs_abi_version(), REDFS_ABI_VERSION);
            return false;
        }
        redfs_set_log(on_redfs_log, nullptr);

        // NULL auto-detects the install by walking up from the running exe --
        // correct inside the game, and it makes MO2 and Vortex work for free.
        if (redfs_depot_open(nullptr, REDFS_SCAN_ALL, &g_depot) != REDFS_OK) {
            sdk->logger->ErrorF(handle, "RedFS: %s", redfs_last_error());
            return false;
        }
        if (!redfs_oodle_available())
            sdk->logger->Warn(handle, "RedFS: no Oodle -- compressed reads will fail");

        // ABSOLUTE path, to a directory that already exists: RedFS creates neither.
        redfs_cache_open(g_depot, "C:\\...\\red4ext\\plugins\\my_plugin\\mesh.cache");
        break;
    }

    case RED4ext::v1::EMainReason::Unload:
        redfs_shutdown();          // order matters -- see Shutdown
        redfs_depot_close(g_depot);
        g_depot = nullptr;
        redfs_set_log(nullptr, nullptr);   // the sink points into this module
        break;
    }
    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* info) {
    info->name = L"MyPlugin";
    info->author = L"You";
    info->version = RED4EXT_V1_SEMVER(1, 0, 0);
    info->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
    info->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() { return RED4EXT_API_VERSION_1; }
```

The `v1` namespace and the `RED4EXT_V1_*` spellings are what the SDK actually
defines — an earlier version of this sample used `RED4ext::PluginHandle` and
`RED4EXT_SEMVER`, which do not exist and never compiled. `examples/red4ext_plugin`
is a working version of the above and is built by CI-less opt-in:

```
cmake -B build -DREDFS_RED4EXT_SDK=C:/path/to/RED4ext.SDK
cmake --build build --target redfs_red4ext_plugin
```

### Never touch RedFS from `DllMain`

`DllMain` runs under the loader lock.

- `redfs_shutdown` joins a thread, and a thread cannot finish exiting while you
  hold the loader lock. **Unconditional deadlock.**
- `redfs_depot_open` resolves Oodle, which may call `LoadLibrary` — the documented
  loader-lock hazard. It tries `GetModuleHandleW("oo2ext_7_win64.dll")` first, and
  inside the game that normally hits, so this one *appears* to work. That is worse
  than failing: it is the shape of a bug that only surfaces on the install where
  Oodle is not already resident.

`Main(Load)` and `Main(Unload)` exist precisely so you never have to.

---

## Startup, in order

1. **`redfs_abi_version()`** — only if you link `RedFS.dll`.
2. **`redfs_set_log()`** — first, or mount diagnostics are lost. Skipped archives, a
   discarded mesh cache and a missing Oodle are reported here and nowhere else.
3. **`redfs_depot_open(NULL, REDFS_SCAN_ALL, &depot)`** — index only, no file
   contents. `NULL` walks up at most five levels from the running executable looking
   for a directory containing `archive\pc`.
4. **`redfs_oodle_available()`** — check once. At 0, essentially every read fails
   with `REDFS_E_OODLE`; warn loudly rather than let each call fail separately.
5. **`redfs_cache_open(depot, file)`** — optional, nearly free, and what makes
   `redfs_mesh_open` cheap on the second run.
6. **`redfs_path_load(depot, list, &kept)`** — optional and *not* free; see
   [cost](#cost). Needed for `redfs_path_from_hash` and for `redfs_find`, which
   searches the dictionary this fills and so matches nothing without it.
7. **`redfs_cache_warm(depot, hashes, n, &computed)`** — optional. Requires a cache
   opened on *this* depot; without one it returns `REDFS_E_INVALID_ARG` rather than
   computing results it would immediately discard.

Two traps in step 5: `redfs_cache_open` returns `REDFS_OK` when the file does not exist
— that is the normal cold start — so a wrong path stays invisible until the flush at
shutdown logs `cannot write ...`; and RedFS creates no directories, so `fopen` on a
path with a missing parent simply fails. Use an absolute path to a directory you made
yourself.

Mount everything here. A depot is immutable once open, and that immutability is what
makes reads concurrency-safe.

---

## Shutdown

1. RED4ext calls `Main(Unload)`.
2. **`redfs_shutdown()`** sets the abort flag, so the read already running gives up
   at its next segment boundary with `REDFS_E_CANCELLED`; swaps out the queue and
   **cancels** it, every callback firing with `REDFS_E_CANCELLED` and none dropped;
   **joins** the worker, which is gone before the call returns; then calls
   `redfs_cache_close()`, which flushes pending mesh entries and clears the cache's
   owner pointer, and `redfs_path_cache_close()`, which writes anything the
   dictionary learned this session.
3. **`redfs_depot_close(depot)`** unmaps every archive index, closes the file and
   mapping handles, frees the index.
4. RED4ext `FreeLibrary`s your plugin. Nothing of yours is running.

**Call `redfs_drain()` before `redfs_shutdown()` if you want queued reads to
finish.** Shutdown never waits for the queue.

**Why that order.** The worker dereferences a raw depot pointer per job, so stopping
it first means no in-flight read is walking a depot you are about to free; and
`redfs_cache_close()` inside `redfs_shutdown` clears the cache's `owner` pointer,
which is compared for identity but outlives the depot if you close the other way
round — a later depot at the same address would then compare equal. The reverse order
is not a use-after-free, though: `redfs_depot_close` cancels anything queued against
that depot and waits out an in-flight read on it, bounded by one segment. **Closing a
depot with work outstanding is safe and is not a reason to call `redfs_drain` first.**

**What shutdown does not cover.** Only the async worker gets the abort flag — a thread
of yours sitting inside `redfs_read` is neither known about nor waited for, so join
your own threads first. Blobs (`redfs_blob_free`) and open handles
(`redfs_mesh_close`, `redfs_cr2w_close`) remain yours.

### It does not scale with how much you queued

Queued-but-unstarted reads are cancelled outright; the running one gives up at its
next segment boundary via an abort flag checked between segments.

Measured, `redfs_lifecycle shutdown-latency`: **400 queued reads of ~96 MB each —
38 GB of nominal work — shut down in 2.1 ms**, 400/400 callbacks resolved (0
completed, 400 cancelled).

```
child: shutdown took 2.1 ms; 400/400 resolved (0 done, 400 cancelled)
```

Read that precisely. It proves the queue is **cancelled rather than drained** and
that no callback goes missing — which is what the test asserts, failing above 2 s
or on any lost callback. It is *not* a segment-decode measurement: fixture archives
store their segments uncompressed, so those 2.1 ms contain no Kraken work at all.

**Why there is no timeout.** If the wait gave up after N ms and the DLL were then
unmapped, the thread would wake into freed code — the exact crash `redfs_shutdown`
prevents, turned into an intermittent one. So the bound comes from doing less work,
never from abandoning the join. The floor is one segment decode, because Oodle cannot
be interrupted mid-block; how long that is against real archives, where a single
texture buffer runs to tens of MB, is **unmeasured**.

### If you skip it

- **Plugin unloaded during the session** (hot reload, or a host that unloads on
  error): the worker is blocked in a condition variable *inside your DLL's code*.
  `FreeLibrary` unmaps that code, the thread wakes into unmapped memory, and the
  access violation is blamed on whatever ran next.
- **At process exit specifically:** survivable, and tested. Windows terminates other
  threads before `DLL_PROCESS_DETACH`, and because RedFS's singletons are never
  destroyed nothing tries to take a mutex a terminated thread was holding.

`redfs_lifecycle` covers both as child processes, since they are about teardown and
cannot be tested in-process. A hang counts as a failure exactly like a crash: every
child is waited on with a timeout.

| scenario | result |
|---|---|
| return from `main`, 500 reads in flight, no shutdown | clean exit, no hang |
| `ExitProcess`, 500 reads in flight, no shutdown | clean exit, no hang |
| `redfs_shutdown` with 400 × 96 MB queued | 2.1 ms, 400/400 callbacks resolved |
| `LoadLibrary` → use → `redfs_shutdown` → `FreeLibrary`, ×3 | clean, also under ASan |

Process exit tends to work even if you forget — **which is why the bug hides until
someone hot-reloads.** The DLL-unload row is the one that fails without
`redfs_shutdown()`.

The worker exists only if you called `redfs_read_async`. If you never do there is no
thread and no risk; calling `redfs_shutdown()` unconditionally costs a lock and a
cache flush, and keeps the code honest.

### Why the singletons are never freed

RedFS leaks three objects on purpose — the async worker, the mesh cache and the path
dictionary — because destroying them from a static destructor would run at
`DLL_PROCESS_DETACH`, under the loader lock, where joining a thread deadlocks. The
split is deliberate: **leak the memory, join the thread.**

The memory is not always small. `redfs_cache_close` clears the cache's entries, but
nothing ever frees the path dictionary's string arena — if you called
`redfs_path_load`, its full resident cost (~40 MB on a stock install) is held until
the process exits. A leak checker reports all three; `done/testing.md` covers how
the suite separates them from real leaks.

---

## Threading rules

A depot is **immutable once open**, which is what the safe rows rest on.

| | safe concurrently? |
|---|---|
| `redfs_read`, `redfs_read_into`, `redfs_part_size`, `redfs_stat`, `redfs_exists`, `redfs_enumerate` | yes |
| `redfs_find`, `redfs_path_*` | yes — the dictionary is internally locked, and `redfs_find` delivers its matches after releasing it |
| `redfs_texture_*`, `redfs_mesh_*`, `redfs_audio_*` | yes |
| `redfs_read_async`, `redfs_drain`, `redfs_blob_free`, `redfs_set_log` | yes |
| `redfs_cr2w_*` on **separate** handles | yes |
| `redfs_cr2w_*` on a **shared** handle | **no** — one handle per thread |
| `redfs_depot_mount`, `redfs_depot_mount_dir`, `redfs_depot_close` | **no** |
| `redfs_cache_*`, `redfs_path_*`, `redfs_shutdown` | **no** |

Mount everything during `Load`, then share the depot freely.

**The `redfs_cr2w` row is not a formality.** Decoding a `CString` caches it *on the
handle* — an `unordered_map` insert and a `vector` push_back — so two threads
calling `redfs_cr2w_get` on one handle mutate the same containers with no lock. That
is heap corruption in the game process, not a stale read. The typed helpers
(`redfs_texture_*`, `redfs_mesh_*`) are unaffected: each builds a private handle per
call.

**Mount is unsafe against readers**, not just against itself: it rebuilds the depot
index in place, and a concurrent read walks that index while it is being reallocated.

**The mesh cache is internally locked**, and decoding happens outside the lock, so
concurrent misses on different meshes proceed in parallel. Two threads racing on the
*same* mesh both decode it; the loser's copy is dropped and both callers get the
winner's, so there is one object per hash. Wasteful in a rare case, and much simpler
than per-key locking.

**Async callbacks run on RedFS's worker, never on your thread.** Marshalling back to
the game thread is yours, because the right way is engine-specific. From inside a
callback: chaining another `redfs_read_async` is the normal pattern; `redfs_drain` and
`redfs_shutdown` are refused with a log line rather than deadlocking; and do not close
a depot, because cancellation cannot run from the worker thread and jobs still queued
against it would outlive the free.

**The log sink has three obligations.** It may be called from the worker thread, it
may be called concurrently — RedFS does not serialize into it — and `message` is
valid only for the duration of the call. It is never invoked with a RedFS lock held,
so calling back into RedFS from the sink is safe.

---

## Mods: layering and overrides

The normal deployment is a stack: base archives, mod archives that override the base
and each other, and mods that add files the base never had. RedFS resolves all
three.

`REDFS_SCAN_ALL` mounts four sets in this order, and **a later mount wins**:

| # | set | flag | search | order within the set |
|---|---|---|---|---|
| 1 | `archive/pc/content` | `REDFS_SCAN_CONTENT` | top level | ordinal path; last wins |
| 2 | `archive/pc/ep1` | `REDFS_SCAN_EP1` | top level | ordinal path; last wins |
| 3 | `mods/<name>/archives` | `REDFS_SCAN_REDMOD` | **recursive** | folders in name order; within one mod, ordinal **full path** then **reversed** — first wins |
| 4 | `archive/pc/mod` | `REDFS_SCAN_MODS` | top level | ordinal path; last wins |

So a legacy `.archive` beats a REDmod one, which beats Phantom Liberty, which beats
the base game. Select layers with the flags when you want a known-good reference
regardless of what the user installed: `REDFS_SCAN_CONTENT | REDFS_SCAN_EP1` sees
vanilla only.

**`zz_` does not always win.** Under `archive/pc/mod` the alphabetically last archive
mounts last and wins, which is why the prefix is a convention there. Inside a single
REDmod the list is reversed, so `zz_` mounts *first* and **loses**. REDmod is also
the only set searched recursively, and its sort is over full paths — a subdirectory
interleaves with the top level by path order rather than being appended after it.

**Parity, honestly.** Sets 3 and 4 match WolvenKit's `ArchiveManager`
(`WolvenKit.Modkit/Managers/ArchiveManager.cs`) call for call: `GetFiles(...,
AllDirectories)` + `Sort(CompareOrdinal)` + `Reverse()` per REDmod, top-level-only and
ordinal for `archive/pc/mod`, REDmod loaded before legacy. Sets 1 and 2 diverge:
WolvenKit loads `ep1` *before* `content`, sorts base archives by a category prefix
(`memoryresident, ep1, basegame, audio, lang`) ahead of name, and skips base-folder
archives whose prefix is none of those. RedFS mounts `content` then `ep1`, plain
ordinal, everything. Vanilla archives do not contest paths with each other in practice
— but neither ordering has been compared against the running game.

### The three cases

```c
redfs_read(depot, redfs_hash("base\\shared.bin"), REDFS_PART_ALL, &blob);
```

- **overridden by a mod** — you get the mod's bytes, from the winning archive
- **overridden by several mods** — you get the last-mounted one
- **added by a mod** — reads like any other file; nothing special to do

`redfs_stat` reports the `archive_index` of the *winning* archive, so you can tell
which mod supplied what you just read:

```c
redfs_file_info info;
redfs_stat(depot, key, &info);
printf("came from %s\n", redfs_depot_archive_path(depot, info.archive_index));
```

`redfs_depot_file_count` counts **distinct keys**, not entries: overrides collapse,
added files increase it.

Covered by `redfs_test layering` — a 72-archive stack (12 base + 60 mods) with a path
every mod overrides, REDmod archives nested in subfolders, the full-path-vs-basename
ordering distinction, and mounting a mod on top mid-session.

**Scale.** Each archive costs a file handle, a mapping handle and one mapped index
view held for the depot's lifetime; segment data is mapped per read and unmapped
again. The base game is 57 archives, mounting in ~30 ms. A heavy mod list adds a few
hundred small archives with tiny indices; that increment is **unmeasured**.

## Mod managers

**Vortex** hardlinks or copies into the real game folder, so everything is physically
present. Nothing special needed.

**MO2**'s USVFS hooks file APIs *in processes MO2 launches*. RedFS discovers archives
with `FindFirstFileA` and `GetFileAttributesA` and opens them with `CreateFileA` +
`CreateFileMappingW`, so:

- **Inside a game MO2 launched** — merged mod archives appear under `archive/pc/mod`
  and `mods/` as if installed, and `REDFS_SCAN_ALL` finds them.
  `redfs_depot_open(NULL, ...)` auto-detects correctly because MO2 launches the real
  executable from the real game directory. *Reasoned from how USVFS works, not
  tested.*
- **In a tool run outside MO2** — the VFS does not exist, so mod archives are
  invisible. Mount the staging folder explicitly:

```c
uint32_t n = 0;
redfs_depot_mount_dir(depot, "D:\\MO2\\mods\\SomeMod\\archive\\pc\\mod", &n);
```

`redfs_depot_mount_dir` mounts every `.archive` directly in the folder (top level
only, name order) on top of everything already mounted, so mount staged mods in the
order MO2 lists them. There is no C++ facade wrapper; call the C function.

**`modlist.txt`.** `archive/pc/mod/modlist.txt` names archives explicitly. RedFS does
not read it — and neither does WolvenKit, in any way that changes what loads.
`LoadModArchives` lists every `*.archive` in the folder, then adds `modlist.txt`
entries *not already in that list* (so, for a file that exists, nothing), warns about
listed archives that are missing, and finally sorts the whole set ordinally by path.
**Listing an archive does not promote it; omitting one does not exclude it.** Whether
the game itself honours the file is not established here. For a set that excludes
archives, build the depot by hand: `redfs_depot_open_empty` plus one
`redfs_depot_mount` per archive, in your order.

## CET (Cyber Engine Tweaks)

CET is Lua; RedFS is C. They meet in your own native DLL, which exposes functions Lua
can call — CET does not load C libraries directly. The practical shape is a RED4ext
plugin registering RTTI functions that CET Lua calls; registration is outside RedFS's
scope (see `../README.md`), but two things about the boundary matter.

**Hashes must cross as decimal strings.** Lua numbers are doubles and lose precision
above 2⁵³, so a `uint64` key silently corrupts:

```cpp
char key[REDFS_HASH_STRING_MAX];             // 21 = 20 digits + NUL, always enough
redfs_hash_string(path, key, sizeof key);    // decimal text
uint64_t back = redfs_hash_parse(key);
```

**Never call a RedFS read directly from a Lua callback.** CET callbacks run on the
game thread. A cold `redfs_mesh_open` is ~1–2 ms for a body mesh, a large texture is
more, and `redfs_audio_probe` decodes the whole main segment to look at 16 bytes —
tens of MB for music. Warm the cache during load, or hand the work to
`redfs_read_async` and deliver on a later frame.

**Under both RED4ext and CET** is fine; both are just DLLs in one process. If your
RED4ext plugin owns the depot and CET calls into it, there is one depot and one
cache. The only trap is opening a **second** depot in a CET-facing layer because it
was convenient: that doubles the index memory and, worse, the second depot silently
bypasses the mesh cache. One depot per process, owned by whoever loads first.

---

## Cost

Measured on a 57-archive, 85 GB install (Steam, 2.3 + Phantom Liberty, 544,670
files):

| | |
|---|---|
| `redfs_depot_open`, 57 archives | ~30 ms, once |
| resident afterwards | 8.7 MB heap + ~89 MB file-backed index mapping (evictable) |
| `redfs_read` of a 40 KB file | 0.16 ms |
| `redfs_mesh_open`, cold | ~1–2 ms for a body mesh |
| `redfs_mesh_open`, cached | ~0 ms |
| `redfs_path_load`, full dictionary | ~135 MB transient, **~40 MB resident**, opt-in |
| `redfs_find` over 544k entries | **~40–150 ms**, hardware-dependent; ~11 MB transient, all under the dictionary lock; roughly pattern-independent |

The path dictionary is the only large optional cost, and its two figures are
different things. `usedhashes.kark` is 3.4 MB on disk and decompresses to ~135 MB,
held only for the duration of the call. What *stays* is the ~40 MB of paths that
actually resolve in the mounted depot — 544,496 of 544,670 on a stock install — as
interned strings in a never-freed arena plus a sorted 16-bytes-per-entry index. Load
it only if you call `redfs_path_from_hash` or `redfs_find`.

Only the loaded list is filtered against the depot: paths learned from CR2W import
tables, and anything passed to `redfs_path_add`, are kept unfiltered, so a hit tells
you what a file is *called*, not that it is readable. Import learning is also why
reads stay concurrency-safe with the dictionary on — it runs inside every CR2W parse,
and the dictionary is internally locked.

That lock is what makes `redfs_find`'s cost worth knowing about in a game process:
the scan holds it start to finish, because the callback must run *outside* it (a
read from your callback parses a CR2W, which re-enters import learning on the same
non-recursive mutex). So a full-dictionary search stalls any concurrent read that
parses a CR2W for the whole scan.

**Narrowing the pattern does not help.** Every entry is matched regardless of how
specific the pattern is, so the cost is set by dictionary size, not by how many
entries hit. Measured over a ~544k dictionary: `*` matching all of them, 149 ms;
an exact single-file pattern matching one, 104 ms. About 30 %, not an order of
magnitude. What actually bounds the stall is **doing the search once and keeping
the result**, or running it off the frame thread.

The figures above were taken on the reference machine against ~77-character paths.

---

## Failure modes

| symptom | cause | what to do |
|---|---|---|
| `redfs_depot_open` → `REDFS_E_NOT_FOUND` | auto-detect found no `archive\pc` within five levels above the exe, or the folder holds no archives | pass an explicit `game_dir` |
| every compressed read → `REDFS_E_OODLE` | `oo2ext_7_win64.dll` not resolved | check `redfs_oodle_available()` at load and warn |
| `REDFS_E_NOT_FOUND` on a path you expected | most invented paths do not exist | `redfs_exists` first, and fail loudly |
| `redfs_find` → `REDFS_E_NO_DICTIONARY` | nothing loaded into the path dictionary, or the loaded list resolved no files in this depot | `redfs_path_load` first; check `redfs_path_count()` |
| mesh bounds recomputed every run | cache discarded, or never written | read the log: wrong fingerprint, bad header, or `cannot write` |
| callbacks arrive `REDFS_E_CANCELLED` | shutdown, or their depot was closed | expected; treat as "no result", not an error |
| mod archives invisible in a tool | MO2's VFS exists only in processes MO2 launched | `redfs_depot_mount_dir` on the staging folder |

**Oodle resolution is not lazy.** `redfs_depot_open` attempts it at mount:
`GetModuleHandleW("oo2ext_7_win64.dll")` first — inside the game it is already
resident — then `<install>\bin\x64\oo2ext_7_win64.dll`, then the DLL search path.
Failure is logged, never returned, because uncompressed segments still read.
Individual reads do not retry; a later `redfs_depot_open` does, so one failed attempt
cannot poison the process.

**Mods changing between runs.** The mesh cache fingerprints the mounted archive set —
per archive: path, entry count, index size, index CRC, declared file size — and
discards itself when that moves. The index CRC is load-bearing: it catches an archive
re-cooked and replaced *in place*, which the other four inputs cannot see. Mounting
after `redfs_cache_open` re-checks, so mount order need not be perfect. The cost of a
change is a cold cache, never stale geometry.

**A path that does not resolve.** Archives store only hashes; there is no path table
to enumerate. A mod that works on your install and not the user's is usually a path
that exists in one and not the other.
