#include "executables/viewer/model_viewer_view.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>  // glm::translate, glm::rotate, glm::scale
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/app/sdl_input_util.hpp"  // NormalizedWheelY
#include "engine/assets/usd_loader.hpp"
#include "engine/rendering/geometry/usd_mesh_adapter.hpp"
#include "engine/rendering/gpu_instance_renderer.hpp"  // GpuInstanceRenderer::InstanceInput
#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/geometry/leaf_voxelizer.hpp"
#include "game/geometry/mesh_lod.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/geometry/tree_options.hpp"
#include "game/visual/foliage_voxel_config.hpp"

namespace badlands {

namespace {

// Flat mid-gray debug floor. Kept dark enough that the sun + sky ambient don't
// clip it to pure white (which washed out thin bark tubes) -- a mid-gray floor
// silhouettes the generated mesh with contrast. Roughness maxed to keep it
// diffuse so shadows read clearly.
constexpr glm::vec3 kFloorGray{0.5f};
constexpr float kFloorRoughness = 1.0f;
constexpr float kFloorSize = 40.0f;
// One floor-UV repeat per ~2 world units instead of stretching one copy.
constexpr float kFloorUvRepeatSpacing = 2.0f;
// Preview height the tree generators are display-scaled to -- see
// foliage_voxel_config.hpp's kFoliagePreviewHeight; aliased under this file's
// existing local name (used throughout this file, both the single-tree and
// Multi-mode paths) rather than a blanket rename.
constexpr float kTreePreviewHeight = kFoliagePreviewHeight;

// lod_level_ layout (ModelViewerView::kVoxelLodCount / kMultiLodLevel, in the
// header so SetInitialLod's clamp and main_viewer's --lod parse share it):
// 0 = "Original" (leaf cards), 1..kVoxelLodCount = "Voxel L0..L3" (one per
// kFoliageVoxelWorldSizes entry), kMultiLodLevel = "Multi".
constexpr int kVoxelLodCount = ModelViewerView::kVoxelLodCount;
constexpr int kMultiLodLevel = ModelViewerView::kMultiLodLevel;

// Multi mode: a grid of one instanced tree model with dynamic GPU LOD
// (distance-based, chosen live by InstancedMeshField::Cull -- NOT the manual
// per-level switch the single-tree Voxel L0..L3 paths use below). The tree
// model declares kVoxelLodCount runtime LODs (one per kFoliageVoxelWorldSizes
// entry), which is what a model gets to choose within the engine's
// GpuInstanceRenderer::kMaxLods compile-time cap -- so Multi mode and the
// single-tree levels now walk the SAME chain, L0..L3.
constexpr int kGridN = 16;
constexpr float kGridSpacing = 8.0f;
// Golden angle: a constant per-instance yaw increment that avoids any
// repeating row/column alignment across the grid.
const float kYawIncrement = glm::radians(137.508f);
// Larger than the single-tree kFloorSize -- the 16x16 grid at kGridSpacing
// spans 120 world units; 160 gives it a visible margin.
constexpr float kMultiFloorSize = 160.0f;
// GPU LOD thresholds, one cutoff between each adjacent pair of the tree
// model's kVoxelLodCount levels, retuned for volumetric-foliage Phase 5's
// voxel-crown fields (the pre-Phase-5 {95, 135} values were tuned for the old
// billboard-card leaf field). Derivation: solve kFoliageVoxelTargetPx's own
// world_size = target_px * distance / focal_px formula (see that constant's
// comment below) for distance, using each LOD's own kFoliageVoxelWorldSizes
// entry as `world_size` -- distance = world_size * focal_px / target_px =
// world_size * 935 / 8. LOD0->LOD1 (world_size = 0.15m): ~17.5m, rounded to
// 18. LOD1->LOD2 (0.20m, Phase 6-retuned -- see that constant's comment):
// ~23.4m, rounded to 23. LOD2->LOD3 (0.60m): ~70.1m, rounded to 70. The first
// two were screenshot-tuned in Phase 6 (both the empty-crown fix and their
// re-derivation off it); the last is the formula's value as-is.
// (The distance cutoffs themselves now live in foliage_voxel_config.hpp as
// kFoliageLodThresholdsPreviewM, since the instanced-field path scales them per
// model -- BuildTreeFieldModel applies them.)

// Voxel-crown LOD (volumetric-foliage Phase 3): progressively coarser
// tet-voxelization cell sizes, one per Voxel L0..L3 mode
// (kFoliageVoxelWorldSizes[lod_level_ - 1] -- see foliage_voxel_config.hpp,
// included above, for the constant itself + its Phase 6 retune derivation),
// given in WORLD (preview) space -- RebuildScene converts to the tree's own
// native units by dividing by `s` (the same kTreePreviewHeight rescale bark/
// leaves already go through) before passing it to
// LeafVoxelizeOptions::cell_size. kFoliageVoxelTargetPx itself is not read by
// any distance-based selection (Voxel L0..L3 is still a manual mode switch)
// -- it documents the screen-space budget
// kFoliageVoxelWorldSizes was derived from: world_size = target_px *
// distance / focal_px, i.e. a world-space threshold distance = size *
// focal_px / target_px. At 1920x1080/60deg vertical fovy, focal_px =
// (height/2) / tan(fovy/2) = 540 / tan(30deg) ~= 935. kMultiLodThresholds
// above (Phase 5) DOES derive its distance-based GPU LOD cutoffs from this
// same formula, applied to Multi mode's own per-LOD voxel sizes.
constexpr float kFoliageVoxelTargetPx = 8.0f;

// lod_level_ of the IMPOSTOR. It takes the slot the coarsest voxel level used to
// hold, so the numbering and the `--lod` flag are unchanged: 0 = Original,
// 1..kVoxelLodCount-1 = Voxel L0..L2, kVoxelLodCount = Impostor.
constexpr int kImpostorLodLevel = kVoxelLodCount;

// The impostor's unit quad, in the standard textured-mesh layout. Position is
// unused -- foliage_impostor.wesl builds the quad from the instance transform --
// so only uv carries information, and it IS the local uv into the baked tile.
TexturedMeshResult MakeImpostorQuad() {
  TexturedMeshResult out;
  out.mesh.geometry_type = GeometryType::kTexturedMesh;
  out.mesh.vertices = {
      0, 0, 0,  0, 0,  0, 0, 1,  1, 0, 0,
      0, 0, 0,  1, 0,  0, 0, 1,  1, 0, 0,
      0, 0, 0,  0, 1,  0, 0, 1,  1, 0, 0,
      0, 0, 0,  1, 1,  0, 0, 1,  1, 0, 0,
  };
  out.mesh.vertex_count = 4;
  out.mesh.indices = {0, 1, 2, 2, 1, 3};
  out.mesh.dirty = true;
  // Bounds are only used for entity culling; the real extent is the placement
  // radius the material expands the quad to.
  out.local_bounds = Aabb{glm::vec3(-1.0f), glm::vec3(1.0f)};
  return out;
}

// Every model directory under assets/models/ that holds a .usdc, sorted by
// name. Sorted because `--generator <n>` indexes this list, so a
// directory-iteration order that varies by filesystem would silently change
// which model a headless screenshot captures.
std::vector<std::filesystem::path> DiscoverPropModels() {
  std::vector<std::filesystem::path> out;
  const std::filesystem::path root{"assets/models"};
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) return out;

  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (!entry.is_directory()) continue;
    for (const auto& file : std::filesystem::directory_iterator(entry.path(), ec)) {
      if (file.path().extension() == ".usdc") {
        out.push_back(file.path());
        break;  // one model per directory
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Concatenates every mesh of an imported prop into one.
//
// A prop is shown as a whole object, but three of the shipped ones are several
// prims (treasure_chest is 5, rock_moss_set_01 is 6). Since every prim of a
// given model resolves to the SAME material pack, one draw is both correct and
// what a viewer entry should be -- splitting a chest into five separately
// selectable pieces would be worse, not more faithful.
TexturedMeshResult MergeImportedMeshes(std::vector<ImportedModel>& models) {
  TexturedMeshResult out;
  out.mesh.geometry_type = GeometryType::kTexturedMesh;

  for (auto& model : models) {
    const auto& src = model.mesh.mesh;
    const uint32_t base = out.mesh.vertex_count;
    out.mesh.vertices.insert(out.mesh.vertices.end(), src.vertices.begin(),
                             src.vertices.end());
    for (uint32_t index : src.indices) out.mesh.indices.push_back(base + index);
    out.mesh.vertex_count += src.vertex_count;
  }
  out.mesh.dirty = true;
  out.local_bounds = ComputeLocalAabb(out.mesh);
  return out;
}

}  // namespace

bool ModelViewerView::ImpostorPreviewIsCurrent() const {
  return impostor_preview_.ok && impostor_preview_generator_ == generator_index_;
}

bool ModelViewerView::EnsureImpostorPreview(
    std::span<const TreeFieldModel> models) {
  if (ImpostorPreviewIsCurrent()) {
    return true;
  }
  impostor_preview_ = BakeImpostorAtlas(device_, queue_, *pipeline_gen_, models);
  impostor_preview_generator_ = generator_index_;
  return impostor_preview_.ok;
}

bool ModelViewerView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;
  pipeline_gen_ = ctx.pipeline_gen;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("ModelViewerView::Initialize: MaterialLibrary init failed");
    return false;
  }

