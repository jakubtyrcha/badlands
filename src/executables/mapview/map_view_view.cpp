#include "executables/mapview/map_view_view.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <limits>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/app/sdl_input_util.hpp"
#include "engine/core/ray.hpp"
#include "engine/rendering/components/forward_component.hpp"
#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/components/transform.hpp"
#include "engine/rendering/geometry/mesh_builder_utils.hpp"     // PushVertex
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // AABB helper
#include "engine/rendering/scene_renderer.hpp"  // debug-view selectors
#include "engine/rendering/texture_loader.hpp"  // UploadTexture2DWithMips
#include "engine/ui/editor_ui.hpp"
#include "foliage/scatter.hpp"             // GenerateFoliage
#include "game/geometry/terrain_mesh.hpp"  // RaycastTerrain(MapData)
#include "game/map/forest_test_map_generator.hpp"
#include "game/map/map_data_terrain_query.hpp"
#include "game/map/visual_map_terrain_query.hpp"
#include "game/visual/forest_catalog.hpp"
#include "mapgen/biome_cover.hpp"
#include "mapgen/biomes.hpp"
#include "mapgen/soil_estimate.hpp"
#include "mapview/ground_splat.hpp"
#include "mapgen/river_arcs.hpp"
#include "mapview/lake_surface.hpp"
#include "mapview/river_surface.hpp"

namespace badlands {

namespace {

// How far a fitted arc may stray from the reach polyline, in TEXELS. Half a
// texel: the polyline itself came out of a Douglas-Peucker pass at ~1 texel, so
// fitting tighter than this spends arcs chasing error the input does not have.
constexpr float kRiverArcToleranceTexels = 0.5f;

// Subdivision exponent for the river corridor: 2^3 = 8x8 sub-quads, so the
// carve is sampled every 0.125 m inside a 1 m lattice.
//
// Both bounds are forced, which is why this is a constant and not a knob. From
// BELOW: the median channel is 0.52 m wide and 0.06 m deep, so a cavity on the
// base lattice is sub-texel and aliases into a one-vertex notch. From ABOVE:
// the cluster triangle budget is 2*tile_quads^2, and one refined quad costs
// 2*4^k triangles, so k <= 3 at the default 128. 0.125 is also a power of two,
// which keeps fine vertex coordinates exact in float -- the seam weld compares
// positions with ==.
constexpr uint8_t kRiverDetailExponent = 3;

// Wrap the patch in the VISUAL map at the raster's own texel spacing.
//
// One class per node, not blended slices: the patch's cover raster is already a
// hard per-texel assignment, and blending it would only soften a debug tint --
// the GROUND MATERIAL does not come from cover at all (see ground_splat.hpp).
VisualMapData MakeVisualMap(const mapgen::PatchData& patch, glm::vec2 size_m) {
  const int sw = patch.height.width, sh = patch.height.height;
  if (sw <= 0 || sh <= 0) return {};
  const float tx = size_m.x / static_cast<float>(sw);
  const float ty = size_m.y / static_cast<float>(sh);
  if (tx <= 0.0f) return {};
  // The lattice has ONE spacing scalar; this wrap is the code that depends on
  // square texels, so the invariant is asserted here (the CLI check in
  // main_mapview is the user-facing error for the same contradiction).
  assert(std::abs(tx - ty) <= 1e-4f * std::max(tx, ty));
  // One more node than texels per axis: node i sits at i * tx, so the
  // lattice spans exactly the map's size_m; edge nodes clamp to the last texel.
  VisualMapData map(sw + 1, sh + 1, tx);
  map.set_terrain_class(patch.terrain_class);
  for (int j = 0; j <= sh; ++j) {
    for (int i = 0; i <= sw; ++i) {
      const int sx = std::min(i, sw - 1), sz = std::min(j, sh - 1);
      map.mutable_height(i, j) = patch.height.at(sx, sz);
      map.set_cover(i, j, static_cast<mapgen::Cover>(patch.cover.at(sx, sz)));
    }
  }
  return map;
}

// --test-map hands over a GAMEPLAY map (the forest fixture predates the split
// and drives the forest plopper through MapDataTerrainQuery). Convert it, so
// the render path downstream sees only one map type.
VisualMapData VisualFromGameplay(const MapData& src) {
  VisualMapData out(src.nodes_x(), src.nodes_z(), src.spacing_m());
  for (int j = 0; j < src.nodes_z(); ++j) {
    for (int i = 0; i < src.nodes_x(); ++i) {
      out.mutable_height(i, j) = src.height(i, j);
      out.set_cover(i, j, mapgen::CoverForBiome(src.WeightsAtNode(i, j).Dominant()));
    }
  }
  return out;
}
}  // namespace


// Turns the river ARC CHAINS into the channel WATER mesh -- the free surface
// inside the carved cavity, on the same still-water material as the lakes.
//
// The geometry is built in src/mapview/river_surface.cpp (pure CPU, tested);
// everything here is the registry/material half. One entity for the whole
// network -- one material, so splitting it per reach would buy nothing and cost
// a draw call each.
//
// No carve, no water: without a cavity there is nothing for a surface to sit
// in, and a sheet at ground level would be the decal the carve replaced. The
// generated-map path has no river graph at all, so it never gets here.
//
// Destroyed and rebuilt on toggle rather than hidden: the registry has no
// per-entity visibility flag, and at this size (tens of thousands of triangles)
// the rebuild is cheaper than the machinery to avoid it.
void MapViewView::BuildRiverMesh() {
  if (registry_.valid(river_mesh_)) {
    registry_.destroy(river_mesh_);
    river_mesh_ = entt::null;
  }
  if (!show_rivers_ || river_arcs_.empty() || !river_carve_ || !water_factory_)
    return;

  // The SAME analytic field the terrain was tessellated from, so the surface
  // and the bed under it can never disagree.
  const std::vector<glm::vec3> tris = BuildRiverWaterTriangles(
      patch_.height.width, map_size_m_, patch_.rivers, river_arcs_,
      [carve = river_carve_.get()](float wx, float wz) {
        return carve->HeightAt(wx, wz);
      });
  if (tris.empty()) return;

  std::vector<float> v;
  v.reserve(tris.size() * kTexturedMeshFloatsPerVertex);
  for (const glm::vec3& p : tris) {
    // uv = world XZ, normal +Y, tangent +X -- exactly the lake surface's
    // convention, and correct for the same reason: each cross-section is FLAT
    // at its own water level, so the surface has no slope to shade.
    PushVertex(v, p, glm::vec2(p.x, p.z), glm::vec3(0.0f, 1.0f, 0.0f),
               glm::vec4(1.0f, 0.0f, 0.0f, kDefaultTangentHandedness));
  }

  river_mesh_ = registry_.create();
  registry_.emplace<Transform>(river_mesh_).matrix = glm::mat4(1.0f);
  auto& mesh = registry_.emplace<StaticTexturedMeshComponent>(river_mesh_);
  mesh.vertex_count =
      static_cast<uint32_t>(v.size() / kTexturedMeshFloatsPerVertex);
  mesh.dirty = true;
  mesh.geometry_type = GeometryType::kTexturedMesh;
  registry_.emplace<StaticMeshAabbComponent>(
      river_mesh_,
      StaticMeshAabbComponent{.local = ComputeLocalAabbFromVertices(
                                  v, kTexturedMeshFloatsPerVertex)});
  mesh.vertices = std::move(v);

  // The lakes' still-water material, on the same forward-transparent pass. Its
  // Beer-Lambert extinction is calibrated to 2.5-10 m visibility depths, so at
  // a 0.06 m median these brooks render nearly clear -- accepted: the CAVITY,
  // not the tint, is what makes a sub-metre channel visible.
  MaterialFactoryComponent fmc;
  fmc.factory = water_factory_.get();
  fmc.pass_type = MaterialPassType::kForwardTransparent;
  fmc.params = StillLakeWaterParams();
  fmc.config_hash = ComputeFactoryConfigHash(fmc);
  registry_.emplace<MaterialFactoryComponent>(river_mesh_, std::move(fmc));
  registry_.emplace<ForwardTransparentRenderable>(river_mesh_);
}

bool MapViewView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;  // shared debug-view selectors need it

