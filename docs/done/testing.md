# Testing and memory tooling

**Status: implemented** — `tests/`, `run-checks.ps1`

`verification.md` covers correctness against external oracles, which needs a game
install. This covers the tooling that does not: unit tests, fixtures, leak
detection, sanitizers, fuzzing, static analysis.

## Scope, before any numbers

**Nothing here has run inside the game.** There is no RED4ext plugin in the tree
and nothing deployed to `red4ext/plugins/`. Every check below runs in a standalone
test process. The lifecycle suite approximates a plugin's life closely — real
`LoadLibrary` / `FreeLibrary` on the real DLL, real abrupt process exit — but it is
an approximation, not the engine.

Read "1175 checks pass" with that in mind.

## The gap the fixtures closed

Before `tests/fixtures.cpp`, every check needed an 85 GB game install. That made
them all integration tests: slow, unrunnable in CI, impossible on a machine
without the game, and incapable of covering malformed input, because a real
install never produces any.

The builders produce `.archive` and CR2W containers byte-exactly from nothing. The
tests run anywhere in under a second and can feed the parsers inputs no real file
would contain.

They were written from the format docs **independently of the reader**. If a
builder and the reader disagree, one of them has the format wrong — a second,
weaker oracle on top of the first.

### Fixtures can lie, deliberately

A fixture that can only produce well-formed data cannot test a parser's response
to malformed data, and the gap is invisible: the test goes green and looks like
coverage. This hid real defects four times. The sharpest was `mipCount`, written
as `static_cast<uint8_t>(mips)` because stock cooks use `Uint8` — so a regression
test for a 4-billion-iteration hang passed against the *unfixed* reader, with
`0xFFFFFFFB` arriving as 251.

CR2W is self-describing: a property's declared type name and an array's element
count both travel in the file, so hostile content picks them freely. Fixtures now
can too.

| capability | what it makes reachable |
|---|---|
| `prop_in_uint(name, value, min_width, type)` | widens to hold the value instead of truncating to the stock width; can label a value with a type its length contradicts |
| `prop_array` / `prop_array_cname` with an explicit count | "claim 0xFFFFFFFF elements, write three" — every path that trusts a declared count |
| `Cr2wBuilder::chunk_extent` | a chunk table entry that lies about extent, reaching `cr2w_find`'s "runs past the end of the blob" check |
| `ArchiveBuilder::segment_range` | an index entry that lies about which segments a file owns, reaching `resolve_part`'s reject and `fill_info`'s clamp |
| `TextureOverrides` | buffer index with no segment behind it; a dangling `renderResourceBlobPC` handle; `mipCount` width and type name |
| `MeshOverrides` | declared chunk / appearance / material / LOD counts, `slotStrides[0]`, appearance handle target |

Every override defaults to what a stock cook writes, so output stays byte-identical
to a real file until a test asks for something a real cook would not produce.

`ArchiveBuilder` also emits a real CRC-64 over the index body — reflected, poly
`0xC96C5795D7870F42`, init and xorout all-ones, matching
`WolvenKit.Core/CRC/CRC64Algo.cs` — plus a content-derived per-entry digest. Both
were previously constant, which made every cache-invalidation fix untestable: the
whole point of the index CRC is to notice an archive rebuilt in place, and a
constant placeholder is blind to exactly that.

## What runs

```
.\run-checks.ps1                                    # no game needed
.\run-checks.ps1 -GameDir "D:\...\Cyberpunk 2077"   # + integration
```

Steps, in order. Each configuration catches something the others cannot:

| configuration | what it runs | what it catches |
|---|---|---|
| release | unit tests, fuzzer (30k), lifecycle | logic errors, contract violations |
| debug (static only) | unit tests | leaks, via the CRT heap at steady state |
| asan | unit tests, fuzzer (30k), fuzzer at seeds 7/42/1337, lifecycle | out-of-bounds, use-after-free, in every parser |
| install (`-GameDir`) | `redfs_cli selftest`, then `redfs_verify` at 12000 | format-level correctness vs DirectXTex and CDPR's own metadata |