  // UV-checker debug material (two distinct grays) for the sphere test object, so
  // its UVs read against the flat gray floor.
  checker_mat_ = matlib_.CheckerAlbedo(glm::vec3(0.85f), glm::vec3(0.35f));
  // Solid dark-brown bark color for the catalog tree meshes.
  bark_mat_ = matlib_.SolidColor(glm::vec3(0.30f, 0.19f, 0.10f), 0.9f);

  // Per-silhouette leaf-card textures, built once and shared by every tree of
  // that silhouette. White RGB (so the AlphaCutout/foliage material's
  // per-tree tint colours it), alpha = leaf shape. Each is a CPU-computed,
  // coverage-preserving mip chain (BuildLeafMipChainRgba8) uploaded
  // level-by-level via WriteTexture -- NOT UploadTexture2DWithMips, whose
  // GPU box-downsample mipgen would recompute (and so destroy) the
  // coverage-preserved alpha this function already produced per level.
  {
    // Trilinear + clamp sampler, shared by every silhouette: the alpha mip
    // chain must be sampled through a Linear mipmapFilter (the material
    // factory's default is Nearest, which would defeat the mips and leave the
    // edges aliased). Repeat was harmless for the old zero-border oval (its
    // edge texels were all transparent), but sprig layouts bake alpha-255
    // texels right up to the card's top/bottom edge rows -- Repeat would wrap
    // those into the opposite edge under bilinear/trilinear filtering,
    // producing detached leaf slivers at card tops/bottoms. Leaf quad UVs are
    // strictly 0..1, so ClampToEdge is correct here.
    wgpu::SamplerDescriptor samp = {};
    samp.minFilter = wgpu::FilterMode::Linear;
    samp.magFilter = wgpu::FilterMode::Linear;
    samp.mipmapFilter = wgpu::MipmapFilterMode::Linear;
    samp.addressModeU = wgpu::AddressMode::ClampToEdge;
    samp.addressModeV = wgpu::AddressMode::ClampToEdge;
    samp.maxAnisotropy = 16;
    leaf_sampler_ = device_.CreateSampler(&samp);

    constexpr uint32_t kLeafTexSize = 512;

    for (LeafSilhouette shape : kAllLeafSilhouettes) {
      // Shared with TreeCatalog's per-silhouette alpha_cutoff assignments
      // (tree_generator.cpp) -- see LeafSilhouetteBakeCutoff's own comment.
      const float cutoff = LeafSilhouetteBakeCutoff(shape);
      const std::vector<std::vector<uint8_t>> mips =
          BuildLeafMipChainRgba8(static_cast<int>(kLeafTexSize),
                                 glm::vec3(1.0f), shape, cutoff);

      wgpu::TextureDescriptor tex_desc;
      tex_desc.size = {kLeafTexSize, kLeafTexSize, 1};
      tex_desc.format = wgpu::TextureFormat::RGBA8Unorm;
      tex_desc.usage =
          wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
      tex_desc.mipLevelCount = static_cast<uint32_t>(mips.size());
      tex_desc.sampleCount = 1;
      tex_desc.dimension = wgpu::TextureDimension::e2D;
      wgpu::Texture texture = device_.CreateTexture(&tex_desc);

      const size_t idx = static_cast<size_t>(shape);
      if (!texture) {
        spdlog::error(
            "ModelViewerView::Initialize: leaf texture creation failed "
            "(silhouette {})", idx);
        return false;
      }

      // Level-by-level upload, mirroring texture_loader.cpp's level-0 upload
      // (WriteTexture has no 256-byte row-alignment requirement, unlike
      // buffer<->texture copies, so the tightly-packed per-level buffers
      // upload directly).
      uint32_t w = kLeafTexSize, h = kLeafTexSize;
      for (uint32_t level = 0; level < mips.size(); ++level) {
        wgpu::TexelCopyTextureInfo dst;
        dst.texture = texture;
        dst.mipLevel = level;
        dst.origin = {0, 0, 0};

        wgpu::TexelCopyBufferLayout layout;
        layout.bytesPerRow = w * 4;
        layout.rowsPerImage = h;

        wgpu::Extent3D extent = {w, h, 1};
        queue_.WriteTexture(&dst, mips[level].data(), mips[level].size(),
                           &layout, &extent);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
      }

      leaf_textures_[idx] = texture;
      leaf_views_[idx] = texture.CreateView();
      if (!leaf_views_[idx]) {
        spdlog::error(
            "ModelViewerView::Initialize: leaf texture view failed "
            "(silhouette {})", idx);
        return false;
      }
    }
  }