  // Map-load profiling: time each load step and log a per-step + cumulative
  // breakdown once. `log_step` accumulates into cum_ms; the closing TOTAL line is
  // the wall-clock span of the whole load (the tiny untimed bits -- camera
  // framing -- are the only gap between the two).
  using clock = std::chrono::steady_clock;
  auto since = [](clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
  };
  const auto t_load = clock::now();
  double cum_ms = 0.0;
  auto log_step = [&](const char* name, double ms) {
    cum_ms += ms;
    spdlog::info("  {:<14} {:>8.1f} ms   (cum {:>8.1f} ms)", name, ms, cum_ms);
  };
  spdlog::info("map load profile (seed {}, {}x{} texels):", foliage_seed_,
               request_.resolution, request_.resolution);

  auto t = clock::now();
  // Start at noon, paused (an inspector holds still until you play/scrub).
  sim_clock_.speed = 0.0f;
  sim_clock_.SeekTimeOfDay(0.5f);
  ApplyDaylight();
  scene_context_.registry = &registry_;
  log_step("daylight", since(t));

  // TWO map sources, mutually exclusive (main_mapview rejects combinations,
  // and requires one).
  t = clock::now();
  if (test_map_) {
    // --test-map: the synthetic forest map instead of fetching from `source_`.
    // A fetched patch is not guaranteed to carry Biome::Forest coverage, so
    // the forest plopper may have nothing to plant into; this map exists to
    // give it one.
    map_size_m_ = ForestTestMapGenerator::kMapSizeM;
    request_.world_size_m = map_size_m_;
    gameplay_map_ = ForestTestMapGenerator(foliage_seed_).Generate();
    terrain_map_ = VisualFromGameplay(gameplay_map_);
    // The ground-material derivation reads the patch rasters, so the fixture
    // has to fill them too -- height for slope and curvature, cover for the
    // vegetation signal.
    patch_.height = mapgen::Field2D<float>(terrain_map_.nodes_x(),
                                           terrain_map_.nodes_z());
    patch_.cover = mapgen::Field2D<uint8_t>(terrain_map_.nodes_x(),
                                            terrain_map_.nodes_z(), 0);
    for (int j = 0; j < terrain_map_.nodes_z(); ++j) {
      for (int i = 0; i < terrain_map_.nodes_x(); ++i) {
        patch_.height.at(i, j) = terrain_map_.height(i, j);
        patch_.cover.at(i, j) = static_cast<uint8_t>(terrain_map_.cover(i, j));
      }
    }
    patch_.texel_m = terrain_map_.spacing_m();
    patch_.soil = mapgen::estimate_soil(patch_.height, patch_.texel_m);
    log_step("test map", since(t));
  } else {
    // `source_->Fetch` is the ONLY seam: a patch cut from a simulated world, a
    // patch read off disk, or one invented analytically by a test all arrive
    // here identically -- see mapgen/patch_source.hpp.
    patch_ = source_->Fetch(request_);
    if (mapgen::empty(patch_)) {
      spdlog::error("MapViewView: source returned an empty patch for the request");
      return false;
    }
    map_size_m_ = request_.world_size_m;
    log_step("mg:fetch", since(t));
  }

