#include "mapgen/synthetic_patch_source.hpp"

#include <algorithm>
#include <cmath>

#include "mapgen/cover.hpp"
#include "mapgen/erosion.hpp"
#include "mapgen/patch_io.hpp"
#include "mapgen/river_graph.hpp"

namespace badlands::mapgen {

namespace {

constexpr float kTwoPi = 6.28318530718f;

// Side relief. Two incommensurate periods so the pattern does not read as a
// tile, and both are world-metre wavelengths so raising the resolution samples
// the same surface more finely rather than changing it.
float hill_term(float x, float z) {
  const float a = std::sin(x * kTwoPi / 37.0f) * std::sin(z * kTwoPi / 53.0f);
  const float b = std::sin(x * kTwoPi / 23.0f + z * kTwoPi / 29.0f);
  return 0.5f * a + 0.25f * b;  // in [-0.75, 0.75]
}

}  // namespace

float synthetic_ground_m(const SyntheticPatchParams& p, float size_m, float x,
                         float z) {
  const float cx = 0.5f * size_m;
  const float half = std::max(1.0f, 0.5f * p.valley_width_m);
  const float t = std::clamp(std::abs(x - cx) / half, 0.0f, 1.0f);

  // Compact support: exactly 0 at the valley rim and C1 there, so the sides
  // meet the plain without a crease and nothing outside the width is dished.
  const float u = 1.0f - t * t;
  const float valley = p.valley_depth_m * u * u;

  // Relief lives on the SIDES only. Tapering by t^2 keeps the channel floor
  // smooth, which is what lets a test say where the thalweg is.
  const float hills = p.hill_relief_m * hill_term(x, z) * t * t;

  return p.base_elevation_m - z * p.downstream_drop - valley + hills;
}

PatchData SyntheticPatchSource::Fetch(const PatchRequest& req) const {
  PatchData out;
  const int n = req.resolution;
  const float size_m = req.world_size_m;
  if (n <= 0 || !(size_m > 0.0f)) return out;

  const float texel_m = patch_texel_m(req);
  out.texel_m = texel_m;
  out.origin_m = req.origin_m;

  // Texel (i, j) is sampled at its own coordinate i*texel_m, matching every
  // other raster in the pipeline (map_io, the carve's mask, the splat).
  out.height = Field2D<float>(n, n, 0.0f);
  for (int j = 0; j < n; ++j) {
    const float z = static_cast<float>(j) * texel_m;
    for (int i = 0; i < n; ++i) {
      const float x = static_cast<float>(i) * texel_m;
      out.height.at(i, j) = synthetic_ground_m(params_, size_m, x, z);
    }
  }

  // --- water -----------------------------------------------------------------
  // The lake fills the downstream band up to the level of the valley floor at
  // its upstream shore, so the waterline lands exactly there on the centreline
  // and rides up the sides from it.
  out.level = out.height;
  float lake_z_start_m = size_m;
  if (params_.lake_fraction > 0.0f) {
    lake_z_start_m = size_m * (1.0f - std::clamp(params_.lake_fraction, 0.0f, 1.0f));
    const float lake_level_m =
        synthetic_ground_m(params_, size_m, 0.5f * size_m, lake_z_start_m);
    for (int j = 0; j < n; ++j) {
      const float z = static_cast<float>(j) * texel_m;
      if (z < lake_z_start_m) continue;
      for (int i = 0; i < n; ++i) {
        if (out.height.at(i, j) < lake_level_m) out.level.at(i, j) = lake_level_m;
      }
    }
  }
  derive_water(out.height, out.level, texel_m, out.water_depth, out.lake_id,
               out.lakes);

  // --- soil, then cover ------------------------------------------------------
  // Soil thins with slope, which is the same physical story the real substrate
  // tells: steep ground sheds its cover and reads as rock.
  out.soil = Field2D<float>(n, n, 0.0f);
  out.cover = Field2D<uint8_t>(n, n, static_cast<uint8_t>(Cover::Grass));
  const float slope_ref = std::tan(params_.soil_slope_ref_deg * 3.14159265f / 180.0f);
  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) {
      const int im = std::max(0, i - 1), ip = std::min(n - 1, i + 1);
      const int jm = std::max(0, j - 1), jp = std::min(n - 1, j + 1);
      const float dx = (out.height.at(ip, j) - out.height.at(im, j)) /
                       (static_cast<float>(ip - im) * texel_m);
      const float dz = (out.height.at(i, jp) - out.height.at(i, jm)) /
                       (static_cast<float>(jp - jm) * texel_m);
      const float slope = std::sqrt(dx * dx + dz * dz);
      const float soil_m =
          params_.soil_max_m * std::clamp(1.0f - slope / std::max(1e-4f, slope_ref),
                                          0.0f, 1.0f);
      out.soil.at(i, j) = soil_m;

      // The same soil thresholds as before, now naming what GROWS rather than
      // an elevation band. The two thin classes both read as ground rather than
      // vegetation, which is what thin cover over bedrock actually looks like.
      Cover c;
      if (out.water_depth.at(i, j) > 0.0f) {
        c = Cover::Water;
      } else if (soil_m < params_.soil_cut_mountain_m) {
        c = Cover::Bare;
      } else if (soil_m < params_.soil_cut_hills_m) {
        c = Cover::Shrub;
      } else if (soil_m < params_.soil_cut_forest_m) {
        c = Cover::Grass;
      } else {
        c = Cover::Tree;
      }
      out.cover.at(i, j) = static_cast<uint8_t>(c);
    }
  }

  // --- the channel -----------------------------------------------------------
  // One reach straight down the thalweg, from the upstream edge to the lake
  // shore (or off the downstream frame when there is no lake). Discharge grows
  // into its stated value, so width and depth have somewhere to go rather than
  // being constant along the reach.
  const float cx = 0.5f * size_m;
  const float z_end = std::min(lake_z_start_m, size_m);
  const float spacing_m = std::max(2.0f, 3.0f * texel_m);
  if (z_end > spacing_m) {
    ErosionParams ep;  // defaults: the same hydraulics the real chain solves
    const int steps = std::max(1, static_cast<int>(z_end / spacing_m));

    RiverEdge e;
    e.from = 0;
    e.to = 1;
    for (int k = 0; k <= steps; ++k) {
      const float s = static_cast<float>(k) / static_cast<float>(steps);
      const float z = s * z_end;
      const float q = params_.river_discharge_m3_s * (0.3f + 0.7f * s);
      const ChannelHydraulics hyd =
          channel_hydraulics(q, params_.downstream_drop, ep);
      e.points_m.push_back(glm::vec2(cx, z));
      e.discharge_m3_s.push_back(q);
      e.width_m.push_back(hyd.width_m);
      e.depth_m.push_back(hyd.depth_m);
      e.speed_m_s.push_back(hyd.speed_m_s);
    }

    auto make_node = [&](size_t idx, RiverNodeKind kind) {
      RiverNode nd;
      nd.pos_m = e.points_m[idx];
      nd.ground_m = synthetic_ground_m(params_, size_m, nd.pos_m.x, nd.pos_m.y);
      nd.discharge_m3_s = e.discharge_m3_s[idx];
      nd.drainage_area_m2 =
          ep.runoff_m_per_s > 0.0f ? nd.discharge_m3_s / ep.runoff_m_per_s : 0.0f;
      nd.width_m = e.width_m[idx];
      nd.depth_m = e.depth_m[idx];
      nd.speed_m_s = e.speed_m_s[idx];
      nd.kind = kind;
      return nd;
    };

    // The mouth is a lake inlet when there is a lake to enter, and a frame exit
    // otherwise. Reading lake_id off the shoreline texel rather than assuming 0
    // keeps this correct if the bed ever ponds somewhere else as well.
    RiverNodeKind end_kind = RiverNodeKind::Mouth;
    int32_t end_lake = -1;
    if (!out.lakes.empty()) {
      const int si = std::clamp(static_cast<int>(cx / texel_m), 0, n - 1);
      const int sj = std::clamp(static_cast<int>(z_end / texel_m), 0, n - 1);
      for (int dj = 0; dj <= 2 && end_lake < 0; ++dj) {
        const int jj = std::min(n - 1, sj + dj);
        if (out.lake_id.at(si, jj) >= 0) end_lake = out.lake_id.at(si, jj);
      }
      if (end_lake >= 0) end_kind = RiverNodeKind::LakeInlet;
    }

    out.rivers.nodes.push_back(make_node(0, RiverNodeKind::Source));
    RiverNode tail = make_node(e.points_m.size() - 1, end_kind);
    tail.lake_id = end_lake;
    if (end_lake >= 0 && end_lake < static_cast<int32_t>(out.lakes.size()))
      tail.lake_kind = out.lakes[static_cast<size_t>(end_lake)].kind;
    out.rivers.nodes.push_back(tail);
    out.rivers.edges.push_back(std::move(e));
  }

  out.elevation_range = compute_elevation_range(out.height);
  return out;
}

}  // namespace badlands::mapgen
