# Voice-over audio: `.opusinfo` and `.opuspak`

**Status: NOT implemented.** Research is complete; everything under *Design* is a
proposal. No `redfs_opus_*` symbol exists.

## What the audio surface does today

RedFS reports what a payload is and where it lives. It never decodes:

- `redfs_audio_probe` — container from the first four bytes. `RIFF` → `WEM`,
  `BKHD` → `BNK`, `OggS` → `OPUSPAK`, the CR2W magic → `UNKNOWN` (a cooked
  resource, not raw audio). Not cheap: it decodes the whole main segment to read
  16 bytes, because Kraken cannot decode a prefix.
- `redfs_audio_info_of` / `redfs_audio_info_parse` — a `.wem`'s codec, channel
  layout, sample rate and payload offset. PCM also gets a sample count and
  duration; compressed codecs deliberately do not, because the byte rate is an
  average and the answer would be wrong in a way a caller trusts.
- `redfs_audio_walk_chunks` — enumerates RIFF chunks, including the non-standard
  `vorb` and `seek` that a decoder front-end needs to find.
- `redfs_read` — the raw bytes.

Vorbis and Opus decoding stay out on purpose: it would mean bundling both codecs,
and Wwise Vorbis additionally needs its stripped codebooks rebuilt. So "`.wem`
works" means the payload reaches a decoder the caller already has — PCM and ADPCM
play directly, Wwise Vorbis needs ww2ogg or vgmstream first.

Two loose ends there, both cheap and both on this work's path:

- **`REDFS_AUDIO_OPUSINFO` is declared but unreachable.** It is in the
  `redfs_audio_format` enum and nothing produces it: `audio_probe` does not test
  the `.opusinfo` magic (`'S' 'N' 'D' ' '`, 0x20444E53), so an index file reports
  `REDFS_AUDIO_UNKNOWN`.
- **The `OggS` → `OPUSPAK` mapping is unverified,** and WolvenKit's reader
  suggests it may be wrong. `OpusInfo.WriteOpusFromPak` seeks to
  `opus_offsets[i]`, and `WriteOpusToPak` writes a `RIFF` fourcc at exactly that
  offset — so if stream 0 sits at offset 0, a pak begins `RIFF` and probes as
  `REDFS_AUDIO_WEM`. If that wrapper is RIFF/WAVE, `redfs_audio_info_of` on a pak
  would also happily describe stream 0 as a `.wem`. Neither reading is confirmed
  against a shipped pak; both are one `redfs_cli` run away.

## How voice-over is stored

In `base\sound\soundbanks\`:

- **`sfx_container_N.opuspak`** — Opus streams end to end, no index of its own
- **`sfx_container.opusinfo`** — one index covering all the paks

So a voice line is not an archive entry. It is a byte range inside a pak, and only
the opusinfo knows the range. Reading one needs two files and an index lookup,
which is why it never fell out of the generic path.

## `.opusinfo` layout

Derived from `WolvenKit.Modkit/RED4/Tools/OpusInfo.cs`. Little-endian.

```
0x00   12   header      'S' 'N' 'D' ' ' 02 00 00 F0 00 00 00 00
0x0C   4    opus_count
0x10   4    grouping_obj_size_4x

then six parallel arrays, each opus_count long, in this order:

  u32  opus_hashes[]           stream id
  u16  pack_indices[]          which sfx_container_N.opuspak
  u32  opus_offsets[]          offset into that pak -- of the RIFF WRAPPER
  u16  riff_opus_offsets[]     size of that wrapper; payload starts after it
  u32  opus_stream_lengths[]   wrapper + Opus payload
  u32  wav_stream_lengths[]    wrapper + decoded PCM

then, until EOF, grouping objects:
  u32  hash
  u32  member_count
  u32  member_hashes[member_count]
```

WolvenKit reads those 12 header bytes and never checks them. Six parallel arrays
rather than an array of structs, so extracting stream *i* means indexing all six at
the same position.

**Both length fields are measured from `opus_offsets[i]`, not from the payload.**
`WriteOpusFromPak` reads `opus_stream_lengths[i] - riff_opus_offsets[i]` bytes
starting at `opus_offsets[i] + riff_opus_offsets[i]`, and the result decodes with
`opusdec`. `wav_stream_lengths` is the same shape by the writer's arithmetic
(`wav.Length - 44 + riff_opus_offsets[i]`), so decoded PCM is
`wav_stream_lengths[i] - riff_opus_offsets[i]` — inferred from the writer, since
WolvenKit's reader never touches that field.

## Design

Three calls, following the shape of the existing typed helpers.

```c
/* Load and index the depot's voice-over index. Opt-in, like the path dictionary:
   costs memory only if voice-over is wanted. */