  BuildGenerators();
  if (generators_.empty()) {
    spdlog::error("ModelViewerView::Initialize: empty generator registry");
    return false;
  }
  generator_index_ =
      std::clamp(generator_index_, 0, static_cast<int>(generators_.size()) - 1);

  // No volumetric fog in the model viewer -- it renders soft media blobs behind
  // the mesh that only make sense in the game world.
  scene_renderer_->MutableFogConfig().enabled = false;

  // The default sun+sky (intensity 3.0 / 1.0) overexposes the scene and washes
  // out thin bark tubes. Dial both back so the floor lands mid-gray and the
  // generated mesh reads with contrast.
  env_.sun_intensity = 2.0f;
  env_.sky_intensity = 0.5f;

  ApplyEnvironment();
  RebuildScene();
  scene_renderer_->SetShadowDebugMode(initial_shadow_debug_mode_);

  if (!matlib_.ok()) {
    spdlog::error("ModelViewerView::Initialize: material load failed");
    return false;
  }
  return true;
}

void ModelViewerView::BuildGenerators() {
  generators_.clear();
  // The "test" generator: the engine's cube-sphere (cube -> 16x16 per face ->
  // normalized sphere, EAC UVs). Future foliage/rock generators append here.
  generators_.push_back(
      {.name = "Sphere (test)", .generate = [] {
         TexturedMeshResult mesh = GenerateSphereTexturedMesh(1.0f, 16);
         // Floor is at y=0: lift the mesh so its lowest point rests on it. The
         // offset is a transform, never baked into the vertices.
         const glm::mat4 transform = glm::translate(
             glm::mat4(1.0f), glm::vec3(0.0f, -mesh.local_bounds.min.y, 0.0f));
         return GeneratedMesh{std::move(mesh), transform};
       }, .material = checker_mat_});
  // One entry per predefined tree setup (the full ez-tree preset catalog).
  // Trees go through the two-material path in RebuildScene (deferred bark +
  // forward-opaque alpha-cutout leaves), so they carry `tree` options rather
  // than a single-mesh `generate` lambda.
  for (const NamedTreeOptions& setup : TreeCatalog()) {
    generators_.push_back({.name = setup.name, .tree = setup.options});
  }
  // One entry per imported prop. The material is resolved eagerly (MaterialLibrary
  // caches per pack directory), but the .usdc is parsed lazily inside `generate`
  // -- these files run to 7 MB and 100k triangles, so parsing all ten at startup
  // to show one would be most of a second wasted per launch.
  for (const std::filesystem::path& usdc : DiscoverPropModels()) {
    const std::string pack_dir = usdc.parent_path().string();
    generators_.push_back(
        {.name = usdc.parent_path().filename().string(),
         .generate = [usdc, pack_dir] {
           UsdSceneData scene = LoadUsdScene(usdc.string());
           UsdMaterialBinding binding;
           // Every shipped prop is single-material, so the default answers for
           // all of its prims; by_material stays empty until one is not.
           binding.default_pack_dir = pack_dir;

           std::vector<ImportedModel> models = BuildImportedModels(scene, binding);
           if (models.empty()) {
             spdlog::error("ModelViewerView: '{}' imported no meshes",
                           usdc.string());
             return GeneratedMesh{};
           }
           TexturedMeshResult mesh = MergeImportedMeshes(models);
           // Same contract as the sphere: rest the mesh on the y=0 floor via a
           // transform, never by baking the offset into the vertices.
           const glm::mat4 transform = glm::translate(
               glm::mat4(1.0f), glm::vec3(0.0f, -mesh.local_bounds.min.y, 0.0f));
           return GeneratedMesh{std::move(mesh), transform};
         },
         .material = matlib_.Get(pack_dir)});
  }
}

void ModelViewerView::ApplyEnvironment() {
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
}

void ModelViewerView::RebuildScene() {
  // Fresh graph drops every prior entity; its ctor resets sun/ambient to
  // SceneGraph defaults, so re-mirror scene_context_'s derived lighting.
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  // Every rebuild starts with no instanced field; the Multi branch below
  // repopulates it. Clearing unconditionally (rather than only in the
  // non-Multi branches) also releases the previous Multi-mode field's GPU
  // resources the moment a rebuild switches away from it (generator change
  // or Multi -> any single-tree level). field_ptr_ is also nulled for
  // hygiene/safety.
  scene_context_.instanced_field_count = 0;
  tree_field_.reset();
  field_ptr_ = nullptr;

  const MeshGenerator& gen = generators_[generator_index_];
  const bool multi = gen.tree.has_value() && lod_level_ == kMultiLodLevel;

  const float floor_size = multi ? kMultiFloorSize : kFloorSize;
  AddFloor(scene_, floor_size, matlib_.SolidColor(kFloorGray, kFloorRoughness),
           floor_size / kFloorUvRepeatSpacing);

  // Frame on the WORLD-space bounds so the orbit centers on the object as it sits
  // on the floor.
  Aabb world_bounds = Aabb::Empty();
  if (multi) {
    // Multi mode: a kGridN x kGridN instanced grid of the selected tree,
    // GPU-culled with dynamic distance LOD (InstancedMeshField::Cull) --
    // NOT the manual kDefaultLodRatios switch the single-tree Voxel L0/L1/L2
    // (lod_level_ 1/2/3) paths use below. Skips the single-tree bark/leaf
    // entities entirely.
    const uint32_t capacity = static_cast<uint32_t>(kGridN * kGridN);

    // One model, prepared exactly the way a forest prepares its 28 (see
    // game/visual/tree_field.hpp): BuildTreeFieldModel does the skeleton, the
    // LOD0 bark, and the per-LOD voxelization at cell sizes retargeted to the
    // requested display height. At kTreePreviewHeight that retargeting is the
    // identity, so the meshes here are byte-identical to what this call site
    // built by hand before.
    const std::array<TreeFieldModel, 1> field_models{
        BuildTreeFieldModel(*gen.tree, kTreePreviewHeight)};
    const float s = field_models[0].native_to_world_scale;

    // Multi walks the SAME chain the game's forest does, impostor included --
    // otherwise the mode that exists to show dynamic LOD would stop one level
    // short of what the game actually draws at distance, and the retired L3
    // would still be its coarsest level.
    TreeFieldImpostor impostor_slot;
    if (EnsureImpostorPreview(field_models)) {
      impostor_slot.atlas = &impostor_preview_.atlas;
      impostor_slot.placement = impostor_preview_.placement;
    }

    std::unique_ptr<TreeField> field =
        BuildTreeField(device_, queue_, *pipeline_gen_, field_models, capacity,
                       impostor_slot);
    if (!field) {
      // Mirrors the malformed-generator branch further down: log and bail
      // with a floor-only scene, leaving the orbit framing unchanged (an
      // empty world_bounds here would otherwise frame on a degenerate
      // Aabb::Empty()).
      spdlog::error(
          "ModelViewerView::RebuildScene: BuildTreeField failed; Multi mode "
          "shows floor only");
      return;
    }

    // Same scale + rest-on-floor transform the single-tree path derives
    // from bark.local_bounds (the `else` branch below), so a Multi-mode
    // grid cell at the origin matches the single-tree preview exactly. `s`
    // came from BuildTreeFieldModel above rather than being re-derived here.
    const TreeModelBounds& bounds = field->model_bounds[0];
    const glm::mat4 xf =
        glm::translate(glm::mat4(1.0f),
                      glm::vec3(0.0f, -bounds.bark_local_bounds.min.y * s,
                                0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(s));

    const Aabb combined_local_bounds = bounds.Combined();

    std::vector<GpuInstanceRenderer::InstanceInput> instances;
    instances.reserve(capacity);
    const float half_extent = (kGridN - 1) * kGridSpacing * 0.5f;
    for (int gz = 0; gz < kGridN; ++gz) {
      for (int gx = 0; gx < kGridN; ++gx) {
        const int i = gz * kGridN + gx;
        const glm::vec3 world_xz(gx * kGridSpacing - half_extent, 0.0f,
                                 gz * kGridSpacing - half_extent);
        const glm::mat4 transform =
            glm::translate(glm::mat4(1.0f), world_xz) *
            glm::rotate(glm::mat4(1.0f), i * kYawIncrement,
                       glm::vec3(0.0f, 1.0f, 0.0f)) *
            xf;

        const Aabb instance_bounds =
            combined_local_bounds.TransformedBy(transform);
        world_bounds = world_bounds.Union(instance_bounds);
        const glm::vec3 instance_center = instance_bounds.Center();
        const float instance_radius =
            glm::length(instance_bounds.max - instance_center);

        GpuInstanceRenderer::InstanceInput input;
        input.transform = transform;
        input.bounds_sphere = glm::vec4(instance_center, instance_radius);
        input.model_info = glm::uvec4(0u);
        instances.push_back(input);
      }
    }
    field->field->UploadInstances(instances);

    tree_field_ = std::move(field);
    field_ptr_ = tree_field_->field.get();
    scene_context_.instanced_fields = &field_ptr_;
    scene_context_.instanced_field_count = 1;
  } else if (gen.tree) {
    // Single tree, one of two leaf representations sharing the tree's local
    // space (and therefore one preview transform, so the leaves/crown stay
    // attached to the branches):
    //   - lod_level_ == 0 ("Original"): deferred solid bark + forward-opaque
    //     alpha-cutout leaf cards -- the pre-Phase-3 path, untouched.
    //   - lod_level_ == 1..kVoxelLodCount ("Voxel L0..L3"): deferred solid
    //     bark (simplified per SimplifyBarkForVoxelLod) + a deferred
    //     VoxelFoliage tet crown (volumetric-foliage Phase 3) in place of the
    //     leaf cards.
    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(*gen.tree);
    BarkMeshStats bark_stats;
    TexturedMeshResult bark = GenerateTreeMesh(*gen.tree, skeleton, &bark_stats);
    // How much of this tree's bark merged into one mesh component. `fallback`
    // counts branches still standing as independent buried tubes, which is the
    // pre-graft behaviour and what limits how far the bark LODs can decimate.
    spdlog::info("bark graft [{}]: {} junctions, {} stitched ({} shrunk), {} fell back",
                 gen.name, bark_stats.junctions, bark_stats.stitched,
                 bark_stats.shrunk, bark_stats.fallback);
    const float h = bark.local_bounds.max.y - bark.local_bounds.min.y;
    const float s = kTreePreviewHeight / std::max(h, 0.001f);
    const glm::mat4 xf =
        glm::translate(glm::mat4(1.0f),
                       glm::vec3(0.0f, -bark.local_bounds.min.y * s, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(s));

    world_bounds = bark.local_bounds.TransformedBy(xf);

    // Manual LOD switch: simplify the bark mesh in place before it's moved
    // into the scene. local_bounds is left as-is (simplification stays
    // within the mesh extent, so orbit framing above is unaffected). Voxel
    // L0..L3 (lod_level_ 1..kVoxelLodCount) map onto voxel level
    // lod_level_ - 1 (Voxel L0 = ratio 1.0, i.e. full bark, same as the old
    // lod-1 mapping shifted by one now that index 0 means "Original" instead
    // of "full detail"); Original (0) stays untouched via the guard below.
    if (lod_level_ > 0) {
      SimplifyBarkForVoxelLod(bark.mesh,
                              static_cast<size_t>(lod_level_ - 1));
    }
    // The impostor level draws NO separate bark: its atlas already contains
    // the bark, so adding the mesh too would show the real trunk in front of a
    // billboard that also depicts one.
    if (lod_level_ != kImpostorLodLevel) {
      bark_tris_ = static_cast<int>(bark.mesh.indices.size() / 3);
      AddMeshEntity(scene_, "bark", std::move(bark), bark_mat_, xf);
    }

    if (lod_level_ == 0) {
      // Original: the pre-Phase-3 card-leaf path, byte-for-byte unchanged.
      TexturedMeshResult leaves = GenerateLeafMesh(*gen.tree, skeleton);
      if (leaves.mesh.vertex_count > 0) {
        world_bounds = world_bounds.Union(leaves.local_bounds.TransformedBy(xf));
      }
      if (leaves.mesh.vertex_count > 0 && kLeafLodRatios[lod_level_] < 1.0f) {
        SimplifiedMesh ls = SimplifyMesh(leaves.mesh.vertices,
                                        kTexturedMeshFloatsPerVertex,
                                        leaves.mesh.indices,
                                        kLeafLodRatios[lod_level_]);
        leaves.mesh.vertices = std::move(ls.vertices);
        leaves.mesh.indices = std::move(ls.indices);
        leaves.mesh.vertex_count = ls.vertex_count;
        leaves.mesh.dirty = true;
      }
      leaf_tris_ = static_cast<int>(leaves.mesh.indices.size() / 3);

      if (leaves.mesh.vertex_count > 0) {
        // DEFERRED, not forward-opaque. The cards used to go through
        // MaterialLibrary::TranslucentFoliage, which left the chain's finest
        // level lit by a different path than L0-L4 -- no GTAO, no contact
        // shadows, and a second lighting model to keep in step. A card is
        // opaque wherever it is opaque; all it needs beyond voxel_foliage is
        // an alpha test.
        if (!leaf_cutout_factory_) {
          FactoryDescriptor desc;
          desc.shader_name = "foliage_cutout";
          desc.shader_path = "game/foliage_cutout";
          desc.supported_pass_types = {MaterialPassType::kDeferred};
          desc.supported_geometry_types = {GeometryType::kTexturedMesh};
          desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                                GBuffer::kMaterialFormat};
          desc.depth_format = GBuffer::kDepthFormat;
          desc.cull_mode = wgpu::CullMode::None;  // double-sided cards
          desc.casts_shadow = true;
          leaf_cutout_factory_ = BuildMaterialInstanceFactory(
              desc, device_, queue_, pipeline_gen_);
        }
        if (leaf_cutout_factory_) {
          InstanceParams params;
          // Reflection-derived slot name for the group-0 texture at binding 1
          // (see tree_field.hpp's deviation note).
          params.texture_overrides.push_back(DefaultTextureView{
              .param_name = "tex_1",
              .view = LeafViewFor(gen.tree->leaves.silhouette),
              .sampler = leaf_sampler_,
              .type = TextureType::k2D,
          });
          params.uniform_overrides["tint"] = MaterialParameterValue(
              glm::vec4(gen.tree->leaves.tint, 1.0f));
          params.uniform_overrides["params"] = MaterialParameterValue(glm::vec4(
              0.9f, gen.tree->leaves.transmission_strength,
              gen.tree->leaves.alpha_cutoff, 1.0f));
          DeferredMaterial lm;
          lm.factory = leaf_cutout_factory_.get();
          lm.params = std::move(params);
          AddMeshEntity(scene_, "leaves", std::move(leaves), lm, xf);
        }
      }
    } else if (lod_level_ == kImpostorLodLevel) {
      // The IMPOSTOR occupies the slot voxel L3 used to hold. L3's tets
      // overscale, so a coarse voxel crown reads as a bigger tree than the one
      // it replaces -- the same defect that killed the earlier attempt at a
      // voxel L4. The impostor is a picture of the tree instead, so it cannot
      // have that error, and it costs two triangles. The field's LOD chain made
      // the same swap (see kFoliageImpostorThresholdPreviewM); this keeps the
      // viewer showing what the game actually draws.
      // Check the cache BEFORE building: BuildTreeFieldModel is a skeleton plus
      // four voxelizations, the dominant cost of a rebuild, and RebuildScene
      // runs on every LOD radio change -- so toggling in and out of this level
      // would otherwise pay for a model the bake then discards.
      bool have_impostor = ImpostorPreviewIsCurrent();
      if (!have_impostor) {
        const std::array<TreeFieldModel, 1> one = {
            BuildTreeFieldModel(*gen.tree, kTreePreviewHeight)};
        have_impostor = EnsureImpostorPreview(one);
      }
      if (have_impostor) {
        if (!impostor_preview_factory_) {
          FactoryDescriptor desc;
          desc.shader_name = "foliage_impostor";
          desc.shader_path = "game/foliage_impostor";
          desc.supported_pass_types = {MaterialPassType::kDeferred};
          desc.supported_geometry_types = {GeometryType::kTexturedMesh};
          desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                                GBuffer::kMaterialFormat};
          desc.depth_format = GBuffer::kDepthFormat;
          desc.cull_mode = wgpu::CullMode::None;
          desc.casts_shadow = true;
          impostor_preview_factory_ = BuildMaterialInstanceFactory(
              desc, device_, queue_, pipeline_gen_);
        }
        if (impostor_preview_factory_) {
          InstanceParams params;
          if (BindImpostorAtlas(impostor_preview_.atlas, params)) {
            const ImpostorPlacement& place = impostor_preview_.placement[0];
            params.uniform_overrides["placement"] = MaterialParameterValue(
                glm::vec4(place.local_center, place.radius));
            // params.w = the atlas layer; the preview bakes one model, so 0.
            params.uniform_overrides["params"] = MaterialParameterValue(
                glm::vec4(0.9f, gen.tree->leaves.transmission_strength,
                          kImpostorAlphaCutoff, 0.0f));

            DeferredMaterial im;
            im.factory = impostor_preview_factory_.get();
            im.params = std::move(params);
            AddMeshEntity(scene_, "impostor", MakeImpostorQuad(), im, xf);
          }
        }
      }
      // The quad is two triangles and carries no bark of its own -- the bake
      // included it, so the separate bark entity is skipped for this level.
      bark_tris_ = 0;
      leaf_tris_ = 2;
    } else {
      // Voxel L0..L2: tet-voxelize the leaf cards instead of simplifying
      // them. cell_size is world_size / s -- kFoliageVoxelWorldSizes is in
      // preview (post-rescale) world units, LeafVoxelizeOptions::cell_size
      // wants the tree's own native units, and `s` is the same
      // kTreePreviewHeight rescale bark/xf above already apply.
      TexturedMeshResult leaves = GenerateLeafMesh(*gen.tree, skeleton);
      LeafVoxelizeOptions voxel_opts;
      const size_t voxel_level = static_cast<size_t>(lod_level_ - 1);
      voxel_opts.cell_size = kFoliageVoxelWorldSizes[voxel_level] / s;
      // Coarse levels shrink the jitter FRACTION so the absolute displacement
      // stays L0's -- otherwise L3's few oversized tets scatter instead of
      // massing into a silhouette. Same call the instanced-field path makes.
      voxel_opts.position_jitter =
          FoliagePositionJitterForLod(voxel_level, voxel_opts.position_jitter);
      TexturedMeshResult voxels = VoxelizeLeafCards(
          leaves.mesh, gen.tree->leaves.silhouette, voxel_opts);
      leaf_tris_ = static_cast<int>(voxels.mesh.indices.size() / 3);

      // Mirrors the Original branch's vertex_count guard above -- a tree
      // preset with leaves disabled (or too sparse to clear
      // occupancy_fraction at this cell size) voxelizes to an empty mesh.
      // Guard BOTH the entity add and the bounds union: an empty
      // TexturedMeshResult's local_bounds is Aabb::Empty() (min=+FLT_MAX,
      // max=-FLT_MAX sentinel corners), and TransformedBy would smear those
      // sentinels into world_bounds, corrupting orbit framing.
      if (voxels.mesh.vertex_count > 0) {
        // Voxel shell exceeds the source cards' AABB (tets overscale past
        // their cell), so union rather than reuse the card bounds.
        world_bounds = world_bounds.Union(voxels.local_bounds.TransformedBy(xf));
        AddMeshEntity(
            scene_, "leaves", std::move(voxels),
            matlib_.VoxelFoliage(gen.tree->leaves.tint, 0.9f,
                                 gen.tree->leaves.transmission_strength),
            xf);
      }
    }
  } else if (gen.generate) {
    GeneratedMesh generated = gen.generate();
    world_bounds = generated.mesh.local_bounds.TransformedBy(generated.transform);
    AddMeshEntity(scene_, "mesh", std::move(generated.mesh), gen.material,
                  generated.transform);
  } else {
    // A MeshGenerator must set exactly one of `tree`/`generate`. Guard the
    // invariant so a malformed entry logs instead of throwing / rendering empty.
    spdlog::error("ModelViewerView::RebuildScene: generator '{}' has no mesh "
                  "generator; showing floor only",
                  gen.name);
    return;  // floor-only scene; leave the orbit framing unchanged
  }

  const glm::vec3 center = world_bounds.Center();
  const float radius = glm::length(world_bounds.max - center);
  orbit_.FrameBounds(center, radius > 0.01f ? radius : 1.0f);
  orbit_.UpdateCamera(camera_);
}

void ModelViewerView::HandleEvent(const SDL_Event& event, int /*width*/,
                                  int /*height*/) {
  if (ImGui::GetIO().WantCaptureMouse) return;

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = true;
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = false;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (left_mouse_down_) {
        orbit_.HandleMouseDrag(event.motion.xrel, event.motion.yrel);
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      orbit_.HandleMouseWheel(NormalizedWheelY(event.wheel));
      break;
    default:
      break;
  }
}

void ModelViewerView::Update(float dt, const bool* /*keyboard_state*/) {
  dt_ = dt;
  orbit_.UpdateCamera(camera_);
  scene_.SyncToRegistry(registry_, scene_context_);
}

void ModelViewerView::DrawUI() {
  if (!scene_renderer_ || generators_.empty()) return;

  // Mesh-setup window: single-select generator list. Give it a sensible default
  // size and a minimum-size floor -- the list now holds the sphere + the full
  // tree catalog, and a previously-persisted tiny window (from when it held only
  // a few entries) would otherwise clip the list. The constraint clamps any
  // stale/tiny persisted size up every frame; the list scrolls if it overflows.
  int selected = generator_index_;
  int lod = lod_level_;
  ImGui::SetNextWindowSize(ImVec2(240.0f, 460.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(200.0f, 240.0f),
                                      ImVec2(4096.0f, 4096.0f));
  ImGui::Begin("Mesh");
  for (int i = 0; i < static_cast<int>(generators_.size()); ++i) {
    if (ImGui::Selectable(generators_[i].name.c_str(), i == generator_index_)) {
      selected = i;
    }
  }
  if (generators_[generator_index_].tree.has_value()) {
    ImGui::Separator();
    ImGui::TextUnformatted("LOD");
    // One per line (rather than the old 0/1/2/Multi single-row SameLine
    // chain) -- the longer volumetric-foliage labels ("Voxel L0".."Voxel L3")
    // would overflow this window's 200px width floor packed onto one row.
    ImGui::RadioButton("Original", &lod, 0);
    for (int level = 0; level < kVoxelLodCount; ++level) {
      // The last slot is the impostor, not a voxel level -- L3 was retired
      // because its tets overscale (see kFoliageImpostorThresholdPreviewM).
      const std::string label = (level == kVoxelLodCount - 1)
                                    ? std::string("Impostor")
                                    : "Voxel L" + std::to_string(level);
      ImGui::RadioButton(label.c_str(), &lod, level + 1);
    }
    ImGui::RadioButton("Multi", &lod, kMultiLodLevel);
    if (lod != kMultiLodLevel) {
      ImGui::Text("bark: %d   leaves: %d   total: %d tris", bark_tris_,
                  leaf_tris_, bark_tris_ + leaf_tris_);
    }
  }
  ImGui::End();

  if (selected != generator_index_) {
    generator_index_ = selected;
    RebuildScene();
  } else if (lod != lod_level_) {
    lod_level_ = lod;
    RebuildScene();
  }

  // Visual-setup window: the shared rendering-debug + light editor ("Debug").
  const bool env_changed = EditorUI::DrawDebugPanel(env_, *scene_renderer_, dt_);
  if (env_changed) {
    ApplyEnvironment();
  }
}

void ModelViewerView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
