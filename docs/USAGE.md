# Using RedFS

Practical guide. For what each of your five calls maps to, see `API.md`; for how
things work internally, see `done/`.

---

## 1. Build it

```
cd C:\Work\WorkSpace\Cyberpunk\RedFS
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Needs MSVC (2022, C++20) and CMake ≥ 3.21. No dependencies to fetch. Out of
`build/`:

| artifact | what it is |
|---|---|
| `redfs_static.lib` | link into your own DLL — **usually what you want** |
| `RedFS.dll` + `redfs_shared.lib` | one shared copy for several mods |
| `redfs_cli.exe` | exploration and diagnostics |
| `redfs_verify.exe` | correctness sweep |
| `example_chest.exe` | worked example |

From a Bash shell you need the MSVC environment first:

```bash
cmd //c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build'
```

## 2. Link it

**CMake**

```cmake
add_subdirectory(third_party/RedFS)
target_link_libraries(my_mod PRIVATE redfs_static)
```

**By hand** — add `include/` to your include path, link `redfs_static.lib`, and
define `REDFS_STATIC` before including the header. Against the DLL, define
nothing (the header defaults to `dllimport`).

```c
#include "redfs.h"      /* C */
#include "redfs.hpp"    /* C++ RAII wrapper, optional */
```

## 3. Open a depot

Once, at startup. Keep it for the process lifetime — reopening re-reads every
index.

```c
redfs_depot* depot = NULL;
redfs_status st = redfs_depot_open(NULL, REDFS_SCAN_ALL, &depot);
if (st != REDFS_OK) {
    log("RedFS: %s (%s)", redfs_status_string(st), redfs_last_error());
    return;
}
```

`NULL` auto-detects the install by walking up from the running executable — right
when you are inside the game. Pass an explicit directory for an external tool:

```c
redfs_depot_open("D:\\SteamLibrary\\steamapps\\common\\Cyberpunk 2077",
                 REDFS_SCAN_ALL, &depot);
```

`REDFS_SCAN_ALL` mounts `content` + `ep1` + `mod`. Narrow it if you want only
vanilla data: `REDFS_SCAN_CONTENT | REDFS_SCAN_EP1`.

Costs ~30 ms and ~8.7 MB of heap for 544,670 files. Tear down with
`redfs_depot_close(depot)`.

## 4. Read something

Everything is addressed by hash. `redfs_hash` normalises for you, so
`Base/Icon/Foo.XBM` and `base\icon\foo.xbm` are the same key.

```c
uint64_t key = redfs_hash("base\\path\\to\\file.xbm");

if (!redfs_exists(depot, key)) { /* not in this install */ }

redfs_blob blob;
if (redfs_read(depot, key, REDFS_PART_ALL, &blob) == REDFS_OK) {
    /* blob.data, blob.size */
    redfs_blob_free(&blob);
}
```

Note the doubled backslashes in C string literals. Forward slashes work too and
are less error-prone: `redfs_hash("base/path/to/file.xbm")`.

### Which part?

A file is a CR2W document plus attached buffers.

```c
REDFS_PART_MAIN   /* just the document -- cheap, use for metadata */
REDFS_PART_ALL    /* document + every buffer */
0, 1, 2, ...      /* one buffer: pixels, vertex streams */
```

Reading `PART_MAIN` of a 40 MB texture costs kilobytes. Prefer it whenever you
only need to inspect.

## 5. Common recipes

### A texture, ready for D3D

```c
redfs_blob dds;
if (redfs_texture_read_dds(depot, redfs_hash(path), &dds) == REDFS_OK) {
    DirectX::CreateDDSTextureFromMemory(device, dds.data, (size_t)dds.size,
                                        &resource, &srv);
    redfs_blob_free(&dds);          /* safe once D3D has copied it */
}
```

Metadata only, without reading pixels:

```c
redfs_texture_desc t;
redfs_texture_desc_of(depot, key, &t);
/* t.width, t.height, t.mip_count, t.dxgi_format, t.data_size */
```

### Mesh chunks with bounding boxes

```c
redfs_mesh* m;
if (redfs_mesh_open(depot, redfs_hash(mesh_path), &m) == REDFS_OK) {
    for (uint32_t i = 0; i < redfs_mesh_chunk_count(m); ++i) {
        const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
        if (c->lod != 1) continue;              /* one copy, not every LOD */
        if (c->bbox_min[2] > 1.2f)              /* upper body, game space Z */
            mask |= (1ull << c->index);         /* index IS the chunkMask bit */
    }
    redfs_mesh_close(m);
}
```

Turn on the cache first (see below) or this costs ~1–2 ms per mesh.

### Audio: getting a `.wem` out

Extraction is the format-agnostic path — no helper needed:

```c
redfs_blob wem;
redfs_read(depot, redfs_hash("base\\localization\\en-us\\vo\\line.wem"),
           REDFS_PART_ALL, &wem);
