# RedFS documentation

Three pages here are for using the library. The rest is research: `done/` records
what was built, and the notes beside it cover what was not.

## Start here

| | |
|---|---|
| [USAGE.md](USAGE.md) | how to build, link and use it — recipes, the C++ wrapper, pitfalls |
| [INTEGRATION.md](INTEGRATION.md) | lifecycle inside the game: RED4ext, CET, mod managers, threading, and what happens at game close |
| [API.md](API.md) | the five requested calls, mapped to what exists |
| [../README.md](../README.md) | overview and measured numbers |

`include/redfs.h` is the reference for signatures, error codes and per-call
guarantees. Nothing here restates it.

## `done/` — implemented, with the reverse-engineering behind it

**A record of how each piece was worked out and why it is shaped that way — not
API documentation.** Each note opens with the source file it describes and says
what was found, what was built, and what was deliberately left out. Where one of
these disagrees with `redfs.h`, the header wins.

| | |
|---|---|
| [done/archive-format.md](done/archive-format.md) | the `.archive` container: index, segments, Kraken, mount order, why the index is mapped rather than copied |
| [done/cr2w-format.md](done/cr2w-format.md) | CR2W, and the insight that lets it be read **without** the RED4 type system |
| [done/path-hashing.md](done/path-hashing.md) | FNV1a64 forward, and why the reverse direction can only ever be a dictionary |
| [done/texture-pipeline.md](done/texture-pipeline.md) | `.xbm` → DDS, the format mapping, and three bugs the verifier caught |
| [done/mesh-geometry.md](done/mesh-geometry.md) | chunk decoding, and computing the bounding boxes the format does not store |
| [done/api-design.md](done/api-design.md) | why the API is shaped this way, including what was cut |
| [done/caching.md](done/caching.md) | the persistent mesh cache, its file format and its invalidation |
| [done/verification.md](done/verification.md) | how correctness was established — five independent oracles, and what the sweep found |
| [done/testing.md](done/testing.md) | unit tests, leak detection, ASan, fuzzing, static analysis — and the out-of-bounds read they caught |

## Not implemented — research and design

Written up so they are actionable rather than vague. Each carries a cost estimate
and an honest read on whether it is worth doing.

| | |
|---|---|
| [audio-opus.md](audio-opus.md) | `.opusinfo` / `.opuspak` voice-over demuxing |
| [vertex-streams.md](vertex-streams.md) | normals, UVs, skinning — beyond positions |
| [roadmap.md](roadmap.md) | everything open, ordered by value, plus what was rejected and why |

## Reading order

If you are picking this up cold:

1. **USAGE.md** — get something working
2. **done/archive-format.md** then **done/cr2w-format.md** — the two formats
   everything else sits on
3. **done/api-design.md** — why the surface looks like it does
4. **done/verification.md** — how much to trust any of it

If you are extending it, read **roadmap.md** first: several obvious ideas are
there under "considered and rejected", with reasons.

## Sources

The formats were derived from [WolvenKit](https://github.com/WolvenKit/WolvenKit)'s
C# implementation, principally `WolvenKit.RED4/Archive/` and
`WolvenKit.Modkit/RED4/Tools/`, then confirmed against a live 2.3 + Phantom
Liberty install. WolvenKit is the reference for this format, and its `texconv.dll`
serves as one of the verification oracles.
