# Animation intermediate: handoff to the consumer

Everything needed to write the badlands-side importer. Facts here were measured
against all 1,220 clips, not sampled — several of them contradict earlier
drafts, and where they do, this document wins.

## 1. Where the data is

**The producer** lives in the `0ad` repo, branch `export/badlands-anim`, at
`source/tools/animexport/`. Three commits; `README.md` there is the canonical
format reference.

**The output is not committed anywhere** — it is 212 MB of derived data.
Regenerate it:

```sh
cd /Users/jakub/repos/0ad
python3 source/tools/animexport/animexport.py extract --out <dir>   # ~25 s
python3 source/tools/animexport/animexport.py audit   --out <dir>   # ~11 s
```

`--family Biped` extracts one skeleton; `--limit N` does a quick pass. A fresh
full run currently sits in `/tmp/animexport` — **treat that as scratch**, it
will not survive a reboot.

```
<dir>/
  manifest.json                  every family, plus clips that failed to read
  <family-slug>/
    rig.json                     joints, sockets, clip table
    clips/<clip_name>.bin        float32 matrices
  audit/
    report.md  clips.csv  sockets.csv
```

**Scale:** 1,220 clips, 68 families, 0 failures, 212 MB. `Biped` is the one that
matters — 651 clips, 55 joints, 28 sockets, 143 MB. Next largest are `gate` (56
clips), `Horse` (40), `Elephantidae_Asian_Quadraped` (37).

Licence: **CC-BY-SA 3.0**. Attribution and share-alike carry to derived
animation data. Every clip records its `source` path for this reason.

## 2. The data format

### `rig.json`

```json
{
  "format": "badlands-anim-intermediate",
  "version": 1,
  "family": "Biped",
  "coordinate_space": "engine",
  "joints":  [{"name": "__root__", "parent": -1}, {"name": "hip", "parent": 0}],
  "sockets": [{"name": "armpad_L", "parent": 45, "offset": [16 floats], "drift": 0.0}],
  "clips":   [{
    "name": "biped__citizen__build",
    "source": "art/animation/biped/citizen/build.dae",
    "frames": 21,
    "frame_rate": 24.000001,
    "frame_times": [0.0, 0.041667, ...],
    "uniform": true,
    "duration": 0.833333,
    "data": "clips/biped__citizen__build.bin",
    "markers": [{"name": "event", "ratio": 0.5}],
    "speed": 100,
    "logical_names": ["Build", "Build_farm"],
    "loop": true,
    "loop_source": "endpoints",
    "root_motion": 0.0,
    "root_motion_track": []
  }]
}
```

### `.bin` payload

- `float32`, **little-endian**, no header.
- Shape `[frames][joints][16]`, joints in `rig.json` order.
- Each 4×4 is **column-major** — memcpy straight into a `glm::mat4`.
  **Do not transpose**; the writer already did.
- Transforms are **parent-local**, already in engine space.
- Frame *i* occurs at `frame_times[i]` seconds. **Not necessarily evenly
  spaced** — see the caveat below.

### Coordinate space

Already converted. Right-handed, **up +Y, forward +Z**, matching
`kCharacterForward`. **`yaw_offset` is 0** for every one of these rigs — do not
add a correction. (The Quaternius rig keeps its own 180° offset; that is
unrelated.)

The conversion was `(x,y,z) → (x,z,−y)` applied at joint 0 only, and verified:
`engine_world == C @ source_world` holds to 5e-08 across all joints.

### Skeleton shape

- **Joint 0 is always `__root__`**, a synthetic single root. Every family has
  exactly one root.
- Joints are ordered **parents before children**; `parent` is an index, `-1`
  only for joint 0. Verified across all 68 families.
- The IK helpers `knee_L/R`, `elbow_L/R`, `handIK_L/R`, `footIK_L/R` are real
  joints, children of `__root__` — siblings of `hip`, not part of the limb
  chains. Unreal's Mannequin does the same thing; they are kept deliberately.

## 3. Caveats — sockets, slots and names

### 3.1 Sockets and joints are ONE namespace

Expose **a single lookup** — `FindAttachment(name)` → handle — resolving
against `joints` *and* `sockets`, hiding which it found. This mirrors Unreal,
where `AttachToComponent` takes a name resolving against both.

This is not a style preference. It is what makes the joint/socket split an
import-time size decision rather than a contract: sockets get promoted to
joints and back (see 3.3) without the consumer changing.

Names are **unique across the union** — verified on every family. Where they
would have collided, the socket was dropped and the joint kept.

### 3.2 The attachpoints you will actually ask for are JOINTS, not sockets

This trips people up. On `Biped`:

| attachpoint | resolves to |
|---|---|
| `weapon_R`, `weapon_L`, `weapon_bow` | **joint** |
| `shield`, `shield_arm` | **joint** |
| `helmet`, `head`, `neck_guard` | **joint** |
| `ammo`, `projectile` | **joint** |
| `quiver_L`, `quiver_R`, `sheath_01_L/R`, `sheath_02_L/R` | **joint** |
| `back`, `backplate`, `breastplate`, `fauld`, `glove_*`, `armpad_*`, … | socket |