ASan is skippable with `-SkipAsan`; the install leg is skipped without `-GameDir`,
and its DirectXTex half is skipped with a warning if `texconv.dll` is absent.

Individual pieces:

```
build\redfs_test.exe [filter]        # 70 cases, 1175 checks
build\redfs_fuzz.exe [iters] [seed]  # mutation fuzzer, deterministic
build\redfs_lifecycle.exe            # teardown: abrupt exit, shutdown, dll unload
cmake -DREDFS_ANALYZE=ON             # MSVC /analyze
ctest --test-dir build               # unit + 4000-iteration fuzz + lifecycle
```

## Coverage

### Covered

| area | cases | notes |
|---|---|---|
| path hashing | 4 | canonical FNV vectors, normalisation, edge cases, decimal round trip |
| archive index and segments | 7 | round trip, buffers as separate parts, not-found, `read_into` sizing, mount-order overrides, garbage and truncated indices rejected |
| mount layering | 8 | install scan order, scan flags, deep override chains, REDmod folder order, nested REDmod archives ordered by full path, remount after adding a mod |
| CR2W | 15 | every value kind, enums, dotted paths into nested structs, handles, deferred buffers (incl. the lowercase spelling), imports, `walk` with early stop, fixed-width / enum / string arrays, repeated string reads, malformed documents, out-of-range chunk index |
| textures | 4 | descriptor and format mapping, non-texture rejection, **emitted DDS describes its own payload**, absurd mip count rejected rather than spun on |
| meshes | 5 | chunk decoding, bounds arithmetic, appearances, non-mesh rejection, declared counts not trusted, impossible position stride, **`desc_of` and `mesh_open` agree** |
| audio | 4 | WEM header parse, PCM duration derivation, RIFF chunk walking, malformed WEM |
| path dictionary | 4 | reverse lookup, learning from imports, non-canonical imports folding to the canonical hash, returned pointer surviving later additions |
| API contract | 19 | null arguments, double free, NUL-terminated blobs, out-of-range `part`, lying segment ranges, **cache round trip preserves the public view**, cache invalidation on mount and on in-place archive replacement, mesh handle surviving `cache_close`, async exactly-once, shutdown cancellation, callback re-entrancy, depot close from a callback, concurrent `mesh_open`, C++ facade instantiation, status strings |
| teardown | 4 scenarios | see lifecycle, below |

The three in bold are the semantic checks — the ones that assert an *answer* rather
than the absence of a memory error. They were added because 1006 checks, ASan, a
30k-iteration fuzzer and a leak checker all passed green while the mesh cache
served stale geometry and every emitted cubemap DDS was unloadable. Neither defect
touched memory wrongly, so nothing in the arsenal could see them.

The structural reason, for textures: every existing test asserted on
`redfs_texture_desc`, which is the encoder's *input*. No number of tests in that
shape can catch an encoder bug. The DDS test parses the header back the way a
loader does and checks it against a separately written mip-chain calculation, so
the two derivations can actually disagree.

Both new invariants were verified by breaking them: reverting the cubemap encoding
fails the DDS test on two independent assertions, and dropping `bounds_valid` from
the cache writer fails the round trip in three places.

### Not covered

