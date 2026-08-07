# Reading imported rigs: the packer, attachments and markers

Status: **implemented.**

The badlands half of the animation arc. The 0 A.D. importer (in that repo, on a
branch) produces an intermediate; this is what turns it into something the engine
loads, and what the engine grew in order to hold it.

Prior art this rests on:
[source findings](2026-08-06-0ad-animation-source-findings.md),
[import chain](2026-08-06-0ad-animation-import-design.md),
[character viewer](2026-08-06-character-viewer-sockets-design.md).

## Decisions

### The runtime format is the existing shape, extended

A JSON manifest beside `skeleton.ozz` and per-clip `.ozz` files. No new binary
container and no new loader: the shape already works, and a single-file archive
would buy a smaller file count against a versioning burden we do not have.

`assets/characters/quaternius/clips.json` keeps loading with no edit. That is the
check that the change stayed additive.

### Attachments are ONE namespace, and joints win

Joints and sockets resolve through a single `FindAttachment(name)`. Nothing
public can ask which it found.

The payoff is that collapsing a prop node into a static socket becomes an
**import-time size decision rather than a contract**. If a socket later has to be
an animated joint again, no consumer changes. This is UE's model, where
`AttachToComponent` and `GetSocketTransform` take a name that resolves against
bones and sockets alike.

Every attachment is `(joint index, mat4 offset)`, so a joint is simply one whose
offset is identity — there is no branch anywhere in the resolution. Ids
`[0, num_joints)` **are** the joint indices, which is what makes a joint
attachment's transform provably its model matrix.

Names are unique across the union and **joints win**. This is not theoretical:
`weapon_R` exists as a joint and as `prop-weapon_R`, measured 0.001 apart,
because 0 A.D. needs both for a mesh/skeleton split we do not have. The joint is
the animated one.

**The collapse happens at pack time**, so a shipped manifest is already
collision-free and the runtime never arbitrates. It warns if it ever sees one
anyway, which would mean a hand-edited manifest or an older tool.

### Markers replace the single pivot

Free-form named points in `[0,1]`. The engine knows only that a clip declares
named instants; whether `event` is when damage lands or when a sound plays is
decided a layer up.

`pivot` becomes the marker of that name and keeps its 1.0 fallback, so
`clip_pivot()` and every caller of it are untouched.

### The shipped 0 A.D. rigs are 31 whole families

`assets/characters/0ad/` packs **940 clips across 31 creature families** (~84 MB
of git LFS; `.ozz` was already routed there). One directory per family, named for
the exporter's own slug, from a single checked-in recipe.

Two decisions inside that:

- **Whole families, not curated clips.** The viewer's job is to show what a rig
  can do, and a curated handful cannot answer "what is in this corpus". The cost
  is stated rather than hidden: these carry 0 A.D.'s clip names, so no rig has a
  clip called `idle`. Nothing points the game at them — it still uses
  `assets/characters/quaternius` — and doing so would need a name mapping.
- **Creatures only.** The excluded 37 families are gates, ships, sails, chariots,
  siege artillery, banners and markers. That exclusion saves only ~9 MB of the
  93, so the list is about relevance, not size.

**The classification is by SOURCE PATH, not family name**, and that mattered: 28
of 68 families are named after a fallback root bone, so `main_pelvis-node` is a
giraffe, `Head_Rotation_IK` a camel, `Main` a shark, `Horse-node` a lion,
`Deer01_Pelvis` a rabbit, `Body` a crow, `fish_shoal_armature` birds. Going the
other way, `Aspron_Armature` — the second-largest non-Biped family at 36 clips —
is a shield prop and no animal at all. Naming alone would have got seven wrong in
one direction and one in the other.

`--all` packs every family the manifest carries, for ad-hoc use; it is how the
excluded families get looked at without shipping them.

### The packer's clip selection is a checked-in recipe

1,220 clips named the way 0 A.D. names them; a game asks for `"idle"`. The recipe
is where those vocabularies meet, and it is data — so the shipped rig is
reproducible from the repo and reviewable in a diff rather than living in
whoever-ran-the-tool's shell history.

### The intermediate is never checked in

`rigpack` takes `--intermediate <dir>`; the recipe holds no machine-specific
path. Regenerating a rig needs the 0ad checkout, which the tool's README states.

### The payload is column-major, and the packer verifies it

COLLADA's `<matrix>` is row-major; glm and ozz are column-major. The exporter
already transposes — "the writer transposes, the reader must not" — so this is a
contract, not a setting.