  // River network, carried by the patch (PatchData::rivers). A test map has
  // no river graph of its own, so this is deliberately gated on there being
  // one to fit.
  if (!patch_.rivers.edges.empty()) {
    t = clock::now();
    const float texel_m =
        request_.world_size_m / static_cast<float>(std::max(1, patch_.height.width));
    river_arcs_ = mapgen::build_river_arcs(
        patch_.rivers, kRiverArcToleranceTexels * texel_m);
    log_step("rivers", since(t));
    float q_max = 0.0f;
    for (const mapgen::RiverEdge& e : patch_.rivers.edges)
      for (float q : e.discharge_m3_s) q_max = std::max(q_max, q);
    spdlog::info(
        "rivers: {} nodes, {} reaches | peak Q {:.4f} m3/s",
        patch_.rivers.nodes.size(), patch_.rivers.edges.size(), q_max);

    // The arc fit, reported as what it buys and what it costs.
    //
    // FIT ERROR is the one that can invalidate everything downstream: the arcs
    // replace the polyline, so if the worst point sits further off than the
    // tolerance allowed, the fit silently moved the river. TIGHT arcs are the
    // other failure -- a radius below the channel's own half-width is a lattice
    // hairpin, not a meander, and it is what the ribbon's fold guard exists for.
    size_t pts = 0, arcs = 0, straight = 0, tight = 0;
    float min_r = std::numeric_limits<float>::infinity();
    float arc_len = 0.0f, worst_fit = 0.0f;
    for (const mapgen::RiverArcChain& c : river_arcs_) {
      const mapgen::RiverEdge& e = patch_.rivers.edges[c.edge];
      arcs += c.arcs.size();
      arc_len += c.length_m;
      pts += e.points_m.size();
      for (const glm::vec2& p : e.points_m) {
        float best = std::numeric_limits<float>::max();
        for (const mapgen::RiverArc& a : c.arcs)
          best = std::min(best, mapgen::arc_distance_m(a, p));
        worst_fit = std::max(worst_fit, best);
      }
      const std::vector<float> ep = mapgen::polyline_params(e.points_m);
      for (const mapgen::RiverArc& a : c.arcs) {
        if (a.curvature_1_m == 0.0f) {
          ++straight;
          continue;
        }
        const float r = mapgen::arc_radius_m(a);
        min_r = std::min(min_r, r);
        // "Tighter than the channel" is measured against the channel's OWN
        // half-width at the arc's midpoint -- the exact quantity the water
        // surface's fold guard clamps.
        const float u_mid = 0.5f * (a.param0 + a.param1);
        if (r < 0.5f * mapgen::sample_at_param(ep, e.width_m, u_mid)) ++tight;
      }
    }
    spdlog::info(
        "river arcs: {} chains, {} arcs ({} straight) from {} polyline points "
        "({:.2f}x) | {:.0f} m of channel | fit error <= {:.2f} m | "
        "min radius {:.1f} m, {} tighter than the channel",
        river_arcs_.size(), arcs, straight, pts,
        pts > 0 ? static_cast<double>(pts) /
                      static_cast<double>(std::max<size_t>(arcs, 1))
                : 0.0,
        arc_len, worst_fit, min_r, tight);

    // THE ADAPTER, and the only place in the codebase where rivers meet the
    // terrain builder. The carve owns the corridor mask and the carved-height
    // field; everything below restates them in the builder's generic
    // vocabulary -- a per-quad subdivision exponent plus a height function --
    // so nothing downstream of here knows what a river is.
    //
    // The carve is HEAP-ALLOCATED and never moved: river_detail_.height_at
    // closes over a raw pointer to it, the DAG build calls that once per fine
    // vertex (and four more times per numeric normal), and BuildRiverMesh calls
    // it again on every toggle.
    t = clock::now();
    river_carve_ = std::make_unique<mapgen::RiverCarve>(
        mapgen::build_river_carve(patch_.rivers, river_arcs_, patch_.height,
                                  request_.world_size_m));
    // Mask texels and quads are DIFFERENT SQUARES, so this is a dilation, not
    // a copy. Mask texel (i, j) is CENTRED on node (i, j) -- RiverCarve::
    // HeightAt indexes it as floor(w/texel + 0.5) -- while quad (i, j) spans
    // [i, i+1] * texel. They are offset by half a texel, and the four quads a
    // mask texel overlaps are (i-1, j-1), (i, j-1), (i-1, j), (i, j).
    //
    // Marking exactly those four is also what the tessellator needs, for a
    // reason beyond the offset: LatticeNodeVertex gives a base-lattice node the
    // COARSE height unless ALL FOUR of its incident quads are refined (it must
    // -- a node bordering a plain quad has to weld with that quad's record).
    // An element-for-element copy therefore left the carve unapplied at nodes
    // just inside the channel, standing them up as posts: MEASURED at 0.4163 m
    // in a 0.602 m cavity, 69% of the carve, on every offset and width tried.
    // The dilation makes "texel set => all four of that node's quads refined"
    // hold by construction, and the same probe then reports 0.0000 m.
    size_t corridor_texels = 0;
    const int mw = river_carve_->mask.width, mh = river_carve_->mask.height;
    river_detail_level_.assign(static_cast<size_t>(mw) * mh, 0);
    for (int j = 0; j < mh; ++j) {
      for (int i = 0; i < mw; ++i) {
        if (river_carve_->mask.at(i, j) == 0) continue;
        ++corridor_texels;
        for (int dz = -1; dz <= 0; ++dz) {
          for (int dx = -1; dx <= 0; ++dx) {
            const int qx = i + dx, qz = j + dz;
            if (qx >= 0 && qz >= 0 && qx < mw && qz < mh)
              river_detail_level_[static_cast<size_t>(qz) * mw + qx] =
                  kRiverDetailExponent;
          }
        }
      }
    }
    river_detail_.level = river_detail_level_.data();
    river_detail_.width = river_carve_->mask.width;
    river_detail_.height = river_carve_->mask.height;
    river_detail_.height_at = [carve = river_carve_.get()](float wx, float wz) {
      return carve->HeightAt(wx, wz);
    };
    log_step("carve", since(t));
    const size_t mask_texels = std::max<size_t>(river_carve_->mask.data.size(), 1);
    // Refined quads are reported SEPARATELY from corridor texels: the dilation
    // means they are not the same count, and quads are what the DAG pays for.
    size_t refined_quads = 0;
    for (uint8_t k : river_detail_level_) refined_quads += (k > 0) ? 1 : 0;
    spdlog::info(
        "river carve: {} corridor texels -> {} refined quads ({:.2f}% of the "
        "map), exponent {} ({:.3f} m sampling)",
        corridor_texels, refined_quads,
        100.0 * static_cast<double>(refined_quads) /
            static_cast<double>(mask_texels),
        static_cast<int>(kRiverDetailExponent),
        (request_.world_size_m / static_cast<float>(std::max(1, patch_.height.width))) /
            static_cast<float>(1 << kRiverDetailExponent));
  }

