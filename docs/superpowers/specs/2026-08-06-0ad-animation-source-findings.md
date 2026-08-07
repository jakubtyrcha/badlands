# 0 A.D. animation source data: measured findings

Facts established by reading `/Users/jakub/repos/0ad` directly, not inferred.
These are the inputs the import-chain spec is designed against. Every number
here came from parsing real files; where something is a sample of one, it says
so.

## The corpus

- **1,220 `.dae` files**, all animation clips, under
  `binaries/data/mods/public/art/animation/`. No `.psa` in the repo — PSA is a
  runtime build artifact, so the DAEs are ground truth.
- Split: `biped/` 651, `quadraped/` 274, `mechanical/` 188, `others/` 60,
  `other/` 47.
- **81 skeleton XMLs** in `art/skeletons/`, declaring ~72 distinct
  `standard_skeleton` ids (`Biped`, `Horse`, `Camelus`, `Wolf`, …).
- Licence: **CC-BY-SA 3.0** (`art/LICENSE.txt`). Attribution and share-alike,
  commercial use permitted.

## What a clip file actually contains

Sampled from `art/animation/biped/infantry/capturing_a.dae`.

- `<up_axis>Z_UP</up_axis>`, `unit name="meter" meter="1"`.
- **94 `JOINT` nodes**, whose `sid` is a clean bone name (`hip`, `spine`,
  `shoulder_L`, `weapon_R`). The `Biped_` prefix appears only on the node `id`
  used as a channel target, so `sid` is the name to key on.
- **Animation is already baked at exactly 30 fps.** The sampler input is
  `0, 0.0333333, 0.0666666, …, 1.0` — 31 uniform samples.
- **Output is per-frame 4×4 matrices**: 496 floats = 31 frames × 16. Joint
  matrices are **parent-local**, per the COLLADA node hierarchy.

The practical consequence: this is already the shape ozz's `RawAnimation` wants.
There is no curve fitting, no resampling, and no interpolation mode to honour.

## Coordinate frame — measured

Derived from rest-pose world positions, not assumed.

**Lateral axis** (rigorous): `shoulder_L` sits at x=+0.068, `shoulder_R` at
x=−0.068. The character's right is **−X**; +X is its left.

**Up axis** (rigorous): `hip`→`head` is `(0, 0.006, 1)`. Up is **+Z**,
consistent with the declared `Z_UP`.

**Facing** (measured, not derived): shoulders alone fix the lateral axis but
leave forward ambiguous up to a sign, so it was pinned with front/back
asymmetric joints:

| behind the body (+Y) | in front (−Y) |
|---|---|
| `cape1/2/3` +0.34…+0.46 | `weapon_R` −0.19 |
| `prop_backplate` +0.38 | `weapon_bow` −0.29 |
| `prop-quiver_back` +0.33 | `shield` −0.18 |
| `prop_seax_back` +0.36 | `projectile` −0.24 |

Capes and backplates are behind; weapons and shields are in front. Forward is
unambiguously **−Y**.

**Source frame: up +Z, forward −Y, character's right −X.**

## Conversion to engine coordinates

The badlands engine is **right-handed, Y-up, +Z forward**
(`kCharacterForward = {0,1}` in XZ, `game/src/components.h`; no
`GLM_FORCE_LEFT_HANDED` anywhere — only `GLM_FORCE_DEPTH_ZERO_TO_ONE`, which
affects projection depth range and not handedness).

The change of basis is a single −90° rotation about X:

```
(x, y, z)_source  ->  (x, z, -y)_engine
```

Verification:

