#include "game/visual/scene_composer.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include "core/geometry_type.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/scene/scene_attachment.hpp"

namespace badlands {

namespace {

// Visual components (internal: the SceneComposer public API is the interface).
struct WorldTransform {
  glm::mat4 m{1.0f};
};
struct ObjectMesh {
  TexturedMeshResult mesh;
};
struct TerrainChunk {
  TerrainMesh mesh;
};
struct WaterSurface {
  TexturedMeshResult mesh;
};
// Object material proxies: exactly one is attached, chosen by RenderMode.
struct BlockoutMaterial {
  glm::vec3 color;
  float roughness;
};
struct DetailedMaterial {
  std::string pack_dir;
};
// Per-instance water look (mode-appropriate params).
struct WaterInstance {
  InstanceParams params;
};

// De-index a TerrainMesh into a flat kTerrainBlend vertex buffer and attach it
// as a deferred SceneGraph node. ResolvedMesh can carry an index buffer, but
// this path intentionally de-indexes into a flat vertex stream and leaves
// indices empty. GameView rebuilds the render registry from the SceneGraph
// every frame (SyncToRegistry clears it), so the game's terrain must live in
// the SceneGraph -- mapview's direct indexed-registry path can't be reused
// here.
NodeHandle AddTerrainToScene(SceneGraph& scene, const char* name,
                             const TerrainMesh& mesh, const DeferredMaterial& mat,
                             const glm::mat4& transform) {
  constexpr int F = TerrainMesh::kFloatsPerVertex;
  ResolvedMesh resolved;
  resolved.geometry_type = GeometryType::kTerrainBlend;
  resolved.vertices.reserve(mesh.indices.size() * F);
  for (uint32_t idx : mesh.indices) {
    const float* p = &mesh.vertices[static_cast<size_t>(idx) * F];
    resolved.vertices.insert(resolved.vertices.end(), p, p + F);
  }
  resolved.vertex_count = static_cast<uint32_t>(mesh.indices.size());
  resolved.local_bounds = ComputeLocalAabbFromVertices(resolved.vertices, F);

  NodeHandle node = scene.CreateNode(name);
  scene.SetLocalTransform(node, Trs::FromMatrix(transform));
  scene.AddAttachment(node, MeshAttachment{
                                .mesh = std::move(resolved),
                                .factory = mat.factory,
                                .pass_type = MaterialPassType::kDeferred,
                                .params = mat.params,
                            });
  return node;
}

}  // namespace

void SceneComposer::AddTerrain(TerrainMesh&& mesh, const glm::mat4& world) {
  const entt::entity e = registry_.create();
  registry_.emplace<TerrainChunk>(e, TerrainChunk{std::move(mesh)});
  registry_.emplace<WorldTransform>(e, WorldTransform{world});
}

void SceneComposer::AddWater(TexturedMeshResult&& mesh,
                             const InstanceParams& params,
                             const glm::mat4& world) {
  const entt::entity e = registry_.create();
  registry_.emplace<WaterSurface>(e, WaterSurface{std::move(mesh)});
  registry_.emplace<WorldTransform>(e, WorldTransform{world});
  registry_.emplace<WaterInstance>(e, WaterInstance{params});
}

void SceneComposer::AddObject(TexturedMeshResult&& mesh, glm::vec3 blockout_color,
                              float blockout_roughness, std::string pack_dir,
                              const glm::mat4& world) {
  const entt::entity e = registry_.create();
  registry_.emplace<ObjectMesh>(e, ObjectMesh{std::move(mesh)});
  registry_.emplace<WorldTransform>(e, WorldTransform{world});
  if (mode_ == RenderMode::Blockout) {
    registry_.emplace<BlockoutMaterial>(
        e, BlockoutMaterial{blockout_color, blockout_roughness});
  } else {
    registry_.emplace<DetailedMaterial>(e,
                                        DetailedMaterial{std::move(pack_dir)});
  }
}

void SceneComposer::ComposeInto(
    SceneGraph& scene, MaterialLibrary& matlib,
    const MaterialLibrary::TerrainArrays& terrain_arrays,
    MaterialInstanceFactory* water_factory) {
  int n = 0;
  auto name = [&](const char* prefix) {
    return std::string(prefix) + std::to_string(n++);
  };

  // Terrain: one shared material from the mode-appropriate arrays.
  {
    const DeferredMaterial terrain_mat = matlib.TerrainBlend(terrain_arrays);
    auto view = registry_.view<TerrainChunk, WorldTransform>();
    for (auto e : view) {
      AddTerrainToScene(scene, name("terrain_").c_str(),
                        view.get<TerrainChunk>(e).mesh, terrain_mat,
                        view.get<WorldTransform>(e).m);
    }
  }

  // Objects -- blockout (solid color) proxy.
  {
    auto view = registry_.view<ObjectMesh, WorldTransform, BlockoutMaterial>();
    for (auto e : view) {
      const auto& bm = view.get<BlockoutMaterial>(e);
      AddMeshEntity(scene, name("object_").c_str(),
                    std::move(view.get<ObjectMesh>(e).mesh),
                    matlib.SolidColor(bm.color, bm.roughness),
                    view.get<WorldTransform>(e).m);
    }
  }
  // Objects -- detailed (PBR pack) proxy.
  {
    auto view = registry_.view<ObjectMesh, WorldTransform, DetailedMaterial>();
    for (auto e : view) {
      AddMeshEntity(scene, name("object_").c_str(),
                    std::move(view.get<ObjectMesh>(e).mesh),
                    matlib.Get(view.get<DetailedMaterial>(e).pack_dir),
                    view.get<WorldTransform>(e).m);
    }
  }

  // Water surfaces (forward-transparent).
  {
    auto view = registry_.view<WaterSurface, WorldTransform, WaterInstance>();
    for (auto e : view) {
      AddTransparentMeshEntity(scene, name("water_").c_str(),
                               std::move(view.get<WaterSurface>(e).mesh),
                               water_factory, view.get<WaterInstance>(e).params,
                               view.get<WorldTransform>(e).m);
    }
  }
}

}  // namespace badlands
