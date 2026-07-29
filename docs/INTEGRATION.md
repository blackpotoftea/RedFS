# Integrating RedFS into a mod

How RedFS actually behaves inside the game process: loading, lifetime, what
happens at game close, and the ways it can go wrong.

`USAGE.md` covers the calls. This covers the *lifecycle*.

---

## The short version

```
game start
  └─ RED4ext loads your plugin
       └─ Main(Load)        -> redfs_depot_open, redfs_cache_open
                               ~30 ms, 8.7 MB heap

gameplay
  └─ your code              -> redfs_mesh_open / redfs_texture_dds / redfs_read
                               milliseconds; use a worker thread

game close
  └─ Main(Unload)           -> redfs_shutdown()   <-- REQUIRED
                               redfs_depot_close
       └─ RED4ext FreeLibrary
  └─ process exit
```

**`redfs_shutdown()` in `Unload` is not optional** if you statically link. Skip it
and you have a running thread inside a DLL that is about to be unmapped.

---

## Where RedFS lives

RedFS is a plain library. It does not hook the game, register RTTI, or install
itself anywhere — it opens files and parses bytes. Two consequences:

- **It does not care whether it is inside RED4ext, CET, or a standalone tool.**
  There is no host integration to get wrong.
- **It has no idea when the game is shutting down.** Nothing tells it. That is
  why shutdown is your call to make.

### Static or shared?

```cmake
target_link_libraries(my_plugin PRIVATE redfs_static)   # usual choice
```

**Static** (`redfs_static.lib`) — one self-contained plugin DLL, nothing extra to
ship. If two plugins both link statically, each gets **its own copy**: two
depots, two indices (~8.7 MB each), two mesh caches, two worker threads. Wasteful
but correct, and the copies cannot interfere.

**Shared** (`RedFS.dll`) — one copy for everyone. Saves the duplication, but now
the DLL outlives any single plugin and you must agree on who calls
`redfs_shutdown()`. Only worth it if several plugins genuinely share a depot.

For a single plugin, static. Always.

---

## RED4ext plugin

RED4ext calls your exported `Main` with `EMainReason::Load` and `Unload`. Both
run on the main thread, as ordinary function calls — **not** under the loader
lock — which is exactly what makes them safe places to start and stop threads.

```cpp
#include <RED4ext/RED4ext.hpp>
#include "redfs.h"

namespace {
redfs_depot* g_depot = nullptr;

void on_redfs_log(const char* msg, void*) {
    // route into your own logger
}
}  // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::PluginHandle handle,
                                        RED4ext::EMainReason reason,
                                        const RED4ext::Sdk* sdk) {
    switch (reason) {
    case RED4ext::EMainReason::Load: {
        redfs_set_log(on_redfs_log, nullptr);

        // NULL auto-detects the install by walking up from the running exe --
        // correct inside the game, and it makes MO2 and Vortex work for free.
        if (redfs_depot_open(nullptr, REDFS_SCAN_ALL, &g_depot) != REDFS_OK) {
            sdk->logger->ErrorF(handle, "RedFS: %s", redfs_last_error());
            return false;
        }

        // Mesh bounds cost a geometry decompress; remember them across runs.
        redfs_cache_open(g_depot, "red4ext/plugins/my_plugin/redfs_mesh.cache");
        break;
    }

    case RED4ext::EMainReason::Unload: {
        // Stops and JOINS the worker thread. Without this, RED4ext's
        // FreeLibrary unmaps code that a live thread is executing.
        redfs_shutdown();
        redfs_depot_close(g_depot);
        g_depot = nullptr;
        break;
    }
    }
    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::PluginInfo* info) {
    info->name = L"MyPlugin";
    info->version = RED4EXT_SEMVER(1, 0, 0);
    info->runtime = RED4EXT_RUNTIME_LATEST;
    info->sdk = RED4EXT_SDK_LATEST;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() { return RED4EXT_API_VERSION_LATEST; }
```

### Never touch RedFS from `DllMain`

