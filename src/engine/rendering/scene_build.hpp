#pragma once

// Task S2.E: shared, game-agnostic scene-building helper. Every AppView that
// turns procedural geometry (engine/rendering/geometry/*) + a PBR material
// pack (MaterialLibrary::Get's DeferredMaterial return type) into a rendered
// entity was duplicating the same four lines (wrap the TexturedMeshResult in
// a ResolvedMesh, create a node, attach a kDeferred MeshAttachment) --
// PlaceholderView::BuildSphereScene (src/engine/app/placeholder_view.cpp) is
// the pattern this factors out, so badlands_viewer/game/ai_sandbox's views
// (Tasks S2.E/F/G) can reuse it instead of re-deriving it. Engine,
// game-agnostic: no game types appear here.

#include <string>

#include <glm/glm.hpp>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"

namespace badlands {

// Creates a node named `name` under `scene`'s root with local transform
// `transform` (decomposed to TRS via Trs::FromMatrix; identity by default),
// and attaches `mesh`/`mat` as a kDeferred MeshAttachment (mat.factory +
// mat.params carried through verbatim). `mesh` is consumed -- its vertex data
// is moved into the attachment's ResolvedMesh, not copied. Returns the new
// node's handle (e.g. for a caller that wants to reposition/reparent it
// later; most callers can ignore it).
NodeHandle AddMeshEntity(SceneGraph& scene, const char* name,
                         TexturedMeshResult&& mesh, const DeferredMaterial& mat,
                         const glm::mat4& transform = glm::mat4(1.0f));

// Adds a horizontal ground quad named "floor" spanning [-size/2, size/2] in
// X and Z at Y=0 (GenerateQuadTexturedMesh in the XY plane, rotated -90deg
// about X so its normal is +Y), textured with the PBR pack at `pack_dir`
// (loaded via `matlib.Get`) and its UVs tiled so the pack repeats across the
// floor rather than stretching one copy across it: `uv_scale` is the number
// of texture repeats across the full `size` span (e.g. size=80, uv_scale=40
// repeats the pack every 2 world units -- matlib's shared sampler is
// Repeat-addressed, so out-of-[0,1] UVs wrap). Factors out the
// near-identical floor builder the three views each had.
NodeHandle AddFloor(SceneGraph& scene, MaterialLibrary& matlib, float size,
                    const std::string& pack_dir, float uv_scale);

// Same quad/transform/UV-tiling as the pack-dir overload above, but attaches
// a pre-built `mat` directly instead of resolving one from a pack dir --
// e.g. a MaterialLibrary::SolidColor() debug material. Stays game-agnostic
// (engine types only): callers that want a gray debug floor build the
// DeferredMaterial themselves and pass it in.
NodeHandle AddFloor(SceneGraph& scene, float size, const DeferredMaterial& mat,
                    float uv_scale);

}  // namespace badlands