fwrite(wem.data, 1, wem.size, out);   // a byte-identical .wem
redfs_blob_free(&wem);
```

If you want to know what you got before handing it to a converter:

```c
redfs_audio_info info;
redfs_audio_info_of(depot, key, &info);
// info.codec == REDFS_CODEC_VORBIS, channels, sample_rate,
// info.data_offset / data_size  -- where the payload actually starts
```

**RedFS does not convert audio, and will not.** That would mean bundling Vorbis
and Opus, and Wwise Vorbis additionally needs its stripped codebooks rebuilt.
That job belongs to tools built for it:

```
wem -> ogg     ww2ogg  (or vgmstream, which handles more Wwise variants)
ogg -> mp3     ffmpeg / lame
```

or `ffmpeg -i in.wem out.mp3` directly, if your ffmpeg build has the codec. What
RedFS gives you is the exact bytes and an honest description of them, which is
the part that needed the archive.

`redfs_audio_walk_chunks` enumerates the RIFF chunks, because Wwise keeps codec
state in non-standard ones (`vorb`, `seek`) that a decoder front-end has to find.

### Anything else — including facial rigs

There is no format-specific helper for most resource types, and mostly there does
not need to be. Any cooked resource is a CR2W, so the generic reader already
handles it. A `.facialsetup`, for instance:

```c
redfs_blob main; redfs_cr2w* f;
redfs_read(depot, key, REDFS_PART_MAIN, &main);
redfs_cr2w_open(main.data, main.size, &f);
// root type          animFacialSetup
// imports[0]         the .rig it drives
// bakedData          -> buffer 0
// mainPosesData      -> buffer 1
// correctivePosesData-> buffer 2
// faceCorrectiveNames-> array of 255 CNames
```

Each `DataBuffer` value gives a buffer index you pass straight to `redfs_read`.
So the whole rig — skeleton reference, baked data, pose data, corrective names —
is reachable without RedFS knowing anything about facial animation.

```c
redfs_path_load(depot, "usedhashes.kark", &kept);   /* once, at startup */
const char* path = redfs_path_from_hash(some_hash); /* NULL if unknown */
```

The list is `WolvenKit.Common/Resources/usedhashes.kark`, or any plain-text file
of one path per line. Ship a copy alongside your mod. 99.97 % coverage on a stock
install; without it, only paths learned from import tables resolve.

### Crossing into Lua

Lua numbers are doubles and cannot hold a `u64` exactly, so move keys as text:

```c
char key[REDFS_HASH_STRING_MAX];        /* 21 bytes is always enough */
redfs_hash_string(path, key, sizeof key);
uint64_t h = redfs_hash_parse(key);
```

### Dependencies of a resource

```c
redfs_blob main; redfs_cr2w* f;
redfs_read(depot, key, REDFS_PART_MAIN, &main);
redfs_cr2w_open(main.data, main.size, &f);

for (uint32_t i = 0; i < redfs_cr2w_import_count(f); ++i)
    printf("%s\n", redfs_cr2w_import_path(f, i));

redfs_cr2w_close(f);
redfs_blob_free(&main);     /* AFTER cr2w_close -- the parser borrows these bytes */
```

### Anything RedFS has no helper for

The generic property reader works on any cooked resource without RedFS knowing
the type:

```c
redfs_value v;
redfs_cr2w_get(f, 0, "someStruct.someField", &v);
switch (v.kind) {
    case REDFS_KIND_UINT:   use(v.as.u); break;
    case REDFS_KIND_NAME:   use(v.as.s); break;   /* CName or enum */
    case REDFS_KIND_HANDLE: use(v.as.chunk); break;
    case REDFS_KIND_BUFFER: redfs_read(depot, key, v.as.buffer, &b); break;
}
```

`redfs_cr2w_walk` enumerates a chunk; `redfs_cr2w_walk_array` iterates an array.
Explore interactively with `redfs_cli cr2w` and `redfs_cli arr` first.

## 6. Turn on the mesh cache

Chunk bounds cost a geometry decompress. Archives never change, so pay once:

```c
redfs_cache_open(depot, "redfs_mesh.cache");
...
redfs_cache_flush();       /* periodically, or at shutdown */
redfs_cache_close();
```

0.72 ms cold → **0.00 ms warm**, and the cache survives process restarts. It
fingerprints the mounted archive set and silently discards itself if that set
changes, so a patch or a new mod can never serve stale geometry.

To precompute a known set at load:

```c
uint32_t computed;
redfs_cache_warm(depot, hashes, count, &computed);
```

Do **not** try to warm the whole depot — ~10⁵ meshes would cost minutes and
gigabytes. Warm what you actually touch.

## 7. Keep it off the render thread

Reads are milliseconds. Either call from your own worker, or:

```c
redfs_read_async(depot, key, REDFS_PART_ALL, on_loaded, user);
/* on_loaded runs on RedFS's worker thread -- marshal back yourself */