  // Lake bathymetry, logged once: the water material's extinction coefficients
  // are derived from a visibility depth in metres, so the depth distribution
  // the generator actually produces is a load-bearing input, not trivia.
  if (!test_map_) {
    std::vector<float> depths;
    depths.reserve(patch_.lakes.size());
    for (const mapgen::LakeInfo& l : patch_.lakes) depths.push_back(l.max_depth_m);
    std::sort(depths.begin(), depths.end());
    int wet = 0;
    for (float d : patch_.water_depth.data) {
      if (d > 0.0f) ++wet;
    }
    const float wet_frac =
        patch_.water_depth.data.empty()
            ? 0.0f
            : static_cast<float>(wet) /
                  static_cast<float>(patch_.water_depth.data.size());
    if (depths.empty()) {
      spdlog::info("lakes: none (wet {:.2f}%)", 100.0f * wet_frac);
    } else {
      spdlog::info(
          "lakes: {}  max_depth_m min/median/max = {:.2f}/{:.2f}/{:.2f}  "
          "wet {:.2f}%",
          depths.size(), depths.front(), depths[depths.size() / 2],
          depths.back(), 100.0f * wet_frac);
    }
  }

  // Wrap the patch in the VISUAL map at the raster's own texel spacing -- the
  // input to the cluster terrain and picking. The cluster LOD's job is to decimate from full detail, so the leaf
  // lattice is the finest source data (one node per texel), not a coarser mesh
  // density; LOD selection manages the triangle cost.
  if (!test_map_) {
    t = clock::now();
    terrain_map_ = MakeVisualMap(patch_, glm::vec2(request_.world_size_m));
    log_step("map->VisualMapData", since(t));
  }