```cpp
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    // WRONG. Both of these deadlock or crash.
    if (reason == DLL_PROCESS_ATTACH) redfs_depot_open(...);
    if (reason == DLL_PROCESS_DETACH) redfs_shutdown();
    return TRUE;
}
```

`DllMain` runs under the loader lock. `redfs_depot_open` loads
`oo2ext_7_win64.dll` — `LoadLibrary` under the loader lock is a deadlock.
`redfs_shutdown` joins a thread, and a thread cannot finish exiting while you
hold the loader lock — also a deadlock.

`Main(Load)` and `Main(Unload)` exist precisely so you never have to.

---

## Mods: layering, overrides and new files

The normal deployment is a stack — base archives, then mod archives that override
the base and each other, plus mods that add files the base never had. RedFS
resolves all three the way the game does.

### Load order

`REDFS_SCAN_ALL` mounts in the game's own sequence, and **later wins**:

```
archive/pc/content      base game
archive/pc/ep1          Phantom Liberty
mods/<name>/archives    REDmod           <- what `redmod deploy` and MO2/Vortex produce
archive/pc/mod          legacy .archive  <- highest priority
```

So a legacy `.archive` beats a REDmod one, which beats the base game. Within a
folder, archives mount in ordinal filename order — which is exactly why modders
prefix with `zz_` to win a conflict, and RedFS honours that automatically.
REDmod folders mount in name order; the archives *inside* one REDmod folder are
reversed, matching WolvenKit's reading of the engine.

Select layers with the flags if you want, e.g.
`REDFS_SCAN_CONTENT | REDFS_SCAN_EP1` to see vanilla data only regardless of what
the user has installed — useful when you need a known-good reference.

### The three cases, all tested

```c
redfs_read(depot, redfs_hash("base\\shared.bin"), ...);
```

- **overridden by a mod** — you get the mod's bytes, from the winning archive
- **overridden by several mods** — you get the last-mounted one
- **added by a mod** — reads like any other file; nothing special to do

`redfs_stat` reports `archive_index` of the *winning* archive, so you can tell
which mod actually supplied what you just read:

```c
redfs_file_info info;
redfs_stat(depot, key, &info);
printf("came from %s\n", redfs_depot_archive_path(depot, info.archive_index));
```

`redfs_depot_file_count` counts **distinct paths**, not entries: overrides
collapse, added files increase it.

Verified in `redfs_test layering`, including a 72-archive stack with a path every
mod overrides, mods adding new files, and mounting a mod on top mid-session.

### Scale

Each archive costs a file handle, a mapping handle and a mapped index view. The
base game is 57 archives; a heavy mod list adds a few hundred small ones, whose
indices are tiny. Mount stays in the tens of milliseconds.

## Mod managers in practice

**Vortex** hardlinks or copies files into the real game folder, so everything is
physically present. Nothing special needed.

**MO2** is different, and worth understanding: its USVFS hooks file APIs *in
processes MO2 launches*. RedFS uses `CreateFileA`, `FindFirstFileA` and
`GetFileAttributesA`, all of which route through the hooked layer — so:

- **Inside a game MO2 launched** — the merged mod archives appear under
  `archive/pc/mod` and `mods/` exactly as if installed, and `REDFS_SCAN_ALL`
  finds them. `redfs_depot_open(NULL, ...)` auto-detects correctly because MO2
  launches the real executable from the real game directory.
- **In a tool run outside MO2** — the VFS does not exist, so the mod archives are
  invisible. Mount the staging folder explicitly:

```c
redfs_depot_mount_dir(depot, "D:\\MO2\\mods\\SomeMod\\archive\\pc\\mod", &n);
```

Mounted last means highest priority, so mount them in the order MO2 lists them.

**`modlist.txt`** — `archive/pc/mod/modlist.txt` can list archives explicitly.
RedFS does **not** read it; it mounts everything in the folder in name order.
That matches WolvenKit's behaviour, which sorts by name after reading the file,
so the file affects which archives are considered rather than their order. If you
need exact parity with a modlist that excludes archives, mount by hand with
`redfs_depot_open_empty` + `redfs_depot_mount`.