void on_loaded(redfs_status st, redfs_blob blob, void* user) {
    if (st == REDFS_OK) { queue_for_main_thread(blob); }  /* you now own it */
}
```

`redfs_drain()` blocks until queued reads finish. A depot is immutable once open,
so concurrent reads need no locking.

## 8. Mod managers

`REDFS_SCAN_ALL` finds base game, `ep1`, REDmod (`mods/<name>/archives`) and
legacy mods (`archive/pc/mod`) automatically, mounting in the game's own order so
later archives override earlier ones. Full details — override resolution, added
files, load order, scale — in [INTEGRATION.md](INTEGRATION.md).

**MO2 / Vortex:** their virtual filesystem only exists inside processes they
launch.

- Running **inside a game MO2 launched** — the merged mod archives already appear
  under `archive/pc/mod`. Nothing to do.
- Running a tool **outside MO2** — they are invisible. Mount the staging folder:

```c
uint32_t mounted;
redfs_depot_mount_dir(depot, "D:\\mods\\SomeMod", &mounted);
```

Or a single archive: `redfs_depot_mount(depot, "...\\thing.archive")`. Both go on
top at highest priority. Neither is thread-safe — mount before sharing the depot.

## 9. C++ wrapper

Same library, less typing. Header-only, RAII, `std::optional` instead of status
codes.

```cpp
#include "redfs.hpp"

auto depot = redfs::Depot::open();          // std::nullopt on failure
depot->enable_cache("redfs_mesh.cache");
depot->load_paths("usedhashes.kark");

if (auto m = depot->mesh("base\\...\\body.mesh")) {
    auto [lo, hi] = m->bounds();
    for (const auto& c : m->chunks())
        if (c.lod == 1 && c.bbox_min[2] > 1.2f)
            mask |= 1ull << c.index;
}

if (auto dds = depot->texture_dds(path))
    upload(dds->data(), dds->size());        // freed on scope exit
```

Mixing the two layers is fine — `handle()` on any wrapper gives the C pointer.

## 10. Diagnose with the CLI

```
redfs_cli [--game DIR] [--cache FILE] <command>

  info                          list mounted archives
  hash <path>...                path -> key
  paths <list> [hash...]        load a dictionary, resolve hashes
  find <list> <substr> [n]      files whose path contains substr
  stat <key>                    where it lives, how big, buffer sizes
  extract <key> <out> [part]    write bytes to a file
  cr2w <key> [chunk] [path]     chunks, imports, properties
  arr <key> <chunk> <path> [n]  elements of an array property
  chunks <key> [appearance]     per-chunk lod, material and bbox
  tex <key> [out.dds]           texture descriptor, optionally as DDS
  mesh <key>                    geometry layout
  audio <key>                   sniff the container
  bench                         read cost vs position in an archive
  selftest                      verify against the install
```

`<key>` is a depot path or a literal `0x`-prefixed hash. Finding something when
you half-remember the path:

```
redfs_cli --game "D:\...\Cyberpunk 2077" find usedhashes.kark "tshirt" 10
```

For verbose internal logging, set `REDFS_VERBOSE=1`.

## Pitfalls

- **`redfs_cr2w` borrows its bytes.** Free the blob only after `cr2w_close`.
- **`redfs_mesh_close` on a cached mesh is a no-op by design** — the cache owns
  it. Do not hold the pointer past `redfs_cache_close`.
- **Bounding boxes are game space, Z up**, not the Y-up flip glTF exporters use.
  Compare against component transforms directly.
- **Chunks repeat per LOD.** Filter on `lod` or you will count geometry twice.
- **`mount` and `close` are not thread-safe.** Everything else is.
- **Oodle is resolved lazily.** Missing `oo2ext_7_win64.dll` is not a mount
  failure; it surfaces as `REDFS_E_OODLE` on the first compressed read.
- **Reading from a path you guessed** usually fails. Confirm with
  `redfs_cli find` first — most invented paths do not exist.