  // Frame the camera BEFORE building the terrain, so the cluster path's initial
  // LOD selection already runs against the real camera position rather than the
  // origin. Start on the map centre at ground-level framing, matching the game's
  // own camera (game_view.cpp: pitch 50, height 42) rather than a bird's-eye
  // view. Scroll to zoom out; max_height reaches far enough to take in the whole
  // map.
  //
  // focus.y is the midpoint of the patch's elevation range, not a flat 0: at a
  // fixed pitch the view ray crosses the terrain band at a constant multiple of
  // the elevation away from the focus, so a patch sitting well above sea level
  // (a real protogen world does) needs its own elevation to be framed at all --
  // see PatchData::elevation_range (mapgen/patch_data.hpp). The test map sits
  // near zero, where 0 already works.
  const float map_depth_m = request_.world_size_m;
  const float focus_y_m =
      test_map_ ? 0.0f
               : 0.5f * (patch_.elevation_range.min_m + patch_.elevation_range.max_m);
  gamecam_.focus = glm::vec3(map_size_m_ * 0.5f, focus_y_m, map_depth_m * 0.5f);
  gamecam_.pitch_deg = 50.0f;
  gamecam_.height = 42.0f;
  gamecam_.min_height = 5.0f;
  gamecam_.max_height = std::max(400.0f, map_size_m_);
  // Headless framing override (--camera-height): clamp into the controller's
  // range so a far shot can pull well back without escaping it.
  if (camera_height_override_ > 0.0f) {
    gamecam_.max_height = std::max(gamecam_.max_height, camera_height_override_);
    gamecam_.height = std::clamp(camera_height_override_, gamecam_.min_height,
                                 gamecam_.max_height);
  }
  gamecam_.UpdateCamera(camera_);

