#pragma once

// The neutral result of reading a USD file: plain arrays and strings, nothing
// else. No tinyusdz types and no Dawn types appear here, and that is the whole
// point of the header existing separately from usd_loader.hpp --
// `TexturedMeshResult` transitively includes <dawn/webgpu_cpp.h> (via
// mesh_components.hpp), so a loader that spoke the engine's mesh type directly
// would drag the GPU stack into every consumer and make the parser untestable
// without a device.
//
// The conversion into the engine's interleaved vertex layout lives one layer
// up, in engine/rendering/geometry/usd_mesh_adapter.hpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace badlands {

// One mesh prim, already triangulated and single-indexable: every attribute
// array below is parallel to `positions` and is indexed by `indices`. USD's
// native face-varying attributes have already been split into unique vertices
// by the loader, because a GPU vertex buffer cannot express them.
//
// `normals`, `uvs` and `tangents` may each legitimately be EMPTY -- a mesh
// authored without UVs has no texcoords to give, and tangents are only
// computed when both normals and UVs exist. Consumers must check rather than
// assume; the adapter skips a mesh it cannot build a full vertex from.
struct UsdMeshData {
  std::string name;           // source prim name (e.g. "Cube_001")
  std::string material_name;  // bound USD material's name; empty if unbound

  // The prim's WORLD transform, and it is not optional decoration: a model's
  // parts are authored around a shared origin and placed by these (a chest's
  // lid sits above its body only because of this matrix). Points below are in
  // the prim's own local space, so dropping it collapses every part onto the
  // origin.
  //
  // 16 floats in COLUMN-MAJOR (glm) order, so a consumer can `glm::make_mat4`
  // it directly. USD stores row-major with a row-vector convention, whose
  // element-wise copy into a column-major column-vector matrix is exactly the
  // same layout -- the loader relies on that rather than transposing.
  // Identity by default.
  std::array<float, 16> transform{1, 0, 0, 0, 0, 1, 0, 0,
                                  0, 0, 1, 0, 0, 0, 0, 1};

  std::vector<float> positions;   // xyz triples, SOURCE units and SOURCE axis
  std::vector<float> normals;     // xyz triples
  std::vector<float> uvs;         // uv pairs
  std::vector<float> tangents;    // xyz triples
  std::vector<uint32_t> indices;  // triangle list, indexes the arrays above

  size_t vertex_count() const { return positions.size() / 3; }
  size_t triangle_count() const { return indices.size() / 3; }
};

// A material as the USD file describes it. Purely diagnostic today: prop
// materials are authored as `material.json` next to the model, so these paths
// are reported (and asserted on in tests) rather than loaded. They are kept
// because the day a model's manifest and its USD disagree, this is the only
// place that disagreement is visible.
struct UsdMaterialData {
  std::string name;        // USD material prim name; the key UsdMaterialBinding uses
  std::string base_color;  // asset path exactly as the USD resolved it
  std::string normal;
  std::string occlusion_roughness_metallic;
};

// Everything one .usdc file yielded. `ok == false` means the load failed and
// every other field is meaningless -- the loader has already logged why.
struct UsdSceneData {
  std::vector<UsdMeshData> meshes;
  std::vector<UsdMaterialData> materials;

  // Stage metadata, carried verbatim. The adapter -- not the loader -- applies
  // them, so the loader's output stays a faithful picture of the file.
  float meters_per_unit = 1.0f;
  bool z_up = false;  // from the stage's `upAxis`; false means Y-up

  bool ok = false;
};

}  // namespace badlands
