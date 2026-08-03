#include "mapgen/patch_io.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "mapgen/biomes.hpp"
#include "mapgen/river_io.hpp"

namespace badlands::mapgen {

namespace {

// Reads exactly `count` elements of T. Anything else -- short file, long file,
// missing file -- is an error. The rasters are headerless, so this size check
// against the manifest is the only thing standing between a wrong resolution
// and a silently reinterpreted patch.
template <typename T>
bool read_raster(const std::string& path, size_t count, std::vector<T>& out,
                 std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  const std::streamsize bytes = f.tellg();
  const size_t want = count * sizeof(T);
  if (bytes < 0 || static_cast<size_t>(bytes) != want) {
    if (error) {
      std::ostringstream os;
      os << path << " is " << bytes << " bytes, expected " << want << " ("
         << count << " x " << sizeof(T) << ") -- does it match the manifest?";
      *error = os.str();
    }
    return false;
  }
  out.resize(count);
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(out.data()),
              static_cast<std::streamsize>(want))) {
    if (error) *error = "short read on " + path;
    return false;
  }
  return true;
}

template <typename T>
bool write_raster(const std::string& path, const std::vector<T>& in,
                  std::string* error) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot open " + path + " for writing";
    return false;
  }
  f.write(reinterpret_cast<const char*>(in.data()),
          static_cast<std::streamsize>(in.size() * sizeof(T)));
  if (!f) {
    if (error) *error = "short write on " + path;
    return false;
  }
  return true;
}

}  // namespace

std::optional<PatchManifest> load_patch_manifest(const std::string& dir,
                                                 std::string* error) {
  const std::string path = dir + "/map.txt";
  std::ifstream f(path);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return std::nullopt;
  }
  PatchManifest m;
  bool have_res = false, have_size = false;
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ls(line);
    std::string key;
    if (!(ls >> key) || key.empty() || key[0] == '#') continue;
    if (key == "resolution") {
      have_res = static_cast<bool>(ls >> m.resolution);
    } else if (key == "world_size_m") {
      have_size = static_cast<bool>(ls >> m.world_size_m);
    } else if (key == "origin_m") {
      // Provenance: where this patch sat in its parent world. Optional, since
      // a synthetic or hand-built patch has no parent.
      double ox = 0.0, oy = 0.0;
      if (ls >> ox >> oy) m.origin_m = glm::dvec2(ox, oy);
    } else if (key == "source") {
      std::getline(ls, m.source);
      if (!m.source.empty() && m.source[0] == ' ') m.source.erase(0, 1);
    }
    // Unknown keys are ignored on purpose: the writer may add provenance
    // fields without breaking older readers.
  }
  if (!have_res || m.resolution <= 0) {
    if (error) *error = path + ": missing or invalid 'resolution'";
    return std::nullopt;
  }
  if (!have_size || !(m.world_size_m > 0.0f)) {
    if (error) *error = path + ": missing or invalid 'world_size_m'";
    return std::nullopt;
  }
  return m;
}

void derive_water(const Field2D<float>& heightmap, const Field2D<float>& level,
                  float texel_m, Field2D<float>& water_depth,
                  Field2D<int32_t>& lake_id, std::vector<LakeInfo>& lakes) {
  const int w = heightmap.width, h = heightmap.height;
  water_depth = Field2D<float>(w, h, 0.0f);
  lake_id = Field2D<int32_t>(w, h, -1);
  lakes.clear();
  if (w <= 0 || h <= 0) return;
  // `level` is indexed over heightmap's extent below. load_patch builds both
  // n*n, but this is exported for other callers, and every other size
  // assumption in this file is checked rather than assumed.
  if (level.width != w || level.height != h) return;

  for (size_t i = 0; i < water_depth.data.size(); ++i) {
    const float d = level.data[i] - heightmap.data[i];
    water_depth.data[i] = d > 0.0f ? d : 0.0f;
  }

  const float cell_area = texel_m * texel_m;
  std::deque<int> queue;
  for (int start = 0; start < w * h; ++start) {
    if (water_depth.data[start] <= 0.0f || lake_id.data[start] >= 0) continue;
    const int32_t id = static_cast<int32_t>(lakes.size());
    LakeInfo info;
    info.kind = LakeKind::Emergent;
    // The level is constant across a component by construction, so the seed
    // texel is representative; taking it verbatim keeps the surface exactly
    // flat rather than averaging float error back into it.
    info.level_m = level.data[start];
    float area = 0.0f, deepest = 0.0f;

    lake_id.data[start] = id;
    queue.clear();
    queue.push_back(start);
    while (!queue.empty()) {
      const int c = queue.front();
      queue.pop_front();
      area += cell_area;
      deepest = std::max(deepest, water_depth.data[c]);
      const int cx = c % w, cy = c / w;
      const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4; ++k) {
        const int nx = cx + dx[k], ny = cy + dy[k];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const int t = ny * w + nx;
        if (water_depth.data[t] <= 0.0f || lake_id.data[t] >= 0) continue;
        lake_id.data[t] = id;
        queue.push_back(t);
      }
    }
    info.area_m2 = area;
    info.max_depth_m = deepest;
    info.outlet_cell = -1;  // no sill information survives the raster form
    lakes.push_back(info);
  }
}