  // Terrain materials: one PBR pack per GROUND SLOT, selected by the patch's
  // terrain class and keyed by name, so a renamed or reordered manifest entry
  // fails loudly instead of silently binding the wrong texture.
  t = clock::now();
  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("MapViewView: MaterialLibrary init failed");
    return false;
  }
  std::vector<std::string> pack_dirs;
  if (!ResolveGroundPacks("assets/materials/terrain_ground.json",
                          patch_.terrain_class, pack_dirs)) {
    spdlog::error("MapViewView: failed to resolve ground packs");
    return false;
  }
  terrain_arrays_ = matlib_.LoadTerrainArrays(pack_dirs);
  if (!matlib_.ok()) {
    spdlog::error("MapViewView: terrain arrays failed to build");
    return false;
  }
  log_step("ground packs", since(t));

  // Ground splat: the per-slot blend weights, sampled by world XZ in the
  // fragment stage rather than carried on the vertices, so the coarsest LOD
  // cluster still gets full-resolution material detail.
  t = clock::now();
  const GroundSplat splat = BuildGroundSplat(patch_);
  if (splat.empty()) {
    spdlog::error("MapViewView: empty ground splat");
    return false;
  }
  splat0_view_ = UploadTexture2DWithMips(
                     device_, queue_, *ctx.pipeline_gen,
                     static_cast<uint32_t>(splat.width),
                     static_cast<uint32_t>(splat.height), splat.slots0.data())
                     .view;
  splat1_view_ = UploadTexture2DWithMips(
                     device_, queue_, *ctx.pipeline_gen,
                     static_cast<uint32_t>(splat.width),
                     static_cast<uint32_t>(splat.height), splat.slots1.data())
                     .view;
  if (!splat0_view_ || !splat1_view_) {
    spdlog::error("MapViewView: ground splat upload failed");
    return false;
  }
  // Trilinear + CLAMP. Mips matter: at max zoom one screen pixel covers several
  // map texels, and unmipped weights alias into a shimmering material mosaic.
  wgpu::SamplerDescriptor splat_sd = {};
  splat_sd.minFilter = wgpu::FilterMode::Linear;
  splat_sd.magFilter = wgpu::FilterMode::Linear;
  splat_sd.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  splat_sd.addressModeU = wgpu::AddressMode::ClampToEdge;
  splat_sd.addressModeV = wgpu::AddressMode::ClampToEdge;
  splat_sampler_ = device_.CreateSampler(&splat_sd);
  // world XZ in [0, size] -> texel CENTRES in [0.5/N, 1 - 0.5/N].
  const float inv_n = 1.0f / static_cast<float>(splat.width);
  const float splat_scale = (1.0f - inv_n) / request_.world_size_m;
  const glm::vec4 splat_uv(splat_scale, splat_scale, 0.5f * inv_n, 0.5f * inv_n);
  log_step("ground splat", since(t));

  // Build the shared cluster-LOD terrain (identity model -- mapview vertices are
  // absolute world coords). --serial-build forces the single-threaded DAG build
  // for the perf A/B (both produce a bit-identical DAG). Seed the debug tint from
  // --lod-tint so a headless run renders tinted on frame one.
  cluster_terrain_.debug_tint_mode() = initial_tint_;
  TerrainClusterParams cluster_params;
  cluster_params.parallel_build = !serial_build_;
  // Local refinement, if a carve produced any. The grid must BE the map's quad
  // grid (both come from the same rasters, so this holds by construction) --
  // the check is a tripwire for a future map source where it would not, and
  // dropping the detail is the safe answer there.
  const TerrainDetailField* detail = nullptr;
  if (river_carve_ && river_detail_.level != nullptr) {
    if (river_detail_.width == terrain_map_.nodes_x() - 1 &&
        river_detail_.height == terrain_map_.nodes_z() - 1) {
      detail = &river_detail_;
    } else {
      spdlog::error(
          "MapViewView: corridor grid {}x{} != quad grid {}x{}; building the "
          "terrain WITHOUT the river carve",
          river_detail_.width, river_detail_.height, terrain_map_.nodes_x() - 1,
          terrain_map_.nodes_z() - 1);
    }
  }
  t = clock::now();
  if (!cluster_terrain_.Build(terrain_map_.Lattice(), ctx, registry_,
                              glm::mat4(1.0f),
                              cluster_params, terrain_arrays_,
                              matlib_.shared_sampler(), splat0_view_,
                              splat1_view_, splat_sampler_, splat_uv, detail)) {
    spdlog::error("MapViewView: cluster terrain build failed");
    return false;
  }
  // Seed the LOD cut once so the first rendered frame (headless --screenshot
  // renders after a single Update) already draws the selected cut.
  cluster_terrain_.UpdateLod(camera_, screen_h_px_);
  log_step("cluster terrain", since(t));

  // Still lake water. The surface deliberately overlaps each shore and runs
  // under the terrain; water tests depth without writing it, so the buried ring
  // is rejected in hardware -- and that overlap is what keeps a later wave
  // displacement from opening a gap at the waterline.
  t = clock::now();
  water_factory_ =
      BuildStillWaterForwardFactory(ctx.device, ctx.queue, ctx.pipeline_gen);
  if (!water_factory_) {
    spdlog::error("MapViewView: water factory build failed");
    return false;
  }
  // The test map has no lakes at all (its water level sits below the lowest
  // ground), so BuildLakeSurfaceTriangles has nothing to read -- skip it rather
  // than hand it the empty patch left over from the skipped fetch.
  const std::vector<glm::vec3> water_tris =
      test_map_ ? std::vector<glm::vec3>{}
                : BuildLakeSurfaceTriangles(patch_, request_.world_size_m);
  if (!water_tris.empty()) {
    std::vector<float> v;
    v.reserve(water_tris.size() * kTexturedMeshFloatsPerVertex);
    for (const glm::vec3& p : water_tris) {
      // uv = world XZ, normal +Y, tangent +X -- a flat plane needs no more.
      PushVertex(v, p, glm::vec2(p.x, p.z), glm::vec3(0.0f, 1.0f, 0.0f),
                 glm::vec4(1.0f, 0.0f, 0.0f, kDefaultTangentHandedness));
    }
    // Created directly in the registry, mirroring what SceneGraph's
    // MeshAttachment path emplaces (mesh + AABB + material + the pass tag).
    // A SceneGraph is not usable here: SyncToRegistry clears the registry.
    const entt::entity e = registry_.create();
    registry_.emplace<Transform>(e).matrix = glm::mat4(1.0f);
    auto& mesh = registry_.emplace<StaticTexturedMeshComponent>(e);
    mesh.vertex_count =
        static_cast<uint32_t>(v.size() / kTexturedMeshFloatsPerVertex);
    mesh.dirty = true;
    mesh.geometry_type = GeometryType::kTexturedMesh;
    registry_.emplace<StaticMeshAabbComponent>(
        e, StaticMeshAabbComponent{
               .local = ComputeLocalAabbFromVertices(
                   v, kTexturedMeshFloatsPerVertex)});
    mesh.vertices = std::move(v);

    MaterialFactoryComponent fmc;
    fmc.factory = water_factory_.get();
    fmc.pass_type = MaterialPassType::kForwardTransparent;
    fmc.params = StillLakeWaterParams();
    fmc.config_hash = ComputeFactoryConfigHash(fmc);
    registry_.emplace<MaterialFactoryComponent>(e, std::move(fmc));
    registry_.emplace<ForwardTransparentRenderable>(e);
  }
  spdlog::info("water: {} triangles over {} lakes", water_tris.size() / 3,
               patch_.lakes.size());
  log_step("water", since(t));

  // The channel water. AFTER the lake water because it shares its factory, and
  // it reads better in that order too: ground, then standing water, then the
  // channels between them.
  t = clock::now();
  BuildRiverMesh();
  if (registry_.valid(river_mesh_)) {
    spdlog::info("rivers: {} water triangles",
                 registry_.get<StaticTexturedMeshComponent>(river_mesh_)
                         .vertex_count / 3);
  }
  log_step("river mesh", since(t));

  // The forest. Placement is pure CPU over the frozen MapData contract, so
  // nothing here knows how the map was made -- a generated map simply reports
  // zero Forest coverage and plants nothing.
  t = clock::now();
  ForestCatalog forest_catalog;
  if (!BuildForestCatalog(forest_catalog)) {
    spdlog::error("MapViewView: forest catalog build failed");
    return false;
  }

  // Reads COVER off the visual map, so every source that reports tree cover
  // plants: the synthetic fixture, a patch dir carrying it, and a terrain-net
  // bundle (new-forest/00 is 93% tree). Reading the gameplay map instead would
  // silently plant nothing anywhere except --test-map, which the coarse path
  // never noticed only because ClassifyBiome cannot return Forest.
  const VisualMapTerrainQuery forest_query(terrain_map_, mapgen::Cover::Tree);
  foliage::FoliageGenParams foliage_params;
  foliage_params.seed = foliage_seed_;
  foliage_params.origin_m = glm::vec2(0.0f);
  foliage_params.size_m = glm::vec2(terrain_map_.size_x_m(),
                                    terrain_map_.size_z_m());

  // Ask whether there is anywhere to grow BEFORE building any geometry.
  // Placement spaces trees by their measured crowns, so the meshes have to
  // exist before GenerateFoliage -- which means an empty field can no longer
  // save that cost after the fact. On a generated map (mapgen emits no Forest
  // biome yet) this is ~1 s of meshing that would be discarded immediately.
  std::vector<InstancedLodModel> forest_models;
  foliage::FoliageField foliage_field;
  if (foliage::AnyCoverage(forest_query, foliage_params)) {
    forest_models = BuildForestModels(forest_catalog);
    log_step("foliage mesh", since(t));

    t = clock::now();
    foliage_field = foliage::GenerateFoliage(forest_catalog.type, forest_query,
                                             foliage_params);
    log_step("foliage place", since(t));
  } else {
    // The empty field below takes ForestRenderer::Build's "nothing to plant"
    // path, which builds no GPU resources either.
    spdlog::info(
        "MapViewView: no Forest coverage on this map -- skipping the foliage "
        "mesh build entirely");
  }

  t = clock::now();
  if (!forest_.Build(ctx.device, ctx.queue, *ctx.pipeline_gen,
                     std::move(forest_catalog), std::move(forest_models),
                     std::move(foliage_field))) {
    spdlog::error("MapViewView: forest renderer build failed");
    return false;
  }
  forest_field_ = forest_.instanced_field();
  if (forest_field_) {
    forest_.Update(camera_, scene_context_.sun_direction);
    scene_context_.instanced_fields = &forest_field_;
    scene_context_.instanced_field_count = 1;
  }
  log_step("forest build", since(t));

  spdlog::info("map load: {:.1f} ms total  ({}x{} texels)", since(t_load),
               request_.resolution, request_.resolution);

  return true;
}

