# Testing and memory tooling

**Status: implemented** — `tests/`, `run-checks.ps1`

`verification.md` covers correctness against external oracles. This covers the
tooling: unit tests, leak detection, sanitizers, fuzzing, static analysis.

## The gap this closed

Before this, every check needed an 85 GB game install. That made them all
integration tests: slow, unrunnable in CI, impossible on a machine without the
game, and incapable of covering malformed input because a real install never
produces any.

`tests/fixtures.cpp` builds `.archive` and CR2W containers byte-exactly from
nothing. The tests now run anywhere in under a second, and can feed the parsers
inputs no real file would ever contain.

The builders were written from the format docs **independently of the reader**.
If a builder and the reader disagree, one of them has the format wrong — which
is a second, weaker oracle on top of the first.

## What runs

```
.\run-checks.ps1                                    # no game needed
.\run-checks.ps1 -GameDir "D:\...\Cyberpunk 2077"   # + integration
```

| configuration | what it catches |
|---|---|
| release | logic errors, contract violations |
| debug | leaks, via the CRT heap |
| asan | out-of-bounds, use-after-free, in every parser |
| install | format-level correctness vs DirectXTex and CDPR's own metadata |

Individual pieces:

```
build\redfs_test.exe [filter]        # 33 cases, 261 checks
build\redfs_fuzz.exe [iters] [seed]  # mutation fuzzer, deterministic
build\redfs_lifecycle.exe            # teardown: abrupt exit, shutdown, dll unload
cmake -DREDFS_ANALYZE=ON             # MSVC /analyze
ctest --test-dir build               # unit + short fuzz + lifecycle
```

## Lifecycle tests

`redfs_test` covers behaviour inside one process. `redfs_lifecycle` covers how
RedFS *dies*, which needs child processes because the scenarios are about
teardown. A hang is treated as a failure just like a crash — every child is
waited on with a timeout.

| scenario | asserts |
|---|---|
| return from `main` with 500 reads in flight, no shutdown | exits cleanly, no hang |
| `ExitProcess` with 500 reads in flight, no shutdown | exits cleanly, no hang |
| `redfs_shutdown` with 400 × 96 MB queued | bounded latency, every callback resolved |
| `LoadLibrary` → use → `redfs_shutdown` → `FreeLibrary`, ×3 | no crash after unload |

The last one is the important one: it is exactly what RED4ext does to a plugin,
and it is the case that fails without `redfs_shutdown()` — the worker would still
be executing code `FreeLibrary` just unmapped. It runs under ASan too, and skips
cleanly in configurations that build only the static library.

Measured: **2.1 ms to shut down with ~38 GB of nominal work queued**, 400/400
callbacks resolved. That bound comes from cancelling queued work and aborting the
in-flight read at its next segment boundary — never from abandoning the join,
which would reintroduce the unmapped-code crash. See `../INTEGRATION.md`.

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

**Two warm-up passes, not one.** Some global state is enabled by a test rather
than at startup: the path dictionary starts off and only begins learning from
CR2W imports once a later test switches it on. So pass 2 populates things pass 1
never reached, and a single warm-up leaves 82 bytes of legitimate growth that
looks like a leak. Two settles it.

That investigation did surface one genuine wart: `paths_load` reserved
`size + text.size()/3` unconditionally, so reloading a dictionary grew capacity
on every call while adding nothing. Now reserved only on first load.

## AddressSanitizer

`cmake -DREDFS_SANITIZE=address`. MSVC ships the runtime; `run-checks.ps1` copies
`clang_rt.asan_dynamic-x86_64.dll` next to the binaries, since it is not on PATH
by default and the executables silently fail to start without it.

Under ASan the unit tests and the fuzzer both become memory-safety sweeps.

## Fuzzing

RedFS parses data it did not write. A corrupt archive or a hostile mod must
produce an error, never a crash and never a read outside the buffer.

