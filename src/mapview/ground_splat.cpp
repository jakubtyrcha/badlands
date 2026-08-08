#include "mapview/ground_splat.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "mapgen/cover.hpp"

namespace badlands {

namespace {

using mapgen::Cover;

// --- the derivation ---------------------------------------------------------
//
// PROVISIONAL. Every constant here is a first pass whose only job is to put
// something differentiated on screen; the brush work replaces the lot. They are
// at least all PHYSICAL -- degrees, metres, 1/metres -- so they can be argued
// with rather than merely tuned.

// Above this, soil does not stay on the slope and rock shows through. Softened
// over kSlopeSoftDeg rather than applied as a step, so a ridge does not get a
// hard outline.
constexpr float kRockSlopeDeg = 38.0f;
constexpr float kSlopeSoftDeg = 10.0f;
// Below this much cover, the ground reads as stony rather than vegetated.
constexpr float kThinSoilM = 0.6f;
constexpr float kDeepSoilM = 2.5f;
// Convergent curvature this strong marks where debris collects -- the foot of a
// face rather than the face itself. 1/m, from the height Laplacian.
constexpr float kScreeCurvature = 0.02f;

float smooth01(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

struct Local {
  float slope_deg;
  float curvature;  // 1/m; NEGATIVE is convergent (a hollow)
};

Local sample_local(const mapgen::Field2D<float>& h, int x, int y,
                   float texel_m) {
  const int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, h.width - 1);
  const int y0 = std::max(y - 1, 0), y1 = std::min(y + 1, h.height - 1);
  // A one-texel-wide raster collapses the clamps and would divide by zero; a
  // NaN slope then survives every bound test downstream, since NaN compares
  // false against all of them.
  const float dzdx = x1 == x0 ? 0.0f
                              : (h.at(x1, y) - h.at(x0, y)) /
                                    (static_cast<float>(x1 - x0) * texel_m);
  const float dzdy = y1 == y0 ? 0.0f
                              : (h.at(x, y1) - h.at(x, y0)) /
                                    (static_cast<float>(y1 - y0) * texel_m);
  Local l;
  l.slope_deg =
      std::atan(std::hypot(dzdx, dzdy)) * (180.0f / 3.14159265358979f);
  // Laplacian, the sum of the two second derivatives.
  l.curvature = (h.at(x1, y) + h.at(x0, y) + h.at(x, y1) + h.at(x, y0) -
                 4.0f * h.at(x, y)) /
                (texel_m * texel_m);  // texel_m > 0 is checked by the caller
  return l;
}

// Separable box blur of one weight plane, radius `r` texels, edges clamped.
void BlurPlane(std::vector<float>& plane, int w, int h, int r,
               std::vector<float>& scratch) {
  if (r <= 0) return;
  const float inv = 1.0f / static_cast<float>(2 * r + 1);
  scratch.assign(plane.size(), 0.0f);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float sum = 0.0f;
      for (int k = -r; k <= r; ++k)
        sum += plane[static_cast<size_t>(y) * w + std::clamp(x + k, 0, w - 1)];
      scratch[static_cast<size_t>(y) * w + x] = sum * inv;
    }
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float sum = 0.0f;
      for (int k = -r; k <= r; ++k)
        sum += scratch[static_cast<size_t>(std::clamp(y + k, 0, h - 1)) * w + x];
      plane[static_cast<size_t>(y) * w + x] = sum * inv;
    }
  }
}

}  // namespace

const char* ground_slot_name(GroundSlot s) {
  switch (s) {
    case GroundSlot::BareRock: return "bare_rock";
    case GroundSlot::Scree: return "scree";
    case GroundSlot::StonyGround: return "stony_ground";
    case GroundSlot::Turf: return "turf";
    case GroundSlot::Heath: return "heath";
    case GroundSlot::Peat: return "peat";
    case GroundSlot::Silt: return "silt";
    case GroundSlot::ForestFloor: return "forest_floor";
  }
  return "bare_rock";
}