void MapViewView::ApplyDaylight() {
  const DaylightState state =
      ComputeDaylight(daylight_cfg_, sim_clock_.TimeOfDay());
  ApplyDaylightEnvironment(state, daylight_cfg_, device_, queue_, sky_cube_,
                           scene_context_);
}

void MapViewView::HandleEvent(const SDL_Event& event, int /*width*/,
                              int /*height*/) {
  if (ImGui::GetIO().WantCaptureMouse) return;

  // Mouse coords are logical points; HandleEvent's width/height are physical
  // pixels. EventWindowLogicalSize keeps both in one space (see its docs).
  switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
      glm::vec2 screen;
      if (!EventWindowLogicalSize(event.motion.windowID, screen)) {
        hover_valid_ = false;
        return;
      }
      const Ray ray = ScreenPointToRay(
          camera_, glm::vec2(event.motion.x, event.motion.y), screen);
      hover_valid_ = RaycastTerrain(terrain_map_.Lattice(), ray, hover_point_);
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      glm::vec2 screen;
      if (!EventWindowLogicalSize(event.wheel.windowID, screen)) return;
      const glm::vec2 pixel(event.wheel.mouse_x, event.wheel.mouse_y);
      ZoomAtCursor(gamecam_, camera_, NormalizedWheelY(event.wheel), pixel,
                   screen);
      // The camera moved under a stationary cursor, so the hover point is stale
      // -- re-pick now rather than waiting for the next motion event (which may
      // never come if the user is only scrolling).
      const Ray ray = ScreenPointToRay(camera_, pixel, screen);
      hover_valid_ = RaycastTerrain(terrain_map_.Lattice(), ray, hover_point_);
      break;
    }
    default:
      break;
  }
}

