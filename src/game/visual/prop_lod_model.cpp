#include "game/visual/prop_lod_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "game/geometry/mesh_lod.hpp"

namespace badlands {

namespace {

constexpr size_t kStride = kTexturedMeshFloatsPerVertex;
// Offsets within the 12-float textured vertex: pos(3) uv(2) normal(3) tangent(4).
constexpr size_t kPosOffset = 0;
constexpr size_t kUvOffset = 3;
constexpr size_t kNormalOffset = 5;
constexpr size_t kTangentOffset = 8;
// Position + UV. The prefix WeldMeshByPrefix compares: merging the flat-shading
// normal splits (which is what unblocks decimation) while keeping UV seams
// apart (which is what stops textures smearing across them).
constexpr size_t kWeldPrefix = 5;

glm::vec3 ReadVec3(const std::vector<float>& v, size_t i, size_t offset) {
  const size_t b = i * kStride + offset;
  return {v[b], v[b + 1], v[b + 2]};
}

void WriteVec3(std::vector<float>& v, size_t i, size_t offset,
               const glm::vec3& value) {
  const size_t b = i * kStride + offset;
  v[b] = value.x;
  v[b + 1] = value.y;
  v[b + 2] = value.z;
}

// Any unit vector perpendicular to `n`, chosen so it is never degenerate.
//
// A CONSTANT fallback is not safe here: the shader orthogonalizes with
// normalize(T - N * dot(T, N)), so a tangent parallel to the normal makes that
// normalize(vec3(0)) -- NaN on the GPU, and invisible on the CPU. Deriving the
// fallback FROM the normal is what guarantees it never lines up with it.
glm::vec3 AnyPerpendicular(const glm::vec3& n) {
  const glm::vec3 axis =
      std::abs(n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
  return glm::normalize(glm::cross(axis, n));
}

// Area-weighted smooth normals plus UV-derived tangents, over the whole mesh.
//
// Both are regenerated, and both have to be. The weld merged vertices that
// disagreed about their normal, so the survivor kept one arbitrary member of
// that set; its authored tangent is stale for the same reason, and a tangent
// inconsistent with its normal flips the normal map's response rather than
// merely tilting it.
//
// NORMALS ARE ACCUMULATED PER POSITION, NOT PER VERTEX, and that distinction is
// the difference between a smooth surface and one creased along every UV seam.
// The weld splits on position+UV, so two vertices at one position either side
// of a seam are distinct entries here; accumulating into them separately gives
// each only the faces on its own side, and the two normals then disagree
// exactly where the authored ones were continuous. Grouping by position first
// and scattering the result back is what keeps the seam invisible.
//
// TANGENTS stay per vertex, deliberately: a UV seam is precisely where the
// parameterization changes, so the two sides legitimately have different
// tangent frames and merging them would be wrong.
//
// Cross products are NOT normalized before accumulation: their magnitude is
// twice the triangle's area, which is exactly the weighting a smooth normal
// wants, so a sliver contributes proportionally to how much surface it is.
void RegenerateNormalsAndTangents(std::vector<float>& vertices,
                                  const std::vector<uint32_t>& indices) {
  const size_t count = vertices.size() / kStride;
  if (count == 0 || indices.size() < 3) return;

  // Position -> group. Bitwise on the three position floats, which is the same
  // identity meshopt's remap uses, so this grouping and the weld agree.
  using PosKey = std::array<float, 3>;
  std::map<PosKey, uint32_t> groups;
  std::vector<uint32_t> group_of(count, 0);
  for (size_t i = 0; i < count; ++i) {
    const PosKey key{vertices[i * kStride + kPosOffset],
                     vertices[i * kStride + kPosOffset + 1],
                     vertices[i * kStride + kPosOffset + 2]};
    const auto [it, inserted] =
        groups.emplace(key, static_cast<uint32_t>(groups.size()));
    group_of[i] = it->second;
  }

  std::vector<glm::vec3> group_normals(groups.size(), glm::vec3(0.0f));
  std::vector<glm::vec3> tangents(count, glm::vec3(0.0f));
  std::vector<glm::vec3> bitangents(count, glm::vec3(0.0f));

  for (size_t t = 0; t + 2 < indices.size(); t += 3) {
    const uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
    if (i0 >= count || i1 >= count || i2 >= count) continue;

    const glm::vec3 p0 = ReadVec3(vertices, i0, kPosOffset);
    const glm::vec3 p1 = ReadVec3(vertices, i1, kPosOffset);
    const glm::vec3 p2 = ReadVec3(vertices, i2, kPosOffset);
    const glm::vec3 e1 = p1 - p0;
    const glm::vec3 e2 = p2 - p0;

    const glm::vec3 face = glm::cross(e1, e2);
    group_normals[group_of[i0]] += face;
    group_normals[group_of[i1]] += face;
    group_normals[group_of[i2]] += face;

    const glm::vec2 uv0(vertices[i0 * kStride + kUvOffset],
                        vertices[i0 * kStride + kUvOffset + 1]);
    const glm::vec2 uv1(vertices[i1 * kStride + kUvOffset],
                        vertices[i1 * kStride + kUvOffset + 1]);
    const glm::vec2 uv2(vertices[i2 * kStride + kUvOffset],
                        vertices[i2 * kStride + kUvOffset + 1]);
    const glm::vec2 d1 = uv1 - uv0;
    const glm::vec2 d2 = uv2 - uv0;
    const float det = d1.x * d2.y - d2.x * d1.y;
    // A degenerate UV triangle (collinear or coincident texcoords) has no
    // tangent frame at all; skipping it leaves the accumulation to its
    // neighbours rather than injecting an infinity that poisons them.
    if (std::abs(det) < 1e-12f) continue;
    const glm::vec3 tangent = (e1 * d2.y - e2 * d1.y) / det;
    // The BITANGENT is accumulated too, purely to recover handedness below.
    const glm::vec3 bitangent = (e2 * d1.x - e1 * d2.x) / det;
    for (uint32_t i : {i0, i1, i2}) {
      tangents[i] += tangent;
      bitangents[i] += bitangent;
    }
  }

  for (size_t i = 0; i < count; ++i) {
    glm::vec3 n = group_normals[group_of[i]];
    const float n_len = glm::length(n);
    // An unreferenced or fully-degenerate vertex keeps a usable frame rather
    // than a zero one: a zero normal reads as an unlit black surface with
    // nothing logged anywhere.
    n = n_len > 1e-12f ? n / n_len : glm::vec3(0.0f, 1.0f, 0.0f);

    // Gram-Schmidt against the final normal, so T is exactly what the shader's
    // own orthogonalization expects to receive.
    glm::vec3 t = tangents[i] - n * glm::dot(n, tangents[i]);
    const float t_len = glm::length(t);
    const bool have_tangent = t_len > 1e-12f;
    t = have_tangent ? t / t_len : AnyPerpendicular(n);

    WriteVec3(vertices, i, kNormalOffset, n);
    WriteVec3(vertices, i, kTangentOffset, t);
    // Handedness, DERIVED rather than assumed +1. A mirrored UV island has its
    // bitangent opposite to cross(N, T), and the shader reconstructs
    // B = w * cross(N, T) -- so writing +1 there inverts the normal map's V
    // response across exactly the islands a modeller mirrored to save texture
    // space. usd_mesh_adapter goes to some trouble to carry authored handedness
    // through (and to flip it for a mirroring prim transform); throwing it away
    // here and then hardcoding a sign would undo that.
    const bool flipped =
        have_tangent && glm::dot(glm::cross(n, t), bitangents[i]) < 0.0f;
    vertices[i * kStride + kTangentOffset + 3] = flipped ? -1.0f : 1.0f;
  }
}

TexturedMeshResult FinalizeLevel(std::vector<float> vertices,
                                 std::vector<uint32_t> indices,
                                 uint32_t vertex_count) {
  RegenerateNormalsAndTangents(vertices, indices);
  TexturedMeshResult out;
  out.mesh.geometry_type = GeometryType::kTexturedMesh;
  out.mesh.vertices = std::move(vertices);
  out.mesh.indices = std::move(indices);
  out.mesh.vertex_count = vertex_count;
  out.mesh.dirty = true;
  out.local_bounds = ComputeLocalAabbFromVertices(out.mesh.vertices, kStride);
  return out;
}

InstancedMaterialSpec PropMaterial(const PropMaterialTextures& textures) {
  InstancedMaterialSpec spec;
  spec.shader_name = "instanced_gbuffer";
  spec.shader_path = "material/instanced_gbuffer";
  spec.cache_namespace = "prop_mesh";
  // Back: an imported prop is a closed surface with consistent winding, unlike
  // the tree generator's grafted bark tubes.
  spec.cull_mode = wgpu::CullMode::Back;
  spec.casts_shadow = true;
  // Slot names, not binding indices -- see material_requirements.cpp's
  // instanced_gbuffer entry. A null view simply leaves the slot at its
  // registered default.
  const auto push = [&](const char* slot, wgpu::TextureView view) {
    if (!view) return;
    spec.textures.push_back(DefaultTextureView{.param_name = slot,
                                               .view = view,
                                               .sampler = textures.sampler,
                                               .type = TextureType::k2D});
  };
  push("albedo", textures.albedo);
  push("normal", textures.normal);
  push("arm", textures.arm);
  // The albedo texture already carries the colour, so the tint is identity.
  spec.uniforms["tint"] = MaterialParameterValue(glm::vec4(1.0f));
  return spec;
}

}  // namespace

InstancedLodModel BuildPropLodModel(const ImportedModel& imported,
                                    const PropMaterialTextures& textures,
                                    const PropLodOptions& options) {
  InstancedLodModel out;
  out.submesh_materials = {PropMaterial(textures)};
  out.native_to_world_scale = 1.0f;  // the adapter already emitted metres

  const StaticTexturedMeshComponent& src = imported.mesh.mesh;
  if (src.vertex_count == 0 || src.indices.size() < 3) {
    spdlog::warn("BuildPropLodModel('{}'): no geometry", imported.name);
    out.levels.push_back({imported.mesh});
    return out;
  }

  // --- LOD 0: welded, with normals and tangents rebuilt. ---
  //
  // Welding at level 0 too, not only for the coarser levels. Every level then
  // shades identically, so there is no faceted-to-smooth pop at the first
  // switch, and the vertex buffer shrinks by up to 6x on a flat-shaded model.
  const SimplifiedMesh welded =
      WeldMeshByPrefix(src.vertices, kStride, src.indices, kWeldPrefix);
  TexturedMeshResult lod0 =
      FinalizeLevel(welded.vertices, welded.indices, welded.vertex_count);

  const Aabb& bounds = lod0.local_bounds;
  // The bounding-sphere DIAMETER, which is what the ladder wants -- a wide flat
  // rock must not be underestimated the way a tree's height happens to work.
  const float size_m = glm::length(bounds.max - bounds.min);
  const size_t source_tris = lod0.mesh.indices.size() / 3;
  const LodLadder ladder =
      BuildLodLadder(size_m, source_tris, options.ladder);

  out.target_height_m = size_m;
  out.thresholds = ladder.thresholds;
  // Each level is a one-submesh vector -- levels is [lod][submesh].
  out.levels.push_back({std::move(lod0)});

  // --- The coarser levels, each decimated from LOD 0. ---
  //
  // From LOD 0 every time rather than from the previous level: error compounds
  // down a chain of successive decimations, and each level's own budget is an
  // absolute target rather than a relative step.
  const std::vector<float>& base_vertices = out.levels[0][0].mesh.vertices;
  const std::vector<uint32_t>& base_indices = out.levels[0][0].mesh.indices;

  for (size_t i = 0; i < ladder.triangle_budgets.size(); ++i) {
    const float ratio = static_cast<float>(ladder.triangle_budgets[i]) /
                        static_cast<float>(source_tris);
    // Try error-bounded edge collapse first: it preserves the silhouette and
    // keeps every surviving vertex's exact attributes. Fall back to vertex
    // clustering only when collapse MISSES -- it cannot merge disconnected
    // components, so a model that is several separate pieces floors well above
    // its target no matter how low the ratio goes.
    //
    // Measured rather than assumed per level, because which one wins is a
    // property of the individual model: boulder_01 collapses to its target
    // exactly, while the mace and the chest floor around 4x it. Clustering has
    // no attribute fidelity, so using it where collapse would have done is a
    // visible UV smear bought for nothing.
    SimplifiedMesh level =
        SimplifyMesh(base_vertices, kStride, base_indices, ratio);
    const size_t budget = static_cast<size_t>(ladder.triangle_budgets[i]);
    const size_t achieved = level.indices.size() / 3;
    if (achieved > static_cast<size_t>(static_cast<float>(budget) *
                                       options.sloppy_fallback_ratio)) {
      spdlog::debug(
          "prop lod '{}': level {} edge collapse floored at {} tris against a "
          "{} budget -- re-cutting with vertex clustering",
          imported.name, i + 1, achieved, budget);
      level = SimplifyMeshSloppy(base_vertices, kStride, base_indices, ratio);
    }
    out.levels.push_back(
        {FinalizeLevel(level.vertices, level.indices, level.vertex_count)});
  }

  // --- The impostor. ---
  out.impostor.roughness = options.impostor_roughness;
  // The ladder's own cutoff, which its stop rule already guaranteed exceeds the
  // last mesh threshold. Taking it from here rather than letting the field
  // builder re-derive one is what keeps props independent of the foliage
  // constants -- see ImpostorBakeSpec::threshold_m.
  out.impostor.threshold_m = ladder.impostor_threshold_m;
  out.impostor.transmission_strength = 0.0f;
  // A prop has no transmitted term, so the thickness pass would render every
  // view and read back an R16Float target to fill a channel the runtime then
  // multiplies by zero.
  out.impostor.opaque = true;
  out.impostor.submeshes.push_back(ImpostorBakeSubmesh{
      .lod = 0,
      .submesh = 0,
      .tint = glm::vec3(1.0f),
      .albedo = textures.albedo,
      .voxel_brightness = 0.0f,
  });

  spdlog::info(
      "prop lod '{}': size {:.2f} m, {} -> {} verts welded, {} levels + "
      "impostor at {:.1f} m",
      imported.name, size_m, src.vertex_count, welded.vertex_count,
      out.levels.size(), ladder.impostor_threshold_m);
  return out;
}

}  // namespace badlands