## CET (Cyber Engine Tweaks)

CET is Lua. RedFS is C. They meet in your own native DLL, which exposes functions
Lua can call — CET does not load C libraries directly.

The practical shape is a RED4ext plugin that registers RTTI functions; CET Lua
calls those, since CET can reach anything in the game's RTTI. Registration is
beyond RedFS's scope (see `../README.md` — RedFS deliberately touches no live
game state), but two things about the boundary matter:

**Hashes must cross as decimal strings.** Lua numbers are doubles and lose
precision above 2⁵³, so a `uint64` key silently corrupts:

```cpp
char key[REDFS_HASH_STRING_MAX];             // 21 bytes is always enough
redfs_hash_string(path, key, sizeof key);    // "13043060094899987578"
uint64_t back = redfs_hash_parse(key);
```

**Never call a RedFS read directly from a Lua callback.** CET callbacks run on
the game thread. A cold `redfs_mesh_open` is 1–2 ms; a large texture is more.
Either warm the cache during load, or hand the work to `redfs_read_async` and
deliver the result on a later frame.

### Running under both RED4ext and CET

They coexist fine — both are just DLLs in the same process. If your RED4ext
plugin owns the depot and CET calls into it, there is one depot and one cache.
Nothing special to do.

The only trap: **do not open a second depot** in a CET-facing layer because it
was convenient. That doubles the index memory and the mesh cache for no benefit.
One depot per process, owned by whoever loads first.

---

## What actually happens at game close

Worth being precise, because "it works on my machine" and "it is correct" differ
here.

### Ordinary shutdown

1. The game begins tearing down.
2. RED4ext calls `Main(Unload)` on each plugin, on the main thread.
3. Your `redfs_shutdown()` drains queued reads, sets the stop flag, wakes the
   worker and **joins** it. The thread is gone before the call returns.
4. `redfs_cache_close()` (which `redfs_shutdown` calls for you) flushes pending
   mesh entries to disk. Skip this and you lose the work, not correctness — the
   next run recomputes.
5. `redfs_depot_close()` unmaps every archive index and closes the handles.
6. RED4ext `FreeLibrary`s your plugin. Nothing of yours is running, so the unmap
   is safe.
7. Process exits.

### What if a lot of work is in flight?

Shutdown does **not** scale with how much you queued. Queued-but-unstarted reads
are cancelled outright, and the one already running gives up at its next segment
boundary via an abort flag it checks between segments.

Measured, from `redfs_lifecycle`: **400 queued reads of ~96 MB each — roughly
38 GB of nominal work — shut down in 2.1 ms.** All 400 callbacks resolved, none
lost. Under ASan, 9.9 ms.

```
child: shutdown took 2.1 ms; 400/400 resolved (0 done, 400 cancelled)
```

Every dropped read gets its callback with `REDFS_E_CANCELLED`, so nothing is left
waiting on a callback that never arrives. If you actually want queued work to
finish, call `redfs_drain()` first — that is what it is for.

### Why there is no timeout

An obvious-looking alternative is "signal the thread and carry on after N ms".
That is unsafe here, and the reason is worth stating: if the wait gives up and
the DLL is then unmapped, the thread wakes into freed code — the exact crash
`redfs_shutdown` exists to prevent. A timeout would convert a guaranteed-correct
shutdown into an intermittent crash.

So the bound comes from **doing less work**, never from abandoning the join. The
floor is one segment decode, because Oodle cannot be interrupted mid-block. In
practice that is single-digit milliseconds.

### If you skip `redfs_shutdown()`

- **Plugin unloaded during the session** (hot reload, or a host that unloads on
  error): the worker thread is blocked in a condition variable *inside your DLL's
  code*. `FreeLibrary` unmaps that code. The thread wakes into unmapped memory →
  access violation, blamed on whatever ran next.
- **At process exit specifically**: survivable, and tested. Windows terminates
  other threads before running `DLL_PROCESS_DETACH`, and because RedFS's
  singletons are deliberately never destroyed, nothing tries to take a mutex a
  terminated thread was holding.