No libFuzzer in this toolchain — Visual Studio ships only `clang-format` and
`clang-tidy`, not `clang-cl` — so `tests/fuzz_redfs.cpp` is a self-contained
mutation loop. That is enough here, because the input space that matters is
"structurally valid file with something broken in it", not arbitrary bytes. The
seed corpus is real synthesized textures, meshes and archives, so mutations land
on meaningful fields instead of being rejected at the magic check.

Mutations target what actually breaks parsers — length and offset fields:

```
flip byte / flip bit        general corruption
zero a region               truncated-looking structures
u32 -> 0x7FFFFFFF           enormous size or offset
u32 -> 0xFFFFFFFF           underflows a subtraction
truncate                    short reads
duplicate byte              index confusion
```

Deterministic: xorshift64*, seeded, so a failure replays exactly.

The archive target is the sharper one — it mounts a corrupted archive and reads
through it, and the index is *mapped*, so a bad segment offset reads outside the
mapping rather than off the end of a heap block.

## What the tooling found

### An out-of-bounds read on corrupt input (ASan + fuzzer)

The fuzzer crashed on its 4th iteration, the first archive case. ASan pointed
straight at it:

```
ERROR: AddressSanitizer: access-violation ... READ memory access
    #0 _asan_wrap_strcmp
    #1 redfs::cr2w_decode          src\cr2w.cpp:193
    #2 redfs::descend              src\cr2w.cpp:302
    #5 redfs::mesh_build           src\mesh.cpp:239
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

This is security-relevant: a malicious or merely damaged `.archive` could trigger
an out-of-bounds read inside the game process.

Fixed two ways — `name()` now routes through `str()`, and `cr2w_parse` requires
the string table to end in a NUL, which upgrades "the offset is in range" to "the
string is safe to walk". 30,000 mutations across four seeds clean afterwards.

### Two bugs in the fixtures themselves

Worth recording, because they are the same class of mistake the library is being
tested for:

- `Cr2wBuilder::build` interned strings *after* writing the string blob, so late
  additions vanished and the reader saw empty class names. The `const_cast` it
  needed was the smell.
- `pending_` was a `std::vector` while `stack_` held pointers into it, so nesting
  `begin_struct` reallocated and dangled them. Now a `std::deque`.

### Static analysis

`/analyze` flagged four sites. All were false positives in reachable code, but
each marked a genuinely unguarded assumption, and a noisy analyser gets ignored:

- `locate()` indexed `archives[]` without a bounds check — safe today because
  `refs` is built from `archives`, but the invariant lives in `reindex()` far
  away, and a stale ref would be a use-after-free rather than a miss
- `resolve_part` dereferenced `loc.archive` without checking it was set
- `Prop` was null-initialised despite every field flowing into `strcmp`; now
  defaults to `""`
- `str()` could return `strings + off` with a null `strings` on a
  default-constructed handle

Analysis is off by default (`-DREDFS_ANALYZE=ON`) because it is slow and noisy on
system headers.

## Coverage, honestly

**Covered:** archive index parsing, segment reads, buffer addressing, mount
ordering and overrides, every CR2W value kind, dotted paths, arrays, imports,
texture descriptor and DDS assembly, mesh chunk decoding and bounds arithmetic,
path hashing and reverse lookup, null handling and double-free across the API,
and malformed input for both containers.

**Not covered:**

- **Kraken decompression.** Every fixture is stored uncompressed, because
  compressing needs `OodleLZ_Compress` from the game DLL. The decompress path is
  exercised only by the integration tests. A round-trip test gated on the DLL
  being present would close this.
- **Threading.** `redfs_read_async`, the worker, and concurrent reads against a
  shared depot are untested. TSan is not available in MSVC.
- **The mesh cache's on-disk format.** Round-trip and invalidation are exercised
  by hand, not by a test.
- **Pixel content.** Sizes and headers are verified; that the bytes form a
  correct image is inferred. See `verification.md`.
- **64-bit boundaries.** No test uses files above 4 GB or archives near the
  address-space limits.
