#include "mapgen/map_io.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <deque>
#include <fstream>
#include <sstream>

#include "mapgen/biomes.hpp"

namespace badlands::mapgen {

namespace {

// Reads exactly `count` elements of T. Anything else -- short file, long file,
// missing file -- is an error. The rasters are headerless, so this size check
// against the manifest is the only thing standing between a wrong --resolution
// and a silently reinterpreted map.
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

}  // namespace

std::optional<MapManifest> load_manifest(const std::string& dir,
                                         std::string* error) {
  const std::string path = dir + "/map.txt";
  std::ifstream f(path);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return std::nullopt;
  }
  MapManifest m;
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

std::optional<MapArtifacts> load_map(const std::string& dir, std::string* error) {
  const std::optional<MapManifest> man = load_manifest(dir, error);
  if (!man) return std::nullopt;

  const int n = man->resolution;
  const size_t count = static_cast<size_t>(n) * n;

  std::vector<float> height, level, soil;
  std::vector<uint8_t> biome;
  if (!read_raster(dir + "/height.f32", count, height, error)) return std::nullopt;
  if (!read_raster(dir + "/level.f32", count, level, error)) return std::nullopt;
  if (!read_raster(dir + "/biome.u8", count, biome, error)) return std::nullopt;
  // Soil is OPTIONAL: it arrived with the two-layer substrate, and a map written
  // before it is still perfectly renderable. A present-but-malformed file is
  // still an error -- only absence is tolerated.
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

  MapArtifacts art;
  art.heightmap = Field2D<float>(n, n);
  art.heightmap.data = std::move(height);
  art.biome = Field2D<uint8_t>(n, n);
  art.biome.data = std::move(biome);
  // mapview reads bedrock only for its dimensions; the latent field the biomes
  // were cut from does not survive the raster form.
  art.bedrock = art.heightmap;
  if (have_soil) {
    art.sediment = Field2D<float>(n, n);
    art.sediment.data = std::move(soil);
  }

  Field2D<float> level_field(n, n);
  level_field.data = std::move(level);
  const float texel_m = man->world_size_m / static_cast<float>(n);
  derive_water(art.heightmap, level_field, texel_m, art.water_depth, art.lake_id,
               art.lakes);

  // The remaining artifacts are river/flow products this form does not carry.
  // They are left empty rather than faked; mapview does not read them.
  return art;
}

}  // namespace badlands::mapgen