It is still **checked**, because getting it wrong produces a rig that loads,
animates, and is transposed everywhere. For an affine transform exactly one
reading puts `(0,0,0,1)` where it belongs; most matrices are ambiguous (a zero
translation reads the same either way), so the check scans until one is decisive.
A disagreement is a line in the report, not a silent shear.

### Frame times, never a frame rate

545 clips are 24 fps, 449 are 30, and **135 are not evenly sampled at all** —
`wolf_idle_01` holds a pose for 5.1 s between frames 0.067 s apart. Every clip
carries an explicit `frame_times` array, and that is what the packer keys on;
`frame_rate` is only a mean and is deliberately not carried into our structs.

`ozz::RawAnimation` takes explicit key times, so the source suits it directly and
nothing is resampled.

### The rest pose comes from frame 0

The intermediate has no bind pose — every clip is written at the family's full
joint width, so the exporter never needs one. Frame 0 of the first readable
recipe clip stands in, which is what keeps a freshly constructed `Pose` looking
like the rig rather than collapsing every joint onto the origin. The report says
which clip it came from.

## Shape

### `tools/rigpack`

`badlands_rigpack_lib` + a thin `main`. Links ozz's offline builders, glm and
nlohmann; **not** SDL3, Dawn or `badlands_engine`.

- `intermediate.{hpp,cpp}` — the importer's `rig.json` + `.bin`. **The one file a
  schema change touches.**
- `recipe.{hpp,cpp}` — `pack.json`.
- `pack.{hpp,cpp}` — the conversion, returning a `PackReport`. Logs nothing; the
  caller decides how to present it and the tests assert on it.
- `main.cpp` — flags and report printing.

The reindexing trap: `SkeletonBuilder` reorders joints, so an index carried
across the build points at the wrong joint. Everything downstream resolves by
name, and the manifest stores socket parents as joint names for the same reason.

### Manifest v2

```json
{
  "family": "Biped",
  "skeleton": "skeleton.ozz",
  "yaw_offset_degrees": 0,
  "sockets": [
    { "name": "backplate", "parent": "spine", "offset": [ 16 floats, column-major ] }
  ],
  "clips": {
    "attack": { "file": "clips/attack.ozz",
                "source": "biped/infantry/swordsman/attack_melee_shield_01.dae",
                "markers": { "pivot": 0.45, "event": 0.5 } }
  }
}
```

`family`, `sockets` and `markers` are all optional. `source` is retained per clip
because the art is CC-BY-SA 3.0 and that is what carries attribution forward.

### `AnimationSet`

```cpp
const std::string& family() const;

int attachment_count() const;
const std::string& attachment_name(int id) const;
int FindAttachment(const std::string& name) const;     // -1 when absent
glm::mat4 AttachmentTransform(int id, const Pose& pose) const;

std::optional<float> clip_marker(int clip, const std::string& name) const;
int clip_marker_count(int clip) const;
const std::string& clip_marker_name(int clip, int marker) const;
float clip_marker_value(int clip, int marker) const;
float clip_pivot(int clip) const;   // = clip_marker(clip, "pivot").value_or(1)
```

`AttachmentTransform` is `pose.models()[joint] * offset` — parent then offset,
read from model space, never from locals.

### `attachment_lines.{hpp,cpp}`

`EmitAttachmentAxes` appends `3 * attachment_count()` lines and clears nothing,
matching `EmitSkeletonLines`' append contract. It lives in the engine rather than
the viewer because `SkeletonDebugOverlay` will want it, and duplicating the
composition is how the two drift.

Arms are `0.05` rig units — not the `0.1` the viewer spec guessed, because
drawing at every attachment means ~74 triads on a 0 A.D. biped and 53 on the
Quaternius rig. A constant, deliberately; there is no control for it.

### Viewer

`--rig <path>` wires the existing `SetManifestPath` and implies `--character`.
`UpdateSkeleton()` emits attachment axes into the same buffer, always on — which
is also what keeps `--screenshot` deterministic. The panel reports family,
attachment names and the current clip's markers, read-only.

## Testing

All in `badlands_animation_tests`. Fixtures are built in-test; nothing asserts
against `assets/characters/`, and the rigpack suite runs on a fresh clone with no
importer output present.

Two of these earn their place specifically:

- **Parent-then-offset.** The offset is a pure rotation and the parent a pure
  translation, which is what makes the two orderings distinguishable. With an
  identity rotation both agree and the reversal — "the sword is attached but
  points the wrong way" — sails through. Verified by mutation: reversing the
  composition turns the socket origin from `5.0` into `-2.0` and the test fails.
