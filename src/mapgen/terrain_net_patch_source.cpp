#include "mapgen/terrain_net_patch_source.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "mapgen/cover.hpp"
#include "mapgen/nodata_fill.hpp"
#include "mapgen/patch_io.hpp"
#include "mapgen/soil_estimate.hpp"
#include "mapgen/standing_water.hpp"

namespace badlands::mapgen {

namespace {

// ESA WorldCover v200 class codes -> badlands' own vocabulary.
//
// The translation is the point of the seam: production terrain will not come
// from WorldCover at all, so the contract must not carry ESA's numbering. An
// unrecognised code is Unknown rather than a guess -- a sentinel that looks
// like a legitimate class is one nobody notices.
//
// This reads landcover.r8, NOT material.r8. The material raster collapses
// shrub, grass, crop and moss into one class and pre-bakes a slope > 45 deg ->
// rock rule at the survey's resolution; both are the wrong input for placing
// vegetation, and the rock rule is one we would rather apply ourselves at 1 m
// against our own soil field.
Cover CoverFromWorldCover(uint8_t code) {
  switch (code) {
    case 10: return Cover::Tree;
    case 20: return Cover::Shrub;
    case 30: return Cover::Grass;
    case 40: return Cover::Crop;
    case 50: return Cover::Built;
    case 60: return Cover::Bare;
    case 70: return Cover::Snow;
    case 80: return Cover::Water;
    case 90: return Cover::Wetland;
    case 95: return Cover::Wetland;  // mangrove; no separate class here
    case 100: return Cover::Moss;
    default: return Cover::Unknown;
  }
}

bool fail(std::string* error, const std::string& why) {
  if (error) *error = why;
  return false;
}

// Reads exactly `count` elements of T, refusing anything longer or shorter.
// The rasters are headerless, so this check is the only thing between a wrong
// sidecar and a map silently reinterpreted at the wrong stride.
template <typename T>
bool read_flat(const std::string& path, size_t count, std::vector<T>& out,
               std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return fail(error, "cannot open " + path);
  const std::streamsize bytes = f.tellg();
  const size_t want = count * sizeof(T);
  if (bytes < 0 || static_cast<size_t>(bytes) != want) {
    std::ostringstream os;
    os << path << ": expected " << want << " bytes, found " << bytes;
    return fail(error, os.str());
  }
  out.resize(count);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(want));
  if (!f) return fail(error, "short read on " + path);
  return true;
}

}  // namespace

bool IsTerrainNetBundle(const std::string& dir) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(dir) / "raw.json", ec);
}

PatchData TerrainNetPatchSource::Fetch(const PatchRequest&) const {
  return patch_;
}

