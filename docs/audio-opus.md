# Voice-over audio: `.opusinfo` and `.opuspak`

**Status: NOT implemented.** Research complete, design below.

What RedFS does today: `redfs_audio_probe` sniffs the container and
`redfs_read` hands back raw bytes. `.wem` works directly that way — it is Wwise
RIFF and any decoder takes it. Voice-over does not, because it is packed.

## How voice-over is stored

Two file kinds, in `base\sound\soundbanks\`:

- **`sfx_container_N.opuspak`** — a concatenation of Opus streams, no index
- **`*.opusinfo`** — the index for all of them

So an individual voice line is not an archive entry. It is a byte range inside a
pak, and the only thing that knows the range is the opusinfo. Reading it needs
two files and an index lookup, which is why it did not fall out of the generic
path.

## `.opusinfo` layout

Derived from `WolvenKit.Modkit/RED4/Tools/OpusInfo.cs`. Little-endian.

```
0x00   12   header      'S' 'N' 'D' ' ' 02 00 00 F0 00 00 00 00
0x0C   4    opus_count
0x10   4    grouping_obj_size_4x

then six parallel arrays, each opus_count long, in this order:

  u32  opus_hashes[]           id of the stream
  u16  pack_indices[]          which sfx_container_N.opuspak
  u32  opus_offsets[]          byte offset into that pak
  u16  riff_opus_offsets[]     offset of the Opus data within the RIFF wrapper
  u32  opus_stream_lengths[]   compressed length
  u32  wav_stream_lengths[]    decoded length

then, until EOF, grouping objects:
  u32  hash
  u32  member_count
  u32  member_hashes[member_count]
```

Six parallel arrays rather than an array of structs — so extracting stream *i*
means indexing all six at the same position.

## Design

Three calls, following the shape of the existing typed helpers.

```c
/* Load and index every .opusinfo in the depot. Opt-in, like the path
   dictionary: costs memory only if voice-over is wanted. */
REDFS_API redfs_status redfs_audio_index(const redfs_depot* depot, uint32_t* out_streams);

typedef struct redfs_opus_desc {
    uint32_t hash;              /* opus stream id                    */
    uint32_t pack_index;        /* sfx_container_N                   */
    uint32_t offset;            /* byte offset into that pak         */
    uint32_t riff_offset;       /* Opus data within the RIFF wrapper */
    uint32_t compressed_size;
    uint32_t decoded_size;
} redfs_opus_desc;

REDFS_API redfs_status redfs_opus_desc_of(const redfs_depot*, uint32_t opus_hash,
                                          redfs_opus_desc* out);

/* Raw Opus bytes for one stream. Decoding to PCM is the caller's job --
   libopus is a dependency RedFS should not acquire. */
REDFS_API redfs_status redfs_opus_read(const redfs_depot*, uint32_t opus_hash,
                                       redfs_blob* out);
```

### Notes on the design

**Indexing is opt-in.** Same reasoning as the path dictionary — a mod that wants
textures should not pay for an audio index it never queries.

**Stream ids are `u32`, not the `u64` depot hashes.** A different namespace, so
they get their own lookup rather than being forced into `redfs_depot::refs`.

**Reads must not pull the whole pak.** A `.opuspak` is large and
`redfs_opus_read` wants a few kilobytes from the middle of it. The existing
`Archive::read_segment` decompresses a whole segment, so this needs either a
ranged variant or a small pak-level cache. The former is cleaner and would also
benefit large-buffer reads generally.

WolvenKit sidesteps this by extracting the entire pak to a `MemoryStream` per
export — acceptable for a batch tool, not for a runtime library.

**Grouping objects are parsed but not exposed** initially. They map a group hash
to member streams; useful for "all lines for this character", but the mapping
from anything user-facing to a group hash is not yet understood.

## Open questions

- How a **stream id relates to anything a modder can name**. The ids are `u32`
  and appear in `.wem`-adjacent metadata; the join from a subtitle line or a
  VO event to an opus hash has not been traced.
- Whether `riff_opus_offsets` means the stream is wrapped in RIFF inside the pak
  or whether the field describes the *original* wrapper. WolvenKit's
  `WriteOpusFromPak` would answer it.
- Whether `.opusinfo` files differ between `content` and `ep1`, and whether
  indices must be merged or kept per-source.

## Effort

Roughly a day. The index format is fully known; the work is the ranged read and
resolving the two questions above. Lower value than it looks, because a caller
still needs libopus to hear anything — RedFS would be handing over compressed
frames.

`.wem` already works via `redfs_read`, and that covers SFX, which is what most
mods want.
