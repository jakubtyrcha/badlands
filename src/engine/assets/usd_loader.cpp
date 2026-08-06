#include "engine/assets/usd_loader.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>

#include <spdlog/spdlog.h>

#include "tinyusdz.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-converter.hh"

namespace badlands {
namespace {

namespace tydra = tinyusdz::tydra;

// Copies a Tydra VertexAttribute into a flat float array, requiring exactly
// `components` floats per vertex.
//
// Tydra hands attributes back as raw bytes plus a format enum, so the format
// check is not a formality: a Vec2 attribute read as Vec3 would silently
// walk off the end of the buffer. Returns an empty vector (after logging) on
// any mismatch, which callers treat as "this mesh has no such attribute".
std::vector<float> CopyFloatAttribute(const tydra::VertexAttribute& attr,
                                      size_t components, size_t vertex_count,
                                      const char* what, const std::string& mesh) {
  if (attr.empty()) return {};

  const tydra::VertexAttributeFormat expected =
      components == 2 ? tydra::VertexAttributeFormat::Vec2
                      : tydra::VertexAttributeFormat::Vec3;

  // Half4 is the one alternative worth decoding rather than rejecting: these
  // props carry Blender-authored `tangents` primvars as half4 (xyz plus a
  // handedness sign in w), and Tydra passes an authored primvar through
  // untouched instead of recomputing it. Rejecting it would silently throw
  // away the tangents on exactly the meshes that shipped good ones.
  //
  // `components == 4` asks for the handedness to be kept; see CopyTangents.
  if (attr.format == tydra::VertexAttributeFormat::Half4 &&
      (components == 3 || components == 4)) {
    // Same guard the float path applies below, and it is not redundant:
    // vertex_count() divides by elementSize * format_size, so an elementSize
    // of 2 still passes the count check while the indexing below reads
    // halves[i * 4 + c] -- every value silently taken from the wrong sample.
    if (attr.elementSize != 1) {
      spdlog::warn("LoadUsdScene: mesh '{}' {} has elementSize {} (expected 1)"
                   " -- skipping",
                   mesh, what, attr.elementSize);
      return {};
    }
    if (attr.vertex_count() != vertex_count) {
      spdlog::warn("LoadUsdScene: mesh '{}' {} has {} entries but {} vertices"
                   " -- skipping",
                   mesh, what, attr.vertex_count(), vertex_count);
      return {};
    }
    const auto* halves =
        reinterpret_cast<const tinyusdz::value::half*>(attr.get_data().data());
    std::vector<float> out(vertex_count * components);
    for (size_t i = 0; i < vertex_count; ++i) {
      for (size_t c = 0; c < components; ++c) {
        out[i * components + c] =
            tinyusdz::value::half_to_float(halves[i * 4 + c]);
      }
    }
    return out;
  }

  if (attr.format != expected) {
    spdlog::warn("LoadUsdScene: mesh '{}' {} has format {} (expected {}) --"
                 " skipping",
                 mesh, what, static_cast<int>(attr.format),
                 static_cast<int>(expected));
    return {};
  }
  if (attr.elementSize != 1) {
    spdlog::warn("LoadUsdScene: mesh '{}' {} has elementSize {} (expected 1)"
                 " -- skipping",
                 mesh, what, attr.elementSize);
    return {};
  }
  // Tydra was asked to make every attribute single-indexable, so anything not
  // parallel to `points` means that step did not do what we configured it to.
  if (attr.vertex_count() != vertex_count) {
    spdlog::warn("LoadUsdScene: mesh '{}' {} has {} entries but {} vertices --"
                 " skipping",
                 mesh, what, attr.vertex_count(), vertex_count);
    return {};
  }

  std::vector<float> out(vertex_count * components);
  std::memcpy(out.data(), attr.get_data().data(), out.size() * sizeof(float));
  return out;
}

// Copies tangents as xyzw quads, whatever the source format.
//
// Two shapes reach us. A Blender-authored `tangents` primvar arrives as half4
// and already carries handedness in w, which CopyFloatAttribute decodes when
// asked for 4 components. A Tydra-COMPUTED tangent is Vec3, and Tydra's solver
// only ever produces a right-handed frame -- so +1 is the correct w, not a
// guess.
std::vector<float> CopyTangents(const tydra::VertexAttribute& attr,
                                size_t vertex_count, const std::string& mesh) {
  if (attr.format == tydra::VertexAttributeFormat::Half4) {
    return CopyFloatAttribute(attr, 4, vertex_count, "tangents", mesh);
  }

  const std::vector<float> xyz =
      CopyFloatAttribute(attr, 3, vertex_count, "tangents", mesh);
  if (xyz.empty()) return {};

  std::vector<float> out(vertex_count * 4);
  for (size_t i = 0; i < vertex_count; ++i) {
    out[i * 4 + 0] = xyz[i * 3 + 0];
    out[i * 4 + 1] = xyz[i * 3 + 1];
    out[i * 4 + 2] = xyz[i * 3 + 2];
    out[i * 4 + 3] = 1.0f;
  }
  return out;
}

// The asset path behind one UsdPreviewSurface slot, or "" when the slot is a
// constant rather than a texture. Diagnostics only -- see UsdMaterialData.
std::string TexturePathFor(const tydra::RenderScene& scene, int32_t texture_id) {
  if (texture_id < 0) return {};
  if (static_cast<size_t>(texture_id) >= scene.textures.size()) return {};

  const int64_t image_id = scene.textures[static_cast<size_t>(texture_id)]
                               .texture_image_id;
  if (image_id < 0) return {};
  if (static_cast<size_t>(image_id) >= scene.images.size()) return {};

  return scene.images[static_cast<size_t>(image_id)].asset_identifier;
}

}  // namespace

UsdSceneData LoadUsdScene(const std::string& path) {
  UsdSceneData result;

  tinyusdz::Stage stage;
  std::string warn, err;
  if (!tinyusdz::LoadUSDCFromFile(path, &stage, &warn, &err)) {
    spdlog::error("LoadUsdScene: failed to load '{}': {}", path, err);
    return result;
  }
  if (!warn.empty()) spdlog::warn("LoadUsdScene: '{}': {}", path, warn);

  tydra::RenderSceneConverterEnv env(stage);
  env.usd_filename = path;
  // Texture asset paths in these files are relative to the .usdc, so the
  // resolver needs its directory to turn them into something meaningful.
  const std::string dir = std::filesystem::path(path).parent_path().string();
  if (!dir.empty()) env.set_search_paths({dir});

  // Tydra's defaults already triangulate (earcut) and split face-varying
  // attributes into single-indexable vertices, which is exactly what a GPU
  // vertex buffer needs. Two defaults are overridden:
  env.mesh_config.compute_normals = true;
  env.mesh_config.compute_tangents_and_binormals = true;
  // ...and this one matters. Upstream only computes tangents for meshes whose
  // bound material has a normal map wired up, but a prop's real material comes
  // from its material.json and is invisible here -- leaving the default would
  // make tangents depend on USD material authoring we never read.
  env.mesh_config.compute_tangents_only_with_normal_map = false;

  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  if (!converter.ConvertToRenderScene(env, &scene)) {
    spdlog::error("LoadUsdScene: failed to convert '{}': {}", path,
                  converter.GetError());
    return result;
  }

  // Mesh points come back in each prim's OWN local space, with placement left
  // in the node hierarchy -- so a model's parts must be gathered from there or
  // they all collapse onto the origin (which is what stacked a treasure
  // chest's lid inside its body). Walk the tree once and record each mesh's
  // world matrix.
  std::vector<std::array<float, 16>> mesh_transforms(
      scene.meshes.size(), std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0,
                                                 0, 0, 1, 0, 0, 0, 0, 1});
  {
    std::function<void(const tydra::Node&)> walk = [&](const tydra::Node& node) {
      // `id` indexes whichever array matches the node's TYPE (lights index
      // lights), so the type check is what keeps a light's index from being
      // read as a mesh's.
      if (node.nodeType == tydra::NodeType::Mesh && node.id >= 0 &&
          static_cast<size_t>(node.id) < mesh_transforms.size()) {
        auto& dst = mesh_transforms[static_cast<size_t>(node.id)];
        for (int c = 0; c < 4; ++c) {
          for (int r = 0; r < 4; ++r) {
            dst[static_cast<size_t>(c * 4 + r)] =
                static_cast<float>(node.global_matrix.m[c][r]);
          }
        }
      }
      for (const auto& child : node.children) walk(child);
    };
    for (const auto& node : scene.nodes) walk(node);
  }