`redfs_lifecycle` covers both directions, as separate processes, because they are
about teardown and cannot be tested in-process:

| scenario | result |
|---|---|
| return from `main` with 500 reads in flight, no shutdown | clean exit, no hang |
| `ExitProcess` with 500 reads in flight, no shutdown | clean exit, no hang |
| shutdown with 400 × 96 MB queued | 2.1 ms, 400/400 callbacks resolved |
| `LoadLibrary` → use → `redfs_shutdown` → `FreeLibrary`, ×3 | clean, also under ASan |

A hang counts as a failure there just as much as a crash: every child is waited
on with a timeout.

So process exit tends to work even if you forget — **which is exactly why the bug
hides until someone hot-reloads.** The DLL unload row is the one that would fail
without `redfs_shutdown()`.

The worker thread only exists if you called `redfs_read_async`. If you never do,
there is no thread and no risk — but calling `redfs_shutdown()` unconditionally
costs nothing and keeps the code honest.

### Why the singletons are never freed

RedFS leaks three objects on purpose: the async worker, the mesh cache and the
path dictionary. Destroying them from a static destructor would run at
`DLL_PROCESS_DETACH`, under the loader lock, where joining a thread deadlocks.

The split is deliberate: **leak the memory, join the thread.** A few hundred
bytes of never-freed singleton is harmless at process exit; a thread running in
unmapped code is not. `redfs_shutdown()` is the join.

This means a leak checker will report those singletons. That is expected — see
`done/testing.md` for how the test suite distinguishes them from real leaks.

---

## Threading rules

A depot is **immutable once open**. So:

| | safe concurrently? |
|---|---|
| `redfs_read`, `redfs_stat`, `redfs_exists`, `redfs_enumerate` | yes |
| `redfs_texture_*`, `redfs_mesh_open`, `redfs_cr2w_*` | yes |
| `redfs_depot_mount`, `redfs_depot_mount_dir`, `redfs_depot_close` | **no** |
| `redfs_cache_open`, `redfs_cache_close`, `redfs_shutdown` | **no** |

Mount everything during `Load`, then share the depot freely.

The mesh cache is internally locked, and the expensive part — decoding — happens
outside the lock, so concurrent misses on different meshes proceed in parallel.
Two threads racing on the *same* mesh both decode it; the loser's copy is
discarded. Wasteful in a rare case, and much simpler than per-key locking.

`redfs_read_async` callbacks run on RedFS's worker, never on your thread.
Marshalling back to the game thread is yours to do, because the right way to do
it is engine-specific.

---

## Cost, so you can budget it

Measured on a 57-archive, 85 GB install:

| | |
|---|---|
| `redfs_depot_open` | ~30 ms for 57 archives, once |
| resident | 8.7 MB heap + ~89 MB file-backed mapping (evictable) |
| `redfs_read` of a 40 KB file | 0.16 ms |
| `redfs_mesh_open`, cold | 1–2 ms |
| `redfs_mesh_open`, cached | ~0 ms |
| `redfs_path_load` (full dictionary) | ~135 MB, opt-in |

The path dictionary is the only large optional cost. Load it only if you call
`redfs_path_from_hash`.

---

## Failure modes worth handling

**Oodle missing.** Resolved lazily and never fatal at mount. A compressed read
returns `REDFS_E_OODLE`. Inside the game it is always present; a standalone tool
pointed at a broken install may hit this.

**A path that does not resolve.** Most invented paths do not exist. Check with
`redfs_exists` and fail loudly rather than silently doing nothing — a mod that
works on your install and not the user's is usually this.

**Mods changing between runs.** The mesh cache fingerprints the mounted archive
set and discards itself when that changes, so stale geometry cannot be served.
The cost is a cold cache after the user installs a mod.

**Running outside MO2.** Under MO2 the VFS only exists in processes MO2 launched.
Inside the game, mod archives appear normally. In a standalone tool they are
invisible — use `redfs_depot_mount_dir` on the staging folder.
