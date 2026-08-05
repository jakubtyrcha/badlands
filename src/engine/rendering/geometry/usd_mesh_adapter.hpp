#pragma once

// Turns the neutral UsdSceneData (engine/assets/usd_scene.hpp) into the
// engine's own mesh type.
//
// This lives in badlands_engine rather than badlands_usd_lib for one concrete
// reason: TexturedMeshResult transitively includes <dawn/webgpu_cpp.h> through
// mesh_components.hpp. Keeping the loader's output free of that is what lets
// the parser be tested with no GPU in the link line, so the Dawn dependency
// starts here and no earlier.
//
// Everything this does is pure CPU -- no device, no queue, no material
// construction. Building the DeferredMaterial from the resolved pack directory
// is the caller's job (MaterialLibrary::Get).

#include <string>
#include <vector>

#include "engine/assets/usd_material_binding.hpp"
#include "engine/assets/usd_scene.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"

namespace badlands {

// One mesh ready to hand to AddMeshEntity, plus the pack directory whose
// material it should be drawn with.
struct ImportedModel {
  std::string name;         // source prim name, for logging and scene-node names
  TexturedMeshResult mesh;  // 11-float interleaved, indexed, Y-up, metres
  std::string pack_dir;     // feeds MaterialLibrary::Get
};

// Converts every usable mesh in `scene`, applying in order:
//   1. `meters_per_unit` scaling, so the result is in metres;
//   2. the Z-up -> Y-up rotation when the stage says Z-up (every prop shipped
//      today does, so this path is the norm rather than the exception);
//   3. interleaving into pos(3) + uv(2) + normal(3) + tangent(3).
//
// A mesh missing normals, UVs or tangents is SKIPPED and logged rather than
// emitted with zeroed attributes: a vertex short of its normal renders as a
// black surface with no error anywhere, which is far harder to diagnose than a
// missing model.
//
// Returns an empty vector if `scene.ok` is false or nothing survived.
std::vector<ImportedModel> BuildImportedModels(
    const UsdSceneData& scene, const UsdMaterialBinding& binding);

}  // namespace badlands