| area | why, and what would close it |
|---|---|
| **Kraken decompression** | Every fixture is stored uncompressed, because compressing needs `OodleLZ_Compress` from the game DLL. The decode path is exercised only by the integration sweep. A round-trip test gated on the DLL being present would close it. |
| **Data races** | Concurrency is *tested* — 8 threads × 200 `mesh_open` calls, async exactly-once, callback re-entrancy — but a passing run is not proof of race-freedom. MSVC ships no TSan, so nothing here can detect a race that happens not to fire. |
| **In-game behaviour** | No plugin exists. See the scope note above. |
| **Phantom Liberty–only content paths** | The reference install has `ep1` mounted (25 of its 57 archives), so PL archives are read by the integration sweep — but nothing targets `ep1`-specific resource shapes deliberately. |
| **Mod and REDmod layering against real content** | The reference install has no `archive/pc/mod` directory at all and zero folders under `mods/`; all 57 archives are `content` + `ep1`. So the legacy-mod path, the REDmod path and the recursive REDmod discovery are exercised by synthesized fixtures only — 5 of the 8 layering cases — and never against an actual installed mod. The fixtures encode WolvenKit's `ArchiveManager` as the reference for what the game does, including the asymmetry that REDmod recurses while `archive/pc/mod` does not; nothing has confirmed that reading against real mod content. |
| **Pixel content** | Sizes and headers are verified; that the bytes form a correct image is demonstrated only for cubemaps and only by hand. See `verification.md`. |
| **64-bit boundaries** | No test uses files above 4 GB or archives near the address-space limits. |
| **Rare intermittents** | Every stage runs *once* per invocation, so a fault that fires at a low rate is effectively invisible. A stale-function-pointer bug in the unload scenario reproduced at 2 % — one bad run in fifty — and survived eight consecutive green suites before showing up. Nothing loops the teardown scenarios, and a 2 % fault needs on the order of 150 runs to be seen reliably. |
| **Peak memory** | The leak checker proves steady state, not a bound. A large verify run reaches ~2.47 GB peak working set. It plateaus rather than growing — 8,000 and 20,445 meshes reach the same figure — so it is a transient from a few large files, and both the CRT leak check and ASan are clean. Nothing asserts on it, which matters because a mod shares an address space with a game already holding 8–12 GB. |

## Lifecycle tests

`redfs_test` covers behaviour inside one process. `redfs_lifecycle` covers how
RedFS *dies*, which needs child processes because the scenarios are about
teardown. Every child is waited on with a timeout, so a hang fails like a crash.

| scenario | asserts |
|---|---|
| return from `main` with 500 reads in flight, no shutdown | exits cleanly, no hang |
| `ExitProcess` with 500 reads in flight, no shutdown | exits cleanly, no hang |
| `redfs_shutdown` with 400 × ~96 MB queued | bounded latency, every callback resolved |
| `LoadLibrary` → use → `redfs_shutdown` → `FreeLibrary`, ×3 | no crash after unload |

The last one is the important one: it is what RED4ext does to a plugin, and it is
the case that fails without `redfs_shutdown()` — the worker would still be
executing code `FreeLibrary` just unmapped. Each round sleeps briefly after the
unload to give a surviving thread a window to fault. It runs under ASan too, and
skips cleanly (rather than failing) in configurations that build only the static
library, because a skip reported as a failure trains people to ignore the test.

That scenario is also a standing trap for the harness itself, and it caught the
harness once. Any function pointer obtained from the module is only valid for
that round: caching one in a function-local `static` pinned round 0's address,
and later rounds passed it to a callback that `redfs_shutdown` invoked after the
image had been unmapped. It faulted at 2 % rather than always, because a reloaded
DLL usually lands at the same base and the stale pointer keeps resolving by
accident. Diagnosis took an unhandled-exception filter reporting the faulting
address against each round's recorded image range — the tell was that the
faulting thread was `main`, not the worker, so nothing had survived the join.
Refetch per round; never cache across a `FreeLibrary`.

The shutdown-latency scenario enforces a 2-second ceiling and all 400 callbacks
accounted for; measured at **2.1 ms with ~38 GB of nominal work queued**. That
bound comes from cancelling queued work and aborting the in-flight read at its next
segment boundary — Oodle cannot be interrupted mid-block, so one segment is the
finest granularity available. It never comes from abandoning the join, which would
reintroduce the unmapped-code crash. See `../INTEGRATION.md`.

## Leak detection: measure steady state, not the process

The naive approach — checkpoint at startup, dump at exit — reports RedFS's three
deliberately-leaked singletons on every run: the async worker, the mesh cache and
the path dictionary. They are never destroyed on purpose, because running their
destructors from a static destructor happens at `DLL_PROCESS_DETACH` under the
loader lock, which deadlocks a mod DLL (see `api-design.md`).

