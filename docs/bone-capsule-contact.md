# Bone-capsule contact detection for paired scenes

**Status: NOT implemented. Design proposal.**

**This is not a RedFS feature.** It is a consumer-side design that touches RedFS at
exactly one point (mesh bounds, below). It lives here because the work is being
done in this tree; move it out when it grows its own repo.

**Confidence warning.** Every other note in `docs/` was derived from WolvenKit and
confirmed against a live 2.3 + Phantom Liberty install. This one is not at that
standard. Nothing here has been run against a running game. The REDengine API
names are from memory and several are load-bearing — see *Unverified assumptions*,
which is the most important section in this document.

## The problem

Two actors in a paired scene. At runtime, decide which body region is in contact
so the right sound plays. Contact moves during the animation, so the answer
changes over the life of the scene.

Precision is not required. The output is a small enum — `None`, `Lower`, `Face` —
not a depth curve, not per-stroke timing. That constraint is what makes the design
small, and it should be revisited before anything here is expanded.

Two facts shape the whole approach:

- **The penetrating geometry is an attached item**, not part of the body mesh. Its
  dimensions vary per install: different mods, different sizes. Nothing about it
  can be hardcoded.
- **No engine gives per-bone actor-vs-actor collision during animation.** Not
  Havok, not PhysX. An animating character is a single capsule to the physics
  system. Every mod that does this — Precision in Skyrim is the reference
  implementation — builds its own volumes from bone transforms and runs its own
  narrowphase. That is what "doing collision" means at this level; it is not a
  shortcut around it.

## Considered and rejected

**Bake the contact timeline offline.** Sample the animation with WolvenKit's glTF
export, compute contact times, ship a table keyed by clip. Zero runtime cost and
patch-proof. **Rejected:** it assumes the geometry is fixed. The attached item
varies per install, so a table baked against one item is wrong for every other
one. Would need a table per item per clip, i.e. a compatibility patch per body
mod — the exact maintenance burden this is meant to avoid.

**Map scene identity to a sound set.** A workspot identifies the act; map
`workspot (+ stage) → sounds` and detect nothing. Cheapest possible, cannot break.
**Rejected:** it cannot answer a question about geometry that varies, it does not
cover animations shipped by other authors, and it says nothing when a single scene
moves between regions. Worth reconsidering only if the scene set ever becomes
closed and fixed.

**PhysX queries via `SpatialQueriesSystem`.** Real engine collision, exposed to
script. **Rejected:** it resolves to the character capsule. It can report "hit a
character" and never "hit this region", which is the only question being asked.

**Read the animated component's pose natively from RED4ext.** Gives every bone
with no entity edits, no slot declarations, arbitrary volume placement.
**Rejected as the primary route, retained as fallback:** it means reverse-
engineered struct layouts that break on game patches. That is the precise
fragility RedFS was built to avoid — `README.md` sells "no address-library offsets
and no hooks" as the reason a patch does not break it. Do not undo that for a
sound effect unless the slot route fails.

## Design

### Volumes

A logical capsule — a segment plus a radius. Not a physics body: nothing is
registered with PhysX and the engine reports nothing back. The capsule is defined
once in bone space and transformed to world each tick.

A sphere is a capsule with `localP == localQ`, so one struct and one code path
covers both.

`Vec3` and `Matrix4` below are illustrative — substitute the engine's own vector
and transform types. `CName` is real.

```cpp
struct Capsule { Vec3 p, q; float radius; };

struct BoneCapsule {
    CName slot;                 // bone-bound slot declared in the .ent
    Vec3  localP, localQ;       // authored once, in slot space
    float radius;

    Capsule ToWorld(const Matrix4& m) const {
        return { m.Transform(localP), m.Transform(localQ), radius };
    }
};
```

Three volumes for the current scope:

| volume | shape | anchored to |
|---|---|---|
| shaft | capsule | attached item |
| lower | sphere | receiver hips slot + offset |
| face | sphere | receiver head slot + offset |

Two tests per tick. The count can grow an order of magnitude before it is
measurable.

### Transform source

"Attached to a bone" means, in REDengine, **a slot bound to that bone**, declared
on the entity and added via ArchiveXL. The slot carries orientation as well as
position, so one slot yields a whole capsule — both endpoints are constants in
slot space. Two slots per capsule are not needed.