REDFS_API redfs_status redfs_audio_index(const redfs_depot* depot, uint32_t* out_streams);

typedef struct redfs_opus_desc {
    uint32_t hash;          /* opus stream id                                  */
    uint32_t pack_index;    /* sfx_container_<pack_index>.opuspak              */
    uint32_t pak_offset;    /* this stream's RIFF wrapper, in that pak         */
    uint32_t riff_offset;   /* wrapper size; payload at pak_offset+riff_offset */
    uint32_t payload_size;  /* Opus bytes, wrapper already subtracted          */
    uint32_t decoded_size;  /* PCM bytes, likewise                             */
} redfs_opus_desc;

REDFS_API redfs_status redfs_opus_desc_of(const redfs_depot*, uint32_t opus_hash,
                                          redfs_opus_desc* out);

/* Raw Opus bytes for one stream. Decoding to PCM is the caller's job --
   libopus is a dependency RedFS should not acquire. */
REDFS_API redfs_status redfs_opus_read(const redfs_depot*, uint32_t opus_hash,
                                       redfs_blob* out);
```

### Notes on the design

**Both files are reached by hashing their names.** The depot is keyed by hash and
cannot be enumerated by extension, so there is no "find every `.opusinfo`" — there
is `redfs_hash("base\\sound\\soundbanks\\sfx_container.opusinfo")` and, per stream,
`sfx_container_<pack_index>.opuspak` beside it. Mod overrides then come out right
for free: a replaced pak wins by the depot's normal last-mount-wins rule.

**Indexing is opt-in.** Same reasoning as the path dictionary — a mod that wants
textures should not pay for an audio index it never queries.

**Stream ids are `u32`, not the `u64` depot hashes.** A different namespace, so
they get their own lookup rather than being forced into `redfs_depot::refs`.

**Sizes in the struct are payload-only.** The file's fields include the wrapper;
doing that subtraction once here beats every caller rediscovering it.

**Reads pull the whole pak, for now.** `read_part` is all-or-nothing and Kraken
cannot decode a prefix, so a few kilobytes from the middle of a pak still costs the
full segment decode. A ranged read (`roadmap.md`) drops the caller's allocation and
copy but not the decode, so it is an optimisation here, not a prerequisite.
WolvenKit does the same thing more expensively: `ExportAllOpus` extracts each pak
into a `MemoryStream`, and `ExportOpusUsingHash` re-extracts it per requested id.

**Grouping objects get parsed but not exposed** initially. They map a group hash to
member streams — useful for "every line for this character" — but the mapping from
anything user-facing to a group hash is not understood.

## Open questions

- How a **stream id relates to anything a modder can name.** The ids are `u32` and
  appear in `.wem`-adjacent metadata; the join from a subtitle line or a VO event
  to an opus hash has not been traced. This one decides whether the feature is
  usable at all, and it is still open.
- Whether **`ep1` ships its own `sfx_container.opusinfo`.** WolvenKit assumes a
  single index at one fixed path, which the depot's override rule already resolves
  to exactly one file — right if the expansion replaces the index wholesale, wrong
  if the two were meant to be merged. One lookup against a Phantom Liberty install
  answers it.

*Resolved:* `riff_opus_offsets` describes a wrapper present **in the pak**, not the
original file's. `WriteOpusToPak` rebuilds the bytes at `opus_offsets[i]` as
`'RIFF'` (4) + declared size (4) + the original wrapper's tail
(`riff_opus_offsets[i] - 12`) + decoded length (4), summing to exactly
`riff_opus_offsets[i]`.

## Effort

**Guess: roughly a day.** The index format is fully known and the ranged read is no
longer on the critical path, so the cost is the index, the lookup, and the naming
question above. Lower value than it looks: a caller still needs libopus to hear
anything, because RedFS would be handing over compressed frames.

`.wem` already works via `redfs_read`, and that covers SFX, which is what most mods
want.
