# 0 A.D. animation import chain

Status: **design, awaiting approval.**

Extracts skeletons, animation clips and sockets from 0 A.D.'s art data into an
intermediate that a later badlands-side packer turns into a runtime rig asset.

The measured facts this design rests on are in
[`2026-08-06-0ad-animation-source-findings.md`](2026-08-06-0ad-animation-source-findings.md).
Where a number appears below, it came from parsing real files.

## Why import goes first

The runtime format was deliberately **not** committed before this work. Designing
a container against assumed data is how you end up with fields nothing fills and
missing fields everything needs.

The investigation already proved the point twice:

- **Sockets need no authoring.** The clips carry ~50 `prop-*` / `prop_*` nodes
  as animated joints. A format designed a week ago would have specified a
  hand-authored socket table nobody needs.
- **0 A.D.'s own skeleton mapping is a trap.** Its `standard_skeleton` collapse
  exists to *discard* those prop nodes. Reusing it would have silently deleted
  every socket in the corpus.

So this spec ends at a well-defined intermediate. The `.rig` format is designed
afterwards, from the audit report.

## Scope

**Lives in the `0ad` repo, on a branch.** It reads that repo's art data and is
useless outside it; keeping it there also keeps the CC-BY-SA provenance next to
the source.

**In scope:** an audit pass, a DAE reader, coordinate conversion, socket
extraction, rig grouping, and an intermediate on disk.

**Out of scope:** the `.rig` binary format, the C++ packer, any badlands change,
any mesh or texture data, skinning, and root-motion support.

## Decisions, with the evidence

### Read the DAEs directly in Python

Blender 5.2 cannot import COLLADA — OpenCollada was removed in 5.0, and
`bpy.ops.wm.collada_import` is an unregistered stub (verified against a real
file, not by feature-check).

Direct parsing is also simply the shorter path. The data is **already baked at
30 fps as parent-local 4×4 matrices** — exactly `ozz::RawAnimation`'s shape — so
a Blender round-trip would decompose and resample data that needs neither, and
glTF has no notion of a prop node to carry sockets through.

Blender stays useful for *viewing* exported results and for future non-0 A.D.
assets. It is not the reader for these files.

### Convert to engine coordinates at import

Source frame, measured from rest-pose joint positions: **up +Z, forward −Y,
character's right −X**. Engine frame: right-handed, **up +Y, forward +Z**.

```
(x, y, z)_source  ->  (x, z, -y)_engine        # -90 degrees about X
```

- **Applied at the root joint only.** COLLADA joint matrices are parent-local,
  so rotating the root carries the subtree; touching every joint double-applies.
- **`yaw_offset` is 0 for these rigs.** That single rotation lands them facing
  `kCharacterForward`. No per-rig fudge.

### Sockets are static parent-relative offsets — mostly

A two-clip sample suggested 51 of 52 prop nodes were rigid, worst drift 0.036.
**The full corpus disagreed**, which is exactly what the audit pass exists to
catch. Five sockets move materially:

| family | socket | worst drift |
| --- | --- | --- |
| Biped | `quiver_B` | 1.5367 |
| Lithobolos_Small_Armature | `operator_03` | 1.4412 |
| Biped | `seax_back` | 0.5577 |
| Tb | `projectile` | 0.4081 |
| Biped | `backplate` | 0.2196 |

`quiver_B` at 1.54 is about a body height — freezing it would have been very
visible.

So: a socket is captured as its **first-frame local matrix**, *unless* it
measurably drifts anywhere in its family, in which case it is **promoted back
to an animated joint** for that whole family. Promotion is per-family because a
joint list must be identical across every clip of one skeleton.

A promoted node is **renamed to its stripped name** (`prop_quiver_B` →
`quiver_B`), so the unified namespace still resolves it. Skipping the rename
would make the attachment silently vanish.

The unified-namespace decision is what makes this cheap: promotion changes
nothing for the consumer.

### A family's canonical joint set is the union

A 40-clip biped sample yielded **two signatures differing by exactly one joint**
(`prop_quiver_B`): 93 shared, 94 in union. The sample spanned infantry, camelry
and elephantry, so even riders share the biped skeleton.

Canonical set is therefore the **union** within a family; a clip missing a joint
is filled from the bind pose. Intersection would discard real sockets to satisfy
the sparsest clip.

## Pipeline

Three stages, each independently runnable, each writing artifacts the next reads.

### Stage 0 — Audit

A pure-read pass over the corpus that writes a report and nothing else. This is
the stage that earns "import first", so it runs and gets read *before* stages 1
and 2 are trusted.

It answers:

