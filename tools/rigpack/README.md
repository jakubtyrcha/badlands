# rigpack — the offline animation packer

Turns the 0 A.D. **import intermediate** into a runtime rig asset that
`AnimationSet` loads.

```sh
# regenerate the intermediate (212 MB, ~25 s), from the 0ad checkout
cd ~/repos/0ad && git checkout export/badlands-anim
python3 source/tools/animexport/animexport.py extract --out /tmp/animexport

scripts/build.sh badlands_rigpack

# what ships: the 31 creature families, one directory each (~7 s)
./build/badlands_rigpack --recipe assets/characters/0ad/pack.json \
                         --intermediate /tmp/animexport

# ad hoc: every family the manifest carries, including gates and siege engines
./build/badlands_rigpack --all --out /tmp/every-rig --intermediate /tmp/animexport
```

`source/tools/animexport/README.md` on that branch is the **canonical format
reference**. When it and this file disagree, it wins.

Rigs are written **beside the recipe**. A `"families"` recipe writes one
directory per family, named for the exporter's own slug:

```
assets/characters/0ad/
  pack.json              the recipe (checked in, hand-authored)
  biped/                                                <- generated
    rig.json             the manifest AnimationSet loads
    skeleton.ozz
    clips/<name>.ozz
  horse/  wolf/  ursidae_armature/  ...                 <- 31 in all
```

A `"family"` recipe (singular) writes one rig beside itself instead — that form
still exists and still curates clips by name.

## You need the 0ad checkout to run this

The intermediate is **not in this repo**. It is produced by the importer in the
`0ad` checkout and named on the command line, which is why `pack.json` carries no
path of its own — nothing in the repo is machine-specific, and nothing binary is
stored twice.

The generated files ARE checked in (they are what ships), so a normal build and
a normal test run never touch this tool. You only run it when a rig changes.

## What a recipe is for

The corpus is 1,220 clips named the way 0 A.D. names them; a game asks for
`"idle"`. The recipe is the one place those two vocabularies meet, and it is
data so the shipped rig is reproducible from the repo and reviewable in a diff.

```json
{
  "family": "Biped",
  "yaw_offset_degrees": 0,
  "clips": {
    "idle":   "biped__citizen__idle_minister_01",
    "attack": "biped__infantry__swordsman__attack_melee_shield_01"
  }
}
```

Clip order is preserved: it becomes the manifest's order, and so the viewer's.

**`"clips": "*"` takes the whole family**, each clip keeping its own name. That is
what `assets/characters/0ad` uses across all 31 families — 940 clips, ~84 MB — so
the viewer can show everything a skeleton does rather than a curated handful. The
cost is that those are 0 A.D.'s names, so such a rig has no clip called `idle`;
pointing the game at one means adding that mapping back.

**`"families": [...]` packs several**, one directory each. The shipped list is
creatures only: the other 37 families are gates, ships, sails, chariots, siege
artillery, banners and markers. Excluding them saves just ~9 MB, so that list is
about relevance, not size.

**Classify by SOURCE PATH, not by family name.** 28 of 68 families are named
after a fallback root bone, so `main_pelvis-node` is a giraffe, `Head_Rotation_IK`
a camel, `Main` a shark, `Horse-node` a lion, `Deer01_Pelvis` a rabbit, `Body` a
crow and `fish_shoal_armature` birds. In the other direction, `Aspron_Armature` is
36 clips of a shield prop and no animal at all.

**Name clips by their `name`, not their logical name.** A clip can also be found
by any of its `logical_names`, case-insensitively — 0 A.D. writes both
`attack_melee` and `Attack_melee` — but a logical name is a **variant class, not
a clip**. On `Biped`, `Idle` matches 232 clips, `Walk` 117 and `attack_melee` 89,
because 0 A.D. picks among variants at random by weight. rigpack takes the first
in file order and warns:

```
recipe clip "idle" matched 232 clips by logical name "Idle";
packed "biped__chariot__idle". Name a clip directly to choose deliberately.
```