A report full of expected noise is a report nobody reads. So the runner instead:

1. runs the full suite twice, discarding output — populating every lazy singleton
2. takes a heap checkpoint
3. runs the suite again and diffs

Anything allocated in the measured pass and not released is real, because by then
nothing is being created for the first time.

**Two warm-up passes, not one.** Some global state is enabled by a test rather than
at startup: the path dictionary starts off and only begins learning from CR2W
imports once a later test switches it on. So pass 2 populates things pass 1 never
reached, and a single warm-up leaves 82 bytes of legitimate growth that looks like
a leak. Two settles it.

This runs in the debug configuration only, which `run-checks.ps1` configures with
`-DREDFS_BUILD_SHARED=OFF`. That is also why the leak step runs `redfs_test` alone:
with no DLL built, the lifecycle suite's unload scenario has nothing to load and
would skip.

## AddressSanitizer

`cmake -DREDFS_SANITIZE=address`. MSVC ships the runtime, but it is not on PATH,
and the executables silently fail to start without it — so `run-checks.ps1` finds
`clang_rt.asan_dynamic-x86_64.dll` under the MSVC toolchain and copies it next to
the binaries. ASan is also incompatible with the incremental linker and with
`/RTC`, both of which the build disables under it.

Under ASan the unit tests, the fuzzer and the lifecycle suite all become
memory-safety sweeps.

## Fuzzing

RedFS parses data it did not write. A corrupt archive or a hostile mod must produce
an error, never a crash and never a read outside the buffer.

No libFuzzer in this toolchain — Visual Studio ships `clang-format` and
`clang-tidy`, not `clang-cl` — so `tests/fuzz_redfs.cpp` is a self-contained
mutation loop. That is enough here, because the input space that matters is
"structurally valid file with something broken in it", not arbitrary bytes. The
seed corpus is synthesized textures, meshes and archives from the fixtures, so
mutations land on meaningful fields instead of being rejected at the magic check.

Mutations target what actually breaks parsers — length and offset fields:

```
flip byte / flip bit        general corruption
zero a region               truncated-looking structures
u32 -> 0x7FFFFFFF           enormous size or offset
u32 -> 0xFFFFFFFF           underflows a subtraction
u32 -> a wrap-to-zero value spins a walk loop that never advances
truncate                    short reads
duplicate byte              index confusion
```

The wrap-to-zero set exists because the other mutations could not reach it.
`struct_end` advanced by `8 + (sz - 4)`, which is zero only for `sz ==
0xFFFFFFFC`; `0x7FFFFFFF` and `0xFFFFFFFF` advance by ~2 GB and 3 respectively, so
20k iterations passed straight over a hang that one well-chosen value finds. A
wrap to exactly zero is far worse than a wrap to something large: the pointer does
not move, so the loop spins instead of failing a bounds check.

Two targets, one archive run in four. The CR2W target parses a mutated document
and then exercises the whole query surface against it — chunk and import
enumeration, dotted-path `get`, `walk` descending into every array — because a
parse *succeeding* is exactly when a bad offset does damage. The archive target is
the sharper one: it writes the mutated bytes to disk, mounts them, enumerates, and
reads every part of every entry plus `texture_desc_of` and `mesh_open`. The index
is *mapped*, so a bad segment offset reads outside the mapping rather than off the
end of a heap block.

**A watchdog thread, because a hang does not announce itself.** A crash is
obvious; an infinite loop just wedges the run and looks like slow progress, which
is how the `struct_end` hang survived 20k iterations. A watchdog aborts if any
single input makes no progress for ~10 s, printing the iteration and seed to
reproduce. Hangs now fail the run like any other defect.

Deterministic throughout: xorshift64*, seeded, so a failure replays exactly.
`run-checks.ps1` runs 30,000 iterations at seed 1 in both release and ASan, plus
10,000 each at seeds 7, 42 and 1337.