Slots bind to the **shared rig**, not to a mesh, so one additive `.ent` edit
covers every body replacer rather than needing one per mod.

If the slot matrix carries non-unit scale, multiply `radius` by it. A resized body
otherwise mis-sizes every volume silently.

### Narrowphase

Capsule-capsule reduces to segment-segment closest distance. Ericson,
*Real-Time Collision Detection* §5.1.9:

```cpp
static float SegSegDistSq(const Vec3& p1, const Vec3& q1,
                          const Vec3& p2, const Vec3& q2)
{
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = d1.Dot(d1), e = d2.Dot(d2), f = d2.Dot(r);
    constexpr float kEps = 1e-6f;
    float s, t;

    if (a <= kEps && e <= kEps) return r.Dot(r);
    if (a <= kEps) { s = 0.f; t = std::clamp(f / e, 0.f, 1.f); }
    else {
        const float c = d1.Dot(r);
        if (e <= kEps) { t = 0.f; s = std::clamp(-c / a, 0.f, 1.f); }
        else {
            const float b = d1.Dot(d2);
            const float denom = a * e - b * b;
            s = denom > kEps ? std::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
            t = (b * s + f) / e;
            if (t < 0.f)      { t = 0.f; s = std::clamp(-c / a, 0.f, 1.f); }
            else if (t > 1.f) { t = 1.f; s = std::clamp((b - c) / a, 0.f, 1.f); }
        }
    }
    return ((p1 + d1 * s) - (p2 + d2 * t)).LengthSquared();
}

inline bool Overlap(const Capsule& a, const Capsule& b, float& penetration)
{
    const float rr  = a.radius + b.radius;
    const float dsq = SegSegDistSq(a.p, a.q, b.p, b.q);
    if (dsq >= rr * rr) return false;
    penetration = rr - std::sqrt(dsq);
    return true;
}
```

`penetration` is a real depth. Nothing in the current scope uses it beyond
picking a winner, but it is free and it is the signal any later per-stroke work
would need — differentiate it and take local maxima.

### Resolution and hysteresis

```cpp
int   bestZone = -1;
float bestPen  = 0.f;
for (int i = 0; i < zoneCount; ++i) {
    float pen;
    if (Overlap(shaft, zones[i], pen) && pen > bestPen) { bestPen = pen; bestZone = i; }
}
```

Deepest overlap wins. Two additions are not optional:

- **Separate enter and exit radii** (exit ≈ 1.3× enter). Without it the zone
  flickers every frame on the boundary.
- **Confirm over 2–3 ticks** before committing a zone change, so one frame of
  blend artifact cannot swap the sound loop.

Tick rate: per frame is affordable but unnecessary. 10 Hz is ample for an enum
this coarse and leaves headroom for more actors.

### Sound

Zone selects the sound set. **Whether an item is attached is a separate question**
— one slot query at scene start, cached — and it selects the variant. It is not a
collision test and must not be conflated with one.

Vanilla Wwise event names play through the audio system directly. Custom audio
needs bank injection, which is what the community audio plugin exists for.

## The RedFS touch point

The shaft capsule's `length` and `radius` come from the attached item's mesh
bounds, read at scene start. This is the whole reason the design tolerates a
varying item: no hardcoded sizes, no per-mod compatibility table, no shipped
data — the volume sizes itself to whatever the user installed.

**Use `redfs_mesh_desc_of`, not `redfs_mesh_open`.**

```c
redfs_mesh_desc d;
if (redfs_mesh_desc_of(depot, item_mesh_hash, &d) == REDFS_OK) {
    /* d.bbox_min / d.bbox_max -- whole-mesh box */
}
```

`redfs_mesh_desc_of` reads the CR2W only and never touches the geometry, so it is
cheap and needs no cache. `redfs_mesh_open` exists to compute **per-chunk** boxes,
which the format does not store — that costs a geometry decompress (median
0.72 ms, p90 2.27 ms uncached per `README.md`) and is not needed here. A
whole-mesh extent is all a single capsule can consume.

Three properties of that box matter:

- **It is in mesh-local game space, Z up**, matching entity and component
  transforms. Not the Y-up convention glTF exporters use. Which local axis is the
  shaft's length is a property of how the item was authored — read it from the box
  extents rather than assuming one.