That is a chariot's idle standing in for a generic one. Convenient for a first
look, wrong for anything shipped.

`family` is the AUTHORED name (`Biped`), not the directory slug (`biped`);
`manifest.json` maps between them, and lookup is **case-sensitive first**. Both
matter: 0 A.D. ships two distinct families named `Main` and `main`, which
previously shared a directory on a case-insensitive filesystem and silently lost
11 clips.

## Read the report

Every run prints one. Five things in it are worth acting on:

- **root(payload)** — should be ~0. The exporter strips root motion, and a
  non-zero number here means it did not. The sim owns movement, so such a clip
  moves a character twice. **root(removed)** is what the exporter says it took
  out; non-zero on four clips corpus-wide, all animal deaths, and rigpack does
  not put it back.
- **even = NO** — the source frames are unevenly spaced (135 clips are). Not a
  problem: frame times are honoured exactly. Worth knowing before judging
  playback by eye.
- **dropped sockets** — usually zero, because the exporter already collapses
  them; on `Biped` it drops 21 duplicate prop nodes before we ever see them. Ours
  is the second line of defence. `weapon_R` exists as a joint *and* as
  `prop-weapon_R` a millimetre away; the joint wins.
- **"likely transposed"** — the payload stopped looking column-major, so the
  exporter's convention changed under us. Nothing fails; the rig is simply wrong
  everywhere.
- **skipped clips** — a recipe name the intermediate does not carry. One missing
  clip costs one animation, not the rig.
- **"strictly increasing" / "clamped into range"** — the source authored two
  frames at the same instant, out of order, or outside `[0, duration]`
  (`rabbit_walk` starts at −0.0417 s). ozz rejects all three, so times are
  clamped into range and the last frame of each equal-time run wins. Three clips
  corpus-wide; all survive.

The report also names the clip the **rest pose** came from: the intermediate has
no bind pose, so frame 0 of the first readable clip stands in.

## Two things that look like bugs and are not

**The rig has long bones radiating from its root.** `knee_L/R`, `elbow_L/R`,
`handIK_L/R` and `footIK_L/R` are real joints, siblings of `hip` under the
synthetic `__root__` rather than part of the limb chains. The exporter keeps them
deliberately, as Unreal keeps its Mannequin's. They draw as spokes.

**`FindAttachment("root")` does not resolve, and neither do `garrisoned`,
`crest`, `rider`, `handle` or `patch_NNN`.** 0 A.D.'s actor XML references these
— `root` 4,130 times — but they come from *mesh* prop points baked into its PMD
files, or from engine code. They are not in the animation rigs and never arrive
here. The skeleton's own root is named `__root__`. If you need them, author them.

## Layout

| File | What it is |
|---|---|
| `intermediate.{hpp,cpp}` | reads the importer's `rig.json` + `.bin`. **The one file a schema change touches.** |
| `recipe.{hpp,cpp}` | reads `pack.json` |
| `pack.{hpp,cpp}` | the conversion; returns a report and logs nothing |
| `main.cpp` | flags, and printing the report |
| `tests/rigpack_tests.cpp` | in `badlands_animation_tests` — synthetic intermediates, packed and loaded back through the real `AnimationSet::Load` |

`badlands_rigpack_lib` links ozz's offline builders, glm and nlohmann, and
deliberately **not** SDL3, Dawn or `badlands_engine` — the same discipline
`badlands_usd_lib` keeps. It is an asset tool; nothing it needs belongs in the
runtime.

## The trap

`SkeletonBuilder` **reorders joints**. An index carried across that boundary
points at the wrong joint, and the result is a rig that loads, plays, and is
wrong — the arm animating with the head's data. Everything downstream of the
build resolves by **name**, and the manifest stores each socket's parent as a
joint name for the same reason. `tests/rigpack_tests.cpp` builds a hierarchy
where the two orderings genuinely disagree, so this stays caught.