- Joint signatures per family, and how many clips share each.
- Which `art/skeletons/*.xml` identifier each clip's root bone resolves to.
- Socket vocabulary per family, and per-socket drift across each clip.
- Marker names and value distributions from `art/variants/**/*.xml`.
- Clips no variant references, and clips that fail to parse.
- **Whether the root or hip translates over a clip** — that is baked root
  motion, and the sim owns movement, so it must be found and stripped rather
  than discovered later as characters sliding at double speed.

Output is markdown plus CSV at a stated path. No viewer, no gallery.

### Stage 1 — Extract

Per clip: parse the `JOINT` hierarchy into an ordered `(sid, parent)` list, read
the matrix channels into per-frame 4×4s, and fill any joint without a channel
from its bind matrix.

Then classify each joint by name: a `prop-` or `prop_` prefix makes it a
**socket** (name = the stripped remainder), everything else a **skeleton joint**.
Both prefixes are accepted, matching `PMDConvert.cpp:82`.

Finally apply the root rotation, and record each socket's first-frame local
matrix along with its observed drift.

`library_geometries`, materials and skin data are skipped entirely.

### Stage 2 — Group into rigs

Group clips by joint signature into families, name each family from the
`art/skeletons/*.xml` identifier its root bone resolves to, and attach logical
clip names and markers from `art/variants/**/*.xml` (`event`, `load` and `sound`
become named markers).

## The intermediate contract

This is the deliverable, and the boundary the badlands packer will later consume.

```
<out>/<family>/rig.json          # structure, human-readable
<out>/<family>/clips/<name>.bin  # float32 matrices, frame-major
```

`rig.json` carries the family name, the canonical joint list as
`(name, parent_index)`, a socket table of
`(name, parent_joint_index, 4x4 offset, drift)`, and per clip: logical names,
source path, frame count, frame rate, duration, markers, authored speed,
`loop` + `loop_source`, `root_motion` + `root_motion_track`, and its `.bin`
path.

**Family directory names are slugged, uniquely and case-insensitively.**
0 A.D. ships two distinct families named `Main` and `main`; on macOS and
Windows they share a directory and one silently overwrites the other. That cost
11 clips with `skipped: 0` and no error anywhere before it was caught by
cross-checking counts.

`.bin` is `float32[frames][joints][16]`, matching the source layout.
**Decomposition to TRS is left to the C++ packer**, where ozz's helpers live —
polar decomposition with shear and negative scale is fiddly enough that it
should happen once, in the language that has the tested implementation.

Size: order **100 MB for the full biped set** (651 clips × ~31 frames × 94
joints × 16 floats). Fine for an offline intermediate, and only the shipped
subset ever needs converting.

## Non-goals, stated so they are not assumed

- **No `.rig` format.** That is the next spec, designed from the audit output.
- **No root motion in the payload.** Only **4 clips** in the whole corpus carry
  any, and all four are animal deaths (`rhino_death` displaces 3.25,
  `donkey_death_01` 2.18, two peacock deaths). It is stripped by default but
  **not destroyed**: `root_motion` records the net displacement and
  `root_motion_track` the per-frame offsets removed, so a consumer can restore
  it. The sim owns locomotion, but a dying animal toppling forward is authored
  motion, and discarding it outright would be the wrong default.
- **No mesh data of any kind.**
- **No hand-edit stage.** If a rig ever genuinely needs manual fixing, a Blender
  stage can be added against the glTF the packer could emit — noted as an
  extension point, not built.

## Testing

Python, so `pytest` beside the tool in the 0ad branch.

- **Coordinate conversion.** A synthetic joint at source `(0,−1,0)` lands at
  engine `(0,0,1)`; determinant of the basis change is +1.
- **Root-only application.** A two-joint chain converts such that the child's
  world position rotates once, not twice — the bug this guards is invisible on a
  single-joint fixture.
- **Socket classification.** Both `prop-x` and `prop_x` yield socket `x`; a
  joint merely *containing* `prop` mid-name does not.
- **Union joint set.** Two signatures differing by one joint produce a canonical
  set containing it, with the sparse clip bind-pose filled.
- **Golden clip.** `capturing_a.dae` extracts to a known joint count, frame
  count and socket count — the regression net for the reader.
- **Drift bound.** Socket drift on the known back-mounted props stays under a
  stated threshold, so a reader change that breaks rigidity is caught.

## Attribution

The art is **CC-BY-SA 3.0** (`art/LICENSE.txt`). Attribution and share-alike
carry to anything derived from it, including converted animation data. The
extracted intermediate should record the source path per clip so provenance
survives the conversion — the `rig.json` `source` field exists for this, not
only for debugging.

## Sequencing

1. **This spec** — audit, extraction, intermediate.
2. **`.rig` format + `tools/rigpack` + the `AnimationSet` surface**, designed
   from the audit report.
3. **[Character viewer: sockets](2026-08-06-character-viewer-sockets-design.md)**
   — already specified, blocked on 2.