- **Joint reindexing.** The intermediate lists joints breadth-first and
  `SkeletonBuilder` emits them depth-first, so the two orderings genuinely
  disagree. A packer indexing tracks by the intermediate's own index animates the
  arm with the head's data. The test `REQUIRE`s the disagreement first, so a
  future ozz that changed its ordering fails loudly rather than quietly ceasing
  to test anything.

- **Uneven frame times.** Three frames at 0.0, 0.1 and 4.0 s, holding a pose
  across the gap. Sampling the midpoint must land inside the hold, which a
  constant-rate reading would not — it would put the midpoint between frames 1
  and 2 and redistribute the motion across the clip.

Plus: markers and the legacy pivot, attachment ids as joint indices, sockets with
unknown parents, the collision drop at both pack and load time, the column-major
payload convention, a transposed payload being reported, clip lookup by
case-insensitive logical name, a recipe naming an absent clip, root-motion
measurement, and the emitter's line count and append contract.

## Not done

- **No skinned meshes.** The skeleton is still the presentation, and attachments
  are axis triads rather than attached geometry.
- **No `game/` or `src/game/` change.** What a clip or a marker *means* to
  gameplay stays undecided, so nothing here commits to a taxonomy.
- **No shipped 0 A.D. rig yet.** The importer is still in flight; `pack.json`
  gets authored when there is an intermediate to point it at.

## Verified against the real exporter

This was first written against an assumed schema and then corrected against the
real one
([handoff](2026-08-06-anim-intermediate-handoff.md), and
`source/tools/animexport/README.md` on the 0ad branch `export/badlands-anim`,
which is canonical). Five assumptions were wrong: the payload is column-major
rather than row-major, the `.bin` path field is `data` not `file`, markers are an
array of `{name, ratio}` rather than an object, there is no `bind` array, and
frame timing is per-frame rather than a rate. All five were confined to
`intermediate.{hpp,cpp}`, which is what that file is for.

Packing the real `Biped` family end to end gives **55 joints + 28 sockets = 83
attachments**, and the namespace matches the handoff exactly:

| name | resolves to |
|---|---|
| `weapon_R`, `shield`, `helmet` | joint |
| `quiver_B`, `backplate` | joint — promoted from a socket because they drift |
| `armpad_L` | socket |

Zero collisions reached rigpack, because the exporter already collapsed the 21
duplicate prop nodes on `Biped`. Our drop is the second line of defence, not the
first. `yaw_offset_degrees` is 0, as the source findings predicted.

Two things are deliberately read and then dropped rather than carried into the
manifest: `loop`/`loop_source` (inferred, not authored — a hint, and the runtime
has no loop concept) and `speed`. Neither has a consumer yet, and inventing one
would be designing against a guess again.

### A logical name is a variant class, not a clip

Checking the implementation back against the handoff caught this, and it is the
one genuinely dangerous thing in the mapping. A recipe may name a clip by any of
its `logical_names` — but on `Biped`, `Idle` matches **232** clips, `Walk` 117
and `attack_melee` 89, because 0 A.D. selects among variants at random by weight.

Taking the first is fine for a first look. Taking it silently is not: the recipe
then reads as a deliberate choice nobody made. Asking for `"Idle"` on the real
corpus yields `biped__chariot__idle` — a chariot's animation standing in for a
generic idle. The packer names the winner and the count it passed over.

### Bad key times are repaired, not fatal

`ozz::RawAnimation` requires key times inside `[0, duration]` and strictly
increasing. The corpus breaks both, in three clips:

- `biped__new__boat_fisherman_idle` and `quadraped__rabbit_idle_01` each author
  three pairs of frames at the same instant.
- `quadraped__rabbit_walk` starts at **−0.0417 s**.

Times are clamped into range, then the last frame of each equal-time run wins —
clamping can itself create a duplicate, so the order matters. Both repairs are
reported. Losing whole clips to a degenerate COLLADA sampler would be the worse
trade, and with these two rules **all 1220 clips across all 68 families convert
with zero losses**.

### Socket drift is checked, not assumed

A socket is baked to one offset, so a surviving one must be rigid. The exporter
promotes anything that measurably moves into a real joint — which is why
`quiver_B` and `backplate` arrive as joints. The packer reports the worst
surviving drift so that promotion staying correct is observable rather than
folklore. On the real `Biped` it is 0.0 across all 28.
