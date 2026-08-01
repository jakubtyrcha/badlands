#include "executables/viewer/model_viewer_view.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>  // glm::translate, glm::rotate, glm::scale
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/app/sdl_input_util.hpp"  // NormalizedWheelY
#include "engine/rendering/gpu_instance_renderer.hpp"  // GpuInstanceRenderer::InstanceInput
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
constexpr std::array<float, kVoxelLodCount - 1> kMultiLodThresholds = {
    18.0f, 23.0f, 70.0f};

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

}  // namespace

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

    // BuildTreeField (volumetric-foliage Phase 5) takes one pre-voxelized
    // leaf-crown mesh per LOD rather than voxelizing internally, so `s` (the
    // preview rescale factor) must be known BEFORE calling it, to convert
    // kFoliageVoxelWorldSizes (preview-space) into native-unit cell sizes --
    // generate the skeleton/bark here to learn `s` and voxelize. BuildTreeField
    // now takes this skeleton + bark mesh directly (review fix -- it used to
    // regenerate its own byte-identical copies of both internally, a second
    // full generation pass on top of this one), so `bark_for_scale` is moved
    // into the call below instead of being thrown away.
    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(*gen.tree);
    TexturedMeshResult bark_for_scale = GenerateTreeMesh(*gen.tree, skeleton);
    const float bark_h = bark_for_scale.local_bounds.max.y -
                         bark_for_scale.local_bounds.min.y;
    const float s = kTreePreviewHeight / std::max(bark_h, 0.001f);

    const TexturedMeshResult leaves_for_voxelize =
        GenerateLeafMesh(*gen.tree, skeleton);
    std::array<TexturedMeshResult, kVoxelLodCount> leaf_lod_meshes;
    for (int lod = 0; lod < kVoxelLodCount; ++lod) {
      LeafVoxelizeOptions voxel_opts;
      voxel_opts.cell_size =
          kFoliageVoxelWorldSizes[static_cast<size_t>(lod)] / s;
      leaf_lod_meshes[static_cast<size_t>(lod)] = VoxelizeLeafCards(
          leaves_for_voxelize.mesh, gen.tree->leaves.silhouette, voxel_opts);
    }

    std::unique_ptr<TreeField> field = BuildTreeField(
        device_, queue_, *pipeline_gen_, *gen.tree, skeleton,
        std::move(bark_for_scale), leaf_lod_meshes, capacity,
        kMultiLodThresholds);
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
    // was already computed above (from a deterministically bit-identical
    // bark regeneration) -- reused here rather than re-derived from
    // field->bark_local_bounds.
    const glm::mat4 xf =
        glm::translate(glm::mat4(1.0f),
                      glm::vec3(0.0f, -field->bark_local_bounds.min.y * s,
                                0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(s));

    Aabb combined_local_bounds = field->bark_local_bounds;
    if (field->has_leaves) {
      combined_local_bounds =
          combined_local_bounds.Union(field->leaf_local_bounds);
    }

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
    TexturedMeshResult bark = GenerateTreeMesh(*gen.tree, skeleton);
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
    bark_tris_ = static_cast<int>(bark.mesh.indices.size() / 3);
    AddMeshEntity(scene_, "bark", std::move(bark), bark_mat_, xf);

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
        DeferredMaterial lm = matlib_.TranslucentFoliage(
            LeafViewFor(gen.tree->leaves.silhouette), leaf_sampler_,
            gen.tree->leaves.alpha_cutoff, gen.tree->leaves.tint,
            gen.tree->leaves.transmission_tint,
            gen.tree->leaves.transmission_strength);
        AddForwardOpaqueMeshEntity(scene_, "leaves", std::move(leaves),
                                   lm.factory, lm.params, xf);
      }
    } else {
      // Voxel L0..L3: tet-voxelize the leaf cards instead of simplifying
      // them. cell_size is world_size / s -- kFoliageVoxelWorldSizes is in
      // preview (post-rescale) world units, LeafVoxelizeOptions::cell_size
      // wants the tree's own native units, and `s` is the same
      // kTreePreviewHeight rescale bark/xf above already apply.
      TexturedMeshResult leaves = GenerateLeafMesh(*gen.tree, skeleton);
      LeafVoxelizeOptions voxel_opts;
      voxel_opts.cell_size = kFoliageVoxelWorldSizes[lod_level_ - 1] / s;
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
      const std::string label = "Voxel L" + std::to_string(level);
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