- up: source `+Z` → `(0, 1, 0)` = engine **+Y** ✓
- forward: source `−Y` → `(0, 0, 1)` = engine **+Z** ✓
- determinant +1, so handedness is preserved ✓
- character's right lands on `−X`, which is exactly `cross(forward, up) =
  cross(Z, Y) = −X` for a +Z-facing character in this frame ✓

Two consequences worth stating plainly:

- **`yaw_offset_radians` is 0 for imported 0 A.D. rigs.** The standard Z-up→Y-up
  rotation alone lands them facing `kCharacterForward`. No extra yaw, no
  per-rig fudge. (The Quaternius rig keeps its 180° offset — it faces −Z and
  cannot be re-exported.)
- **Apply the rotation at the root joint only.** COLLADA joint matrices are
  parent-local, so rotating the root carries the whole subtree; touching every
  joint would double-apply it.

## Sockets come free from the source data

This was the biggest surprise, and it changes the socket plan.

The clip files already carry **~50 socket nodes as animated JOINTs**, in two
naming conventions that 0 A.D.'s own `PMDConvert.cpp:82` accepts
interchangeably:

- `prop-` (hyphen): `prop-weapon_R`, `prop-weapon_L`, `prop-weapon_bow`,
  `prop-shield`, `prop-shield_arm`, `prop-helmet`, `prop-head`, `prop-back`,
  `prop-ammo`, `prop-projectile`, `prop-quiver_R`, `prop-quiver_back`,
  `prop-neck_guard`, `prop-sheath_L/R`, `prop-shoulderpad_L/R`,
  `prop-armpad_L/R`
- `prop_` (underscore): `prop_backplate`, `prop_breastplate`, `prop_bevor`,
  `prop_fauld`, `prop_cuisse_L/R`, `prop_couter_L/R`, `prop_brace_L/R`,
  `prop_rerebrace_L/R`, `prop_glove_hand_L/R`, `prop_glove_wrist_L/R`,
  `prop_glove_finger_L/R`, `prop_foot_L/R`, `prop_leg_L/R`,
  `prop_sheath_01_L/R`, `prop_sheath_02_L/R`, `prop_quiver_L/R/B`,
  `prop_seax_front`, `prop_seax_back`, `prop_chaff_L/R`

Plain attach bones exist *alongside* their prop twins at nearly identical
positions — `weapon_R` at y=−0.186 and `prop-weapon_R` at y=−0.185 — so the
`prop-*` nodes are duplicates or children of the attach bones.

**Sockets therefore need no manual authoring.** They are extracted, named and
positioned by the importer.

> **Corrected by the full corpus.** The rigidity claim below was measured on
> two clips. Run over all 1,220, five sockets turn out to move materially —
> worst is `Biped/quiver_B` at **1.54**, about a body height. Those are promoted
> back to animated joints. See the import design for the table.

### The trap this exposes

0 A.D.'s own `standard_skeleton` mapping (`art/skeletons/*.xml`) exists to
**collapse** these away: its runtime skeleton is ~40 bones, and prop points get
baked into the mesh as PMD prop points instead.

**Our import must not reuse that collapse blindly.** We want the canonical
joints *and* the prop nodes promoted to sockets. Applying 0 A.D.'s mapping
as-is would silently discard every socket in the corpus — which is precisely
the kind of mistake that only surfaces much later, as "why does nothing attach".

## Blocking finding: Blender cannot read these files

**Blender 5.2.0 LTS has no COLLADA importer.** `bpy.ops.wm.collada_import`
exists as an attribute stub but is not registered:

```
poll_error: Polling operator "bpy.ops.wm.collada_import" error, could not be found
IMPORT_FAILED: AttributeError Calling operator "bpy.ops.wm.collada_import" ...
```

OpenCollada was removed in Blender 5.0. The installed build ships
`io_scene_gltf2` and USD only.

This blocks the originally chosen "Blender headless: DAE → glTF" chain as
stated. Options are recorded in the import-chain spec; the short version is
that **the DAEs are simple enough to parse directly** — the joint hierarchy and
per-frame matrices above were extracted with ~15 lines of `xml.etree` and
`numpy`, with no Blender involved.

## Clip semantics live outside the DAE

`art/variants/*.xml` is the catalogue, and the DAE carries none of this:

```xml
<animation file="biped/infantry/spearman/idle_relax_shield_01.dae"
           name="Idle" id="idle1" frequency="10" speed="80"/>
<animation event="0.5" file="biped/infantry/swordsman/attack_melee_shield_01.dae"
           name="Attack_melee" id="attack1" speed="120"/>
```

- `name` — logical state; `id` — variant key; `frequency` — random-selection weight
- `speed` — playback rate
- `event`, `load`, `sound` — normalized `[0,1]` timeline markers, validated by
  `variant.rng` as decimals in `[0,1]`

These become our named clip markers. Loop-vs-oneshot is **not declared
anywhere** and will have to be inferred (by name, or by comparing first and last
frame) with a manual override.