std::unique_ptr<TerrainNetPatchSource> LoadTerrainNetPatchSource(
    const std::string& dir, std::string* error) {
  const std::filesystem::path base(dir);

  nlohmann::json raw;
  {
    std::ifstream f(base / "raw.json");
    if (!f) {
      fail(error, "cannot open " + dir + "/raw.json");
      return nullptr;
    }
    try {
      f >> raw;
    } catch (const nlohmann::json::exception& e) {
      fail(error, dir + "/raw.json: " + e.what());
      return nullptr;
    }
  }

  int w = 0, h = 0;
  double res_m = 0.0, nodata = -9999.0;
  bool landcover_present = false;
  try {
    w = raw.at("width").get<int>();
    h = raw.at("height").get<int>();
    res_m = raw.at("res_m").get<double>();
    nodata = raw.value("height_nodata", -9999.0);
    landcover_present = raw.value("landcover_present", false);
  } catch (const nlohmann::json::exception& e) {
    fail(error, dir + "/raw.json: " + e.what());
    return nullptr;
  }
  if (w <= 0 || h <= 0 || !(res_m > 0.0)) {
    fail(error, dir + "/raw.json: degenerate grid");
    return nullptr;
  }
  if (!landcover_present) {
    // The sidecar states this outright rather than leaving it to be inferred
    // from a missing file, so refusing here is a decision on stated fact. A
    // bundle without land cover has no observed water and no vegetation
    // signal -- it would render as one undifferentiated surface.
    fail(error, dir + ": bundle carries no land cover (re-fetch with land "
                      "cover enabled; raw.json says landcover_present=false)");
    return nullptr;
  }

  const size_t src_count = static_cast<size_t>(w) * static_cast<size_t>(h);
  std::vector<float> height;
  std::vector<uint8_t> landcover;
  if (!read_flat(base.string() + "/height.r32", src_count, height, error) ||
      !read_flat(base.string() + "/landcover.r8", src_count, landcover, error)) {
    return nullptr;
  }

  auto src = std::unique_ptr<TerrainNetPatchSource>(new TerrainNetPatchSource());
  PatchData& p = src->patch_;

  // CROPPED TO THE LARGEST CENTRED SQUARE. A bundle is whatever the survey's
  // tiling and the requested bbox produced -- 1088x1072 for lake-district -- but
  // PatchRequest carries ONE `resolution`, because a patch is square everywhere
  // else in the contract. Cropping a few columns is a far smaller thing than
  // loosening the one frozen interface in the split, and the discarded strip is
  // 1.5% of the extent.
  const int n = std::min(w, h);
  const int off_x = (w - n) / 2;
  const int off_y = (h - n) / 2;

  // The bundle's rows run NORTH TO SOUTH; the patch lattice is zero-based with
  // +z increasing with the row index. Reading the sidecar's row_order rather
  // than assuming it is what keeps a future provider from loading mirrored.
  const bool flip = raw.value("row_order", std::string("north_to_south")) ==
                    std::string("north_to_south");

  p.texel_m = static_cast<float>(res_m);
  p.origin_m = glm::dvec2(0.0);  // provenance only; the survey's CRS is not ours
  p.height = Field2D<float>(n, n);
  p.cover = Field2D<uint8_t>(n, n, static_cast<uint8_t>(Cover::Unknown));
  for (int y = 0; y < n; ++y) {
    const int row = off_y + y;
    const int sy = flip ? (h - 1 - row) : row;
    for (int x = 0; x < n; ++x) {
      const size_t si = static_cast<size_t>(sy) * w + (off_x + x);
      p.height.at(x, y) = height[si];
      p.cover.at(x, y) = static_cast<uint8_t>(CoverFromWorldCover(landcover[si]));
    }
  }

  // 1. THE PREPASS, before anything differentiates the field. A sentinel is
  //    hundreds of metres below its neighbours, so every void boundary is a
  //    one-texel cliff to slope, curvature, the hillshade and the cluster DAG
  //    alike.
  const Field2D<uint8_t> filled =
      fill_nodata(p.height, static_cast<float>(nodata));
  const size_t count = p.height.size();
  size_t filled_count = 0;
  for (uint8_t v : filled.data) filled_count += (v != 0);
  src->nodata_fraction_ =
      count > 0 ? static_cast<float>(filled_count) / static_cast<float>(count)
                : 0.0f;
  if (src->nodata_fraction_ > kMaxNodataFraction) {
    std::ostringstream os;
    os << dir << ": " << (src->nodata_fraction_ * 100.0f)
       << "% of the height raster is nodata, past the "
       << (kMaxNodataFraction * 100.0f) << "% limit";
    fail(error, os.str());
    return nullptr;
  }
  // Filled ground is extrapolated, not surveyed. Saying so costs nothing and
  // stops a consumer treating an invention as an observation.
  for (size_t i = 0; i < count; ++i) {
    if (filled.data[i]) p.cover.data[i] = static_cast<uint8_t>(Cover::Unknown);
  }

  // 2. Standing water. The survey's value under water is the SURFACE, so this
  //    returns both the level and a bed carved beneath it -- see
  //    standing_water.hpp for the measurement that forces it.
  StandingWater water = derive_standing_water(p.height, p.cover, p.texel_m);
  p.height = std::move(water.bed);
  p.level = std::move(water.level);
  derive_water(p.height, p.level, p.texel_m, p.water_depth, p.lake_id, p.lakes);

  // 3. Soil, estimated from slope. TEMPORARY -- see soil_estimate.hpp.
  p.soil = estimate_soil(p.height, p.texel_m);

  // 4. Rivers stay empty. A ~1 km window has no catchment worth extracting.
  p.elevation_range = compute_elevation_range(p.height);

  // The morphology label lives in manifest.json, which is the bundle's real
  // manifest; raw.json is only the sidecar for the flat arrays. Its absence is
  // tolerated -- a --bbox fetch is legitimately unlabelled.
  {
    std::ifstream f(base / "manifest.json");
    if (f) {
      try {
        nlohmann::json man;
        f >> man;
        const auto& area = man.at("area");
        if (area.contains("detail_class") && area["detail_class"].is_string()) {
          p.terrain_class =
              terrain_class_from_name(area["detail_class"].get<std::string>());
        }
        src->area_ = area.value("name", area.value("key", std::string{}));
      } catch (const nlohmann::json::exception&) {
        // Provenance only. A malformed manifest costs a label, not the patch.
      }
    }
  }

  src->native_.origin_m = p.origin_m;
  src->native_.world_size_m = p.texel_m * static_cast<float>(n);
  src->native_.resolution = n;
  return src;
}

}  // namespace badlands::mapgen