std::optional<PatchData> load_patch(const std::string& dir, std::string* error) {
  const std::optional<PatchManifest> man = load_patch_manifest(dir, error);
  if (!man) return std::nullopt;

  const int n = man->resolution;
  const size_t count = static_cast<size_t>(n) * n;

  std::vector<float> height, level, soil;
  std::vector<uint8_t> biome;
  if (!read_raster(dir + "/height.f32", count, height, error)) return std::nullopt;
  if (!read_raster(dir + "/level.f32", count, level, error)) return std::nullopt;
  if (!read_raster(dir + "/biome.u8", count, biome, error)) return std::nullopt;
  // Soil is optional ON DISK (see the header): a patch written before the
  // two-layer substrate is still perfectly renderable, and loads as zeros. A
  // present-but-malformed file is still an error.
  const bool have_soil =
      std::filesystem::exists(std::filesystem::path(dir) / "soil.f32");
  if (have_soil && !read_raster(dir + "/soil.f32", count, soil, error))
    return std::nullopt;

  // A NaN reaches the GPU as a hole in the terrain with no diagnostic, and a
  // single out-of-range biome samples the wrong texture array layer silently.
  // Both are cheap to reject here.
  for (size_t i = 0; i < count; ++i) {
    if (!std::isfinite(height[i]) || !std::isfinite(level[i])) {
      if (error) *error = dir + ": non-finite sample in height.f32/level.f32";
      return std::nullopt;
    }
    if (biome[i] >= kBiomeCount) {
      if (error) {
        std::ostringstream os;
        os << dir << ": biome.u8 holds " << int(biome[i]) << ", valid range is 0.."
           << (kBiomeCount - 1);
        *error = os.str();
      }
      return std::nullopt;
    }
  }

  PatchData p;
  p.texel_m = man->world_size_m / static_cast<float>(n);
  p.origin_m = man->origin_m;

  p.height = Field2D<float>(n, n);
  p.height.data = std::move(height);
  p.level = Field2D<float>(n, n);
  p.level.data = std::move(level);
  p.biome = Field2D<uint8_t>(n, n);
  p.biome.data = std::move(biome);
  p.soil = Field2D<float>(n, n, 0.0f);
  if (have_soil) p.soil.data = std::move(soil);

  derive_water(p.height, p.level, p.texel_m, p.water_depth, p.lake_id, p.lakes);
  p.elevation_range = compute_elevation_range(p.height);

  // Rivers are optional on disk, the same way soil is: absence means a patch
  // written before rivers.bin existed, not an error. A present-but-malformed
  // file IS an error.
  const std::string rivers_path = dir + "/rivers.bin";
  if (std::filesystem::exists(rivers_path)) {
    std::optional<RiverGraph> rivers = read_river_graph(rivers_path, error);
    if (!rivers) return std::nullopt;
    p.rivers = std::move(*rivers);
  }
  return p;
}

bool write_patch(const std::string& dir, const PatchData& patch,
                 const std::string& source, std::string* error) {
  const int n = patch.height.width;
  if (n <= 0 || patch.height.height != n) {
    if (error) *error = "write_patch: patch height raster is empty or non-square";
    return false;
  }
  const size_t count = static_cast<size_t>(n) * n;
  if (patch.level.data.size() != count || patch.biome.data.size() != count ||
      patch.soil.data.size() != count) {
    if (error) *error = "write_patch: rasters disagree with the height extent";
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    if (error) *error = "cannot create " + dir + ": " + ec.message();
    return false;
  }

  {
    std::ofstream f(dir + "/map.txt", std::ios::trunc);
    if (!f) {
      if (error) *error = "cannot open " + dir + "/map.txt for writing";
      return false;
    }
    // ENOUGH DIGITS TO ROUND-TRIP. The stream default is 6 SIGNIFICANT digits,
    // which was harmless while every patch had a round world_size_m -- but a
    // CoarseWorldPatchSource patch can sit at any origin and any extent, and a
    // 1234.5678 m patch reloading as 1234.57 m changes the derived texel_m and
    // with it every world-metre coordinate the patch carries. 9 digits is
    // float's round-trip guarantee; 17 is double's, which origin_m needs.
    f << "resolution " << n << "\n";
    f << std::setprecision(9)
      << "world_size_m " << (patch.texel_m * static_cast<float>(n)) << "\n";
    f << std::setprecision(17)
      << "origin_m " << patch.origin_m.x << " " << patch.origin_m.y << "\n";
    if (!source.empty()) f << "source " << source << "\n";
    if (!f) {
      if (error) *error = "short write on " + dir + "/map.txt";
      return false;
    }
  }

  return write_raster(dir + "/height.f32", patch.height.data, error) &&
         write_raster(dir + "/level.f32", patch.level.data, error) &&
         write_raster(dir + "/biome.u8", patch.biome.data, error) &&
         write_raster(dir + "/soil.f32", patch.soil.data, error) &&
         write_river_graph(dir + "/rivers.bin", patch.rivers, error);
}

}  // namespace badlands::mapgen