void MapViewView::Update(float dt, const bool* keyboard_state) {
  dt_ = dt;

  // Advance the shared clock; when it's running, move the sun (paused =>
  // holds). The daylight re-bake is throttled implicitly: ApplyDaylight only
  // runs while time actually moves.
  const double sim_dt = sim_clock_.Advance(dt);
  if (sim_dt > 0.0) ApplyDaylight();

  if (keyboard_state != nullptr && ImGui::GetCurrentContext() != nullptr &&
      !ImGui::GetIO().WantCaptureKeyboard) {
    glm::vec2 dir(0.0f);
    if (keyboard_state[SDL_SCANCODE_W] || keyboard_state[SDL_SCANCODE_UP]) dir.y -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_S] || keyboard_state[SDL_SCANCODE_DOWN]) dir.y += 1.0f;
    if (keyboard_state[SDL_SCANCODE_A] || keyboard_state[SDL_SCANCODE_LEFT]) dir.x -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_D] || keyboard_state[SDL_SCANCODE_RIGHT]) dir.x += 1.0f;
    gamecam_.PanKeyboard(dir, dt);  // zoom-scaled; no-op when dir is zero
  }
  gamecam_.UpdateCamera(camera_);

  // Re-select the LOD cluster cut for the new camera and rewrite the draw
  // ranges. Cheap flat pass over the DAG; no buffer re-upload.
  cluster_terrain_.UpdateLod(camera_, screen_h_px_);

  // Coarse-cull the 32 m foliage cells and re-upload only if the visible set
  // changed (see forest_renderer.hpp). No-op for a forest-less map.
  forest_.Update(camera_, scene_context_.sun_direction);
}

void MapViewView::DrawUI() {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Begin("Map");
  ImGui::Text("seed %u  %dx%d texels  %.0fx%.0f m", foliage_seed_,
              request_.resolution, request_.resolution, request_.world_size_m,
              request_.world_size_m);
  cluster_terrain_.DrawDebugUI();
  if (!patch_.rivers.edges.empty()) {
    if (ImGui::Checkbox("river water", &show_rivers_)) BuildRiverMesh();
    size_t arcs = 0;
    for (const mapgen::RiverArcChain& c : river_arcs_) arcs += c.arcs.size();
    ImGui::Text("  %zu reaches, %zu chains, %zu arcs",
                patch_.rivers.edges.size(), river_arcs_.size(), arcs);
  }
  ImGui::Text("focus: (%.0f, %.0f)", gamecam_.focus.x, gamecam_.focus.z);
  if (hover_valid_) {
    const std::string_view bn = mapgen::cover_name(
        terrain_map_.CoverAt(hover_point_.x, hover_point_.z));
    ImGui::Text("hover: (%.1f, %.1f, %.1f)  %.*s", hover_point_.x,
                hover_point_.y, hover_point_.z, static_cast<int>(bn.size()),
                bn.data());
  } else {
    ImGui::TextUnformatted("hover: (off terrain)");
  }
  ImGui::End();

  // Shared sim/daylight/debug controls (same helpers the game uses). Re-bake the
  // sky immediately on a scrub or a config edit so it's visible without waiting
  // for the clock; while playing, Update already re-bakes as time advances.
  ImGui::Begin("Sim / Daylight / Debug");
  if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (EditorUI::DrawSimClockControls(sim_clock_)) ApplyDaylight();
  }
  if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (EditorUI::DrawDaylightEditor(daylight_cfg_)) ApplyDaylight();
  }
  if (scene_renderer_ != nullptr) {
    if (ImGui::CollapsingHeader("Debug Views")) {
      EditorUI::DrawGBufferDebugSelector(*scene_renderer_);
      EditorUI::DrawShadowDebugSelector(*scene_renderer_);
    }
  }
  EditorUI::DrawStats(dt_);
  ImGui::End();
}

void MapViewView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
  // The LOD screen-space-error metric is in pixels, so it needs the viewport
  // height in pixels -- exactly what OnResize carries (physical pixels windowed,
  // the capture height headless).
  if (height > 0) screen_h_px_ = static_cast<float>(height);
}

}  // namespace badlands