  result.meters_per_unit = static_cast<float>(scene.meta.metersPerUnit);
  result.z_up = (scene.meta.upAxis == "Z");

  result.materials.reserve(scene.materials.size());
  for (const auto& src : scene.materials) {
    UsdMaterialData mat;
    mat.name = src.name;
    if (src.surfaceShader) {
      mat.base_color = TexturePathFor(scene, src.surfaceShader->diffuseColor.texture_id);
      mat.normal = TexturePathFor(scene, src.surfaceShader->normal.texture_id);
      mat.occlusion_roughness_metallic =
          TexturePathFor(scene, src.surfaceShader->occlusion.texture_id);
      if (mat.occlusion_roughness_metallic.empty()) {
        // Poly Haven packs AO/roughness/metallic into one image, so whichever
        // of the three slots is wired identifies the same file.
        mat.occlusion_roughness_metallic =
            TexturePathFor(scene, src.surfaceShader->roughness.texture_id);
      }
    }
    result.materials.push_back(std::move(mat));
  }

  result.meshes.reserve(scene.meshes.size());
  for (size_t mesh_index = 0; mesh_index < scene.meshes.size(); ++mesh_index) {
    const auto& src = scene.meshes[mesh_index];
    const size_t vertex_count = src.points.size();
    if (vertex_count == 0 || src.faceVertexIndices().empty()) {
      spdlog::warn("LoadUsdScene: '{}' mesh '{}' has no geometry -- skipping",
                   path, src.prim_name);
      continue;
    }
    if (!src.is_single_indexable) {
      // Configured not to happen; if it does, the arrays below would not line
      // up and emitting them anyway would corrupt the mesh silently.
      spdlog::warn("LoadUsdScene: '{}' mesh '{}' is not single-indexable --"
                   " skipping",
                   path, src.prim_name);
      continue;
    }

    // faceVertexIndices() falls back to the raw, UNTRIANGULATED indices
    // whenever both triangulated arrays are empty, so "Tydra was asked to
    // triangulate" is not a guarantee. Left unchecked, n-gon indices reach the
    // adapter and get sliced into triangles three at a time -- garbage
    // geometry with nothing in the log.
    const std::vector<uint32_t>& indices = src.faceVertexIndices();
    if (indices.size() % 3 != 0) {
      spdlog::warn("LoadUsdScene: '{}' mesh '{}' has {} indices, not a whole"
                   " number of triangles -- skipping",
                   path, src.prim_name, indices.size());
      continue;
    }
    const bool indices_in_range =
        std::all_of(indices.begin(), indices.end(),
                    [vertex_count](uint32_t i) { return i < vertex_count; });
    if (!indices_in_range) {
      spdlog::warn("LoadUsdScene: '{}' mesh '{}' has an index past its {}"
                   " vertices -- skipping",
                   path, src.prim_name, vertex_count);
      continue;
    }

    UsdMeshData mesh;
    mesh.name = src.prim_name;
    mesh.transform = mesh_transforms[mesh_index];
    // USD's `orientation`. Tydra records the flag but never acts on it (it only
    // copies it when merging), so a leftHanded prop keeps reversed winding and
    // would vanish under backface culling with nothing logged.
    mesh.right_handed = src.is_rightHanded;
    if (src.material_id >= 0 &&
        static_cast<size_t>(src.material_id) < result.materials.size()) {
      mesh.material_name = result.materials[static_cast<size_t>(src.material_id)].name;
    }

    mesh.positions.resize(vertex_count * 3);
    std::memcpy(mesh.positions.data(), src.points.data(),
                mesh.positions.size() * sizeof(float));

    mesh.indices = indices;
    mesh.normals = CopyFloatAttribute(src.normals, 3, vertex_count, "normals",
                                      src.prim_name);
    mesh.tangents = CopyTangents(src.tangents, vertex_count, src.prim_name);

    // Slot 0 is the primary UV set (`st` by default); higher slots are
    // multi-texturing we have no material path for.
    const auto uv_it = src.texcoords.find(0);
    if (uv_it != src.texcoords.end()) {
      mesh.uvs = CopyFloatAttribute(uv_it->second, 2, vertex_count, "texcoords",
                                    src.prim_name);
    }

    result.meshes.push_back(std::move(mesh));
  }

  if (result.meshes.empty()) {
    spdlog::error("LoadUsdScene: '{}' yielded no usable meshes", path);
    return result;
  }

  result.ok = true;
  return result;
}

}  // namespace badlands
