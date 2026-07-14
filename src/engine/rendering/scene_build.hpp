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
// about X so its normal is +Y), with a neutral solid-color material from
// `matlib.SolidColor(tint, roughness)`. Factors out the near-identical floor
// builder the three views each had.
//
// `tint` defaults to ~110/255 gray: an upward-facing floor is near-parallel
// to the default sun and gets close to the scene's highest NdotL, so a
// brighter albedo clips to white after tonemapping (see model_viewer_view.cpp
// AddFloor's original comment / the S2.E task report's empirical sweep).
void AddGrayFloor(SceneGraph& scene, MaterialLibrary& matlib, float size,
                  glm::vec3 tint = glm::vec3(110.0f / 255.0f),
                  float roughness = 0.9f);

}  // namespace badlands
