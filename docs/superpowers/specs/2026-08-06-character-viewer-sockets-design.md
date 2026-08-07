# Character viewer: arbitrary rigs, sockets and markers

Status: **implemented, with one decision superseded.** See
[rig import reading](2026-08-06-rig-import-reading-design.md), which is what
built this.

> **Superseded: sockets are not their own concept.** This document treats a
> socket as a thing a caller can ask about. It is not. Joints and sockets share
> ONE attachment namespace, UE-style: `FindAttachment(name)` resolves against
> both and nothing public can ask which it found, so collapsing a prop node into
> a socket is an import-time size decision rather than a contract.
>
> Concretely, everything below naming `socket_lines.{hpp,cpp}`, `EmitSocketAxes`
> or `socket_count()` shipped as `attachment_lines.{hpp,cpp}`,
> `EmitAttachmentAxes` and `attachment_count()`, drawing a triad at **every**
> attachment rather than at sockets only. The axis length is `0.05`, not the
> `0.1` guessed under *Open questions*, because ~74 triads on a 0 A.D. biped need
> to stay legible.
>
> The rest of the document — why the viewer is the right home, model-space
> resolution, markers, no new controls — held up and was built as written.

This document exists so the viewer half of the animation work is not lost while
the import chain goes first. It records what the viewer must become, and why,
independent of when it gets built.

## Why

`badlands_viewer --character` is the only place a rig can be inspected on its
own, with no Sim. Today it loads exactly one hardcoded asset shape, shows a
skeleton as debug lines, and knows nothing about sockets or clip markers.

The animation work adds two things the viewer is the natural home for:

- **Arbitrary rigs.** More than one skeleton, seeded from 0 A.D.'s ~72.
- **Sockets.** Named attach points, because characters will hold items, and a
  socket that is silently wrong is otherwise only discovered through a
  floating sword in the game.

The viewer is where both get *verified by eye* before anything depends on them.

## Scope

**In scope — `src/engine/animation/` and `src/executables/viewer/` only.**

**Explicitly out of scope: `game/` and `src/game/`.** No sim change, no
`SkeletonDebugOverlay` change, no touching `LogicalClip`. The semantic layer —
what a clip *means* to gameplay — is deliberately left undecided, so nothing
here commits to a taxonomy. The viewer reads whatever names the asset declares
and displays them; it never interprets them.

**Also out of scope: skinned meshes.** The skeleton remains the presentation.
Sockets are demonstrated as axis triads, not as attached geometry.

## Decisions already taken

These were settled during design and are recorded here so the eventual plan
does not relitigate them:

- **Socket = named joint + offset transform**, resolved to a joint index at
  bake time. This is UE's `FSocket` model. A socket sitting on a real attach
  bone simply carries an identity offset.
  - Since confirmed empirically: 51 of 52 prop nodes in the 0 A.D. corpus are
    rigid relative to their parent, and the three that drift do so by under 2 %
    of body height. See
    [source findings](2026-08-06-0ad-animation-source-findings.md).
- **A socket transform is read from model space**, after `LocalToModel`:
  `pose.models()[parent_joint] * offset`. Never derived from local transforms.
- **Markers are free-form named points** in `[0,1]` on a clip. The existing
  `pivot` becomes the marker named `"pivot"`, keeping its 1.0 fallback.
- **Socket axes only**, no placeholder item mesh. It proves the transform is
  correct and follows the animation with no new render path.

## Design

### Engine: one new emitter

`src/engine/animation/socket_lines.{hpp,cpp}`, modelled exactly on
`skeleton_lines.{hpp,cpp}` — same append-don't-own contract, same
`(skeleton, pose, world)` shape, same game-agnostic rule.

```cpp
// Appends an RGB axis triad at each of `set`'s sockets, posed by `pose` and
// transformed by `world`. `pose.models()` must already be filled.
// X is red, Y green, Z blue, each `length` long in rig units.
// Output grows by exactly 3 * socket_count() lines.
void EmitSocketAxes(const AnimationSet& set, const Pose& pose,
                    const glm::mat4& world, DebugLineBuffer& out,
                    float length = 0.1f, float thickness = 2.0f);
```

It lives in the engine rather than the viewer because `SkeletonDebugOverlay`
will want it later, and duplicating the transform composition is exactly how
the two drift apart.

### Viewer: what changes

`CharacterViewerView` gains no new subsystem. The changes are:

- **`UpdateSkeleton()` also emits socket axes** into the same
  `skeleton_lines_` buffer, under the same `world` yaw correction. A rig with
  no sockets emits none, which is how a `clips.json` asset degrades.
- **`--rig <path>` wires the existing `SetManifestPath`.** The setter is
  already there and already documented; it just has no CLI flag. Naming a rig
  implies `--character`, the same way `--clip` already does.
- **The ImGui panel reports what the rig declares**, read-only: the family
  tag, the socket names, and the current clip's markers with their ratios.

**No new controls.** Per the project's working agreement, socket axes are
always drawn when the rig has sockets — there is no toggle, no length slider,
no colour picker. Always-on is also what keeps `--screenshot` deterministic,
so no headless flag is needed either.

### What deliberately does not change

- `Pose`, `ClipSampler`, `BlendPoses`, `LocalToModel`, `EmitSkeletonLines` —
  untouched. The socket work is purely additive.
- `AnimationSet::Load` keeps accepting `clips.json`. The game keeps pointing at
  it and keeps working with no edit, which is the whole reason the format
  dispatches on extension.
- The `--clip` and `--anim-time` flags keep their current meaning.

## Testing

- **Socket transform, identity offset.** A socket with an identity offset on
  joint *J* equals `pose.models()[J]` exactly, for a posed (not rest) frame.
- **Socket transform, known offset.** A non-identity offset composes in the
  documented order — parent-then-offset, not offset-then-parent. This test is
  the one that catches the reversal, which otherwise reads as "the sword is
  attached but points the wrong way".
- **Socket axes follow animation.** The same socket sampled at two different
  ratios of a clip that moves its parent joint yields two different world
  transforms.
- **Line count.** `EmitSocketAxes` appends exactly `3 * socket_count()` lines
  and clears nothing, matching `EmitSkeletonLines`' append contract.
- **Graceful degradation.** Loading a socket-less `clips.json` through the
  viewer path emits zero socket lines and does not fail the load.

Existing `badlands_animation_tests` must keep passing unchanged — that is the
check that this stayed additive.

## Sequencing

This spec is third in the arc:

1. **0 A.D. import chain** (in the `0ad` repo, on a branch). Blender headless
   converts COLLADA to canonical-named glTF; sidecars carry logical clip names
   and markers. Import goes first *deliberately*, so the format is designed
   against data that actually exists rather than assumed data.
2. **`.rig` format + `tools/rigpack` + the `AnimationSet` surface.** Shaped by
   what step 1 yields.
3. **This document.**

The viewer cannot be built before step 2, because it has nothing to load. It is
written down now because its requirements are already known and are what step 2
must serve.

## Open questions for the plan

- **Axis length.** `0.1` rig units is a guess. The right default depends on the
  rig scale the import chain settles on, and should be fixed once, as a
  constant, when real rigs exist.
- **Socket name display.** Text labels at each triad would be clearer than a
  side list, but the debug-line pass draws lines only. Deferred until there is
  a reason to add world-space text.
