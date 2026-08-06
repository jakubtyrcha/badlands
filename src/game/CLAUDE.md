# src/game/ — the render/scene layer

Geometry generation, map data, scene composition, UI logic, picking. **Not** the
simulation — that is `game/` (see `game/CLAUDE.md`); the `src/game/` vs `game/` split
is a real trap.

## The instanced LOD chain has THREE layers, and only the top one has a domain
- **`visual/instanced_lod_model.hpp` is neutral by contract.** `InstancedLodModel` is
  `levels[lod][submesh]` + one `InstancedMaterialSpec` per submesh + ascending
  thresholds + an `ImpostorBakeSpec`. No tree, bark, leaf, foliage or prop vocabulary
  may enter it — that is what lets one field builder and one impostor baker serve both.
- **`visual/instanced_lod_field.cpp` and `visual/impostor_baker.cpp` consume that type
  and nothing else.** They used to be `BuildTreeField`/`BakeImpostorAtlas` shaped around
  exactly two submeshes with the materials and the bark decimation policy hardcoded.
- **Producers own their domain:** `tree_lod_model` (2 submeshes: bark + voxel crown,
  `SimplifyBarkForVoxelLod`), `prop_lod_model` (1 submesh, triangle LODs). Adding a kind
  of model means adding a producer, never touching the two layers below.
- **The model type is DEVICE-FREE on purpose.** `BuildForestModels` runs producers under
  `ParallelFor` across ~28 models with no GPU, so a material wanting a flat support
  texture asks via `SolidColorTextureSpec` (the builder creates and dedupes it) instead
  of carrying a `wgpu::TextureView` it would have needed a device to make.
- **`LodModelBounds` is per-submesh, not one box.** A tree rests on its BARK — resting it
  on the union sinks it by the crown's overhang, since voxel tets overscale past their
  cards — while the GPU bounds sphere and the cell-cull padding want `Combined()`.

## Triangle LODs for imported meshes: the weld is the whole trick
- **`SimplifyMesh` welds on the FULL 12-float vertex, and a flat-shaded model has no two
  vertices alike.** Every triangle carries its own three with a face normal, so edge
  collapse has no shared edge: boulder_01 and brass_vase_03 return **1.000** of their
  source triangles at every ratio down to 0.05.
- **`WeldMeshByPrefix(…, 5)` is the fix** — position+UV, fusing the flat-shading normal
  splits while keeping UV seams apart. `meshopt_simplifyWithAttributes` is NOT an
  alternative: it operates on the index buffer it is handed and cannot merge what that
  buffer keeps apart (`meshoptimizer.h:402` says to reindex first).
- **Welding runs at LOD 0 too**, so every level shades identically (no faceted→smooth pop
  at the first switch) and the vertex buffer shrinks up to 6x.
- **Normals AND tangents are regenerated after every weld and decimation.** A merge
  survivor kept one arbitrary member's normal, and a tangent inconsistent with its normal
  inverts the normal map's response. The degenerate-tangent fallback must be derived from
  the normal (`AnyPerpendicular`), never a constant — the shader's
  `normalize(T - N*dot(T,N))` is NaN when they are parallel.
- **Edge collapse vs vertex clustering is MEASURED per level, not a fixed index.**
  Post-weld boulder_01 collapses to its target exactly (clustering there is a UV smear
  for nothing), while the mace and the chest are genuinely several disconnected pieces
  and floor ~4x high. See `game/tests/prop_lod_report_tests.cpp` (`[.report]`).

## LOD thresholds
- **Props derive theirs** from size + triangle count (`visual/lod_screen_space.hpp`), so
  the level COUNT is per model.
- **Foliage does NOT go through that helper.** `kFoliageVoxelWorldSizes`, the preview
  thresholds and the 130 m impostor cutoff are screenshot-tuned around measured failures
  (the pine needle-sprig dead zone; the parallax A/B that rejected 70 m). Re-deriving
  them would silently change the forest. The two coexist deliberately.