0 A.D. authors each attach point twice — a bone `weapon_R` *and* a prop node
`prop-weapon_R`, measured 0.001 apart — because it needs both for a mesh/skeleton
split we do not have. **On collision the joint wins and the socket is dropped**,
so 21 prop nodes disappear on Biped. That is intentional, not data loss.

Sockets are therefore mostly the *armour decoration* vocabulary. The
weapon/shield/helmet vocabulary is joints. **Both reachable through the one
lookup** — which is exactly why you should not build two.

### 3.3 Some sockets became joints, and were renamed

Five sockets across the corpus measurably move relative to their parent and are
promoted to animated joints, per family:

| family | name | drift |
|---|---|---|
| Biped | `quiver_B` | 1.54 |
| Lithobolos_Small_Armature | `operator_03` | 1.44 |
| Biped | `seax_back` | 0.56 |
| Tb | `projectile` | 0.41 |
| Biped | `backplate` | 0.22 |

`quiver_B` drifts about a body height — freezing it would have been very
visible. A promoted node is **renamed to its plain name** (`prop_quiver_B` →
`quiver_B`), so the unified lookup still resolves it.

Consequence: **a name can be a socket in one family and a joint in another.**
Never hardcode which. Always go through the lookup.

### 3.4 Attachpoints 0 A.D. uses that DO NOT exist here

Actor XML references these; the animation skeletons do not contain them:

`root` (4,130 uses), `garrisoned` (338), `crest` (142), `rider` (184),
`handle` (82), and the `patch_NNN` building decorations.

They come from **mesh** prop points baked into 0 A.D.'s PMD files, or are added
in engine code — not from the animation rigs. `FindAttachment` will not resolve
them. If you need them, author them yourself.

### 3.5 Frame timing is NOT a constant rate

**The single most likely way to get this wrong.** Do not assume 30 fps, or any
fixed rate:

- 545 clips are 24 fps, 449 are 30, the rest scatter from 0.2 to 60.
- **135 clips are not evenly sampled at all.** `wolf_idle_01.dae` has 64 frames
  over 11.3 s and holds a pose for **5.1 s** between two frames spaced 0.067 s
  apart — a 76× spread.

**Use `frame_times[i]`.** `frame_rate` is only the mean step; `uniform` tells
you whether it means anything. ozz's `RawAnimation` takes explicit key times, so
this suits you directly — no resampling needed, and none was done.

`frame_times[0]` is **always 0.0**, and `frame_times[-1] == duration`. That is
an invariant of the format, not of the source: 275 clips are keyed at an offset
on a shared authoring timeline (every `BlenderFeline` clip at 6 s, `feline_run`
at 23.4 s, `rabbit_walk` at −0.0417 s), and the exporter rebases them. Before it
did, `duration` and `frame_times` disagreed and a consumer honouring the times
held frame 0 for the whole lead-in — `feline_run` sat still for 92 % of its
length. **Re-extract if your intermediate predates 2026-08-07.**

### 3.6 `loop` is inferred, not authored

0 A.D. declares loop nowhere. Across the corpus: 934 clips decided by matching
endpoints, 98 by a name heuristic, 188 unknown. `loop_source` says which.
**Treat it as a hint**, especially the 98 name-derived ones.

### 3.7 Root motion is stripped but recoverable

Only 4 clips have any, all animal deaths (`rhino_death` displaces 3.25,
`donkey_death_01` 2.18, two peacock deaths). The payload has it removed;
`root_motion` holds the net displacement and `root_motion_track` the per-frame
offsets, so you can restore it. Everything else has `root_motion: 0.0` and an
empty track.

### 3.8 Family names and directory slugs differ

`rig.json`'s `family` is the authored name (`Biped`, `Ship Row`); the directory
is a slug (`biped`, `ship_row`). **Key off `manifest.json`**, which maps one to
the other, rather than deriving either.

0 A.D. ships two *distinct* families named `Main` and `main` — they slug to
`main` and `main_2`. On a case-insensitive filesystem they previously shared a
directory and one silently overwrote the other, costing 11 clips with no error.

28 of 68 families are named from a fallback (the root bone) because their root
did not resolve against `art/skeletons/*.xml` — names like `armature`,
`base_node`, `ground`, `anchor_1`. Cosmetic, but do not read meaning into them.

### 3.9 Joint sets vary within a family

24 of 68 families have clips with differing joint sets. The canonical set is the
**union**; a clip missing a joint gets it from the bind pose. Every clip is
written at the family's full width, so `.bin` size is always
`frames × joints × 16 × 4` bytes. Assert it.

### 3.10 Clip names and logical names

`name` is the source path flattened (`biped/infantry/capturing_a.dae` →
`biped__infantry__capturing_a`), unique by construction.

`logical_names` comes from `art/variants/*.xml` and is a **list**, because one
clip serves several states — and 0 A.D. is inconsistent about case, so you will
see both `attack_melee` and `Attack_melee` on the same clip. 725 of 1,220 clips
have any; the other 495 are referenced by no variant.

`markers` are normalized `[0,1]` points: `event` (172 clips), `load` (47),
`sound` (4). Where two variants disagreed on a value, the most common won — 4
clips had such a conflict.