- **It is returned as found.** The header is explicit that RedFS does not verify
  `CMesh.boundingBox` beyond replacing non-finite values with 0. A degenerate box
  yields a zero-length capsule that silently never collides, which is
  indistinguishable at runtime from "the animation never made contact". Range-check
  the extents and fall back to a default size rather than trusting the file.
- **It covers the whole mesh.** If an item bundles geometry beyond the shaft and
  the box comes out too coarse, that is the point to escalate to `redfs_mesh_open`
  and per-chunk boxes — and only then do the chunk caveats apply: filter on `lod`
  (chunks repeat per LOD) and test `bounds_valid` before trusting a box, because
  an invalid one is all-zero and reads as a real chunk at the origin.

Nothing else in this design needs RedFS.

## Debugging: drawing the capsules

The capsule constants are authored by eye, so they cannot be tuned blind.

A CET ImGui overlay renders every frame — including with the overlay closed — via
`registerForEvent('onDraw', ...)` and a full-screen window flagged
`NoBackground | NoInputs | NoDecoration | NoNav`.

Do not project a true capsule silhouette. Draw a circle at each endpoint and a
line between them; it reads correctly and costs five lines.

For the screen-space radius, skip FOV math entirely: project `center` and
`center + camRight * worldRadius` and take the pixel distance between them. Exact,
and it needs no projection constants.

Three things that will cost time if not anticipated:

1. **Clip points behind the camera** (`w <= 0` after the view-projection multiply).
   Otherwise mirrored geometry smears across the screen and reads as broken
   transforms when the transforms are fine.
2. **The view-projection matrix must be built by hand** from camera transform and
   FOV. This is the only real work in the debug path. Validate it against a marker
   at a known static world position before trusting it on moving bones.
3. **The overlay has no depth testing** and draws over everything. For tuning
   volumes inside a body that is the desired behaviour.

Tune in CET Lua even though the plugin is C++ — hot reload turns a rebuild-and-
restart cycle into a save. Then bake the constants into the RED4ext side.

One trap: CET and the plugin must read slot transforms **the same way**, or the
tuned numbers will not match what ships. Confirm one capsule agrees in both before
tuning the rest.

## Unverified assumptions

Ordered by how much collapses if false.

1. **Slot transforms update per frame under animation.** Everything rests on this.
   If slots only refresh on attach, the entire slot route is dead and the fallback
   is the native pose read. *Check: attach a marker to a slot, play a scene, log
   its world position for a few seconds.* Ten minutes. **Do this before writing
   any other code in this document.**
2. **The attached item is a separate entity with a readable world transform.** If
   the geometry is skinned into the body appearance instead, there is no
   attachment to read and the shaft capsule needs its own declared slot like the
   zones do. *Check: dump the entity and look for it as an attachment vs. an
   appearance chunk.*
3. **ArchiveXL can add slots bound to arbitrary rig bones**, additively, without
   replacing the base entity.
4. **The attachment exposes orientation, not just position.** The capsule needs a
   direction. If only position is available, derive direction from two slots.
5. **CET exposes `ImGui.GetWindowDrawList`.** Debug-path only; if absent, fall
   back to spawning marker meshes. Does not affect the shipping design.
6. **Rig bone names.** Not yet read. Dump the `.rig` and confirm rather than
   assuming anything published secondhand.

Items 1 and 2 are the go/no-go. Items 3–6 change the amount of work, not the
approach.

## Cost

| | |
|---|---|
| Verify assumptions 1–2 | under an hour, and it gates everything |
| Slot declarations (ArchiveXL) | small, once the bone names are confirmed |
| Narrowphase + state machine | a few hundred lines, no dependencies, testable offline |
| Debug draw path | the view-projection matrix is most of it |
| Tuning the constants | open-ended; the debug draw is what bounds it |

Runtime: ~40 flops per pair, three volumes, 10 Hz. Plus one
`redfs_mesh_desc_of` per scene, which reads no geometry. Not measurable.

## Honest read

The engineering is small and the risk is concentrated almost entirely in
assumption 1. If slot transforms track animation, this is a few days of work with
a well-understood shape and no ongoing per-mod maintenance. If they do not, the
only remaining route trades patch stability for capability, and that trade
deserves its own decision rather than being made by momentum.

The narrowphase, the state machine, and the RedFS bounds lookup are all
independent of that answer and can be written and tested before it lands. The
slot plumbing cannot. Order the work accordingly.