GroundSplat BuildGroundSplat(const mapgen::PatchData& patch) {
  GroundSplat out;
  const int w = patch.height.width, h = patch.height.height;
  if (w <= 0 || h <= 0) return out;
  const float texel_m = patch.texel_m > 0.0f ? patch.texel_m : 1.0f;
  const bool have_soil = patch.soil.width == w && patch.soil.height == h;
  const bool have_cover = patch.cover.width == w && patch.cover.height == h;
  const bool have_water =
      patch.water_depth.width == w && patch.water_depth.height == h;

  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  std::vector<std::vector<float>> planes(kGroundSlotCount,
                                         std::vector<float>(n, 0.0f));

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const Local l = sample_local(patch.height, x, y, texel_m);
      const float soil = have_soil ? patch.soil.data[i] : 1.0f;
      const Cover cov =
          have_cover ? static_cast<Cover>(patch.cover.data[i]) : Cover::Grass;

      // How exposed the bedrock is: steep OR stripped. Both, physically, are
      // the same statement -- soil does not stay where it cannot.
      const float steep =
          smooth01((l.slope_deg - kRockSlopeDeg) / kSlopeSoftDeg + 0.5f);
      const float stripped =
          1.0f - smooth01((soil - kThinSoilM) / (kDeepSoilM - kThinSoilM));
      const float exposure = std::max(steep, steep * 0.5f + stripped * 0.5f);

      // Debris collects where the ground is convergent AND steep -- the apron
      // at the foot of a face, not the face.
      const float convergent = smooth01(-l.curvature / kScreeCurvature);
      const float scree = exposure * convergent;

      auto add = [&](GroundSlot s, float v) {
        if (v > 0.0f) planes[static_cast<int>(s)][i] += v;
      };

      // Standing water wins outright: a lake bed is silt whatever the slope of
      // the ground beneath it says.
      if (have_water && patch.water_depth.data[i] > 0.0f) {
        add(GroundSlot::Silt, 1.0f);
        continue;
      }

      add(GroundSlot::BareRock, exposure * (1.0f - convergent));
      add(GroundSlot::Scree, scree);

      // What is left of the texel after the rock takes its share is decided by
      // what grows on it.
      const float vegetated = std::max(0.0f, 1.0f - exposure);
      switch (cov) {
        case Cover::Tree:
          add(GroundSlot::ForestFloor, vegetated);
          break;
        case Cover::Shrub:
        case Cover::Moss:
          add(GroundSlot::Heath, vegetated);
          break;
        case Cover::Wetland:
          add(GroundSlot::Peat, vegetated);
          break;
        case Cover::Water:
          add(GroundSlot::Silt, vegetated);
          break;
        case Cover::Grass:
        case Cover::Crop:
          add(GroundSlot::Turf, vegetated);
          break;
        case Cover::Bare:
        case Cover::Snow:
        case Cover::Built:
        case Cover::Unknown:
          // Nothing observed growing, so the ground speaks for itself: stony
          // where cover is thin, turf where it is deep.
          add(GroundSlot::StonyGround, vegetated * stripped);
          add(GroundSlot::Turf, vegetated * (1.0f - stripped));
          break;
      }
    }
  }

  const int radius = static_cast<int>(std::lround(kGroundBlendM / texel_m));
  std::vector<float> scratch;
  for (auto& p : planes) BlurPlane(p, w, h, radius, scratch);

  out.width = w;
  out.height = h;
  out.slots0.assign(n * 4, 0);
  out.slots1.assign(n * 4, 0);

  for (size_t i = 0; i < n; ++i) {
    int best = 0, second = -1;
    for (int s = 1; s < kGroundSlotCount; ++s)
      if (planes[s][i] > planes[best][i]) best = s;
    for (int s = 0; s < kGroundSlotCount; ++s) {
      if (s == best) continue;
      if (second < 0 || planes[s][i] > planes[second][i]) second = s;
    }
    const float w0 = planes[best][i];
    const float w1 = (second >= 0) ? planes[second][i] : 0.0f;
    const float sum = w0 + w1;
    if (sum <= 0.0f) {
      // No slot claimed this texel. Give it bare rock rather than nothing --
      // an all-zero weight vector renormalises to a black surface.
      out.slots0[i * 4 + static_cast<int>(GroundSlot::BareRock)] = 255;
      continue;
    }

    // Quantize so the PAIR sums to exactly 255: the shader renormalises by
    // whatever it receives, but an exact sum keeps the invariant checkable and
    // the bilinear interpolation between neighbouring texels energy-preserving.
    const int q0 =
        std::clamp<int>(static_cast<int>(std::lround(255.0f * w0 / sum)), 0, 255);
    auto put = [&](int slot, int v) {
      if (v <= 0) return;
      const auto b = static_cast<uint8_t>(v);
      if (slot < 4) {
        out.slots0[i * 4 + slot] = b;
      } else {
        out.slots1[i * 4 + (slot - 4)] = b;
      }
    };
    if (second < 0 || q0 >= 255) {
      put(best, 255);
    } else {
      put(best, q0);
      put(second, 255 - q0);
    }
  }

  return out;
}

bool ResolveGroundPacks(const std::string& manifest_path,
                        mapgen::TerrainClass terrain_class,
                        std::vector<std::string>& out_pack_dirs) {
  out_pack_dirs.clear();

  std::ifstream f(manifest_path);
  if (!f) {
    spdlog::error("ResolveGroundPacks: cannot open {}", manifest_path);
    return false;
  }
  nlohmann::json manifest;
  try {
    f >> manifest;
  } catch (const nlohmann::json::exception& e) {
    spdlog::error("ResolveGroundPacks: {}: {}", manifest_path, e.what());
    return false;
  }

  if (!manifest.contains("default") || !manifest["default"].is_object()) {
    spdlog::error("ResolveGroundPacks: {} has no \"default\" block",
                  manifest_path);
    return false;
  }
  const nlohmann::json& fallback = manifest["default"];

  // A class with no block of its own is not an error: an unlabelled patch, or
  // one from a newer terrain-net with a ninth class, should still render.
  const std::string key(mapgen::terrain_class_name(terrain_class));
  const nlohmann::json* block = &fallback;
  if (manifest.contains(key) && manifest[key].is_object()) {
    block = &manifest[key];
  }

  out_pack_dirs.reserve(kGroundSlotCount);
  for (int s = 0; s < kGroundSlotCount; ++s) {
    const char* name = ground_slot_name(static_cast<GroundSlot>(s));
    // Per-class blocks may be PARTIAL -- naming only the rock a tor needs and
    // inheriting the rest -- so each slot falls back independently.
    const nlohmann::json* src = block->contains(name) ? block : &fallback;
    if (!src->contains(name) || !(*src)[name].is_string()) {
      spdlog::error("ResolveGroundPacks: {}: slot \"{}\" missing or not a string",
                    manifest_path, name);
      out_pack_dirs.clear();
      return false;
    }
    out_pack_dirs.push_back((*src)[name].get<std::string>());
  }
  return true;
}

}  // namespace badlands