## What the tooling found

### An out-of-bounds read on corrupt input (ASan + fuzzer)

The fuzzer crashed on its 4th iteration, the first archive case. ASan pointed
straight at it:

```
ERROR: AddressSanitizer: access-violation ... READ memory access
    #0 _asan_wrap_strcmp
    #1 redfs::cr2w_decode          src\cr2w.cpp
    #2 redfs::descend              src\cr2w.cpp
    #5 redfs::mesh_build           src\mesh.cpp
```

The cause, in `redfs_cr2w`:

```cpp
const char* name(uint32_t i) const {
    return i < names.size() ? strings + names[i] : "";   // index checked, OFFSET not
}
const char* str(uint32_t off) const {
    return off < strings_size ? strings + off : "";      // offset checked
}
```

`name()` validated the *index* but not the string-table *offset* it looked up,
while its sibling `str()` validated exactly that. A corrupt name table holds
arbitrary offsets, so the returned pointer landed outside the buffer and the
`strcmp` in `cr2w_decode` walked until it hit an unmapped page.

Security-relevant: a malicious or merely damaged `.archive` could trigger an
out-of-bounds read inside the game process.

Fixed two ways — `name()` now routes through `str()`, and `cr2w_parse` requires the
string table to end in a NUL, which upgrades "the offset is in range" to "the
string is safe to walk". 30,000 mutations across four seeds clean afterwards.

### A use-after-free closing a depot from a read callback (ASan)

`Worker::cancel_for` began `if (on_worker_thread()) return;`, reasoning about the
in-flight job — which is indeed the one running the callback. But the function
exists to drain the *queued* jobs, each holding a raw depot pointer that
`redfs_depot_close` then deletes. Closing a depot from inside a read callback left
the worker walking freed memory; ASan reported heap-use-after-free in
`redfs_depot::locate`.

Only the wait needs skipping on the worker thread; the purge always has to happen.
Found by review tracing a header promise — "closing a depot with work outstanding
is safe" — into the code to see which of the two was wrong, then named precisely by
ASan. Regression test added and verified by reverting the fix.

### Two bugs in the fixtures themselves

Worth recording, because they are the same class of mistake the library is being
tested for:

- `Cr2wBuilder::build` interned strings *after* writing the string blob, so late
  additions vanished and the reader saw empty class names. Names must settle before
  strings do, because interning a class name adds a name and every name needs a
  string. The `const_cast` the old version needed was the smell.
- `pending_` was a `std::vector` while `stack_` held pointers into it, so nesting
  `begin_struct` reallocated and dangled them. Now a `std::deque`.

### Static analysis

`/analyze` flagged four sites. All were false positives in reachable code, but each
marked a genuinely unguarded assumption, and a noisy analyser gets ignored:

- `locate()` indexed `archives[]` without a bounds check — safe today because
  `refs` is built from `archives`, but the invariant lives in `reindex()` far away,
  and a stale ref would be a use-after-free rather than a miss
- `resolve_part` dereferenced `loc.archive` without checking it was set
- `Prop` was null-initialised despite every field flowing into `strcmp`; now
  defaults to `""`
- `str()` could return `strings + off` with a null `strings` on a
  default-constructed handle

All four are now guarded. Analysis stays off by default (`-DREDFS_ANALYZE=ON`)
because it is slow and noisy on system headers.

### A compile error shipped in the C++ facade

`redfs.hpp`'s `for_each` and `Cr2w::walk` took `Fn&&` and then did
`static_cast<Fn*>`. For an lvalue callable `Fn` deduces to `L&`, and `L&*` is not a
type — MSVC rejects it with C2528 — so passing a *named* lambda did not compile
while an inline one did.

Nothing in the tree instantiated either template: the C++ facade had no coverage at
all. `test_redfs.cpp` now includes `redfs.hpp` and
`cpp_facade_accepts_named_callables` exists mostly to be **compiled**. If the
deduction regresses, the build breaks there.
